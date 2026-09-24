#include "controllers/ProfileController.h"

#include "app/ContactRequestService.h"
#include "app/ProfileSession.h"
#include "controllers/ChatController.h"
#include "controllers/ProfileReferencePages.h"
#include "diagnostics/Logging.h"
#include "domain/Handle.h"
#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"
#include "network/RelayClient.h"
#include "network/SyncEngine.h"
#include "profile/ProfileBackgroundImage.h"
#include "profile/ProfileFonts.h"
#include "profile/ProfileMediaStore.h"
#include "profile/ProfileReadability.h"
#include "profile/SongImport.h"
#include "profile/SongPlayer.h"
#include "storage/SqlCipherContactRepository.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSettings>

#include <algorithm>
#include <utility>

namespace OpenChat {

namespace {

using Profile::Origin;
using Profile::Relationship;

const QString lastTabKey = QStringLiteral("Profiles/lastEditorTab");
const QString recentColorsKey = QStringLiteral("Profiles/recentColors");

// A stranger's handle is asked of the relay again after a failed lookup, but
// not more often than this (the 5 s refresh would otherwise hammer it).
constexpr qint64 handleRetryMs = 60'000;

std::optional<ProfileController::SyncTuning> &syncTuning()
{
    static std::optional<ProfileController::SyncTuning> tuning;
    return tuning;
}

[[nodiscard]] qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

[[nodiscard]] std::optional<AccountId> accountFromHex(const QString &hex)
{
    if (hex.size() != AccountId::byteCount * 2)
        return std::nullopt;
    const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
    if (bytes.toHex() != hex.toLower().toLatin1())
        return std::nullopt; // not all hex digits
    return AccountId::fromBytes(bytes);
}

[[nodiscard]] QString canonicalHandle(const QString &handle)
{
    const std::optional<QString> canonical = normalizeHandle(handle);
    return canonical ? *canonical : QString();
}

[[nodiscard]] QString firstNameOf(const QString &name)
{
    const QStringList words = name.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return words.isEmpty() ? QString() : words.first();
}

// The label a contact without a known handle goes by (as in the chat list).
[[nodiscard]] QString shortIdLabel(const QString &accountHex)
{
    return QStringLiteral("ID ") + accountHex.left(10);
}

[[nodiscard]] QString originLabel(Origin origin)
{
    switch (origin) {
    case Origin::FromChat:
    case Origin::FromFriendSpace:
        return QStringLiteral("Chat");
    case Origin::FromCall:
        return QStringLiteral("Call");
    case Origin::FromRequests:
        return QStringLiteral("Requests");
    case Origin::FromSearch:
        return QStringLiteral("Search");
    case Origin::FromSettings:
        return QStringLiteral("Settings");
    }
    return QStringLiteral("Chat");
}

[[nodiscard]] QString localPath(const QUrl &file)
{
    return file.isLocalFile() ? file.toLocalFile() : file.toString();
}

[[nodiscard]] bool isShippedPreset(int preset)
{
    return preset >= int(Profile::Preset::AeroSkyPreset) && preset <= int(Profile::Preset::ChromeY2KPreset);
}

// Whether a contact would take these bytes as this kind of blob: the
// receiver's own decoder on the message we would send (ProfilePageCodec).
[[nodiscard]] bool passesArrivalChecks(Profile::MediaKind kind, const QByteArray &bytes)
{
    return decodePageMedia(encodePageMedia({kind, pageMediaHash(bytes), bytes})).has_value();
}

[[nodiscard]] QString mediaGoneNotice(Profile::MediaKind kind)
{
    return kind == Profile::MediaKind::SongMedia
               ? QStringLiteral("Your profile song is no longer available. Choose it again.")
               : QStringLiteral("Your background picture is no longer available. Choose it again.");
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void ProfileController::setSyncTuningForTesting(std::optional<SyncTuning> tuning)
{
    syncTuning() = std::move(tuning);
}

ProfileController::ProfileController(ChatController &chats, QObject *parent)
    : QObject(parent)
    , m_chats(chats)
    , m_view(ProfilePageObject::Kind::View, this)
    , m_draft(ProfilePageObject::Kind::Draft, this)
    , m_tryOn(ProfilePageObject::Kind::TryOn, this)
    , m_published(Profile::defaultPage())
{
    m_pick = [](int bound) { return bound <= 1 ? 0 : int(QRandomGenerator::global()->bounded(bound)); };

    for (const QString &id : ProfileReferencePages::rosterIds()) {
        if (std::optional<Profile::Page> page = ProfileReferencePages::seededPage(id))
            m_mockPages.insert(id, *page);
    }
    m_mockPublished = Profile::defaultPage();

    const auto tiles = [this](const QVector<Profile::TopFriend> &friends) { return tilesFor(friends); };
    m_view.setTileResolver(tiles);
    m_draft.setTileResolver(tiles);
    m_tryOn.setTileResolver(tiles);
    m_draftShadow = m_draft.page();
    connect(&m_draft, &ProfilePageObject::edited, this, &ProfileController::onDraftEdited);

    m_refreshTimer.setInterval(refreshIntervalMs);
    connect(&m_refreshTimer, &QTimer::timeout, this, [this] {
        // Presence changes of rows the search filter hides raise no signal,
        // and the engine never says the link went down: look again.
        updateLinkOnline();
        refreshPerson();
    });
    m_idleMediaTimer.setSingleShot(true);
    m_idleMediaTimer.setInterval(idleMediaReleaseMs);
    connect(&m_idleMediaTimer, &QTimer::timeout, this, &ProfileController::releaseIdleMedia);
    m_autosaveTimer.setSingleShot(true);
    m_autosaveTimer.setInterval(autosaveDelayMs);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &ProfileController::saveDraftNow);
    m_songWindowTimer.setSingleShot(true);
    m_songWindowTimer.setInterval(songWindowDelayMs);
    connect(&m_songWindowTimer, &QTimer::timeout, this, &ProfileController::startSongWindowEncode);

    ContactListModel *roster = m_chats.contacts();
    connect(roster, &QAbstractItemModel::modelReset, this, &ProfileController::onRosterChanged);
    connect(roster, &QAbstractItemModel::dataChanged, this, [this] { onRosterChanged(); });
    connect(roster, &QAbstractItemModel::rowsInserted, this, [this] { onRosterChanged(); });
    connect(roster, &QAbstractItemModel::rowsRemoved, this, [this] { onRosterChanged(); });
    connect(&m_chats, &ChatController::localProfileChanged, this, &ProfileController::onRosterChanged);
    connect(&m_chats, &ChatController::localUserNameChanged, this, &ProfileController::onRosterChanged);
    connect(&m_chats, &ChatController::localOnlineChanged, this, [this] {
        updateLinkOnline();
        onRosterChanged();
    });
    connect(&ProfileMediaStore::instance(), &ProfileMediaStore::imageReady, this, &ProfileController::onImageReady);

    const QSettings settings;
    m_lastTab = std::clamp(settings.value(lastTabKey, 0).toInt(), 0, int(Profile::EditorTab::LayoutTab));
    for (const QString &name : settings.value(recentColorsKey).toStringList()) {
        if (QColor::isValidColorName(name) && !m_recentColors.contains(name) && m_recentColors.size() < maxRecentColors)
            m_recentColors.append(name);
    }
}

ProfileController::~ProfileController()
{
    flushAutosave();
    // No import result may land in a half-destroyed controller.
    m_songWindowTimer.stop();
    if (m_songImporter)
        m_songImporter->cancel();
    if (m_backgroundImporter)
        m_backgroundImporter->cancel();
    for (const HeldMedia *held : {&m_viewMedia, &m_draftMedia, &m_tryOnMedia}) {
        if (!held->imageKey.isEmpty())
            ProfileMediaStore::instance().release(held->imageKey);
    }
}

// ---------------------------------------------------------------------------
// Live services
// ---------------------------------------------------------------------------

void ProfileController::setLiveServices(ProfileSession *session, SyncEngine *engine,
                                        ContactRequestService *requests)
{
    if (session == nullptr || engine == nullptr)
        return;
    // Whatever the mock was showing or editing ends here.
    flushAutosave();
    closeAll();
    cancelImports();
    dropSync();
    m_mockPages.clear();
    m_mockMedia.clear();
    m_mockPublished = Profile::defaultPage();
    m_mockDraft.reset();
    m_mockDraftSource.clear();
    m_mockDraftAtMs = 0;

    m_live = true;
    m_session = session;
    m_engine = engine;
    m_requests = requests;

    const std::optional<SyncTuning> &tuning = syncTuning();
    ProfilePageSync::Clock clock = tuning ? tuning->clock : ProfilePageSync::Clock{};
    ProfilePageSync::Random random = tuning && tuning->random
                                         ? tuning->random
                                         : ProfilePageSync::Random([](qint64 bound) {
                                               return bound > 0 ? QRandomGenerator::global()->bounded(bound) : 0;
                                           });
    m_sync = std::make_unique<ProfilePageSync>(*session, *engine, std::move(clock), std::move(random),
                                               tuning ? tuning->limits : ProfilePageSync::Limits{});
    m_sync->setCallActive(m_callActive);
    connect(m_sync.get(), &ProfilePageSync::contactPageChanged, this, [this](const AccountId &contact) {
        if (m_person.accountHex == contact.toHex())
            refreshPerson();
    });
    connect(m_sync.get(), &ProfilePageSync::awaitingChanged, this, [this](const AccountId &contact) {
        if (m_person.accountHex == contact.toHex())
            refreshPerson();
    });
    connect(engine, &SyncEngine::linkUp, this, &ProfileController::updateLinkOnline, Qt::UniqueConnection);
    // The sync borrows the session and the engine: it must be gone before
    // the session locks, and the engine is the first thing lock() destroys.
    connect(engine, &QObject::destroyed, this, &ProfileController::dropSync, Qt::UniqueConnection);
    if (requests != nullptr) {
        connect(requests, &ContactRequestService::contactAccepted, m_sync.get(),
                &ProfilePageSync::onContactAccepted);
    }

    m_localHandle = session->handle();
    m_ownHandleRequested = false;
    reloadPublished();
    loadDraft(m_published);
    requestOwnHandle();
    updateLinkOnline();
    emit localIdentityChanged();
    emit publishedChanged();
}

void ProfileController::dropSync()
{
    if (!m_sync)
        return;
    flushAutosave();
    m_sync.reset();
    m_session = nullptr;
    m_engine = nullptr;
    closeAll();
    updateLinkOnline();
}

void ProfileController::setRelay(RelayClient *relay)
{
    if (m_relay == relay)
        return;
    if (m_relay)
        disconnect(m_relay, nullptr, this, nullptr);
    m_relay = relay;
    m_pendingLookups.clear();
    m_failedLookups.clear();
    m_ownHandleRequested = false;
    if (relay != nullptr) {
        connect(relay, &RelayClient::accountResolved, this, &ProfileController::onAccountResolved);
        connect(relay, &RelayClient::accountResolutionFailed, this,
                [this](const AccountId &account, RelayDirectoryError) { onAccountResolutionFailed(account); });
        connect(relay, &RelayClient::connected, this, [this] {
            m_ownHandleRequested = false;
            requestOwnHandle();
        });
    }
    requestOwnHandle();
}

void ProfileController::requestOwnHandle()
{
    if (!m_live || m_session == nullptr || !m_relay || !m_localHandle.isEmpty() || m_ownHandleRequested)
        return;
    const auto account = m_session->accountId();
    if (!account.hasValue())
        return;
    m_ownHandleRequested = true;
    m_relay->resolveAccount(account.value());
}

void ProfileController::requestHandle(const AccountId &account)
{
    const QString hex = account.toHex();
    if (!m_relay || m_pendingLookups.contains(hex) || m_resolvedHandles.contains(hex))
        return;
    if (const auto failed = m_failedLookups.constFind(hex);
        failed != m_failedLookups.cend() && nowMs() - *failed < handleRetryMs)
        return;
    m_pendingLookups.insert(hex);
    m_relay->resolveAccount(account);
}

void ProfileController::onAccountResolved(const AccountId &account, const QString &handle)
{
    const QString hex = account.toHex();
    m_pendingLookups.remove(hex);
    m_failedLookups.remove(hex);
    const QString canonical = canonicalHandle(handle);
    if (canonical.isEmpty())
        return;
    m_resolvedHandles.insert(hex, canonical);
    if (hex == localAccountHex() && m_localHandle.isEmpty()) {
        // Profiles made before handles were stored learn theirs here, once.
        if (m_session == nullptr || m_session->setHandle(canonical).hasValue()) {
            m_localHandle = canonical;
            emit localIdentityChanged();
        }
    }
    refreshPerson();
}

void ProfileController::onAccountResolutionFailed(const AccountId &account)
{
    const QString hex = account.toHex();
    if (hex == localAccountHex())
        m_ownHandleRequested = false; // try again on the next connect
    if (!m_pendingLookups.remove(hex))
        return;
    m_failedLookups.insert(hex, nowMs());
    refreshPerson();
}

// ---------------------------------------------------------------------------
// Mock seams
// ---------------------------------------------------------------------------

void ProfileController::setMockPage(const QString &contactId, const Profile::Page &page)
{
    if (m_live)
        return;
    const Profile::Page normalized = Profile::normalized(page);
    const std::optional<AccountId> account = accountFor(contactId);
    if (contactId == ProfileReferencePages::selfId() || (account && account->toHex() == localAccountHex())) {
        m_mockPublished = normalized;
        reloadPublished();
        updateDirty();
        emit publishedChanged();
    } else if (account) {
        const QString id = ProfileReferencePages::mockIdFor(*account);
        m_mockPages.insert(id.isEmpty() ? account->toHex() : id, normalized);
    }
    refreshPerson();
}

Profile::MediaRef ProfileController::addMockMedia(Profile::MediaKind kind, const QByteArray &bytes)
{
    Profile::MediaRef ref;
    if (m_live || !passesArrivalChecks(kind, bytes))
        return ref; // live pages carry only media that really arrived
    ref.sha256 = pageMediaHash(bytes);
    ref.bytes = quint32(bytes.size());
    if (kind == Profile::MediaKind::BackgroundImageMedia) {
        QBuffer buffer;
        buffer.setData(bytes);
        QImageReader reader(&buffer);
        const QSize size = reader.size();
        ref.width = quint16(std::clamp(size.width(), 0, Profile::maxBackgroundDimension));
        ref.height = quint16(std::clamp(size.height(), 0, Profile::maxBackgroundDimension));
    } else if (const auto song = decodeSongContainer(bytes)) {
        ref.durationMs = quint32(song->durationMs());
    }
    m_mockMedia.insert(ref.sha256, bytes);
    refreshPerson();
    return ref;
}

void ProfileController::setMockLinkOnline(bool online)
{
    m_mockLinkOnline = online;
    updateLinkOnline();
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

bool ProfileController::openContact(const QString &contactId, int origin)
{
    const std::optional<AccountId> account = accountFor(contactId);
    if (account && account->toHex() == localAccountHex()) {
        openOwn(origin); // your own id: your own profile
        return true;
    }
    if (!account || !rosterContact(rosterIdFor(*account)))
        return false; // a group, or nobody this roster knows
    startStack(personForAccount(*account, QString(), QString(), Origin::FromChat), originFromSection(origin));
    return true;
}

void ProfileController::openOwn(int origin)
{
    startStack(PersonRef{}, originFromSection(origin));
}

void ProfileController::openRequest(const QString &requestId, const QString &accountId, const QString &name,
                                    const QString &handle)
{
    PersonRef ref;
    ref.kind = PersonRef::Kind::Request;
    if (const std::optional<AccountId> account = accountFor(accountId))
        ref.accountHex = account->toHex();
    if (!ref.accountHex.isEmpty() && ref.accountHex == localAccountHex()) {
        openOwn(int(Origin::FromRequests));
        return;
    }
    ref.requestId = requestId;
    ref.label = name;
    ref.handle = canonicalHandle(handle);
    ref.reason = Origin::FromRequests;
    startStack(ref, Origin::FromRequests);
}

void ProfileController::openHandle(const QString &handle)
{
    const QString canonical = canonicalHandle(handle);
    if (canonical.isEmpty())
        return;
    PersonRef ref;
    ref.kind = PersonRef::Kind::Handle;
    ref.handle = canonical;
    ref.reason = Origin::FromSearch;
    startStack(ref, Origin::FromSearch);
}

void ProfileController::openPerson(const QString &accountId, const QString &name, const QString &avatarKey)
{
    const std::optional<AccountId> account = accountFor(accountId);
    if (!account)
        return;
    startStack(personForAccount(*account, name, avatarKey, Origin::FromCall), Origin::FromCall);
}

bool ProfileController::openTopFriend(int index)
{
    if (m_editing || m_stack.isEmpty())
        return false;
    const QVector<Profile::TopFriend> &friends = m_view.page().topFriends;
    if (index < 0 || index >= friends.size())
        return false;
    const std::optional<AccountId> account = AccountId::fromBytes(friends.at(index).accountId);
    if (!account)
        return false;
    // The owner's label is only a stub's provisional name; a friend who is
    // the viewer's contact resolves to the roster instead.
    PersonRef ref = personForAccount(*account, friends.at(index).name, QString(), Origin::FromFriendSpace);
    ref.referrerName = m_person.firstName;
    if (samePerson(ref, m_stack.last().person))
        return false;
    for (int entry = 0; entry < m_stack.size(); ++entry) {
        if (samePerson(ref, m_stack.at(entry).person)) {
            truncateTo(entry); // history never loops
            return true;
        }
    }
    pushEntry(ref);
    return true;
}

void ProfileController::back()
{
    if (m_editing) {
        endEditing(); // the editor's Back returns to the profile
        return;
    }
    if (m_stack.size() <= 1) {
        closeAll();
        return;
    }
    truncateTo(int(m_stack.size()) - 2);
}

void ProfileController::popTo(int index)
{
    if (index < 0 || index >= int(m_stack.size()) - 1)
        return;
    if (m_editing) {
        flushAutosave();
        leaveEditing();
    }
    truncateTo(index);
}

void ProfileController::closeAll()
{
    flushAutosave();
    leaveEditing();
    if (m_stack.isEmpty())
        return;
    m_stack.clear();
    m_entryScrollY = 0;
    m_refreshTimer.stop();
    m_idleMediaTimer.start();
    ++m_navigation;
    emit navigationChanged();
}

void ProfileController::setEntryScrollY(qreal y)
{
    // Kept for when this entry is popped back to; the view already shows it.
    if (!m_stack.isEmpty())
        m_stack.last().scrollY = y;
}

void ProfileController::refresh()
{
    updateLinkOnline();
    refreshPerson();
}

void ProfileController::startStack(const PersonRef &person, Origin origin)
{
    if (m_editing) {
        flushAutosave();
        leaveEditing();
    }
    // The page component is created right after `open` turns true; its
    // faces must be there by then (and never before a page is wanted).
    ProfileFonts::ensureRegistered();
    m_stack = {Entry{person, 0}};
    m_origin = origin;
    m_entryScrollY = 0;
    navigated();
}

void ProfileController::pushEntry(const PersonRef &person)
{
    if (m_stack.size() >= maxStackDepth)
        m_stack.removeFirst();
    m_stack.append(Entry{person, 0});
    m_entryScrollY = 0;
    navigated();
}

void ProfileController::truncateTo(int index)
{
    m_stack.resize(index + 1);
    m_entryScrollY = m_stack.last().scrollY;
    navigated();
}

void ProfileController::navigated()
{
    ++m_navigation;
    if (!m_refreshTimer.isActive())
        m_refreshTimer.start();
    // Resolve first, so whatever reacts to the move reads the new person.
    refreshPerson();
    emit navigationChanged();
}

Origin ProfileController::originFromSection(int origin) const
{
    if (origin >= int(Origin::FromChat) && origin <= int(Origin::FromFriendSpace))
        return Origin(origin);
    switch (m_chats.navSection()) {
    case ChatController::NavSection::Call:
        return Origin::FromCall;
    case ChatController::NavSection::Settings:
        return Origin::FromSettings;
    case ChatController::NavSection::Chat:
        break;
    }
    return Origin::FromChat;
}

bool ProfileController::samePerson(const PersonRef &a, const PersonRef &b)
{
    using Kind = PersonRef::Kind;
    if (a.kind == Kind::Own || b.kind == Kind::Own)
        return a.kind == b.kind;
    if (!a.accountHex.isEmpty() || !b.accountHex.isEmpty())
        return a.accountHex == b.accountHex;
    if (a.kind == Kind::Request && b.kind == Kind::Request)
        return a.requestId == b.requestId;
    return a.kind == b.kind && a.handle == b.handle;
}

ProfileController::PersonRef ProfileController::personForAccount(const AccountId &account, const QString &label,
                                                                 const QString &avatarKey, Origin reason) const
{
    PersonRef ref;
    if (account.toHex() == localAccountHex())
        return ref; // yourself: your own profile
    ref.kind = PersonRef::Kind::Account;
    ref.accountHex = account.toHex();
    ref.label = label;
    ref.avatarKey = avatarKey;
    ref.reason = reason;
    return ref;
}

QString ProfileController::backLabel() const
{
    if (m_editing)
        return QStringLiteral("Profile");
    if (m_stack.size() <= 1)
        return originLabel(m_origin);
    return resolveIdentity(m_stack.at(m_stack.size() - 2).person).firstName;
}

QVariantList ProfileController::history() const
{
    QVariantList entries;
    for (int index = int(m_stack.size()) - 2; index >= 0; --index) {
        const PersonState state = resolveIdentity(m_stack.at(index).person);
        entries.append(QVariantMap{{QStringLiteral("index"), index},
                                   {QStringLiteral("name"), state.name},
                                   {QStringLiteral("avatarKey"), state.avatarKey}});
    }
    return entries;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

std::optional<AccountId> ProfileController::accountFor(const QString &idOrHex) const
{
    if (idOrHex.isEmpty())
        return std::nullopt;
    if (const std::optional<AccountId> account = accountFromHex(idOrHex))
        return account;
    // Mock ids ("michael", "self", "dana-whitfield") name mock accounts.
    if (!m_live)
        return ProfileReferencePages::mockAccountFor(idOrHex);
    return std::nullopt;
}

QString ProfileController::rosterIdFor(const AccountId &account) const
{
    if (m_live)
        return account.toHex();
    if (QString id = ProfileReferencePages::mockIdFor(account); !id.isEmpty())
        return id;
    // A mock roster someone else filled (a preview, a test) names people the
    // reference pages do not know; their accounts derive from their ids.
    const ContactListModel *roster = m_chats.contacts();
    for (int row = 0; row < roster->rowCount(); ++row) {
        const std::optional<Contact> contact = roster->contactAt(row);
        if (contact && !contact->isGroup && ProfileReferencePages::mockAccountFor(contact->id) == account)
            return contact->id;
    }
    return {};
}

std::optional<Contact> ProfileController::rosterContact(const QString &rosterId) const
{
    if (rosterId.isEmpty())
        return std::nullopt;
    std::optional<Contact> contact = m_chats.contacts()->contactById(rosterId);
    if (!contact || contact->isGroup)
        return std::nullopt;
    return contact;
}

std::optional<ContactRecord> ProfileController::contactRecord(const AccountId &account) const
{
    if (!m_live || m_session == nullptr || m_session->contacts() == nullptr)
        return std::nullopt;
    auto found = m_session->contacts()->find(account);
    if (!found.hasValue())
        return std::nullopt;
    return found.value();
}

QVector<ContactRecord> ProfileController::contactRecords() const
{
    if (!m_live || m_session == nullptr || m_session->contacts() == nullptr)
        return {};
    auto all = m_session->contacts()->contacts();
    return all.hasValue() ? all.value() : QVector<ContactRecord>{};
}

QString ProfileController::localAccountHex() const
{
    if (!m_live)
        return ProfileReferencePages::mockAccountFor(ProfileReferencePages::selfId()).toHex();
    if (m_session == nullptr)
        return {};
    const auto account = m_session->accountId();
    return account.hasValue() ? account.value().toHex() : QString();
}

QString ProfileController::localAccountId() const
{
    return localAccountHex();
}

QString ProfileController::localName() const
{
    const QString name = m_chats.localUserName();
    if (!name.isEmpty())
        return name;
    return m_localHandle.isEmpty() ? QStringLiteral("You") : m_localHandle;
}

ProfileController::PersonState ProfileController::resolveIdentity(const PersonRef &ref) const
{
    PersonState state;
    const auto self = [&] {
        state.relationship = Relationship::SelfPerson;
        state.accountHex = localAccountHex();
        state.id = m_live ? state.accountHex : ProfileReferencePages::selfId();
        state.name = localName();
        state.handle = m_localHandle;
        state.avatarKey = m_chats.localAvatarKey();
        state.presence = m_chats.localOnline() ? m_chats.localPresence() : int(Presence::Offline);
        state.online = state.presence == int(Presence::Available);
        state.statusLine = m_chats.localStatusText();
        state.pageState = Profile::PageState::DefaultPage;
    };
    const auto contactState = [&](const AccountId &account, const Contact &contact) {
        state.relationship = Relationship::ContactPerson;
        state.accountHex = account.toHex();
        state.id = contact.id;
        state.name = contact.name;
        state.avatarKey = contact.avatarKey.isEmpty() ? QStringLiteral("userpfp_none") : contact.avatarKey;
        state.presence = int(contact.presence);
        state.online = contact.presence == Presence::Available;
        // A status written earlier never reads as if they were there.
        state.statusLine = contact.presence == Presence::Offline ? QString() : contact.statusText;
        if (m_live) {
            if (const std::optional<ContactRecord> record = contactRecord(account)) {
                state.handle = canonicalHandle(record->handle);
                state.verified = record->verified;
            }
        } else {
            state.handle = canonicalHandle(contact.id); // mock handles are the ids
        }
        if (state.handle.isEmpty())
            state.handle = m_resolvedHandles.value(state.accountHex);
        state.handlePending = state.handle.isEmpty() && m_pendingLookups.contains(state.accountHex);
        state.pageState = Profile::PageState::DefaultPage;
    };
    // Someone who is not (or not yet) a contact, known by account.
    const auto strangerState = [&](const AccountId &account, const PersonRef &person) {
        state.relationship = Relationship::StrangerPerson;
        state.accountHex = account.toHex();
        if (m_live) {
            state.id = state.accountHex;
        } else {
            const QString mockId = ProfileReferencePages::mockIdFor(account);
            state.id = mockId.isEmpty() ? state.accountHex : mockId;
        }
        QString handle;
        if (const std::optional<ContactRecord> record = contactRecord(account)) {
            switch (record->state) {
            case ContactState::PendingIncoming:
                state.relationship = Relationship::IncomingRequestPerson;
                if (record->conversationId)
                    state.requestId = record->conversationId->toHex();
                break;
            case ContactState::PendingOutgoing:
                state.relationship = Relationship::OutgoingRequestPerson;
                break;
            case ContactState::Blocked:
                state.blocked = true;
                break;
            case ContactState::Accepted:
                break; // accepted without a chat yet: still shown as a stub
            }
            // A roster row's handle came from the relay or from what the
            // user typed to add them, never from someone's page.
            handle = canonicalHandle(record->handle);
        }
        if (handle.isEmpty())
            handle = m_resolvedHandles.value(state.accountHex);
        if (handle.isEmpty() && person.kind == PersonRef::Kind::Request)
            handle = person.handle;
        state.handle = handle;
        state.handlePending = handle.isEmpty() && m_pendingLookups.contains(state.accountHex);
        // A request goes by the name its row shows (the app's, from the
        // relay); anyone else by the relay-confirmed handle once there is
        // one, and meanwhile by the label they were opened with (a Top
        // Friend's is the page owner's word for them, so it gives way).
        if (person.kind == PersonRef::Kind::Request && !person.label.isEmpty())
            state.name = person.label;
        else
            state.name = !handle.isEmpty()         ? handle
                         : !person.label.isEmpty() ? person.label
                                                   : shortIdLabel(state.accountHex);
        state.avatarKey = person.avatarKey.isEmpty() ? QStringLiteral("userpfp_none") : person.avatarKey;
        state.referrerName = person.referrerName;
        state.presence = int(Presence::Offline);
    };

    switch (ref.kind) {
    case PersonRef::Kind::Own:
        self();
        break;
    case PersonRef::Kind::Account:
    case PersonRef::Kind::Request: {
        const std::optional<AccountId> account = accountFromHex(ref.accountHex);
        if (account && account->toHex() == localAccountHex()) {
            self();
        } else if (std::optional<Contact> contact = account ? rosterContact(rosterIdFor(*account)) : std::nullopt) {
            contactState(*account, *contact); // a stub whose request was accepted becomes the page
        } else if (account) {
            strangerState(*account, ref);
        }
        if (ref.kind == PersonRef::Kind::Request && state.relationship == Relationship::StrangerPerson
            && !state.blocked) {
            // The request row this was opened from, even before its account
            // (or its roster row) is known here.
            state.relationship = Relationship::IncomingRequestPerson;
            state.requestId = ref.requestId;
        }
        if (!account && ref.kind == PersonRef::Kind::Request) {
            state.relationship = Relationship::IncomingRequestPerson;
            state.requestId = ref.requestId;
            state.handle = ref.handle;
            state.name = !ref.label.isEmpty() ? ref.label : ref.handle;
            state.avatarKey = QStringLiteral("userpfp_none");
        }
        break;
    }
    case PersonRef::Kind::Handle: {
        if (!ref.handle.isEmpty() && ref.handle == m_localHandle) {
            self();
            break;
        }
        // A roster row with that handle wins (ARCH §7.2 openHandle).
        std::optional<AccountId> account;
        if (m_live) {
            for (const ContactRecord &record : contactRecords()) {
                if (canonicalHandle(record.handle) == ref.handle) {
                    account = record.accountId;
                    break;
                }
            }
        } else if (rosterContact(ref.handle)) {
            account = ProfileReferencePages::mockAccountFor(ref.handle);
        }
        if (account) {
            if (std::optional<Contact> contact = rosterContact(rosterIdFor(*account)))
                contactState(*account, *contact);
            else
                strangerState(*account, ref);
            break;
        }
        state.relationship = Relationship::StrangerPerson;
        state.name = ref.handle;
        state.handle = ref.handle;
        state.avatarKey = QStringLiteral("userpfp_none");
        state.presence = int(Presence::Offline);
        break;
    }
    }
    state.firstName = firstNameOf(state.name);
    if (state.relationship != Relationship::SelfPerson && state.relationship != Relationship::ContactPerson)
        state.pageState = Profile::PageState::StubPage;
    return state;
}

void ProfileController::refreshPerson()
{
    if (m_stack.isEmpty())
        return;
    PersonState state = resolveIdentity(m_stack.last().person);
    // Only the relay may name someone the roster does not.
    if (state.relationship != Relationship::SelfPerson && state.handle.isEmpty() && !state.handlePending) {
        if (const std::optional<AccountId> account = accountFromHex(state.accountHex)) {
            requestHandle(*account);
            state.handlePending = m_pendingLookups.contains(state.accountHex);
        }
    }
    loadViewPage(state);
    if (state != m_person) {
        m_person = state;
        emit personChanged();
    }
    // Recorded once per visit: it orders eviction, and asks again only for
    // media evicted under the storage cap (ProfilePageSync::markViewed).
    if (m_person.relationship == Relationship::ContactPerson && m_viewedNavigation != m_navigation) {
        m_viewedNavigation = m_navigation;
        if (m_sync) {
            if (const std::optional<AccountId> account = accountFromHex(m_person.accountHex))
                m_sync->markViewed(*account);
        }
    }
}

void ProfileController::loadViewPage(PersonState &state)
{
    ProfileRenderStyle::Viewer viewer{m_darkMode, false};
    MediaSource source;
    Profile::Page page = Profile::defaultPage();
    switch (state.relationship) {
    case Relationship::SelfPerson:
        page = m_published;
        source.owner = MediaSource::Owner::Own;
        state.pageState = page.revision > 0 ? Profile::PageState::CustomPage : Profile::PageState::DefaultPage;
        break;
    case Relationship::ContactPerson:
        if (std::optional<Profile::Page> received = contactPageFor(state.accountHex))
            page = *received;
        source = {MediaSource::Owner::Contact, state.accountHex};
        viewer.plain = m_plainStyle;
        state.pageState = page.revision > 0 ? Profile::PageState::CustomPage : Profile::PageState::DefaultPage;
        break;
    default:
        // Stubs are always plain and carry no page data.
        viewer.plain = true;
        state.pageState = Profile::PageState::StubPage;
        break;
    }
    m_view.setViewer(viewer);
    m_view.load(page);
    updateMedia(m_view, source);
    state.pageIncomplete = state.pageState == Profile::PageState::CustomPage
                           && (m_view.backgroundPending() || m_view.songPending());
    state.pageLoading = false;
    if (state.relationship == Relationship::ContactPerson && state.pageState == Profile::PageState::DefaultPage
        && m_sync) {
        if (const std::optional<AccountId> account = accountFromHex(state.accountHex))
            state.pageLoading = m_sync->isAwaitingPage(*account);
    }
}

std::optional<Profile::Page> ProfileController::contactPageFor(const QString &accountHex) const
{
    const std::optional<AccountId> account = accountFromHex(accountHex);
    if (!account)
        return std::nullopt;
    if (m_live) {
        if (!m_sync)
            return std::nullopt;
        std::optional<ProfilePageSync::ReceivedPage> received = m_sync->contactPage(*account);
        if (!received)
            return std::nullopt;
        return received->page;
    }
    const QString id = ProfileReferencePages::mockIdFor(*account);
    const auto found = m_mockPages.constFind(id.isEmpty() ? accountHex : id);
    if (found == m_mockPages.cend())
        return std::nullopt;
    return *found;
}

QVariantList ProfileController::tilesFor(const QVector<Profile::TopFriend> &friends) const
{
    const QString self = localAccountHex();
    QVariantList tiles;
    for (qsizetype index = 0; index < friends.size(); ++index) {
        const Profile::TopFriend &person = friends.at(index);
        const QString hex = QString::fromLatin1(person.accountId.toHex());
        const bool isSelf = !self.isEmpty() && hex == self;
        std::optional<Contact> contact;
        if (!isSelf) {
            if (const std::optional<AccountId> account = AccountId::fromBytes(person.accountId))
                contact = rosterContact(rosterIdFor(*account));
        }
        // A page can never put a chosen label under a real, recognisable
        // photo: a picture comes only with the viewer's own name for them.
        const QString name = isSelf ? localName() : contact ? contact->name : person.name;
        QString avatar = isSelf ? m_chats.localAvatarKey() : contact ? contact->avatarKey : QString();
        if ((isSelf || contact) && avatar.isEmpty())
            avatar = QStringLiteral("userpfp_none"); // someone the viewer knows is never a monogram
        tiles.append(QVariantMap{{QStringLiteral("index"), int(index)},
                                 {QStringLiteral("accountId"), hex},
                                 {QStringLiteral("name"), name},
                                 {QStringLiteral("initials"), ProfilePageObject::initialsFor(name)},
                                 {QStringLiteral("avatarKey"), avatar},
                                 {QStringLiteral("isContact"), contact.has_value()},
                                 {QStringLiteral("isSelf"), isSelf}});
    }
    return tiles;
}

void ProfileController::onRosterChanged()
{
    m_view.refreshTiles();
    m_draft.refreshTiles();
    m_tryOn.refreshTiles();
    if (m_editing)
        updateCandidates();
    refreshPerson();
}

void ProfileController::updateLinkOnline()
{
    const bool online = m_live ? (m_sync && m_sync->isLinkUp()) : m_mockLinkOnline;
    if (online == m_linkOnline)
        return;
    m_linkOnline = online;
    emit viewerChanged();
}

void ProfileController::updateCandidates()
{
    QStringList ids;
    if (m_live) {
        for (const ContactRecord &record : contactRecords()) {
            if (record.state == ContactState::Accepted)
                ids.append(record.accountId.toHex());
        }
    } else {
        // Everyone in the mock roster is an accepted contact: the reference
        // people (found even while a sidebar search hides their rows), then
        // anyone a preview or test put in the roster instead.
        for (const QString &id : ProfileReferencePages::rosterIds()) {
            if (rosterContact(id))
                ids.append(id);
        }
        const ContactListModel *roster = m_chats.contacts();
        for (int row = 0; row < roster->rowCount(); ++row) {
            const std::optional<Contact> contact = roster->contactAt(row);
            if (contact && !contact->isGroup && !ids.contains(contact->id))
                ids.append(contact->id);
        }
    }
    const QVector<Profile::TopFriend> &placed = m_draft.page().topFriends;
    QVariantList candidates;
    for (const QString &id : std::as_const(ids)) {
        const std::optional<Contact> contact = rosterContact(id);
        const std::optional<AccountId> account = accountFor(id);
        if (!contact || !account)
            continue; // accepted, but not a chat (yet)
        int placedIndex = -1;
        for (qsizetype index = 0; index < placed.size(); ++index) {
            if (placed.at(index).accountId == account->bytes())
                placedIndex = int(index);
        }
        candidates.append(QVariantMap{{QStringLiteral("contactId"), contact->id},
                                      {QStringLiteral("name"), contact->name},
                                      {QStringLiteral("avatarKey"), contact->avatarKey},
                                      {QStringLiteral("placedIndex"), placedIndex}});
    }
    if (candidates == m_candidates)
        return;
    m_candidates = candidates;
    emit topFriendCandidatesChanged();
}

// ---------------------------------------------------------------------------
// Pages and media
// ---------------------------------------------------------------------------

void ProfileController::reloadPublished()
{
    m_published = m_live ? (m_sync ? m_sync->publishedPage() : Profile::defaultPage()) : m_mockPublished;
}

qint64 ProfileController::publishedRevision() const
{
    return m_published.revision;
}

ProfileController::HeldMedia &ProfileController::heldMediaFor(const ProfilePageObject &object)
{
    switch (object.kind()) {
    case ProfilePageObject::Kind::View:
        return m_viewMedia;
    case ProfilePageObject::Kind::Draft:
        return m_draftMedia;
    case ProfilePageObject::Kind::TryOn:
        break;
    }
    return m_tryOnMedia;
}

QByteArray ProfileController::blob(const MediaSource &source, const QByteArray &sha256, Profile::MediaKind kind) const
{
    if (sha256.size() != 32)
        return {};
    if (!m_live) {
        if (source.owner == MediaSource::Owner::None)
            return {};
        const QByteArray added = m_mockMedia.value(sha256);
        return added.isEmpty() ? ProfileReferencePages::referenceMedia(sha256) : added;
    }
    if (!m_sync)
        return {};
    switch (source.owner) {
    case MediaSource::Owner::None:
        return {};
    case MediaSource::Owner::Own:
        return m_sync->localMedia(sha256);
    case MediaSource::Owner::Contact:
        if (const std::optional<AccountId> account = accountFromHex(source.accountHex))
            return m_sync->contactMedia(*account, sha256, kind);
        return {};
    }
    return {};
}

void ProfileController::updateMedia(ProfilePageObject &object, const MediaSource &source, bool force)
{
    HeldMedia &held = heldMediaFor(object);
    const Profile::Page &page = object.page();
    const bool sourceChanged = held.source != source;

    // The picture is decoded only when it will really be shown: an image
    // background, and not in Plain style (ARCH §6.2).
    const QByteArray backgroundHash = page.background.isSet() ? page.background.sha256 : QByteArray();
    const bool showImage = !backgroundHash.isEmpty()
                           && page.theme.backgroundKind == Profile::BackgroundKind::ImageBackground
                           && !object.viewer().plain;
    if (force || sourceChanged || backgroundHash != held.backgroundHash || !held.backgroundPresent
        || showImage != held.imageShown) {
        const QByteArray bytes = backgroundHash.isEmpty()
                                     ? QByteArray()
                                     : blob(source, backgroundHash, Profile::MediaKind::BackgroundImageMedia);
        const QString oldKey = held.imageKey;
        held.backgroundHash = backgroundHash;
        held.backgroundPresent = !bytes.isEmpty();
        held.imageShown = showImage;
        held.imageKey = held.backgroundPresent && showImage ? ProfileMediaStore::instance().requestImage(bytes)
                                                            : QString();
        if (!oldKey.isEmpty() && oldKey != held.imageKey)
            releaseImage(oldKey);
    }

    const QByteArray songHash = page.song.isSet() ? page.song.sha256 : QByteArray();
    if (force || sourceChanged || songHash != held.songHash || !held.songPresent) {
        const QByteArray bytes =
            songHash.isEmpty() ? QByteArray() : blob(source, songHash, Profile::MediaKind::SongMedia);
        const QByteArray oldHash = held.songHash;
        const bool hadSong = held.songPresent;
        held.songHash = songHash;
        held.songPresent = !bytes.isEmpty();
        // The page's player is handed only the key; the bytes wait here.
        if (held.songPresent)
            SongLibrary::instance().put(QString::fromLatin1(songHash.toHex()), bytes);
        if (hadSong && (oldHash != songHash || !held.songPresent))
            releaseSong(oldHash);
    }
    held.source = source;
    object.setMediaState({held.backgroundPresent, held.imageKey, held.songPresent});
}

void ProfileController::releaseImage(const QString &key)
{
    for (const HeldMedia *held : {&m_viewMedia, &m_draftMedia, &m_tryOnMedia}) {
        if (held->imageKey == key)
            return; // another page object still shows it
    }
    ProfileMediaStore::instance().release(key);
}

void ProfileController::releaseSong(const QByteArray &sha256)
{
    for (const HeldMedia *held : {&m_viewMedia, &m_draftMedia, &m_tryOnMedia}) {
        if (held->songPresent && held->songHash == sha256)
            return; // another page object still hands it to a player
    }
    SongLibrary::instance().release(QString::fromLatin1(sha256.toHex()));
}

void ProfileController::onImageReady(const QString &key)
{
    // The picture's statistics now join the readability samples.
    if (m_viewMedia.imageKey == key)
        m_view.refreshRender();
    if (m_draftMedia.imageKey == key)
        m_draft.refreshRender();
    if (m_tryOnMedia.imageKey == key)
        m_tryOn.refreshRender();
}

ProfileController::MediaSource ProfileController::draftMediaSource() const
{
    return m_editing ? MediaSource{MediaSource::Owner::Own, {}} : MediaSource{};
}

void ProfileController::releaseIdleMedia()
{
    // A closed page keeps its words (the close fade shows them) but not its
    // decoded picture or song bytes; reopening fetches them again.
    if (m_stack.isEmpty())
        updateMedia(m_view, {});
    if (!m_editing) {
        updateMedia(m_draft, {});
        updateMedia(m_tryOn, {});
    }
}

std::optional<QByteArray> ProfileController::storeLocalMedia(Profile::MediaKind kind, const QByteArray &bytes)
{
    if (m_live)
        return m_sync ? m_sync->addLocalMedia(kind, bytes) : std::nullopt;
    if (!passesArrivalChecks(kind, bytes))
        return std::nullopt;
    const QByteArray sha256 = pageMediaHash(bytes);
    m_mockMedia.insert(sha256, bytes);
    return sha256;
}

void ProfileController::applyViewer()
{
    ProfileRenderStyle::Viewer viewer{m_darkMode, false};
    if (m_person.relationship == Relationship::ContactPerson)
        viewer.plain = m_plainStyle;
    else if (m_person.relationship != Relationship::SelfPerson)
        viewer.plain = true;
    m_view.setViewer(viewer);
    // Plain style never applies to your own page or the editor.
    m_draft.setViewer({m_darkMode, false});
    m_tryOn.setViewer({m_darkMode, false});
    updateMedia(m_view, m_viewMedia.source);
    updateMedia(m_draft, draftMediaSource());
    if (m_tryOnPreset >= 0)
        updateMedia(m_tryOn, draftMediaSource());
}

void ProfileController::setDarkMode(bool dark)
{
    if (dark == m_darkMode)
        return;
    m_darkMode = dark;
    applyViewer();
    emit viewerChanged();
}

void ProfileController::setPlainStyle(bool plain)
{
    if (plain == m_plainStyle)
        return;
    m_plainStyle = plain;
    applyViewer();
    emit viewerChanged();
}

void ProfileController::setCallActive(bool active)
{
    if (active == m_callActive)
        return;
    m_callActive = active;
    // Media and answers wait while a call owns the uplink (ARCH §4.3).
    if (m_sync)
        m_sync->setCallActive(active);
    emit viewerChanged();
}

// ---------------------------------------------------------------------------
// Clipboard and notices
// ---------------------------------------------------------------------------

void ProfileController::copyHandle()
{
    if (m_person.handle.isEmpty())
        return;
    const QString handle = QLatin1Char('@') + m_person.handle;
    copyText(handle, QStringLiteral("Copied ") + handle);
}

void ProfileController::copyText(const QString &text, const QString &notice)
{
    if (text.isEmpty())
        return;
    // Only a GUI application has a clipboard (tst_e2e runs without one).
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()) == nullptr)
        return;
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr)
        return;
    clipboard->setText(text);
    setNotice(notice);
}

void ProfileController::clearNotice()
{
    setNotice({});
}

void ProfileController::setNotice(const QString &notice)
{
    if (notice == m_notice)
        return;
    m_notice = notice;
    emit noticeChanged();
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

bool ProfileController::beginEditing()
{
    if (m_stack.isEmpty() || m_person.relationship != Relationship::SelfPerson)
        return false;
    if (m_editing)
        return true;
    std::optional<Profile::Page> stored;
    QString source;
    qint64 storedAtMs = 0;
    if (m_live) {
        if (!m_sync)
            return false;
        stored = m_sync->draft();
        source = m_sync->draftSongSource();
        storedAtMs = m_sync->draftUpdatedAtMs();
    } else {
        stored = m_mockDraft;
        source = m_mockDraftSource;
        storedAtMs = m_mockDraftAtMs;
    }
    // Unsaved changes survive a restart; a stored draft equal to the
    // published page is nothing to offer.
    const bool surviving = stored && !Profile::samePublishedContent(*stored, m_published);
    // The files songs were cut from stay known by song for this run, so the
    // Song tab still shows a published song's file card after a save.
    m_songSource = {};
    m_history.clear();
    m_tryOnPreset = -1;
    m_editing = true; // before loading: the editor shows the draft's media
    if (surviving)
        restoreSongSource(source);
    loadDraft(surviving ? *stored : m_published);
    m_survivingDraftAtMs = surviving ? storedAtMs : 0;
    // A blob only the draft named may have been collected meanwhile.
    m_applyingHistory = true;
    (void)dropUnresolvedDraftMedia();
    m_applyingHistory = false;
    updateDirty();
    updateCandidates();
    emit editingChanged();
    emit historyChanged();
    emit tryOnChanged();
    emit importChanged();
    emit navigationChanged(); // the Back chip now reads "Profile"
    return true;
}

void ProfileController::continueDraft()
{
    if (m_survivingDraftAtMs == 0)
        return;
    m_survivingDraftAtMs = 0;
    emit editingChanged();
}

void ProfileController::startOver()
{
    if (!m_editing)
        return;
    resetDraftToPublished();
}

void ProfileController::discardChanges()
{
    if (!m_editing)
        return;
    resetDraftToPublished();
}

void ProfileController::resetDraftToPublished()
{
    cancelImports();
    m_autosaveTimer.stop();
    if (m_live) {
        if (m_sync)
            (void)m_sync->discardDraft();
    } else {
        m_mockDraft.reset();
        m_mockDraftSource.clear();
        m_mockDraftAtMs = 0;
    }
    m_songSource = {};
    m_history.clear();
    m_survivingDraftAtMs = 0;
    loadDraft(m_published);
    updateDirty();
    updateCandidates();
    emit editingChanged();
    emit historyChanged();
    emit importChanged();
}

void ProfileController::endEditing()
{
    if (!m_editing)
        return;
    flushAutosave();
    leaveEditing();
}

void ProfileController::leaveEditing()
{
    if (!m_editing)
        return;
    m_editing = false;
    m_survivingDraftAtMs = 0;
    m_history.clear();
    const bool tryingOn = m_tryOnPreset >= 0;
    m_tryOnPreset = -1;
    m_idleMediaTimer.start();
    emit editingChanged();
    emit historyChanged();
    if (tryingOn)
        emit tryOnChanged();
    emit navigationChanged();
}

void ProfileController::loadDraft(const Profile::Page &page)
{
    m_draft.setViewer({m_darkMode, false});
    m_draft.load(page);
    m_draftShadow = m_draft.page();
    followDraftSong();
    updateMedia(m_draft, draftMediaSource(), true);
    refreshTryOn();
}

bool ProfileController::publish()
{
    if (!m_editing)
        return false;
    if (importsBusy()) {
        // "Saving…": the page goes out as soon as the picture or song is in.
        if (!m_publishPending) {
            m_publishPending = true;
            emit publishedChanged();
        }
        return true;
    }
    return publishNow();
}

bool ProfileController::publishNow()
{
    closeGesture();
    // The draft's stored media columns are what a publish checks its refs
    // against, so they must name the draft's blobs first.
    saveDraftNow();
    m_applyingHistory = true;
    const bool dropped = dropUnresolvedDraftMedia();
    m_applyingHistory = false;
    if (dropped)
        return false; // the owner is told what went, and saves again

    const Profile::Page page = m_draft.page();
    if (m_live) {
        if (!m_sync || m_sync->publish(page) == 0) {
            setNotice(QStringLiteral("Your profile could not be saved. Try again."));
            return false;
        }
    } else {
        Profile::Page stored = Profile::normalized(page);
        const qint64 now = nowMs();
        stored.revision = Profile::nextRevision(m_mockPublished.revision, now);
        stored.publishedAtMs = now;
        m_mockPublished = stored;
        m_mockDraft.reset();
        m_mockDraftSource.clear();
        m_mockDraftAtMs = 0;
    }
    // The display name stays page-only: the session name is never touched,
    // since group rosters carry it to people who are not contacts.
    m_autosaveTimer.stop();
    reloadPublished();
    loadDraft(m_published);
    leaveEditing();
    updateDirty();
    refreshPerson();
    updateLinkOnline();
    emit publishedChanged();
    emit published(!m_linkOnline);
    return true;
}

void ProfileController::finishPendingPublish(bool importSucceeded)
{
    if (!m_publishPending || importsBusy())
        return;
    m_publishPending = false;
    if (importSucceeded && m_editing)
        (void)publishNow();
    emit publishedChanged();
}

bool ProfileController::importsBusy() const
{
    return m_backgroundImporting || songImporting();
}

bool ProfileController::dropUnresolvedDraftMedia()
{
    Profile::Page page = m_draft.page();
    const MediaSource own{MediaSource::Owner::Own, {}};
    QString notice;
    if (page.background.isSet()
        && blob(own, page.background.sha256, Profile::MediaKind::BackgroundImageMedia).isEmpty()) {
        page.background = {};
        if (page.theme.backgroundKind == Profile::BackgroundKind::ImageBackground)
            page.theme.backgroundKind = Profile::BackgroundKind::SolidBackground;
        notice = mediaGoneNotice(Profile::MediaKind::BackgroundImageMedia);
    }
    if (page.song.isSet() && blob(own, page.song.sha256, Profile::MediaKind::SongMedia).isEmpty()) {
        page.song = {};
        notice = mediaGoneNotice(Profile::MediaKind::SongMedia);
    }
    if (notice.isEmpty())
        return false;
    m_draft.edit(page);
    setNotice(notice);
    return true;
}

void ProfileController::onDraftEdited()
{
    const Profile::Page current = m_draft.page();
    const bool couldUndo = canUndo();
    const bool couldRedo = canRedo();
    const bool friendsChanged = current.topFriends != m_draftShadow.topFriends;
    if (!m_applyingHistory)
        m_history.record(m_draftShadow, current);
    m_draftShadow = current;
    scheduleAutosave();
    updateDirty();
    followDraftSong();
    refreshTryOn();
    // After an undo the blobs resolve only once the draft is saved; the
    // history step reads its media then (applyHistoryPage).
    if (!m_applyingHistory)
        updateMedia(m_draft, draftMediaSource());
    if (friendsChanged)
        updateCandidates();
    emitHistoryIfChanged(couldUndo, couldRedo);
}

void ProfileController::editDraft(const Profile::Page &page)
{
    closeGesture();
    m_draft.edit(page);
}

void ProfileController::closeGesture()
{
    if (!m_history.inGesture())
        return;
    const bool couldUndo = canUndo();
    const bool couldRedo = canRedo();
    m_history.endGesture(m_draft.page());
    emitHistoryIfChanged(couldUndo, couldRedo);
}

void ProfileController::emitHistoryIfChanged(bool couldUndo, bool couldRedo)
{
    if (couldUndo != canUndo() || couldRedo != canRedo())
        emit historyChanged();
}

void ProfileController::updateDirty()
{
    const bool dirty = !Profile::samePublishedContent(m_draft.page(), m_published);
    if (dirty == m_draftDirty)
        return;
    m_draftDirty = dirty;
    emit draftDirtyChanged();
}

void ProfileController::refreshTryOn()
{
    if (m_tryOnPreset < 0)
        return;
    m_tryOn.setViewer({m_darkMode, false});
    m_tryOn.load(Profile::applyPreset(m_draft.page(), Profile::Preset(m_tryOnPreset)));
    updateMedia(m_tryOn, draftMediaSource());
}

void ProfileController::scheduleAutosave()
{
    m_autosaveTimer.start();
}

void ProfileController::flushAutosave()
{
    if (m_autosaveTimer.isActive())
        saveDraftNow();
}

void ProfileController::saveDraftNow()
{
    m_autosaveTimer.stop();
    const Profile::Page page = m_draft.page();
    // A draft equal to the published page is no draft: nothing to offer on
    // the next visit.
    const bool dirty = !Profile::samePublishedContent(page, m_published);
    if (m_live) {
        if (!m_sync)
            return;
        if (dirty ? !m_sync->saveDraft(page, songSourceJson()) : !m_sync->discardDraft())
            setNotice(QStringLiteral("Your changes could not be saved."));
        return;
    }
    if (dirty) {
        m_mockDraft = page;
        m_mockDraftSource = songSourceJson();
        m_mockDraftAtMs = nowMs();
    } else {
        m_mockDraft.reset();
        m_mockDraftSource.clear();
        m_mockDraftAtMs = 0;
    }
}

void ProfileController::applyPreset(int preset)
{
    if (!m_editing || !isShippedPreset(preset))
        return;
    if (m_tryOnPreset >= 0) {
        m_tryOnPreset = -1;
        emit tryOnChanged();
    }
    editDraft(Profile::applyPreset(m_draft.page(), Profile::Preset(preset)));
}

void ProfileController::setTryOnPreset(int preset)
{
    if (!m_editing || !isShippedPreset(preset))
        preset = -1;
    if (preset == m_tryOnPreset)
        return;
    m_tryOnPreset = preset;
    refreshTryOn();
    emit tryOnChanged();
}

void ProfileController::resetToPreset()
{
    if (!m_editing)
        return;
    // Only the style knobs go back (what the "edited" tag compares); the
    // arrangement the owner made since picking the preset stays.
    Profile::Page page = m_draft.page();
    page.theme = Profile::presetTheme(page.preset); // a Custom page: the default look
    if (page == m_draft.page())
        return;
    editDraft(page);
}

void ProfileController::surpriseMe()
{
    if (!m_editing)
        return;
    const Profile::Page page = m_draft.page();
    QVector<Profile::Preset> others;
    for (int preset = int(Profile::Preset::AeroSkyPreset); preset <= int(Profile::Preset::ChromeY2KPreset); ++preset) {
        if (Profile::Preset(preset) != page.preset)
            others.push_back(Profile::Preset(preset));
    }
    const Profile::Preset preset = others.at(m_pick(int(others.size())));
    Profile::Page next = Profile::applyPreset(page, preset);
    const QVector<Profile::Motif> family = Profile::motifFamily(preset);
    if (!family.isEmpty())
        next.theme.motif = family.at(m_pick(int(family.size())));
    editDraft(next);
}

void ProfileController::undo()
{
    if (!m_editing)
        return;
    if (std::optional<Profile::Page> previous = m_history.undo(m_draft.page()))
        applyHistoryPage(*previous);
}

void ProfileController::redo()
{
    if (!m_editing)
        return;
    if (std::optional<Profile::Page> next = m_history.redo(m_draft.page()))
        applyHistoryPage(*next);
}

void ProfileController::applyHistoryPage(const Profile::Page &page)
{
    m_applyingHistory = true;
    m_draft.edit(page);
    // A snapshot may name a blob the draft let go of; saving the draft
    // points its media columns back at it, and one collected meanwhile
    // (only the history named it) is dropped with a notice.
    saveDraftNow();
    (void)dropUnresolvedDraftMedia();
    m_applyingHistory = false;
    m_draftShadow = m_draft.page();
    updateDirty();
    updateMedia(m_draft, draftMediaSource(), true);
    refreshTryOn();
    emit historyChanged();
}

void ProfileController::beginGesture(const QString &key)
{
    if (!m_editing || key.isEmpty())
        return;
    const bool couldUndo = canUndo();
    const bool couldRedo = canRedo();
    m_history.beginGesture(key, m_draft.page());
    emitHistoryIfChanged(couldUndo, couldRedo);
}

void ProfileController::endGesture()
{
    closeGesture();
}

// --- Top Friends and layout ---------------------------------------------------

bool ProfileController::addTopFriend(const QString &contactId)
{
    if (!m_editing)
        return false;
    const std::optional<Contact> contact = rosterContact(contactId);
    const std::optional<AccountId> account = accountFor(contactId);
    if (!contact || !account || account->toHex() == localAccountHex())
        return false; // accepted people only
    Profile::Page page = m_draft.page();
    for (const Profile::TopFriend &placed : std::as_const(page.topFriends)) {
        if (placed.accountId == account->bytes())
            return false;
    }
    if (page.topFriends.size() >= Profile::maxTopFriends) {
        setNotice(QStringLiteral("Your Friend Space holds eight people. Remove someone first."));
        return false;
    }
    // The label goes on the wire, so it is the name as this owner knows them.
    page.topFriends.push_back({account->bytes(), Profile::sanitizeLine(contact->name, Profile::TextBounds::friendName)});
    editDraft(page);
    return true;
}

void ProfileController::removeTopFriend(int index)
{
    if (!m_editing)
        return;
    Profile::Page page = m_draft.page();
    if (index < 0 || index >= page.topFriends.size())
        return;
    page.topFriends.removeAt(index);
    editDraft(page);
}

void ProfileController::moveTopFriend(int from, int to)
{
    if (!m_editing)
        return;
    Profile::Page page = m_draft.page();
    const int count = int(page.topFriends.size());
    if (from < 0 || from >= count || count == 0)
        return;
    to = std::clamp(to, 0, count - 1);
    if (from == to)
        return;
    page.topFriends.move(from, to);
    editDraft(page);
}

void ProfileController::moveModule(int module, int column, int index)
{
    if (!m_editing || module < int(Profile::Module::HandleModule) || module > int(Profile::Module::TopFriendsModule)
        || (column != int(Profile::Column::NarrowColumn) && column != int(Profile::Column::WideColumn)))
        return;
    Profile::Page page = m_draft.page();
    QVector<Profile::ModulePlacement> modules = page.modules.isEmpty() ? Profile::defaultModules() : page.modules;
    const auto found = std::find_if(modules.begin(), modules.end(), [module](const Profile::ModulePlacement &entry) {
        return int(entry.module) == module;
    });
    if (found == modules.end())
        return;
    Profile::ModulePlacement placement = *found;
    modules.erase(found);
    placement.column = Profile::Column(column);
    // The arrangement is kept canonical: the narrow column's modules, then
    // the wide column's, each in order.
    QVector<Profile::ModulePlacement> narrow;
    QVector<Profile::ModulePlacement> wide;
    for (const Profile::ModulePlacement &entry : std::as_const(modules))
        (entry.column == Profile::Column::NarrowColumn ? narrow : wide).push_back(entry);
    QVector<Profile::ModulePlacement> &target = placement.column == Profile::Column::NarrowColumn ? narrow : wide;
    target.insert(std::clamp<qsizetype>(index, 0, target.size()), placement);
    page.modules = narrow + wide;
    if (page == m_draft.page())
        return;
    editDraft(page);
}

void ProfileController::setModuleVisible(int module, bool visible)
{
    if (!m_editing)
        return;
    Profile::Page page = m_draft.page();
    if (page.modules.isEmpty())
        page.modules = Profile::defaultModules();
    for (Profile::ModulePlacement &entry : page.modules) {
        if (int(entry.module) == module)
            entry.visible = visible;
    }
    if (page == m_draft.page())
        return;
    editDraft(page);
}

// --- Colours ---------------------------------------------------------------------

void ProfileController::rememberColor(const QColor &color)
{
    if (!color.isValid())
        return;
    const QString name = color.name(QColor::HexRgb).toUpper();
    QStringList recent = m_recentColors;
    recent.removeAll(name);
    recent.prepend(name);
    while (recent.size() > maxRecentColors)
        recent.removeLast();
    if (recent == m_recentColors)
        return;
    m_recentColors = recent;
    QSettings settings;
    settings.setValue(recentColorsKey, m_recentColors);
    emit recentColorsChanged();
}

QVariantList ProfileController::recentColors() const
{
    QVariantList colours;
    for (const QString &name : m_recentColors)
        colours.append(QColor(name));
    return colours;
}

QVariantMap ProfileController::contrastFor(int inkRole, const QColor &color) const
{
    if (inkRole < int(Profile::InkRole::BodyInk) || inkRole > int(Profile::InkRole::AltHeaderTextInk)
        || !color.isValid())
        return {};
    // Measured on the draft as the editor shows it (the colour picker is the
    // editor's).
    const Profile::Theme theme = ProfileRenderStyle::viewedTheme(m_draft.page().theme, m_draft.viewer());
    const QString key = m_draftMedia.imageShown ? m_draftMedia.imageKey : QString();
    const std::optional<ImageStats> stats =
        key.isEmpty() ? std::nullopt : ProfileMediaStore::instance().stats(key);
    const ProfileReadability::ContrastReport report = ProfileReadability::contrastFor(
        theme, ProfileReadability::pageSamples(theme, stats), Profile::InkRole(inkRole), color);
    return {{QStringLiteral("ratio"), report.ratio},
            {QStringLiteral("passes"), report.passes},
            {QStringLiteral("shown"), report.shown}};
}

void ProfileController::setLastTab(int tab)
{
    if (tab < int(Profile::EditorTab::ThemesTab) || tab > int(Profile::EditorTab::LayoutTab) || tab == m_lastTab)
        return;
    m_lastTab = tab;
    QSettings settings;
    settings.setValue(lastTabKey, tab);
    emit editingChanged();
}

// ---------------------------------------------------------------------------
// Imports
// ---------------------------------------------------------------------------

ProfileBackgroundImporter &ProfileController::backgroundImporter()
{
    if (!m_backgroundImporter) {
        m_backgroundImporter = std::make_unique<ProfileBackgroundImporter>();
        connect(m_backgroundImporter.get(), &ProfileBackgroundImporter::progressChanged, this, [this](qreal progress) {
            m_backgroundProgress = progress;
            emit importChanged();
        });
        connect(m_backgroundImporter.get(), &ProfileBackgroundImporter::finished, this,
                &ProfileController::onBackgroundImported);
        connect(m_backgroundImporter.get(), &ProfileBackgroundImporter::failed, this,
                [this](ProfileImageError, const QString &message) { onBackgroundFailed(message); });
    }
    return *m_backgroundImporter;
}

SongImporter &ProfileController::songImporter()
{
    if (!m_songImporter) {
        m_songImporter = std::make_unique<SongImporter>();
        connect(m_songImporter.get(), &SongImporter::analysed, this, &ProfileController::onSongAnalysed);
        connect(m_songImporter.get(), &SongImporter::encoded, this, &ProfileController::onSongEncoded);
        connect(m_songImporter.get(), &SongImporter::failed, this,
                [this](SongImportError, const QString &message) { onSongFailed(message); });
    }
    return *m_songImporter;
}

void ProfileController::importBackground(const QUrl &file)
{
    if (!m_editing)
        return;
    setNotice({});
    m_backgroundImporting = true;
    m_backgroundProgress = 0;
    emit importChanged();
    // Transparency is composited onto the page's own base colour.
    backgroundImporter().start(localPath(file), m_draft.backgroundColor1());
}

void ProfileController::onBackgroundImported(const QByteArray &jpeg, QSize size)
{
    m_backgroundImporting = false;
    m_backgroundProgress = 1;
    const std::optional<QByteArray> sha256 = storeLocalMedia(Profile::MediaKind::BackgroundImageMedia, jpeg);
    if (!sha256) {
        setNotice(QStringLiteral("This picture could not be saved."));
        emit importChanged();
        finishPendingPublish(false);
        return;
    }
    Profile::Page page = m_draft.page();
    page.background.sha256 = *sha256;
    page.background.bytes = quint32(jpeg.size());
    page.background.width = quint16(std::clamp(size.width(), 0, Profile::maxBackgroundDimension));
    page.background.height = quint16(std::clamp(size.height(), 0, Profile::maxBackgroundDimension));
    page.background.durationMs = 0;
    page.theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
    editDraft(page);
    saveDraftNow();
    updateMedia(m_draft, draftMediaSource(), true);
    emit importChanged();
    finishPendingPublish(true);
}

void ProfileController::onBackgroundFailed(const QString &message)
{
    m_backgroundImporting = false;
    setNotice(message);
    emit importChanged();
    finishPendingPublish(false);
}

void ProfileController::removeBackgroundImage()
{
    if (!m_editing)
        return;
    Profile::Page page = m_draft.page();
    if (!page.background.isSet())
        return;
    page.background = {};
    if (page.theme.backgroundKind == Profile::BackgroundKind::ImageBackground)
        page.theme.backgroundKind = Profile::BackgroundKind::SolidBackground;
    editDraft(page);
}

void ProfileController::importSong(const QUrl &file)
{
    if (!m_editing)
        return;
    setNotice({});
    m_songWindowTimer.stop();
    m_songImport = {};
    m_songImport.path = localPath(file);
    m_songIsNewImport = true;
    m_songAnalysing = true;
    m_songEncoding = false;
    emit importChanged();
    songImporter().analyse(m_songImport.path);
}

void ProfileController::onSongAnalysed(const SongSourceInfo &info)
{
    m_songAnalysing = false;
    m_songImport.fileName = info.fileName;
    m_songImport.formatLabel = info.formatLabel;
    m_songImport.durationMs = info.durationMs;
    m_songImport.peaks = info.peaks;
    m_songImport.windowStartMs = info.defaultWindowStartMs;
    m_songTitleTag = info.title;
    m_songArtistTag = info.artist;
    m_songEncoding = true;
    emit importChanged();
    songImporter().encodeWindow(m_songImport.path, info.defaultWindowStartMs);
}

void ProfileController::onSongEncoded(const QByteArray &container, qint64 durationMs, qint64 windowStartMs)
{
    m_songEncoding = false;
    const std::optional<QByteArray> sha256 = storeLocalMedia(Profile::MediaKind::SongMedia, container);
    if (!sha256) {
        m_songIsNewImport = false;
        m_songImport = {};
        setNotice(QStringLiteral("This song could not be saved."));
        emit importChanged();
        finishPendingPublish(false);
        return;
    }
    SongSource source = m_songIsNewImport ? m_songImport : m_songSource;
    // Where the window really starts: the importer clamps it to the file.
    source.windowStartMs = windowStartMs;
    source.songHash = *sha256;
    m_songSources.insert(*sha256, source);
    m_songSource = source;

    Profile::Page page = m_draft.page();
    page.song = {};
    page.song.sha256 = *sha256;
    page.song.bytes = quint32(container.size());
    page.song.durationMs = quint32(std::clamp<qint64>(durationMs, 0, Profile::maxSongRefDurationMs));
    if (m_songIsNewImport) {
        // A new file brings its own title and artist (the base name when it
        // has no tags); a new window of the same file keeps what was typed.
        page.content.songTitle = Profile::sanitizeLine(m_songTitleTag, Profile::TextBounds::songTitle);
        page.content.songArtist = Profile::sanitizeLine(m_songArtistTag, Profile::TextBounds::songArtist);
    }
    m_songIsNewImport = false;
    m_songImport = {};
    SongLibrary::instance().put(QString::fromLatin1(sha256->toHex()), container);
    editDraft(page);
    saveDraftNow();
    updateMedia(m_draft, draftMediaSource(), true);
    emit importChanged();
    finishPendingPublish(true);
}

void ProfileController::onSongFailed(const QString &message)
{
    m_songAnalysing = false;
    m_songEncoding = false;
    m_songIsNewImport = false;
    m_songImport = {};
    setNotice(message);
    emit importChanged();
    finishPendingPublish(false);
}

void ProfileController::setSongWindow(qint64 startMs)
{
    if (!m_editing)
        return;
    SongSource &source = m_songIsNewImport && !m_songImport.fileName.isEmpty() ? m_songImport : m_songSource;
    if (source.path.isEmpty())
        return;
    source.windowStartMs = std::max<qint64>(0, startMs);
    // A window encode still running is for where the handle was: its result
    // would only flash in before the one for where it rests.
    if (m_songEncoding && !m_songAnalysing && m_songImporter) {
        m_songImporter->cancel();
        m_songEncoding = false;
    }
    // Re-encoded once the handle rests: a drag is one encode, not fifty.
    m_songWindowTimer.start();
    emit importChanged();
}

void ProfileController::startSongWindowEncode()
{
    const SongSource &source = shownSongSource();
    if (source.path.isEmpty())
        return;
    m_songEncoding = true;
    emit importChanged();
    songImporter().encodeWindow(source.path, source.windowStartMs);
}

const ProfileController::SongSource &ProfileController::shownSongSource() const
{
    return m_songIsNewImport && !m_songImport.fileName.isEmpty() ? m_songImport : m_songSource;
}

void ProfileController::removeSong()
{
    if (!m_editing)
        return;
    Profile::Page page = m_draft.page();
    if (!page.song.isSet())
        return;
    page.song = {};
    page.content.songTitle.clear();
    page.content.songArtist.clear();
    editDraft(page);
    emit importChanged();
}

void ProfileController::cancelImports()
{
    if (m_backgroundImporter)
        m_backgroundImporter->cancel();
    if (m_songImporter)
        m_songImporter->cancel();
    m_songWindowTimer.stop();
    const bool wasBusy = importsBusy() || m_songIsNewImport;
    m_backgroundImporting = false;
    m_songAnalysing = false;
    m_songEncoding = false;
    m_songIsNewImport = false;
    m_songImport = {};
    if (m_publishPending) {
        m_publishPending = false;
        emit publishedChanged();
    }
    if (wasBusy)
        emit importChanged();
}

bool ProfileController::songImporting() const
{
    return m_songAnalysing || m_songEncoding || m_songWindowTimer.isActive();
}

QVariantMap ProfileController::songSource() const
{
    const SongSource &source = shownSongSource();
    if (source.fileName.isEmpty())
        return {};
    return {{QStringLiteral("fileName"), source.fileName},
            {QStringLiteral("formatLabel"), source.formatLabel},
            {QStringLiteral("durationMs"), source.durationMs},
            // The window can only be moved while the file is still there.
            {QStringLiteral("available"), !source.path.isEmpty() && QFileInfo::exists(source.path)}};
}

QVariantList ProfileController::songPeaks() const
{
    QVariantList peaks;
    for (const quint8 peak : shownSongSource().peaks)
        peaks.append(int(peak));
    return peaks;
}

qint64 ProfileController::songWindowMs() const
{
    return std::min<qint64>(SongContainer::maxDurationMs, shownSongSource().durationMs);
}

qint64 ProfileController::songClipBytes() const
{
    return m_draft.page().song.isSet() ? qint64(m_draft.page().song.bytes) : 0;
}

void ProfileController::followDraftSong()
{
    const Profile::MediaRef &song = m_draft.page().song;
    const SongSource next = song.isSet() ? m_songSources.value(song.sha256) : SongSource{};
    if (next.songHash == m_songSource.songHash && next.path == m_songSource.path
        && next.windowStartMs == m_songSource.windowStartMs)
        return;
    m_songSource = next;
    emit importChanged();
}

QString ProfileController::songSourceJson() const
{
    if (m_songSource.path.isEmpty() || m_songSource.songHash.isEmpty())
        return {};
    QJsonArray peaks;
    for (const quint8 peak : m_songSource.peaks)
        peaks.append(int(peak));
    const QJsonObject object{{QStringLiteral("path"), m_songSource.path},
                             {QStringLiteral("fileName"), m_songSource.fileName},
                             {QStringLiteral("formatLabel"), m_songSource.formatLabel},
                             {QStringLiteral("durationMs"), double(m_songSource.durationMs)},
                             {QStringLiteral("windowStartMs"), double(m_songSource.windowStartMs)},
                             {QStringLiteral("peaks"), peaks},
                             {QStringLiteral("song"), QString::fromLatin1(m_songSource.songHash.toHex())}};
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void ProfileController::restoreSongSource(const QString &json)
{
    const QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
    const QByteArray songHash = QByteArray::fromHex(object.value(QStringLiteral("song")).toString().toLatin1());
    if (songHash.size() != 32)
        return;
    SongSource source;
    source.path = object.value(QStringLiteral("path")).toString();
    source.fileName = object.value(QStringLiteral("fileName")).toString();
    source.formatLabel = object.value(QStringLiteral("formatLabel")).toString();
    source.durationMs = qint64(object.value(QStringLiteral("durationMs")).toDouble());
    source.windowStartMs = qint64(object.value(QStringLiteral("windowStartMs")).toDouble());
    for (const QJsonValue &peak : object.value(QStringLiteral("peaks")).toArray())
        source.peaks.push_back(quint8(std::clamp(peak.toInt(), 0, 255)));
    source.songHash = songHash;
    m_songSources.insert(songHash, source);
    followDraftSong();
}

// ---------------------------------------------------------------------------
// Catalogues
// ---------------------------------------------------------------------------

QVariantList ProfileController::presets()
{
    QVariantList list;
    for (int preset = int(Profile::Preset::AeroSkyPreset); preset <= int(Profile::Preset::ChromeY2KPreset); ++preset) {
        list.append(QVariantMap{{QStringLiteral("id"), preset},
                                {QStringLiteral("name"), Profile::presetName(Profile::Preset(preset))},
                                {QStringLiteral("slug"), Profile::presetSlug(Profile::Preset(preset))}});
    }
    return list;
}

QVariantList ProfileController::fontChoices()
{
    QVariantList list;
    for (int id = int(Profile::Font::InterfaceFont); id <= int(Profile::Font::MarkerFont); ++id) {
        const auto font = Profile::Font(id);
        list.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), Profile::fontName(font)},
            {QStringLiteral("category"), Profile::fontCategory(font)},
            {QStringLiteral("bodySafe"), Profile::isBodySafe(font)},
            {QStringLiteral("headingCapable"), Profile::isHeadingCapable(font)},
            // For the editor's samples in each face ("" = the interface font).
            {QStringLiteral("headingFamily"), ProfileFonts::family(font, ProfileFonts::Role::Heading)},
            {QStringLiteral("nameFamily"), ProfileFonts::family(font, ProfileFonts::Role::Name)},
        });
    }
    return list;
}

