#pragma once

#include "domain/Attachment.h"
#include "domain/ChatTypes.h"
#include "domain/Identifiers.h"
#include "repositories/AttachmentRepository.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace OpenChat {

class AttachmentFileStore;
class ProfileSession;
class SyncEngine;

// AttachmentTransfer's tunables. Declared outside the class only so the
// constructor can default it (see ProfilePageSyncLimits).
struct AttachmentTransferLimits final {
    // --- Sending
    qint64 pumpIntervalMs = 100;
    qint64 pumpBacklogBytes = 64 * 1024; // a frame goes only while the link's unsent bytes are below this
    // --- Recovery (receiver side)
    qint64 recoveryCheckIntervalMs = 30'000;
    qint64 requestGraceMs = 2LL * 60 * 1000;          // no progress this long, link up: ask for what is missing
    qint64 requestMaxIntervalMs = 6LL * 60 * 60 * 1000; // the back-off doubles up to this (under 12 a day)
    int maxRequests = 48;                             // per attachment, then it is left as it is
    // --- Answers (sender side)
    qint64 answerIntervalMs = 10LL * 60 * 1000;       // per attachment and requester
    // --- Storage
    qint64 collectIntervalMs = 60LL * 60 * 1000;
    qint64 orphanTtlMs = 24LL * 60 * 60 * 1000;       // frames whose message never came
    qint64 orphanBytesPerSender = 32LL * 1024 * 1024; // per conversation and sender, before their message
    int orphanPreviewsPerSender = 16;
    qint64 minFreeDiskBytes = 512LL * 1024 * 1024;
    qint64 maxReceivedBytes = 8LL * 1024 * 1024 * 1024;
};

// One attachment as the tray prepared it (profile/ChatAttachmentImport.h):
// the descriptor without its id and key, which the transfer mints, the blob
// in its kind's format and the preview JPEG (may be empty).
struct OutgoingAttachment final {
    AttachmentDescriptor descriptor;
    QByteArray blob;
    QByteArray preview;
};

// Moves chat attachments' bytes (docs/chat-attachments.md) between this
// device and the members of its conversations, as sealed frames on
// AttachmentControl envelopes, and keeps them in the file store.
//
// Sending: stageOutgoing seals every part under a fresh key into the file
// store (off the GUI thread) before the message that describes them is
// encrypted, so a disk that cannot take them never moves the ratchet. Once
// the engine reports the message Sent, a paced pump hands the engine one
// frame at a time, the preview first: only while the link is up, the engine
// running, no call on, no earlier frame still in the outbox and little
// waiting in the socket. Frames go to the devices the message went to that
// are still in the conversation, nobody else. A message that Failed, or
// nobody left to send to, stops the transfer.
//
// Receiving: a frame is kept only for a live conversation, and only when it
// opens with its attachment's key (a frame that overtook its message is kept
// sealed, within a per-sender budget, and checked when the message comes).
// When every part is in, one worker at a time assembles and checks the blob
// against the message's hash. A transfer that stalls asks the sender for
// what is missing, with back-off; a sender answers only members its message
// went to, at most once per ten minutes each.
//
// Everything here only moves ciphertext and bookkeeping: it keeps running
// while the interface withholds plaintext. Blobs are opened only for
// loadBlob, off the GUI thread.
//
// Lifetimes: the session and engine are borrowed; destroy this before
// either (ChatController drops it when the engine is destroyed). Work on
// worker threads never reaches a destroyed transfer. GUI thread only.
class AttachmentTransfer final : public QObject
{
    Q_OBJECT

public:
    // The devices of `conversation` other than this one, as the roster
    // stands now (the peer's, or every other member's); empty for a
    // conversation that is gone.
    using RosterFn = std::function<QList<DeviceId>(const ConversationId &)>;
    using Clock = std::function<qint64()>;
    using Limits = AttachmentTransferLimits;
    // A sealed attachment's descriptor (its id and key set), ready for
    // SyncEngine::enqueueAttachment, or why it could not be sealed.
    using Staged = std::variant<AttachmentDescriptor, QString>;

    AttachmentTransfer(ProfileSession &session, SyncEngine &engine, RosterFn roster, Clock clock,
                       Limits limits = {}, QObject *parent = nullptr);
    ~AttachmentTransfer() override;

    AttachmentTransfer(const AttachmentTransfer &) = delete;
    AttachmentTransfer &operator=(const AttachmentTransfer &) = delete;

    // Mints the id and key, seals every part into the file store and hands
    // back the descriptor (on this thread, later). On any failure the file
    // is removed and `done` gets a sentence for the user.
    void stageOutgoing(const ConversationId &conversation, OutgoingAttachment attachment,
                       std::function<void(Staged)> done);
    // The engine refused the message: its sealed parts go.
    void discardStaged(const ConversationId &conversation, const AttachmentId &attachmentId);
    // Stops an attachment this device is sending (Cancelled, and every
    // recipient is told). False when it is not one still being sent.
    bool cancel(const MessageId &messageId);
    // While a call is on, no frame is sent.
    void setCallActive(bool active);
    // Something the pump waits on may have changed (the roster).
    void wake();

