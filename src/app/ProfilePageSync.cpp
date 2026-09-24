#include "app/ProfilePageSync.h"
#include "diagnostics/Logging.h"

#include "app/ProfileSession.h"
#include "domain/ProfilePageCodec.h"
#include "network/SyncEngine.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherProfilePageRepository.h"

#include <QDateTime>
#include <QMetaObject>

#include <algorithm>
#include <climits>
#include <utility>

namespace OpenChat {

namespace {

// Answers are budgeted per contact per day (Limits::answersPerDay).
constexpr qint64 answerWindowMs = 24LL * 60 * 60 * 1000;
// The request back-off doubles at most this often before its cap applies.
constexpr int maxBackoffDoublings = 6;
// The schedule timer never sleeps longer than this, so a wall clock that jumps
// forward (a suspend, a clock correction) is noticed within a minute.
constexpr qint64 maxScheduleWaitMs = 60'000;
// The back-off stops growing long before this; bounded so it cannot overflow.
constexpr int maxUnanswered = 1'000;

[[nodiscard]] int wireKind(Profile::MediaKind kind)
{
    return static_cast<int>(kind);
}

[[nodiscard]] std::optional<QByteArray> hashOf(const Profile::MediaRef &ref)
{
    return ref.isSet() ? std::optional<QByteArray>(ref.sha256) : std::nullopt;
}

// The hash `stored` names in the slot for `kind`.
[[nodiscard]] const std::optional<QByteArray> &slotOf(const StoredContactPage &stored,
                                                      Profile::MediaKind kind)
{
    return kind == Profile::MediaKind::SongMedia ? stored.song : stored.background;
}

[[nodiscard]] bool isDeliverable(const ContactRecord &contact)
{
    return contact.state == ContactState::Accepted && contact.conversationId.has_value()
           && contact.peerDeviceId.has_value();
}

[[nodiscard]] const ContactRecord *findContact(const QVector<ContactRecord> &contacts,
                                               const AccountId &account)
{
    const auto found = std::find_if(contacts.cbegin(), contacts.cend(),
                                    [&](const ContactRecord &contact) {
                                        return contact.accountId == account;
                                    });
    return found == contacts.cend() ? nullptr : &*found;
}

// Whether contacts will take this blob: the receiver's own decoder, run on
// the very message we would send (hash, per-kind cap, JPEG scan walk or song
// container).
[[nodiscard]] bool passesArrivalChecks(Profile::MediaKind kind, const QByteArray &sha256,
                                       const QByteArray &data)
{
    return decodePageMedia(encodePageMedia({kind, sha256, data})).has_value();
}

} // namespace

// One payload for one contact, and what to note once the engine has taken it.
struct ProfilePageSync::PumpJob final {
    AccountId contact;
    ConversationId conversation;
    DeviceId device;
    QByteArray payload;
    std::function<void()> record; // after sendProfileUpdate returned, engine still running
    std::function<void()> drop;   // the payload can never be sent: do not offer it again
};

ProfilePageSync::ProfilePageSync(ProfileSession &session, SyncEngine &engine, Clock clock,
                                 Random random, Limits limits, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_engine(engine)
    , m_clock(std::move(clock))
    , m_random(std::move(random))
    , m_limits(limits)
{
    connect(&m_engine, &SyncEngine::profileUpdateReceived, this,
            &ProfilePageSync::onProfileUpdateReceived);
    connect(&m_engine, &SyncEngine::linkUp, this, &ProfilePageSync::onLinkUp);

    m_pumpTimer.setInterval(static_cast<int>(std::clamp<qint64>(m_limits.pumpIntervalMs, 1, INT_MAX)));
    connect(&m_pumpTimer, &QTimer::timeout, this, &ProfilePageSync::pumpOnce);
    m_scheduleTimer.setSingleShot(true);
    connect(&m_scheduleTimer, &QTimer::timeout, this, &ProfilePageSync::runDueSchedule);

    reloadLocal();
    collectGarbage();
    scheduleStartupRequests();
    // Reconciliation: whatever a crash or an offline publish left owed goes
    // out now (the pump finds it and stops again if nothing is).
    wakePump();
}

ProfilePageSync::~ProfilePageSync() = default;

// ---------------------------------------------------------------------------
// Own page
// ---------------------------------------------------------------------------

Profile::Page ProfilePageSync::publishedPage() const
{
    if (m_local.publishedCore.isEmpty())
        return Profile::defaultPage();
    auto page = decodePageCore(m_local.publishedCore);
    if (!page) {
        qCWarning(contactsLog) << "The stored published profile page does not decode";
        return Profile::defaultPage();
    }
    return *std::move(page);
}

qint64 ProfilePageSync::publishedRevision() const
{
    return m_local.publishedRevision;
}

std::optional<Profile::Page> ProfilePageSync::draft() const
{
    if (m_local.draftCore.isEmpty())
        return std::nullopt;
    return decodePageCore(m_local.draftCore);
}

QString ProfilePageSync::draftSongSource() const
{
    return m_local.draftSongSource;
}

qint64 ProfilePageSync::draftUpdatedAtMs() const
{
    return m_local.draftUpdatedAtMs;
}

bool ProfilePageSync::saveDraft(const Profile::Page &draft, const QString &songSource)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return false;
    const Profile::Page page = Profile::normalized(draft);
    const QByteArray core = encodePageCore(page);
    // draft() decodes with the wire cap, so a larger draft could never be read back.
    if (core.size() > maxPageCoreBytes) {
        qCWarning(contactsLog) << "Not saving a profile page draft of" << core.size() << "bytes";
        return false;
    }
    if (!repository->saveDraft(core, hashOf(page.background), hashOf(page.song), songSource, now())
             .hasValue()) {
        qCWarning(contactsLog) << "Could not save the profile page draft";
        return false;
    }
    reloadLocal();
    return true;
}

bool ProfilePageSync::discardDraft()
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return false;
    if (!repository->clearDraft(now()).hasValue()) {
        qCWarning(contactsLog) << "Could not discard the profile page draft";
        return false;
    }
    reloadLocal();
    return true;
}