QVariantList ProfileController::motifChoices()
{
    QVariantList list;
    for (int id = int(Profile::Motif::Stars); id <= int(Profile::Motif::LinenWeave); ++id) {
        list.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("name"), Profile::motifName(Profile::Motif(id))}});
    }
    return list;
}

QVariantList ProfileController::nameEffectChoices()
{
    // The second colour's well is labelled by what it does (SPEC §14.9).
    static const std::array<std::pair<const char *, const char *>, 7> effects{{
        {"None", ""},
        {"Glow", "Glow colour"},
        {"Outline", "Outline colour"},
        {"Gradient", "Fades to"},
        {"Glitter", "Sparkle colour"},
        {"Chrome", "Steel tint"},
        {"Shadow", "Shadow colour"},
    }};
    QVariantList list;
    for (int id = 0; id < int(effects.size()); ++id) {
        list.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("name"), QString::fromLatin1(effects.at(id).first)},
                                {QStringLiteral("colour2Label"), QString::fromLatin1(effects.at(id).second)}});
    }
    return list;
}

QVariantList ProfileController::flourishChoices()
{
    QVariantList list;
    for (int id = int(Profile::Flourish::NoFlourish); id <= int(Profile::Flourish::FlowerFlourish); ++id) {
        const auto flourish = Profile::Flourish(id);
        const QString prefix = Profile::flourishPrefix(flourish);
        // The chip shows the mark itself: – ★ xXx ♥ ~* ♫ ✿
        const QString chip = prefix.trimmed().isEmpty() ? QString(QChar(0x2013)) : prefix.trimmed();
        list.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("name"), chip},
                                {QStringLiteral("prefix"), prefix},
                                {QStringLiteral("suffix"), Profile::flourishSuffix(flourish)}});
    }
    return list;
}

