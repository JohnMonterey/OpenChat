#pragma once

#include "domain/Contact.h"
#include "domain/Identifiers.h"
#include "domain/ProfilePage.h"
#include "repositories/ProfilePageRepository.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

#include <functional>
#include <optional>
#include <utility>

namespace OpenChat {

class ProfileSession;
class SyncEngine;

// ProfilePageSync's tunables (ProfilePageSync::Limits). Declared outside the
// class only so the constructor can default it: a nested struct with member
// initialisers cannot be a default argument inside its own enclosing class.
struct ProfilePageSyncLimits final {
    // --- Viewer side
    qint64 requestBaseIntervalMs = 30LL * 60 * 1000;     // back-off 30 min, doubling per unanswered…
    qint64 requestMaxIntervalMs = 24LL * 60 * 60 * 1000; // …up to a day
    qint64 acceptRequestDelayMs = 30'000;                // ask a new contact if no page came by then
    qint64 acceptRequestJitterMs = 30'000;
    qint64 startupRequestJitterMs = 120'000;             // spread background asks after start/reconnect
    qint64 missingMediaGraceMs = 60'000;                 // a core may precede its media by this much
    qint64 pageLoadingWindowMs = 5'000;                  // "Getting …'s page…"
    // --- Owner side
    qint64 answerIntervalMs = 10LL * 60 * 1000;          // one answer per contact per 10 min…
    int answersPerDay = 4;                               // …and at most 4 a day
    qint64 mediaResendGuardMs = 60LL * 60 * 1000;        // never re-send a blob to one contact within 1 h
    // --- Delivery pump
    qint64 pumpIntervalMs = 500;
    qint64 pumpBacklogBytes = 64 * 1024; // send only while the link's unsent bytes are below this
    int pumpPayloadsPerTick = 8;
    // --- Storage
    qint64 pendingMediaTtlMs = 24LL * 60 * 60 * 1000;
    int maxPendingMediaPerContact = 2 + Profile::PanelBounds::maxMedia; // every blob one page may name
    qint64 localBlobGraceMs = 10LL * 60 * 1000;
    qint64 receivedMediaSoftCapBytes = 96LL * 1024 * 1024;
    qint64 receivedMediaTargetBytes = 80LL * 1024 * 1024;
};

// Moves profile pages between the local user and their contacts, over the
// ProfileUpdate envelopes (kind 7) of each contact's 1:1 conversation.
//
// Owner side: the editor's draft and published page live in the profile
// database; publish() only stores the page, and a paced pump delivers it. The
// pump sends one payload at a time while the relay link's unsent backlog is
// small, so chat and call signalling never queue behind page data; only the
// latest revision is ever sent; media goes only to contacts that have shown
// they understand pages (any page message from them) and waits while a call
// is on; and "sent" is recorded only once the engine has taken the payload.
// Every Accepted contact whose recorded revision is behind, or whose device
// changed, is owed a delivery, so a crash or a publish made offline is
// repaired at the next start or reconnect.
//
// Viewer side: pages arrive pushed, or answer our background requests, which
// are never triggered by opening a page (the owner cannot tell when a page
// was looked at), except to recover media evicted under the storage cap.
// Media is owner-scoped and kind-checked: a blob counts for a contact only if
// that contact sent it, in the slot its own core names it for.
//
// Answers to requests are bounded per contact (one per 10 minutes, four a
// day) and only ever carry the current published page. Nothing is sent to or
// accepted from anyone but an Accepted contact on their own 1:1 conversation
// and pinned device.
//
// Every payload is checked against maxPageSendBytes before it reaches the
// engine: a failed MLS encrypt would stop the whole engine for the session.
//
// Lifetimes: the session and engine are borrowed and must outlive this
// object; destroy it before ProfileSession::lock(). Single-threaded (the GUI
// thread), like the engine.
class ProfilePageSync final : public QObject
{
    Q_OBJECT

public:
    using Clock = std::function<qint64()>;
    using Random = std::function<qint64(qint64 bound)>; // uniform in [0, bound); injectable for tests
    using Limits = ProfilePageSyncLimits;

    struct ReceivedPage final {
        Profile::Page page;
        bool backgroundPresent = false; // this contact sent the background its core names
        bool songPresent = false;
        QSet<QByteArray> panelMediaPresent; // the panel blobs this contact sent as their core names them
        qint64 receivedAtMs = 0;
    };