std::optional<QByteArray> ProfilePageSync::addLocalMedia(Profile::MediaKind kind,
                                                         const QByteArray &bytes)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return std::nullopt;
    const QByteArray sha256 = pageMediaHash(bytes);
    if (!passesArrivalChecks(kind, sha256, bytes)) {
        qCWarning(contactsLog) << "Refused profile media contacts would reject; kind"
                               << wireKind(kind) << "bytes" << bytes.size();
        return std::nullopt;
    }
    if (!repository->putLocalDraftMedia(wireKind(kind), sha256, bytes, now()).hasValue()) {
        qCWarning(contactsLog) << "Could not store profile media";
        return std::nullopt;
    }
    reloadLocal();
    return sha256;
}

QByteArray ProfilePageSync::localMedia(const QByteArray &sha256) const
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr || sha256.size() != 32)
        return {};
    auto data = repository->localMedia(sha256);
    if (!data.hasValue() || !data.value())
        return {};
    return *std::move(data).value();
}

qint64 ProfilePageSync::publish(const Profile::Page &input)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return 0;
    const qint64 nowMs = now();
    Profile::Page page = Profile::normalized(input);
    page.revision = Profile::nextRevision(publishedRevision(), nowMs);
    page.publishedAtMs = nowMs;

    // Every ref must name a local blob that passes its slot's arrival checks;
    // a page naming media nobody can receive would show a hole forever.
    const std::pair<Profile::MediaKind, const Profile::MediaRef *> refs[] = {
        {Profile::MediaKind::BackgroundImageMedia, &page.background},
        {Profile::MediaKind::SongMedia, &page.song}};
    for (const auto &[kind, ref] : refs) {
        if (!ref->isSet())
            continue;
        const QByteArray data = localMedia(ref->sha256);
        if (data.isEmpty() || !passesArrivalChecks(kind, ref->sha256, data)) {
            qCWarning(contactsLog) << "Not publishing: a media ref names no usable local blob";
            return 0;
        }
    }

    const QByteArray core = encodePageCore(page);
    if (core.size() > maxPageCoreBytes) {
        qCWarning(contactsLog) << "Not publishing a profile page core of" << core.size() << "bytes";
        return 0;
    }
    const auto background = hashOf(page.background);
    const auto song = hashOf(page.song);
    if (!repository->savePublished(core, page.revision, background, song, nowMs).hasValue()) {
        qCWarning(contactsLog) << "Could not store the published profile page";
        return 0;
    }
    reloadLocal();

    // Records of blobs the new page does not use are forgotten, so a later
    // revision that brings one back sends it again; the kept ones make a
    // re-publish with unchanged media cost only the core.
    QVector<QByteArray> keep;
    if (background)
        keep.push_back(*background);
    if (song && song != background)
        keep.push_back(*song);
    if (!repository->forgetSentMediaExcept(keep).hasValue())
        qCWarning(contactsLog) << "Could not update the profile media delivery records";
    collectGarbage();

    emit localPagePublished(page.revision);
    wakePump();
    return page.revision;
}

// ---------------------------------------------------------------------------
// Contacts' pages
// ---------------------------------------------------------------------------

std::optional<ProfilePageSync::ReceivedPage>
ProfilePageSync::contactPage(const AccountId &contact) const
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return std::nullopt;
    auto stored = repository->contactPage(contact);
    if (!stored.hasValue()) {
        qCWarning(contactsLog) << "Could not read a contact's profile page";
        return std::nullopt;
    }
    if (!stored.value())
        return std::nullopt;
    const StoredContactPage &row = *stored.value();
    auto page = decodePageCore(row.core);
    if (!page) {
        qCWarning(contactsLog) << "A stored contact profile page does not decode";
        return std::nullopt;
    }
    const auto present = [&](const std::optional<QByteArray> &hash, Profile::MediaKind kind) {
        if (!hash)
            return false;
        const auto has = repository->hasContactMedia(contact, *hash, wireKind(kind));
        return has.hasValue() && has.value();
    };
    ReceivedPage received;
    received.page = *std::move(page);
    received.backgroundPresent = present(row.background, Profile::MediaKind::BackgroundImageMedia);
    received.songPresent = present(row.song, Profile::MediaKind::SongMedia);
    received.receivedAtMs = row.receivedAtMs;
    return received;
}

QByteArray ProfilePageSync::contactMedia(const AccountId &contact, const QByteArray &sha256,
                                         Profile::MediaKind kind) const
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr || sha256.size() != 32)
        return {};
    // Only in the slot their current core names it for: a pending blob, or
    // one declared for the other slot, is never shown.
    const auto stored = repository->contactPage(contact);
    if (!stored.hasValue() || !stored.value() || slotOf(*stored.value(), kind) != sha256)
        return {};
    auto data = repository->contactMedia(contact, sha256, wireKind(kind));
    if (!data.hasValue() || !data.value())
        return {};
    return *std::move(data).value();
}