QVariantList ProfileController::ambientChoices()
{
    static const std::array<const char *, 5> names{"None", "Stars", "Hearts", "Snow", "Sparkles"};
    QVariantList list;
    for (int id = 0; id < int(names.size()); ++id)
        list.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), QString::fromLatin1(names.at(id))}});
    return list;
}

QVariantList ProfileController::zodiacChoices()
{
    QVariantList list;
    for (int id = int(Profile::Zodiac::NoZodiac); id <= int(Profile::Zodiac::Pisces); ++id) {
        list.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("name"), Profile::zodiacName(Profile::Zodiac(id))}});
    }
    return list;
}

QVariantList ProfileController::moodChoices()
{
    QVariantList list;
    for (int id = int(Profile::Mood::NoMood); id <= Profile::maxMood; ++id) {
        const auto mood = Profile::Mood(id);
        list.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("label"), Profile::moodName(mood)},
                                {QStringLiteral("face"), int(Profile::moodFace(mood))}});
    }
    return list;
}

QVariantList ProfileController::hereForChoices()
{
    QVariantList list;
    for (int bit = 0; bit < 6; ++bit) {
        const int flag = 1 << bit;
        list.append(QVariantMap{{QStringLiteral("bit"), flag},
                                {QStringLiteral("label"), Profile::hereForText(quint8(flag))}});
    }
    return list;
}

QVariantMap ProfileController::limits()
{
    using Bounds = Profile::TextBounds;
    return {{QStringLiteral("displayName"), Bounds::displayName},
            {QStringLiteral("headline"), Bounds::headline},
            {QStringLiteral("infoLine"), Bounds::infoLine},
            {QStringLiteral("infoLines"), Bounds::infoLines},
            {QStringLiteral("interest"), Bounds::interest},
            {QStringLiteral("detail"), Bounds::detail},
            {QStringLiteral("aboutMe"), Bounds::aboutMe},
            {QStringLiteral("meet"), Bounds::meet},
            {QStringLiteral("songTitle"), Bounds::songTitle},
            {QStringLiteral("songArtist"), Bounds::songArtist},
            {QStringLiteral("friendName"), Bounds::friendName},
            {QStringLiteral("maxTopFriends"), Profile::maxTopFriends},
            {QStringLiteral("songBytes"), qint64(maxSongBytes)},
            {QStringLiteral("backgroundBytes"), qint64(maxBackgroundImageBytes)},
            {QStringLiteral("songMs"), SongContainer::maxDurationMs},
            {QStringLiteral("backgroundSide"), Profile::maxBackgroundDimension}};
}

} // namespace OpenChat