    // Connects engine.profileUpdateReceived and engine.linkUp; runs a
    // collection; schedules background requests for page-less contacts and
    // for stored pages whose media never came (again on every linkUp);
    // starts the pump if work is owed.
    ProfilePageSync(ProfileSession &session, SyncEngine &engine, Clock clock, Random random,
                    Limits limits = {}, QObject *parent = nullptr);
    ~ProfilePageSync() override;

    ProfilePageSync(const ProfilePageSync &) = delete;
    ProfilePageSync &operator=(const ProfilePageSync &) = delete;

    // --- Own page

    [[nodiscard]] Profile::Page publishedPage() const; // defaultPage() (revision 0) when never published
    [[nodiscard]] qint64 publishedRevision() const;
    [[nodiscard]] std::optional<Profile::Page> draft() const;
    [[nodiscard]] QString draftSongSource() const;
    [[nodiscard]] qint64 draftUpdatedAtMs() const;
    // Normalises and stores the draft with its media refs. False when the
    // store refused it.
    bool saveDraft(const Profile::Page &draft, const QString &songSource);
    bool discardDraft();
    // Tests: saveDraft and discardDraft fail as a refused database write
    // would (nothing stored, false returned) until turned off again.
    void failDraftWritesForTesting(bool fail) { m_failDraftWritesForTesting = fail; }
    // Stores a processed blob and points the draft's slot for `kind` at it,
    // in one transaction. The bytes must pass the checks every contact will
    // make on arrival (a JPEG with 1 to 32 scans, or a well-formed song
    // container, within 224 KiB); anything else is refused here rather than
    // published and silently dropped by every viewer. Returns the hash.
    [[nodiscard]] std::optional<QByteArray> addLocalMedia(Profile::MediaKind kind, const QByteArray &bytes);
    [[nodiscard]] QByteArray localMedia(const QByteArray &sha256) const; // empty when absent
    // Stores the page as the next revision and wakes the pump; sends nothing
    // itself, so it works offline. Every set media ref must name a local blob
    // of its slot's kind. Returns the new revision, 0 on failure.
    qint64 publish(const Profile::Page &page);

    // --- Contacts' pages

    [[nodiscard]] std::optional<ReceivedPage> contactPage(const AccountId &contact) const;
    // The blob, only when `contact` sent it as `kind` and their current core
    // names it in that slot; empty otherwise.
    [[nodiscard]] QByteArray contactMedia(const AccountId &contact, const QByteArray &sha256,
                                          Profile::MediaKind kind) const;
    // Records that the page was looked at (it orders eviction). Sends
    // nothing, unless media of this page was evicted under the storage cap:
    // then it asks the owner for it again (back-off applies).
    void markViewed(const AccountId &contact);
    // No page from them yet, and a background request is about to go or went
    // out within pageLoadingWindowMs: "Getting …'s page…".
    [[nodiscard]] bool isAwaitingPage(const AccountId &contact) const;

    // --- Environment

    // While a call is active (ringing, in progress or still shown) only cores
    // and requests go out; media and answers wait.
    void setCallActive(bool active);
    [[nodiscard]] bool isLinkUp() const;
    [[nodiscard]] bool hasOwedDeliveries() const; // tests / diagnostics
    void collectGarbage();

public slots:
    // Connected to ContactRequestService::contactAccepted: delivers the
    // published page to the new contact and asks for theirs if none arrives.
    void onContactAccepted(const OpenChat::AccountId &contact);

signals:
    void contactPageChanged(const OpenChat::AccountId &contact);
    void awaitingChanged(const OpenChat::AccountId &contact);
    void localPagePublished(qint64 revision);

private:
    // The four background request triggers (ARCH §4.7 A–D).
    // Refresh: once, for a page stored before panels (docs/profile-panels.md).
    enum class Trigger : quint8 { Acceptance, Startup, MissingMedia, Viewed, Refresh };

    // A timed piece of viewer work: a background request, or (no trigger)
    // the end of a request's loading window, when awaitingChanged is due.
    struct Scheduled final {
        AccountId contact;
        std::optional<Trigger> trigger;
        qint64 dueAtMs = 0;
    };

    // An admitted request, answered by the pump one payload at a time.
    struct Answer final {
        AccountId contact;
        bool sendCore = false;
        QVector<QByteArray> blobs; // published hashes still to send
    };

    // A viewer request waiting for the pump.
    struct Request final {
        AccountId contact;
        Trigger trigger = Trigger::Startup;
        std::optional<qint64> haveRevision;
        QVector<QByteArray> wantMedia;
    };