void ProfilePageSync::markViewed(const AccountId &contact)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    if (!repository->markViewed(contact, now()).hasValue())
        qCWarning(contactsLog) << "Could not record a profile page view";

    // Opening a page sends nothing (the owner must not learn when their page
    // was looked at), with one documented exception: media we evicted under
    // the storage cap, which only a request can bring back. Eviction leaves
    // the revision's media marked unasked (enforceSoftCap); media still in
    // transit has its grace request scheduled, and media we asked for once
    // is marked asked, so neither is chased from here.
    const auto stored = repository->contactPage(contact);
    if (!stored.hasValue() || !stored.value() || missingMedia(*stored.value()).isEmpty())
        return;
    const auto state = repository->requestState(contact);
    if (!state.hasValue() || state.value().mediaRequestedRevision >= stored.value()->revision
        || isScheduled(contact, Trigger::MissingMedia))
        return;
    requestIfDue(contact, Trigger::Viewed);
}

bool ProfilePageSync::isAwaitingPage(const AccountId &contact) const
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return false;
    const auto stored = repository->contactPage(contact);
    if (!stored.hasValue() || stored.value())
        return false;
    // The acceptance-time request, still to come or waiting for the pump.
    if (isScheduled(contact, Trigger::Acceptance)
        || std::any_of(m_requests.cbegin(), m_requests.cend(), [&](const Request &request) {
               return request.contact == contact && request.trigger == Trigger::Acceptance;
           }))
        return true;
    const auto state = repository->requestState(contact);
    if (!state.hasValue() || state.value().lastRequestAtMs <= 0)
        return false;
    const qint64 age = now() - state.value().lastRequestAtMs;
    return age >= 0 && age < m_limits.pageLoadingWindowMs;
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

void ProfilePageSync::setCallActive(bool active)
{
    if (m_callActive == active)
        return;
    m_callActive = active;
    if (!active)
        wakePump(); // media and answers held back during the call
}

bool ProfilePageSync::isLinkUp() const
{
    return m_engine.isLinkUp();
}

bool ProfilePageSync::hasOwedDeliveries() const
{
    if (!m_answers.isEmpty() || !m_requests.isEmpty())
        return true;
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return false;
    const qint64 revision = publishedRevision();
    const auto blobs = publishedBlobs();
    for (const ContactRecord &contact : deliverableContacts()) {
        const auto delivery = repository->delivery(contact.accountId);
        if (!delivery.hasValue())
            continue;
        const PageDelivery &recorded = delivery.value();
        // A changed device voids every record (see currentDelivery).
        const bool restart = recorded.device != contact.peerDeviceId;
        if (revision > 0 && m_unsendableRevision != revision
            && (restart || recorded.sentRevision < revision))
            return true;
        if (restart || !recorded.pageCapable)
            continue;
        for (const auto &[kind, hash] : blobs) {
            const auto sentAt = repository->mediaSentAt(contact.accountId, hash);
            if (sentAt.hasValue() && !sentAt.value())
                return true;
        }
    }
    return false;
}

void ProfilePageSync::collectGarbage()
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    const qint64 nowMs = now();
    const auto collected = repository->collectGarbage(nowMs - m_limits.pendingMediaTtlMs,
                                                      nowMs - m_limits.localBlobGraceMs);
    if (!collected.hasValue())
        qCWarning(contactsLog) << "Profile media collection failed";
    else if (collected.value() > 0)
        qCDebug(contactsLog) << "Collected" << collected.value() << "profile media blobs";
}

void ProfilePageSync::onContactAccepted(const AccountId &contact)
{
    // A new binding: the sender cache may still name an old one.
    m_senderCache.clear();
    // The published page is owed to them now; the pump works that out.
    if (publishedRevision() > 0)
        wakePump();
    // Ask for theirs, unless it arrives first.
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    const auto stored = repository->contactPage(contact);
    if (!stored.hasValue() || stored.value())
        return;
    unschedule(contact, Trigger::Startup); // one request is enough
    schedule(contact, Trigger::Acceptance,
             now() + m_limits.acceptRequestDelayMs + randomBelow(m_limits.acceptRequestJitterMs));
    emit awaitingChanged(contact);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

ProfilePageRepository *ProfilePageSync::pages() const
{
    return m_session.profilePages();
}

qint64 ProfilePageSync::now() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

qint64 ProfilePageSync::randomBelow(qint64 bound) const
{
    if (bound <= 0 || !m_random)
        return 0;
    return std::clamp<qint64>(m_random(bound), 0, bound - 1);
}

void ProfilePageSync::reloadLocal()
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    auto local = repository->localPage();
    if (!local.hasValue()) {
        qCWarning(contactsLog) << "Could not read the local profile page";
        return;
    }
    m_local = std::move(local).value();
}

QVector<std::pair<Profile::MediaKind, QByteArray>> ProfilePageSync::publishedBlobs() const
{
    QVector<std::pair<Profile::MediaKind, QByteArray>> blobs;
    if (m_local.publishedBackground)
        blobs.push_back({Profile::MediaKind::BackgroundImageMedia, *m_local.publishedBackground});
    if (m_local.publishedSong)
        blobs.push_back({Profile::MediaKind::SongMedia, *m_local.publishedSong});
    return blobs;
}

std::optional<Profile::MediaKind> ProfilePageSync::publishedKindOf(const QByteArray &sha256) const
{
    for (const auto &[kind, hash] : publishedBlobs()) {
        if (hash == sha256)
            return kind;
    }
    return std::nullopt;
}

QVector<ContactRecord> ProfilePageSync::deliverableContacts() const
{
    QVector<ContactRecord> deliverable;
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (contacts == nullptr)
        return deliverable;
    const auto all = contacts->contacts();
    if (!all.hasValue()) {
        qCWarning(contactsLog) << "Could not read the contacts for profile pages";
        return deliverable;
    }
    for (const ContactRecord &contact : all.value()) {
        if (isDeliverable(contact))
            deliverable.push_back(contact);
    }
    return deliverable;
}