    // The whole blob of an attachment, opened and checked against its hash on
    // a worker, handed to `done` on this thread: one this device sent (all of
    // it was sealed here, whatever became of the sending), or one it
    // received completely. Empty for anything else, or bytes that do not
    // check out.
    void loadBlob(const MessageId &messageId, std::function<void(const QByteArray &)> done);
    // The checked preview of an attachment; empty when there is none (yet).
    [[nodiscard]] QByteArray previewFor(const MessageId &messageId);

    // Drops frames whose message never came, and the bytes of attachments
    // that failed or were cancelled. Runs at start and hourly.
    void collectGarbage();

    // Tests: how many frames the pump handed to the engine so far.
    [[nodiscard]] int framesSentForTesting() const noexcept { return m_framesSent; }

signals:
    // An attachment's bytes moved on: its state (AttachmentState), why it
    // stopped (AttachmentFailure), and parts sent or held of `total`.
    void transferChanged(const OpenChat::MessageId &messageId, int state, int reason, int done, int total);
    // A preview arrived for an attachment whose message was already shown.
    void previewArrived(const OpenChat::MessageId &messageId);

private:
    // An answer to a request for missing parts, sent through the pump to
    // the one device that asked: the parts still to send, or (for an
    // attachment that stopped) a Cancel frame.
    struct Answer final {
        StoredAttachment stored;
        DeviceId requester;
        std::deque<int> parts;
        bool cancel = false;
    };

    [[nodiscard]] AttachmentRepository *repository() const;
    [[nodiscard]] std::optional<DeviceId> localDevice() const;
    [[nodiscard]] qint64 now() const;
    void report(const StoredAttachment &stored, AttachmentState state, AttachmentFailure reason, int done);

    // Sending.
    void onMessageQueued(const MessageRecord &message);
    void onMessageStateChanged(const MessageId &messageId, DeliveryState state);
    void onLinkUp();
    void wakePump();
    void pumpOnce();
    [[nodiscard]] bool pumpGateOpen() const;
    // One frame of `stored` handed to the engine; false when none was (it
    // finished or stopped, or the engine would not take it now).
    [[nodiscard]] bool sendNextFrame(const StoredAttachment &stored);
    [[nodiscard]] bool sendAnswerFrame();
    [[nodiscard]] QByteArray partFrame(const StoredAttachment &stored, int index) const;
    [[nodiscard]] QList<DeviceId> currentRecipients(const StoredAttachment &stored) const;
    void finishOutgoing(const StoredAttachment &stored, AttachmentState state, AttachmentFailure reason);

    // Receiving.
    void onFrameReceived(const ConversationId &conversation, const DeviceId &sender, const QByteArray &frame);
    void onMessageReceived(const MessageRecord &message);
    void receivePart(const AttachmentRef &ref, int index, const QByteArray &frame);
    void receivePreview(const AttachmentRef &ref, const QByteArray &frame);
    void receiveRequest(const AttachmentRef &ref, const DeviceId &requester, const QByteArray &frame);
    void receiveCancel(const AttachmentRef &ref, const QByteArray &frame);
    [[nodiscard]] bool storageAllows(qint64 bytes);
    void failIncoming(const StoredAttachment &stored, AttachmentFailure reason);
    // A message whose frames came first: every part kept so far is opened
    // (on a worker) and the ones that do not open are dropped.
    void adoptOrphans(const StoredAttachment &stored, const AttachmentTransferRecord &transfer);
    // Every part is here: one worker at a time assembles the blob and checks
    // it against the message's hash.
    void validate(const StoredAttachment &stored);

    // Recovery.
    void runRecovery();

    ProfileSession &m_session;
    SyncEngine &m_engine;
    RosterFn m_roster;
    Clock m_clock;
    Limits m_limits;
    std::unique_ptr<AttachmentFileStore> m_files;
    // Sealing, opening and assembling run here, never on the GUI thread.
    // Waited for when the transfer goes, so no plaintext outlives it.
    std::unique_ptr<QThreadPool> m_pool;
    std::shared_ptr<std::atomic_bool> m_stopping;

    QTimer m_pumpTimer;
    QTimer m_recoveryTimer;
    QTimer m_collectTimer;
    bool m_pumpWakeQueued = false;
    bool m_callActive = false;
    bool m_failedClosedLogged = false;
    int m_framesSent = 0;
    // Since when the link has been up (0: down), so a device that just came
    // back gives the relay's backlog time to arrive before asking for parts.
    qint64 m_linkUpSinceMs = 0;
    // Round-robin over the attachments being sent.
    QByteArray m_lastServed;
    // Previews of attachments staged here, until their message is queued and
    // the preview can be stored with it.
    QHash<QByteArray, QByteArray> m_stagedPreviews; // attachment id bytes → JPEG
    std::deque<Answer> m_answers;
    QHash<QByteArray, qint64> m_lastAnswerMs; // message id ‖ requester → when
    // Attachments being assembled or adopted on a worker (message id bytes).
    QSet<QByteArray> m_validating;
    // When the message of each incoming attachment still under way arrived
    // in this run (message id bytes → ms).
    QHash<QByteArray, qint64> m_arrivedAtMs;
};

} // namespace OpenChat