    struct PumpJob;

    [[nodiscard]] ProfilePageRepository *pages() const;
    [[nodiscard]] qint64 now() const;
    [[nodiscard]] qint64 randomBelow(qint64 bound) const;
    void reloadLocal();
    [[nodiscard]] QVector<std::pair<Profile::MediaKind, QByteArray>> publishedBlobs() const;
    [[nodiscard]] std::optional<Profile::MediaKind> publishedKindOf(const QByteArray &sha256) const;

    // Accepted contacts that can be addressed: a conversation and a device.
    [[nodiscard]] QVector<ContactRecord> deliverableContacts() const;
    [[nodiscard]] std::optional<ContactRecord> deliverableContact(const AccountId &account) const;
    [[nodiscard]] std::optional<ContactRecord> resolveSender(const ConversationId &conversation,
                                                             const DeviceId &senderDevice);
    // The contact's delivery row, restarted first if their device changed.
    [[nodiscard]] std::optional<PageDelivery> currentDelivery(const ContactRecord &contact);
    [[nodiscard]] QVector<QByteArray> missingMedia(const StoredContactPage &stored) const;

    // Receiving.
    void onProfileUpdateReceived(const ConversationId &conversation, const DeviceId &senderDevice,
                                 const QByteArray &payload);
    void onLinkUp();
    void notePageMessageFrom(const ContactRecord &contact);
    void receiveCore(const ContactRecord &contact, const QByteArray &payload);
    void receiveMedia(const ContactRecord &contact, const QByteArray &payload);
    void receiveRequest(const ContactRecord &contact, const QByteArray &payload);
    void answerRequest(const AccountId &account, const std::optional<qint64> &haveRevision,
                       const QVector<QByteArray> &wantMedia);
    void enforceSoftCap();

    // Viewer requests.
    void requestIfDue(const AccountId &account, Trigger trigger);
    void scheduleStartupRequests();
    // Records what pages stored before panels name, and asks their owners once.
    void upgradeLegacyPages();
    void setMediaRequestedRevision(const AccountId &account, qint64 revision);
    void schedule(const AccountId &contact, std::optional<Trigger> trigger, qint64 dueAtMs);
    void unschedule(const AccountId &contact, Trigger trigger);
    [[nodiscard]] bool isScheduled(const AccountId &contact, Trigger trigger) const;
    void armScheduleTimer();
    void runDueSchedule();

    // The pump.
    void wakePump();
    void pumpOnce();
    [[nodiscard]] std::optional<PumpJob> nextJob(const QVector<ContactRecord> &contacts);
    [[nodiscard]] std::optional<PumpJob> nextAnswerJob(const QVector<ContactRecord> &contacts);
    [[nodiscard]] std::optional<PumpJob> nextRequestJob(const QVector<ContactRecord> &contacts);
    [[nodiscard]] std::optional<PumpJob> nextDeliveryJob(const QVector<ContactRecord> &contacts);
    [[nodiscard]] std::optional<PumpJob> mediaJob(const ContactRecord &contact, Profile::MediaKind kind,
                                                  const QByteArray &sha256);
    void recordCoreSent(const AccountId &account, qint64 revision);
    void recordRequestSent(const Request &request);

    ProfileSession &m_session;
    SyncEngine &m_engine;
    Clock m_clock;
    Random m_random;
    Limits m_limits;

    StoredLocalPage m_local;
    QVector<std::pair<Profile::MediaKind, QByteArray>> m_publishedBlobs; // from m_local.publishedCore
    // conversation → contact, a hint only: every hit is re-checked against
    // the stored contact, and any miss rebuilds it from a fresh read.
    QHash<ConversationId, AccountId> m_senderCache;

    QTimer m_pumpTimer;
    bool m_pumpWakeQueued = false;
    bool m_callActive = false;
    bool m_failedClosedLogged = false;
    bool m_failDraftWritesForTesting = false;
    QList<Answer> m_answers;   // FIFO, at most one per contact
    QList<Request> m_requests; // FIFO, at most one per contact
    std::optional<AccountId> m_lastServed; // round-robin cursor over deliveries
    // A published core the pump found over the send ceiling (only a damaged
    // store can produce one): never offered again, so it is logged once.
    std::optional<qint64> m_unsendableRevision;

    QList<Scheduled> m_schedule;
    QTimer m_scheduleTimer;
};

} // namespace OpenChat