std::optional<ContactRecord> ProfilePageSync::deliverableContact(const AccountId &account) const
{
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (contacts == nullptr)
        return std::nullopt;
    auto found = contacts->find(account);
    if (!found.hasValue() || !found.value() || !isDeliverable(*found.value()))
        return std::nullopt;
    return *std::move(found).value();
}

std::optional<ContactRecord> ProfilePageSync::resolveSender(const ConversationId &conversation,
                                                            const DeviceId &senderDevice)
{
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (contacts == nullptr)
        return std::nullopt;
    // The contact whose own 1:1 conversation this is, on the device they
    // pinned, and Accepted: not a group, not a pending or blocked peer.
    const auto isSender = [&](const ContactRecord &contact) {
        return contact.state == ContactState::Accepted && contact.conversationId == conversation
               && contact.peerDeviceId == senderDevice;
    };
    if (const auto cached = m_senderCache.constFind(conversation); cached != m_senderCache.cend()) {
        const auto found = contacts->find(cached.value());
        if (found.hasValue() && found.value() && isSender(*found.value()))
            return *found.value();
    }
    m_senderCache.clear();
    const auto all = contacts->contacts();
    if (!all.hasValue()) {
        qCWarning(contactsLog) << "Could not read the contacts for profile pages";
        return std::nullopt;
    }
    std::optional<ContactRecord> sender;
    for (const ContactRecord &contact : all.value()) {
        if (contact.state == ContactState::Accepted && contact.conversationId)
            m_senderCache.insert(*contact.conversationId, contact.accountId);
        if (!sender && isSender(contact))
            sender = contact;
    }
    return sender;
}

std::optional<PageDelivery> ProfilePageSync::currentDelivery(const ContactRecord &contact)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr || !contact.peerDeviceId)
        return std::nullopt;
    const auto stored = repository->delivery(contact.accountId);
    if (!stored.hasValue()) {
        qCWarning(contactsLog) << "Could not read a profile page delivery record";
        return std::nullopt;
    }
    PageDelivery delivery = stored.value();
    if (delivery.device == contact.peerDeviceId)
        return delivery;
    // A device we never delivered to (a new contact, or a contact on a new
    // device) holds nothing we sent: start over. The answer budget belongs to
    // the contact, not the device, so it carries over.
    if (!repository->forgetSentMedia(contact.accountId).hasValue()) {
        qCWarning(contactsLog) << "Could not reset a profile page delivery record";
        return std::nullopt;
    }
    delivery.device = contact.peerDeviceId;
    delivery.sentRevision = -1;
    delivery.sentAtMs = 0;
    delivery.pageCapable = false;
    if (!repository->saveDelivery(delivery).hasValue()) {
        qCWarning(contactsLog) << "Could not reset a profile page delivery record";
        return std::nullopt;
    }
    return delivery;
}

QVector<QByteArray> ProfilePageSync::missingMedia(const StoredContactPage &stored) const
{
    QVector<QByteArray> missing;
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return missing;
    const std::pair<const std::optional<QByteArray> *, Profile::MediaKind> named[] = {
        {&stored.background, Profile::MediaKind::BackgroundImageMedia},
        {&stored.song, Profile::MediaKind::SongMedia}};
    for (const auto &[hash, kind] : named) {
        if (!*hash)
            continue;
        // Present only if THIS contact sent it, as the kind of its slot.
        const auto has = repository->hasContactMedia(stored.account, **hash, wireKind(kind));
        if (has.hasValue() && !has.value())
            missing.push_back(**hash);
    }
    return missing;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void ProfilePageSync::onProfileUpdateReceived(const ConversationId &conversation,
                                              const DeviceId &senderDevice,
                                              const QByteArray &payload)
{
    const ProfilePayloadKind kind = classifyProfilePayload(payload);
    if (kind == ProfilePayloadKind::Legacy)
        return; // presence, status line and picture: ChatController's
    const auto contact = resolveSender(conversation, senderDevice);
    if (!contact) {
        qCDebug(contactsLog) << "Ignored a profile page message from outside a contact's conversation";
        return;
    }
    notePageMessageFrom(*contact);
    switch (kind) {
    case ProfilePayloadKind::PageCore:
        receiveCore(*contact, payload);
        break;
    case ProfilePayloadKind::PageMedia:
        receiveMedia(*contact, payload);
        break;
    case ProfilePayloadKind::PageRequest:
        receiveRequest(*contact, payload);
        break;
    case ProfilePayloadKind::UnknownPage:
        qCDebug(contactsLog) << "Ignored a profile page message this client cannot read";
        break;
    case ProfilePayloadKind::Legacy:
        break;
    }
}

void ProfilePageSync::onLinkUp()
{
    // The engine reports the link even after it has failed closed; nothing
    // can be sent then (the pump says so once).
    if (m_engine.isFailedClosed())
        return;
    scheduleStartupRequests();
    wakePump();
}

void ProfilePageSync::notePageMessageFrom(const ContactRecord &contact)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    // Their client understands pages, so pushing media to them is not wasted.
    if (auto delivery = currentDelivery(contact); delivery && !delivery->pageCapable) {
        delivery->pageCapable = true;
        if (!repository->saveDelivery(*delivery).hasValue())
            qCWarning(contactsLog) << "Could not record a contact as page capable";
        else if (!publishedBlobs().isEmpty())
            wakePump();
    }
    // And they answer: our requests to them start from the base interval again.
    const auto state = repository->requestState(contact.accountId);
    if (state.hasValue() && state.value().unanswered != 0) {
        PageRequestState answered = state.value();
        answered.unanswered = 0;
        if (!repository->saveRequestState(answered).hasValue())
            qCWarning(contactsLog) << "Could not reset a profile page request back-off";
    }
}

void ProfilePageSync::receiveCore(const ContactRecord &contact, const QByteArray &payload)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    const auto page = decodePageCore(payload);
    if (!page) {
        qCWarning(contactsLog) << "Dropped a profile page core that does not decode";
        return;
    }
    const AccountId &account = contact.accountId;
    const bool wasAwaiting = isAwaitingPage(account);

    StoredContactPage stored{account};
    stored.revision = page->revision;
    stored.core = payload; // as received; it decoded, so it is within the core cap
    stored.background = hashOf(page->background);
    stored.song = hashOf(page->song);
    stored.receivedAtMs = now();
    const auto newer = repository->storeContactPage(stored);
    if (!newer.hasValue()) {
        qCWarning(contactsLog) << "Could not store a contact's profile page";
        return;
    }
    if (!newer.value()) {
        qCDebug(contactsLog) << "Ignored a profile page core that is not newer than the stored one";
        return;
    }

    // Their page is here: every request for it, scheduled or queued, is moot.
    // Missing media of this revision gets its own request below.
    for (const Trigger trigger : {Trigger::Acceptance, Trigger::Startup, Trigger::MissingMedia})
        unschedule(account, trigger);
    m_requests.removeIf([&](const Request &request) { return request.contact == account; });

    collectGarbage();
    enforceSoftCap();
    emit contactPageChanged(account);
    if (wasAwaiting)
        emit awaitingChanged(account);

    // Media may trail its core in transit; ask once for this revision if it
    // has not come by the grace.
    if (missingMedia(stored).isEmpty())
        return;
    const auto state = repository->requestState(account);
    if (state.hasValue() && state.value().mediaRequestedRevision < stored.revision)
        schedule(account, Trigger::MissingMedia, now() + m_limits.missingMediaGraceMs);
}

void ProfilePageSync::receiveMedia(const ContactRecord &contact, const QByteArray &payload)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    const auto media = decodePageMedia(payload);
    if (!media) {
        qCWarning(contactsLog) << "Dropped profile page media that failed its checks";
        return;
    }
    const AccountId &account = contact.accountId;
    const int kind = wireKind(media->kind);
    const auto stored = repository->contactPage(account);
    if (!stored.hasValue()) {
        qCWarning(contactsLog) << "Could not read a contact's profile page";
        return;
    }
    // Adopted only in the slot of its own kind; anything else waits as
    // "pending" (media can overtake its core), a bounded few per contact.
    const bool named = stored.value() && slotOf(*stored.value(), media->kind) == media->sha256;
    if (!named) {
        const auto already = repository->hasContactMedia(account, media->sha256, kind);
        if (!already.hasValue())
            return;
        if (!already.value()) {
            const auto pending = repository->pendingContactMediaCount(account);
            if (!pending.hasValue() || pending.value() >= m_limits.maxPendingMediaPerContact) {
                qCDebug(contactsLog) << "Dropped profile page media no page of theirs names";
                return;
            }
        }
    }
    if (!repository->putContactMedia(account, kind, media->sha256, media->data, now()).hasValue()) {
        qCWarning(contactsLog) << "Could not store a contact's profile media";
        return;
    }
    // A page complete now needs no grace request. Left scheduled, it would
    // fire later and fetch again anything the cap evicts meanwhile, which is
    // for opening the page to do (markViewed).
    if (named && stored.value() && missingMedia(*stored.value()).isEmpty())
        unschedule(account, Trigger::MissingMedia);
    collectGarbage();
    enforceSoftCap();
    if (named)
        emit contactPageChanged(account);
}

void ProfilePageSync::receiveRequest(const ContactRecord &contact, const QByteArray &payload)
{
    auto request = decodePageRequest(payload);
    if (!request) {
        qCWarning(contactsLog) << "Dropped a profile page request that does not decode";
        return;
    }
    // Answered from the event loop, never inside the engine's receive (a send
    // from here would queue behind this very receive on the conversation's
    // lane), and with copies only: nothing may point into a container that
    // can change before it runs.
    QMetaObject::invokeMethod(
        this,
        [this, account = contact.accountId, haveRevision = request->haveRevision,
         wantMedia = request->wantMedia] { answerRequest(account, haveRevision, wantMedia); },
        Qt::QueuedConnection);
}

void ProfilePageSync::answerRequest(const AccountId &account,
                                    const std::optional<qint64> &haveRevision,
                                    const QVector<QByteArray> &wantMedia)
{
    ProfilePageRepository *repository = pages();
    const auto contact = deliverableContact(account);
    if (repository == nullptr || !contact)
        return;
    auto delivery = currentDelivery(*contact);
    if (!delivery)
        return;

    // A few hundred bytes from a contact must not buy megabytes of uploads:
    // one answer per interval, and a daily budget.
    PageDelivery budget = *delivery;
    const qint64 nowMs = now();
    if (nowMs - budget.answerWindowStartMs >= answerWindowMs) {
        budget.answerWindowStartMs = nowMs;
        budget.answersInWindow = 0;
    }
    if (budget.answersInWindow >= m_limits.answersPerDay
        || (budget.lastAnswerAtMs > 0 && nowMs - budget.lastAnswerAtMs < m_limits.answerIntervalMs)) {
        qCDebug(contactsLog) << "Dropped a profile page request over the answer budget";
        return;
    }
    // Admission counts, even if the answer then waits for a call to end.
    budget.lastAnswerAtMs = nowMs;
    budget.answersInWindow += 1;
    if (!repository->saveDelivery(budget).hasValue()) {
        qCWarning(contactsLog) << "Could not record a profile page answer";
        return;
    }

    // A viewer without our current revision gets the core and every blob in
    // one exchange; one that has it gets only what it asked for. Either way
    // only the page published now is ever sent.
    const bool full = !haveRevision || *haveRevision != publishedRevision();
    Answer answer{account, full, {}};
    for (const auto &[kind, hash] : publishedBlobs()) {
        if (full || wantMedia.contains(hash))
            answer.blobs.push_back(hash);
    }
    if (!answer.sendCore && answer.blobs.isEmpty())
        return;
    const auto queued = std::find_if(m_answers.begin(), m_answers.end(),
                                     [&](const Answer &other) { return other.contact == account; });
    if (queued != m_answers.end())
        *queued = std::move(answer);
    else
        m_answers.push_back(std::move(answer));
    wakePump();
}

void ProfilePageSync::enforceSoftCap()
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    const auto bytes = repository->receivedMediaBytes();
    if (!bytes.hasValue() || bytes.value() <= m_limits.receivedMediaSoftCapBytes)
        return;
    const auto order = repository->contactsLeastRecentlyViewed();
    if (!order.hasValue()) {
        qCWarning(contactsLog) << "Could not order contacts for profile media eviction";
        return;
    }
    // Least recently viewed first, down to the target; their cores stay, so
    // their pages still show and still know what they are missing.
    QVector<AccountId> evicted;
    qint64 remaining = bytes.value();
    for (const AccountId &account : order.value()) {
        if (remaining <= m_limits.receivedMediaTargetBytes)
            break;
        if (!repository->evictContactMedia(account).hasValue()) {
            qCWarning(contactsLog) << "Could not evict a contact's profile media";
            break;
        }
        evicted.push_back(account);
        // Mark this revision's media unasked: opening the page is then what
        // asks for it again (markViewed), never a background request.
        unschedule(account, Trigger::MissingMedia);
        if (const auto state = repository->requestState(account);
            state.hasValue() && state.value().mediaRequestedRevision != -1) {
            PageRequestState unasked = state.value();
            unasked.mediaRequestedRevision = -1;
            if (!repository->saveRequestState(unasked).hasValue())
                qCWarning(contactsLog) << "Could not mark evicted profile media";
        }
        const auto after = repository->receivedMediaBytes();
        if (!after.hasValue())
            break;
        remaining = after.value();
    }
    qCDebug(contactsLog) << "Evicted the profile media of" << evicted.size() << "contacts";
    for (const AccountId &account : std::as_const(evicted))
        emit contactPageChanged(account);
}

// ---------------------------------------------------------------------------
// Viewer requests
// ---------------------------------------------------------------------------

void ProfilePageSync::requestIfDue(const AccountId &account, Trigger trigger)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr || !deliverableContact(account))
        return;
    const auto stored = repository->contactPage(account);
    if (!stored.hasValue())
        return;
    const QVector<QByteArray> missing = stored.value() ? missingMedia(*stored.value())
                                                       : QVector<QByteArray>();
    if (stored.value() && missing.isEmpty())
        return; // nothing to ask for
    const auto state = repository->requestState(account);
    if (!state.hasValue())
        return;
    const PageRequestState &asked = state.value();
    if (trigger == Trigger::MissingMedia) {
        // Once per core revision, and never held back by the back-off.
        if (!stored.value() || asked.mediaRequestedRevision >= stored.value()->revision)
            return;
    } else {
        // An older client never answers: 30 min, 1 h, 2 h … up to a day.
        const int doublings = std::clamp(asked.unanswered, 0, maxBackoffDoublings);
        const qint64 interval = std::min(m_limits.requestMaxIntervalMs,
                                         m_limits.requestBaseIntervalMs << doublings);
        if (asked.lastRequestAtMs > 0 && now() - asked.lastRequestAtMs < interval) {
            qCDebug(contactsLog) << "Held back a profile page request (back-off)";
            return;
        }
    }

    Request request{account, trigger, std::nullopt, missing};
    if (stored.value())
        request.haveRevision = stored.value()->revision;
    // One queued request per contact. A missing-media one wins: it carries
    // the revision it answers for.
    const auto queued = std::find_if(m_requests.begin(), m_requests.end(),
                                     [&](const Request &other) { return other.contact == account; });
    if (queued != m_requests.end()) {
        if (trigger == Trigger::MissingMedia)
            *queued = std::move(request);
        return;
    }
    m_requests.push_back(std::move(request));
    wakePump();
}

void ProfilePageSync::recordRequestSent(const Request &request)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    const auto state = repository->requestState(request.contact);
    if (!state.hasValue()) {
        qCWarning(contactsLog) << "Could not read a profile page request record";
        return;
    }
    PageRequestState asked = state.value();
    asked.lastRequestAtMs = now();
    asked.unanswered = std::min(std::max(asked.unanswered, 0) + 1, maxUnanswered);
    if (request.trigger == Trigger::MissingMedia && request.haveRevision)
        asked.mediaRequestedRevision = *request.haveRevision;
    if (!repository->saveRequestState(asked).hasValue())
        qCWarning(contactsLog) << "Could not record a profile page request";
    // "Getting …'s page…" ends on its own after the loading window.
    schedule(request.contact, std::nullopt, asked.lastRequestAtMs + m_limits.pageLoadingWindowMs);
    emit awaitingChanged(request.contact);
}

void ProfilePageSync::scheduleStartupRequests()
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    const qint64 nowMs = now();
    for (const ContactRecord &contact : deliverableContacts()) {
        const auto stored = repository->contactPage(contact.accountId);
        if (!stored.hasValue() || stored.value())
            continue;
        if (isScheduled(contact.accountId, Trigger::Acceptance))
            continue; // its own request is coming
        // Spread out, so a start or reconnect does not ask everyone at once.
        schedule(contact.accountId, Trigger::Startup,
                 nowMs + randomBelow(m_limits.startupRequestJitterMs));
    }
}

void ProfilePageSync::schedule(const AccountId &contact, std::optional<Trigger> trigger,
                               qint64 dueAtMs)
{
    m_schedule.removeIf([&](const Scheduled &entry) {
        return entry.contact == contact && entry.trigger == trigger;
    });
    m_schedule.push_back(Scheduled{contact, trigger, dueAtMs});
    armScheduleTimer();
}

void ProfilePageSync::unschedule(const AccountId &contact, Trigger trigger)
{
    if (m_schedule.removeIf([&](const Scheduled &entry) {
            return entry.contact == contact && entry.trigger == trigger;
        })
        > 0)
        armScheduleTimer();
}

bool ProfilePageSync::isScheduled(const AccountId &contact, Trigger trigger) const
{
    return std::any_of(m_schedule.cbegin(), m_schedule.cend(), [&](const Scheduled &entry) {
        return entry.contact == contact && entry.trigger == trigger;
    });
}

void ProfilePageSync::armScheduleTimer()
{
    if (m_schedule.isEmpty()) {
        m_scheduleTimer.stop();
        return;
    }
    qint64 earliest = m_schedule.constFirst().dueAtMs;
    for (const Scheduled &entry : std::as_const(m_schedule))
        earliest = std::min(earliest, entry.dueAtMs);
    const qint64 wait = std::clamp<qint64>(earliest - now(), 0, maxScheduleWaitMs);
    m_scheduleTimer.start(static_cast<int>(wait));
}

void ProfilePageSync::runDueSchedule()
{
    const qint64 nowMs = now();
    QList<Scheduled> due;
    for (qsizetype index = 0; index < m_schedule.size();) {
        if (m_schedule.at(index).dueAtMs <= nowMs)
            due.push_back(m_schedule.takeAt(index));
        else
            ++index;
    }
    // Handlers may schedule again (a sent request opens its loading window),
    // so this works on its own copy.
    for (const Scheduled &entry : std::as_const(due)) {
        if (!entry.trigger) {
            emit awaitingChanged(entry.contact);
            continue;
        }
        requestIfDue(entry.contact, *entry.trigger);
        // The acceptance window closes here unless a request is now queued.
        if (*entry.trigger == Trigger::Acceptance)
            emit awaitingChanged(entry.contact);
    }
    armScheduleTimer();
}

// ---------------------------------------------------------------------------
// The pump
// ---------------------------------------------------------------------------

void ProfilePageSync::wakePump()
{
    if (!m_pumpTimer.isActive())
        m_pumpTimer.start();
    if (m_pumpWakeQueued)
        return;
    m_pumpWakeQueued = true;
    // Soon, but never on the caller's stack: publish() and the receive
    // handlers return before anything is sent.
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_pumpWakeQueued = false;
            pumpOnce();
        },
        Qt::QueuedConnection);
}

void ProfilePageSync::pumpOnce()
{
    const auto stopFailedClosed = [this] {
        m_pumpTimer.stop();
        if (!m_failedClosedLogged) {
            m_failedClosedLogged = true;
            qCWarning(contactsLog) << "The sync engine has stopped; profile pages are not delivered";
        }
    };
    if (m_engine.isFailedClosed()) {
        stopFailedClosed();
        return;
    }
    // Nothing queues while the link is down; linkUp() wakes the pump again.
    if (!m_engine.isLinkUp()) {
        m_pumpTimer.stop();
        return;
    }
    if (!m_pumpTimer.isActive())
        m_pumpTimer.start();

    const QVector<ContactRecord> contacts = deliverableContacts();
    for (int handled = 0; handled < m_limits.pumpPayloadsPerTick; ++handled) {
        // Page data waits while the link is busy, so chat and call signalling
        // never queue behind it. A transport that cannot tell (-1) never gates.
        if (m_limits.pumpBacklogBytes > 0
            && m_engine.pendingSendBytes() > m_limits.pumpBacklogBytes)
            return; // the timer looks again
        std::optional<PumpJob> job = nextJob(contacts);
        if (!job) {
            m_pumpTimer.stop();
            return;
        }
        // Over the ceiling, MLS could refuse to encrypt it, and a failed
        // encrypt stops the whole engine.
        if (job->payload.size() > maxPageSendBytes) {
            qCWarning(contactsLog) << "Not sending a profile page payload of" << job->payload.size()
                                   << "bytes, over the" << maxPageSendBytes << "byte ceiling";
            if (job->drop)
                job->drop();
            continue;
        }
        m_engine.sendProfileUpdate(job->conversation, job->device, job->payload);
        // "Sent" means the engine took it; a send that stopped the engine is not.
        if (m_engine.isFailedClosed()) {
            stopFailedClosed();
            return;
        }
        if (job->record)
            job->record();
    }
}

std::optional<ProfilePageSync::PumpJob> ProfilePageSync::nextJob(const QVector<ContactRecord> &contacts)
{
    // Answers first (a viewer is waiting), except during a call; then our own
    // tiny requests; then pushes.
    if (!m_callActive) {
        if (auto job = nextAnswerJob(contacts))
            return job;
    }
    if (auto job = nextRequestJob(contacts))
        return job;
    return nextDeliveryJob(contacts);
}

std::optional<ProfilePageSync::PumpJob>
ProfilePageSync::nextAnswerJob(const QVector<ContactRecord> &contacts)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return std::nullopt;
    while (!m_answers.isEmpty()) {
        Answer &answer = m_answers.first();
        const ContactRecord *contact = findContact(contacts, answer.contact);
        if (contact == nullptr) {
            m_answers.removeFirst(); // no longer a contact we may address
            continue;
        }
        if (answer.sendCore) {
            answer.sendCore = false;
            const qint64 revision = publishedRevision();
            if (revision <= 0 || m_unsendableRevision != revision) {
                // Never published: the default page, so the viewer stops asking.
                QByteArray core = revision > 0 ? m_local.publishedCore
                                               : encodePageCore(Profile::defaultPage());
                const AccountId account = contact->accountId;
                return PumpJob{account,
                               *contact->conversationId,
                               *contact->peerDeviceId,
                               std::move(core),
                               [this, account, revision] { recordCoreSent(account, revision); },
                               [this, revision] { m_unsendableRevision = revision; }};
            }
        }
        while (!answer.blobs.isEmpty()) {
            const QByteArray hash = answer.blobs.takeFirst();
            // Still part of the page published now, and not sent to them lately.
            const auto kind = publishedKindOf(hash);
            if (!kind)
                continue;
            const auto sentAt = repository->mediaSentAt(contact->accountId, hash);
            if (!sentAt.hasValue()
                || (sentAt.value() && now() - *sentAt.value() < m_limits.mediaResendGuardMs))
                continue;
            if (auto job = mediaJob(*contact, *kind, hash))
                return job;
        }
        m_answers.removeFirst();
    }
    return std::nullopt;
}

std::optional<ProfilePageSync::PumpJob>
ProfilePageSync::nextRequestJob(const QVector<ContactRecord> &contacts)
{
    while (!m_requests.isEmpty()) {
        Request request = m_requests.takeFirst();
        const ContactRecord *contact = findContact(contacts, request.contact);
        if (contact == nullptr)
            continue;
        QByteArray payload = encodePageRequest({request.haveRevision, request.wantMedia});
        return PumpJob{request.contact,
                       *contact->conversationId,
                       *contact->peerDeviceId,
                       std::move(payload),
                       [this, request] { recordRequestSent(request); },
                       {}};
    }
    return std::nullopt;
}

std::optional<ProfilePageSync::PumpJob>
ProfilePageSync::nextDeliveryJob(const QVector<ContactRecord> &contacts)
{
    ProfilePageRepository *repository = pages();
    const qint64 revision = publishedRevision();
    if (repository == nullptr || contacts.isEmpty() || revision <= 0)
        return std::nullopt; // nothing is pushed before the first publish
    const auto blobs = publishedBlobs();

    // Round robin, starting after the contact served last, so one contact's
    // media never holds everybody else's core back.
    qsizetype start = 0;
    if (m_lastServed) {
        for (qsizetype index = 0; index < contacts.size(); ++index) {
            if (contacts.at(index).accountId == *m_lastServed) {
                start = index + 1;
                break;
            }
        }
    }
    for (qsizetype step = 0; step < contacts.size(); ++step) {
        const ContactRecord &contact = contacts.at((start + step) % contacts.size());
        const auto delivery = currentDelivery(contact);
        if (!delivery)
            continue;
        // Only the latest revision is ever sent, however many were published
        // since the last delivery. Its media waits for it.
        if (delivery->sentRevision < revision) {
            if (m_unsendableRevision == revision)
                continue;
            m_lastServed = contact.accountId;
            const AccountId account = contact.accountId;
            return PumpJob{account,
                           *contact.conversationId,
                           *contact.peerDeviceId,
                           m_local.publishedCore,
                           [this, account, revision] { recordCoreSent(account, revision); },
                           [this, revision] { m_unsendableRevision = revision; }};
        }
        // Media only to a client that understands pages (an older one would
        // just drop it), and never during a call.
        if (!delivery->pageCapable || m_callActive)
            continue;
        for (const auto &[kind, hash] : blobs) {
            const auto sentAt = repository->mediaSentAt(contact.accountId, hash);
            if (!sentAt.hasValue() || sentAt.value())
                continue;
            if (auto job = mediaJob(contact, kind, hash)) {
                m_lastServed = contact.accountId;
                return job;
            }
        }
    }
    return std::nullopt;
}

std::optional<ProfilePageSync::PumpJob>
ProfilePageSync::mediaJob(const ContactRecord &contact, Profile::MediaKind kind,
                          const QByteArray &sha256)
{
    const QByteArray data = localMedia(sha256);
    if (data.isEmpty()) {
        qCWarning(contactsLog) << "A published profile media blob is missing from the store";
        return std::nullopt;
    }
    const AccountId account = contact.accountId;
    return PumpJob{account,
                   *contact.conversationId,
                   *contact.peerDeviceId,
                   encodePageMedia({kind, sha256, data}),
                   [this, account, sha256] {
                       ProfilePageRepository *repository = pages();
                       if (repository == nullptr
                           || !repository->recordMediaSent(account, sha256, now()).hasValue())
                           qCWarning(contactsLog) << "Could not record sent profile media";
                   },
                   {}};
}

void ProfilePageSync::recordCoreSent(const AccountId &account, qint64 revision)
{
    ProfilePageRepository *repository = pages();
    if (repository == nullptr)
        return;
    const auto stored = repository->delivery(account);
    if (!stored.hasValue()) {
        qCWarning(contactsLog) << "Could not read a profile page delivery record";
        return;
    }
    PageDelivery delivery = stored.value();
    delivery.sentRevision = std::max(delivery.sentRevision, revision);
    delivery.sentAtMs = now();
    if (!repository->saveDelivery(delivery).hasValue())
        qCWarning(contactsLog) << "Could not record a delivered profile page";
}

} // namespace OpenChat
