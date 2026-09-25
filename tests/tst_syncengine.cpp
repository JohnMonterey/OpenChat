#include "network/SyncEngine.h"

#include "domain/Attachment.h"
#include "domain/MessageContent.h"
#include "network/RelayClient.h"
#include "protocol/CanonicalCborCodec.h"
#include "protocol/CiphertextEnvelope.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <optional>

using namespace OpenChat;

Q_DECLARE_METATYPE(OpenChat::DeliveryState)

namespace {

// Serialized device credential the MLS layer authenticates:
// version(1) || deviceId(16) || signingKey(32).
QByteArray credentialBytes(const DeviceId &device)
{
    QByteArray credential;
    credential.append(char{1});
    credential.append(device.bytes());
    credential.append(QByteArray(32, 'k'));
    return credential;
}

// Deterministic MLS stand-in: encrypt prefixes "ENC:", process strips it (and
// reports Application), and an unknown payload is a stale/invalid message. The
// authenticated sender it reports is `senderDevice`.
class FakeMls final : public SyncMlsSession
{
public:
    Result<QByteArray, MlsError> encrypt(const ConversationId &, QByteArrayView plaintext) override
    {
        ++encryptCount;
        ++stateVersion;
        return Result<QByteArray, MlsError>::success(QByteArray("ENC:") + plaintext.toByteArray());
    }

    Result<SyncProcessOutcome, MlsError> process(const ConversationId &,
                                                 QByteArrayView mlsMessage) override
    {
        ++processCount;
        const QByteArray bytes = mlsMessage.toByteArray();
        if (!bytes.startsWith("ENC:"))
            return Result<SyncProcessOutcome, MlsError>::failure(MlsError::InvalidMessage);
        ++stateVersion;
        SyncProcessOutcome outcome;
        outcome.kind = SyncProcessOutcome::Kind::Application;
        outcome.applicationData = bytes.mid(4);
        outcome.senderIdentity = credentialBytes(senderDevice);
        return Result<SyncProcessOutcome, MlsError>::success(outcome);
    }

    Result<QList<QByteArray>, MlsError> inspectWelcome(QByteArrayView welcome) override
    {
        ++inspectWelcomeCount;
        lastInspectedWelcome = welcome.toByteArray();
        if (failInspect)
            return Result<QList<QByteArray>, MlsError>::failure(MlsError::InvalidMessage);
        return Result<QList<QByteArray>, MlsError>::success(inspectMembers);
    }

    Result<void, MlsError> joinGroup(const ConversationId &, QByteArrayView welcome) override
    {
        ++joinGroupCount;
        lastJoinedWelcome = welcome.toByteArray();
        if (failJoin)
            return Result<void, MlsError>::failure(MlsError::InvalidMessage);
        ++stateVersion;
        return Result<void, MlsError>::success();
    }

    QByteArray takePendingState() override
    {
        return QByteArray("state-") + QByteArray::number(stateVersion);
    }

    DeviceId senderDevice = DeviceId::generate();
    int encryptCount = 0;
    int processCount = 0;
    int stateVersion = 0;
    // inspectWelcome returns these members (default: one credential naming
    // senderDevice, so authentication passes); set failInspect to reject.
    QList<QByteArray> inspectMembers{credentialBytes(senderDevice)};
    bool failInspect = false;
    bool failJoin = false;
    int inspectWelcomeCount = 0;
    int joinGroupCount = 0;
    QByteArray lastInspectedWelcome;
    QByteArray lastJoinedWelcome;
};

struct StoredOutbox final {
    OutboxRecord record;
};

// In-memory SyncStore modelling the atomic durable operations and the outbox
// lease/retry contract.
class FakeStore final : public SyncStore
{
public:
    Result<void, RepositoryError> failSend(const EnvelopeId &id, const MessageId &messageId) override
    {
        for (auto &outbox : outboxes) {
            if (outbox.record.envelopeId == id && outbox.record.messageId == messageId) {
                outbox.record.state = OutboxState::Failed;
                deliveryStates.insert(messageId.bytes(), DeliveryState::Failed);
                return ok();
            }
        }
        return err();
    }

    Result<void, RepositoryError> commitSend(const MessageRecord &message,
                                             const OutboxRecord &outbox,
                                             QByteArrayView mlsState) override
    {
        if (failSendCommit)
            return err();
        messages.append(message);
        outboxes.append(StoredOutbox{outbox});
        lastMlsState = mlsState.toByteArray();
        ++commitSendCount;
        return ok();
    }

    Result<void, RepositoryError> commitControlSend(const OutboxRecord &outbox,
                                                    QByteArrayView mlsState) override
    {
        if (failSendCommit)
            return err();
        outboxes.append(StoredOutbox{outbox});
        lastMlsState = mlsState.toByteArray();
        return ok();
    }

    Result<bool, RepositoryError> commitReceive(const MessageRecord &message,
                                                const EnvelopeId &envelopeId, quint64 watermark,
                                                QByteArrayView mlsState) override
    {
        if (failReceive)
            return Result<bool, RepositoryError>::failure(error());
        if (seen.contains(envelopeId.bytes()))
            return Result<bool, RepositoryError>::success(false);
        seen.insert(envelopeId.bytes());
        received.append(message);
        if (message.attachment)
            attachmentIds.insert(message.conversationId.bytes() + message.attachment->attachmentId.bytes());
        lastMlsState = mlsState.toByteArray();
        watermarkValue = std::max(watermarkValue, watermark);
        return Result<bool, RepositoryError>::success(true);
    }

    Result<bool, RepositoryError> commitControlReceive(const EnvelopeId &envelopeId,
                                                       const DeviceId &, quint64 watermark,
                                                       QByteArrayView mlsState) override
    {
        if (failReceive)
            return Result<bool, RepositoryError>::failure(error());
        if (seen.contains(envelopeId.bytes()))
            return Result<bool, RepositoryError>::success(false);
        seen.insert(envelopeId.bytes());
        lastMlsState = mlsState.toByteArray();
        watermarkValue = std::max(watermarkValue, watermark);
        return Result<bool, RepositoryError>::success(true);
    }

    Result<HandshakeReceiveOutcome, RepositoryError>
    commitHandshakeReceive(const EnvelopeId &envelopeId, const AccountId &senderAccountId,
                           const DeviceId &senderDeviceId, const ConversationId &conversationId,
                           QByteArrayView welcome, qint64 receivedAtMs, quint64 watermark) override
    {
        ++commitHandshakeReceiveCount;
        handshakeReceiveEnvelopeId = envelopeId;
        handshakeReceiveSenderAccount = senderAccountId;
        handshakeReceiveSenderDevice = senderDeviceId;
        handshakeReceiveConversation = conversationId;
        handshakeReceiveWelcome = welcome.toByteArray();
        handshakeReceiveReceivedAtMs = receivedAtMs;
        handshakeReceiveWatermark = watermark;
        if (failHandshakeReceive)
            return Result<HandshakeReceiveOutcome, RepositoryError>::failure(error());
        return Result<HandshakeReceiveOutcome, RepositoryError>::success(handshakeReceiveOutcome);
    }

    Result<void, RepositoryError> commitHandshakeAccept(const AccountId &accountId,
                                                        const ConversationId &conversationId,
                                                        qint64 updatedAtMs,
                                                        QByteArrayView mlsState,
                                                        QByteArrayView peerSigningKey) override
    {
        ++commitHandshakeAcceptCount;
        handshakeAcceptAccount = accountId;
        handshakeAcceptConversation = conversationId;
        handshakeAcceptUpdatedAtMs = updatedAtMs;
        handshakeAcceptMlsState = mlsState.toByteArray();
        handshakeAcceptPeerKey = peerSigningKey.toByteArray();
        if (failHandshakeAccept)
            return err();
        return ok();
    }

    Result<bool, RepositoryError> hasSeen(const EnvelopeId &envelopeId) override
    {
        return Result<bool, RepositoryError>::success(!hideSeen && seen.contains(envelopeId.bytes()));
    }

    // Priority first, as the SQL store orders it: an attachment frame (1)
    // leaves only when nothing else (0) is due.
    Result<QVector<OutboxRecord>, RepositoryError> claimDue(qint64 nowMs, int limit,
                                                            qint64 leaseUntilMs) override
    {
        QVector<OutboxRecord> due;
        for (int priority = 0; priority <= 1 && due.size() < limit; ++priority) {
            for (StoredOutbox &item : outboxes) {
                if (item.record.state == OutboxState::Accepted || item.record.priority != priority)
                    continue;
                const bool leaseExpired =
                    item.record.state == OutboxState::Leased && item.record.leaseUntilMs <= nowMs;
                const bool claimable =
                    item.record.state == OutboxState::Pending || leaseExpired;
                if (!claimable || item.record.nextAttemptMs > nowMs)
                    continue;
                item.record.state = OutboxState::Leased;
                item.record.leaseUntilMs = leaseUntilMs;
                due.append(item.record);
                if (due.size() >= limit)
                    break;
            }
        }
        return Result<QVector<OutboxRecord>, RepositoryError>::success(due);
    }

    Result<void, RepositoryError> markAccepted(const EnvelopeId &envelopeId) override
    {
        for (StoredOutbox &item : outboxes)
            if (item.record.envelopeId == envelopeId)
                item.record.state = OutboxState::Accepted;
        return ok();
    }

    Result<void, RepositoryError> scheduleRetry(const EnvelopeId &envelopeId, int attemptCount,
                                                qint64 nextAttemptMs) override
    {
        for (StoredOutbox &item : outboxes) {
            if (item.record.envelopeId == envelopeId && item.record.state != OutboxState::Accepted) {
                item.record.attemptCount = attemptCount;
                item.record.nextAttemptMs = nextAttemptMs;
                item.record.state = OutboxState::Pending;
            }
        }
        return ok();
    }

    Result<void, RepositoryError> advanceDeliveryState(const MessageId &messageId,
                                                       DeliveryState state) override
    {
        deliveryStates.insert(messageId.bytes(), state);
        return ok();
    }

    // Group fan-out: one message row, every outbox row, one state — or nothing.
    Result<void, RepositoryError> commitGroupSend(const MessageRecord &message,
                                                  const QVector<OutboxRecord> &records,
                                                  QByteArrayView mlsState) override
    {
        if (failSendCommit)
            return err();
        messages.append(message);
        for (const OutboxRecord &outbox : records)
            outboxes.append(StoredOutbox{outbox});
        lastMlsState = mlsState.toByteArray();
        ++commitGroupSendCount;
        return ok();
    }

    Result<void, RepositoryError> commitControlSendMany(const QVector<OutboxRecord> &records,
                                                        QByteArrayView mlsState) override
    {
        if (failSendCommit)
            return err();
        for (const OutboxRecord &outbox : records)
            outboxes.append(StoredOutbox{outbox});
        lastMlsState = mlsState.toByteArray();
        ++commitControlSendManyCount;
        return ok();
    }

    // The SQL store's predicate for editing a sent message, over the rows here.
    MessageRecord *editableSent(const ConversationId &conversation, const MessageId &target)
    {
        for (MessageRecord &message : messages) {
            const DeliveryState state =
                deliveryStates.value(message.id.bytes(), message.deliveryState);
            if (message.id == target && message.conversationId == conversation
                && message.sharedId && message.kind == ContentKind::Text
                && (state == DeliveryState::Sent || state == DeliveryState::Delivered
                    || state == DeliveryState::Read))
                return &message;
        }
        return nullptr;
    }

    Result<bool, RepositoryError> canEditSent(const ConversationId &conversation,
                                              const MessageId &target) override
    {
        return Result<bool, RepositoryError>::success(editableSent(conversation, target) != nullptr);
    }

    Result<void, RepositoryError> commitEditSend(const ConversationId &conversation,
                                                 const MessageId &target, const QString &body,
                                                 qint64 editedAtMs,
                                                 const QVector<OutboxRecord> &records,
                                                 QByteArrayView mlsState) override
    {
        MessageRecord *message = editableSent(conversation, target);
        if (failSendCommit || message == nullptr || message->editedAtMs >= editedAtMs)
            return err();
        message->body = body;
        message->editedAtMs = editedAtMs;
        for (const OutboxRecord &outbox : records)
            outboxes.append(StoredOutbox{outbox});
        lastMlsState = mlsState.toByteArray();
        return ok();
    }

    // The SQL store's predicate for an inbound edit: a text the same device
    // sent into the same conversation under a shared id, older than the edit.
    Result<EditReceiveOutcome, RepositoryError>
    commitEditReceive(const EnvelopeId &envelopeId, const DeviceId &sender,
                      const ConversationId &conversation, const MessageId &target,
                      const QString &body, qint64 editedAtMs, quint64 watermark,
                      QByteArrayView mlsState) override
    {
        if (failReceive)
            return Result<EditReceiveOutcome, RepositoryError>::failure(error());
        if (seen.contains(envelopeId.bytes()))
            return Result<EditReceiveOutcome, RepositoryError>::success(
                EditReceiveOutcome::AlreadySeen);
        seen.insert(envelopeId.bytes());
        lastMlsState = mlsState.toByteArray();
        watermarkValue = std::max(watermarkValue, watermark);
        for (MessageRecord &message : received) {
            if (message.id == target && message.conversationId == conversation
                && message.senderDeviceId == sender && message.sharedId
                && message.kind == ContentKind::Text && message.editedAtMs < editedAtMs) {
                message.body = body;
                message.editedAtMs = editedAtMs;
                return Result<EditReceiveOutcome, RepositoryError>::success(
                    EditReceiveOutcome::Applied);
            }
        }
        return Result<EditReceiveOutcome, RepositoryError>::success(EditReceiveOutcome::Ignored);
    }

    Result<void, RepositoryError> failEnvelope(const EnvelopeId &id) override
    {
        for (auto &outbox : outboxes) {
            if (outbox.record.envelopeId == id) {
                outbox.record.state = OutboxState::Failed;
                ++failEnvelopeCount;
                return ok();
            }
        }
        return err();
    }

    Result<void, RepositoryError> commitMlsStateOnly(QByteArrayView mlsState) override
    {
        lastMlsState = mlsState.toByteArray();
        ++commitMlsStateOnlyCount;
        return ok();
    }

    Result<bool, RepositoryError> canJoinGroup(const AccountId &sender,
                                               const ConversationId &conversation) override
    {
        ++canJoinGroupCount;
        if (failCanJoinGroup)
            return Result<bool, RepositoryError>::failure(error());
        return Result<bool, RepositoryError>::success(
            acceptedAccounts.contains(sender.bytes()) && !knownConversations.contains(conversation.bytes()));
    }

    Result<bool, RepositoryError> commitGroupWelcome(const EnvelopeId &envelopeId,
                                                     const DeviceId &, const ConversationId &conversation,
                                                     quint64 watermark, qint64,
                                                     QByteArrayView mlsState, bool joined) override
    {
        if (failReceive)
            return Result<bool, RepositoryError>::failure(error());
        if (seen.contains(envelopeId.bytes()))
            return Result<bool, RepositoryError>::success(false);
        seen.insert(envelopeId.bytes());
        watermarkValue = std::max(watermarkValue, watermark);
        if (joined) {
            knownConversations.insert(conversation.bytes());
            lastMlsState = mlsState.toByteArray();
            ++groupWelcomeJoinCount;
        } else {
            ++groupWelcomeRefuseCount;
        }
        return Result<bool, RepositoryError>::success(true);
    }

    // Attachments: the message row, its outbox rows and the descriptor, in
    // one step, as commitGroupSend does; the ids used so far per conversation.
    Result<void, RepositoryError> commitAttachmentSend(const MessageRecord &message,
                                                       const QVector<OutboxRecord> &records,
                                                       const QByteArray &recipients,
                                                       QByteArrayView mlsState) override
    {
        if (failSendCommit || !message.attachment)
            return err();
        messages.append(message);
        for (const OutboxRecord &outbox : records)
            outboxes.append(StoredOutbox{outbox});
        attachmentIds.insert(message.conversationId.bytes() + message.attachment->attachmentId.bytes());
        lastAttachmentRecipients = recipients;
        lastMlsState = mlsState.toByteArray();
        ++commitAttachmentSendCount;
        return ok();
    }

    Result<bool, RepositoryError> canEnqueueAttachment(const ConversationId &conversation,
                                                       const AttachmentId &attachmentId) override
    {
        ++canEnqueueAttachmentCount;
        if (failCanEnqueueAttachment)
            return Result<bool, RepositoryError>::failure(error());
        return Result<bool, RepositoryError>::success(
            !attachmentIds.contains(conversation.bytes() + attachmentId.bytes()));
    }

    Result<int, RepositoryError> pendingLowPriorityCount() override
    {
        int pending = 0;
        for (const StoredOutbox &item : outboxes)
            pending += item.record.priority == 1 && item.record.attemptCount <= 1
                       && (item.record.state == OutboxState::Pending
                           || item.record.state == OutboxState::Leased);
        return Result<int, RepositoryError>::success(pending);
    }

    // hasSeen answers no, so a redelivery reaches the commit's replay guard.
    bool hideSeen = false;
    int commitAttachmentSendCount = 0;
    int canEnqueueAttachmentCount = 0;
    bool failCanEnqueueAttachment = false;
    QSet<QByteArray> attachmentIds;
    QByteArray lastAttachmentRecipients;

    int commitGroupSendCount = 0;
    int commitControlSendManyCount = 0;
    int failEnvelopeCount = 0;
    int commitMlsStateOnlyCount = 0;
    int canJoinGroupCount = 0;
    bool failCanJoinGroup = false;
    QSet<QByteArray> acceptedAccounts;
    QSet<QByteArray> knownConversations;
    int groupWelcomeJoinCount = 0;
    int groupWelcomeRefuseCount = 0;

    QVector<MessageRecord> messages;
    QVector<MessageRecord> received;
    QVector<StoredOutbox> outboxes;
    QSet<QByteArray> seen;
    QHash<QByteArray, DeliveryState> deliveryStates;
    QByteArray lastMlsState;
    quint64 watermarkValue = 0;
    int commitSendCount = 0;
    bool failSendCommit = false;
    bool failReceive = false;

    // commitHandshakeReceive controls + recorded args.
    HandshakeReceiveOutcome handshakeReceiveOutcome = HandshakeReceiveOutcome::Stashed;
    bool failHandshakeReceive = false;
    int commitHandshakeReceiveCount = 0;
    std::optional<EnvelopeId> handshakeReceiveEnvelopeId;
    std::optional<AccountId> handshakeReceiveSenderAccount;
    std::optional<DeviceId> handshakeReceiveSenderDevice;
    std::optional<ConversationId> handshakeReceiveConversation;
    QByteArray handshakeReceiveWelcome;
    qint64 handshakeReceiveReceivedAtMs = 0;
    quint64 handshakeReceiveWatermark = 0;

    // commitHandshakeAccept controls + recorded args.
    bool failHandshakeAccept = false;
    int commitHandshakeAcceptCount = 0;
    std::optional<AccountId> handshakeAcceptAccount;
    std::optional<ConversationId> handshakeAcceptConversation;
    qint64 handshakeAcceptUpdatedAtMs = 0;
    QByteArray handshakeAcceptMlsState;
    QByteArray handshakeAcceptPeerKey;

private:
    static RepositoryError error()
    {
        return RepositoryError{RepositoryErrorCode::Internal, QStringLiteral("fake")};
    }
    static Result<void, RepositoryError> ok() { return Result<void, RepositoryError>::success(); }
    static Result<void, RepositoryError> err()
    {
        return Result<void, RepositoryError>::failure(error());
    }
};

class FakeTransport final : public SyncTransport
{
public:
    bool isConnected() const override { return connected; }
    void sendEnvelope(const CiphertextEnvelopeV1 &envelope) override
    {
        sent.append(envelope);
        if (backlogGrowsOnSend && backlog >= 0)
            backlog += encodeCanonical(envelope).size();
    }
    void sendDatagram(const CiphertextEnvelopeV1 &envelope) override
    {
        // RelayClient's rule: a datagram is dropped while more than its
        // limit is unsent, and one admitted adds to the backlog like any
        // other write.
        if (backlogGrowsOnSend && backlog >= 0) {
            if (backlog > RelayClient::maxDatagramBacklogBytes)
                return;
            backlog += encodeCanonical(envelope).size();
        }
        datagrams.append(envelope);
    }
    void acknowledge(const EnvelopeId &envelopeId, quint64 watermark) override
    {
        acks.append({envelopeId, watermark});
    }
    qint64 pendingSendBytes() const override { return backlog; }

    bool connected = true;
    // -1 is "cannot say", what a transport without a backlog figure answers.
    qint64 backlog = -1;
    // A socket that writes nothing: what it is handed stays unsent until the
    // test empties the backlog.
    bool backlogGrowsOnSend = false;
    QVector<CiphertextEnvelopeV1> sent;
    QVector<CiphertextEnvelopeV1> datagrams;
    QVector<std::pair<EnvelopeId, quint64>> acks;
};

CiphertextEnvelopeV1 incomingEnvelope(const ConversationId &conversation, const DeviceId &sender,
                                      const QByteArray &ciphertext)
{
    return CiphertextEnvelopeV1{
        1,
        EnvelopeId::generate(),
        AccountId::generate(),
        sender,
        DeviceId::generate(),
        conversation,
        EnvelopeMessageKind::MlsPrivateMessage,
        1'700'000'000'000,
        1'700'000'060'000,
        EnvelopeId::generate(),
        ciphertext,
        QByteArray(32, '\x02'),
        QByteArray(64, '\x03')};
}

// An inbound contact-handshake envelope (messageKind MlsHandshake) whose
// ciphertext IS the raw Welcome. expiresAtMs sits after the test clock so the
// expiry check does not fire.
CiphertextEnvelopeV1 incomingHandshakeEnvelope(const ConversationId &conversation,
                                               const AccountId &senderAccount,
                                               const DeviceId &senderDevice,
                                               const QByteArray &welcome)
{
    return CiphertextEnvelopeV1{
        1,
        EnvelopeId::generate(),
        senderAccount,
        senderDevice,
        DeviceId::generate(),
        conversation,
        EnvelopeMessageKind::MlsHandshake,
        1'700'000'000'000,
        1'700'000'060'000,
        EnvelopeId::generate(),
        welcome,
        QByteArray(32, '\x02'),
        QByteArray(64, '\x03')};
}

// Every messageEdited the engine emits. Ids have no default value, so
// QSignalSpy cannot hand them back through QVariant.
struct EditLog final {
    struct Entry final {
        ConversationId conversation;
        MessageId id;
        QString body;
        qint64 editedAtMs = 0;
    };
    explicit EditLog(SyncEngine &engine)
    {
        QObject::connect(&engine, &SyncEngine::messageEdited, &engine,
                         [this](const ConversationId &conversation, const MessageId &id,
                                const QString &body, qint64 editedAtMs) {
                             entries.append(Entry{conversation, id, body, editedAtMs});
                         });
    }
    QVector<Entry> entries;
};

// A photo's descriptor as the sender stages it.
AttachmentDescriptor photoDescriptor()
{
    AttachmentDescriptor descriptor;
    descriptor.key = QByteArray(AttachmentLimits::keyBytes, 'k');
    descriptor.kind = AttachmentKind::Image;
    descriptor.byteCount = 300'000;
    descriptor.sha256 = QByteArray(32, 's');
    descriptor.partCount = attachmentPartCount(descriptor.byteCount);
    descriptor.mimeType = QStringLiteral("image/jpeg");
    descriptor.fileName = QStringLiteral("Harbour.jpg");
    descriptor.width = 1600;
    descriptor.height = 1200;
    descriptor.hasPreview = true;
    return descriptor;
}

// A frame as the attachment layer seals it; the engine only ever looks at
// its clear header.
QByteArray partFrame(const AttachmentId &attachment = AttachmentId::generate(), quint32 index = 0)
{
    return attachmentFrameHeader(AttachmentFrameType::Part, attachment, index) + QByteArray(64, 'p');
}

CiphertextEnvelopeV1 incomingFrame(const ConversationId &conversation, const DeviceId &sender,
                                   const QByteArray &frame)
{
    CiphertextEnvelopeV1 envelope = incomingEnvelope(conversation, sender, frame);
    envelope.messageKind = EnvelopeMessageKind::AttachmentControl;
    return envelope;
}

// Every attachmentFrameReceived the engine emits (ids have no default value,
// so QSignalSpy cannot hand them back through QVariant).
struct FrameLog final {
    struct Entry final {
        ConversationId conversation;
        DeviceId sender;
        QByteArray frame;
    };
    explicit FrameLog(SyncEngine &engine)
    {
        QObject::connect(&engine, &SyncEngine::attachmentFrameReceived, &engine,
                         [this](const ConversationId &conversation, const DeviceId &sender,
                                const QByteArray &frame) {
                             entries.append(Entry{conversation, sender, frame});
                         });
    }
    QVector<Entry> entries;
};

SyncEngine::Config makeConfig(int maxAttempts = 8)
{
    return SyncEngine::Config{AccountId::generate(), DeviceId::generate(), maxAttempts, 32, 30'000};
}

SyncEngine::Signer okSigner()
{
    return [](QByteArrayView) { return QByteArray(64, 'S'); };
}

} // namespace

class SyncEngineTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { qRegisterMetaType<DeliveryState>(); }
    // Every test starts from the same clock: some move it on by days, and the
    // fixed incoming envelopes expire a minute after it.
    void init() { m_now = 1'700'000'000'000; }

    void sendEncryptsPersistsAndReportsQueued();
    void relayAcceptanceMarksSent();
    void recipientUnavailableStopsRetries()
    {
        FakeStore store;
        FakeMls mls;
        FakeTransport transport;
        SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
        engine.start();
        engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("hello"));
        QCOMPARE(transport.sent.size(), 1);
        transport.onRecipientUnavailable(transport.sent.first().envelopeId);
        QCOMPARE(store.deliveryStates.value(store.messages.first().id.bytes()), DeliveryState::Failed);
        transport.onConnected();
        QCOMPARE(transport.sent.size(), 1);
    }
    void timerRetriesWithoutAnotherSendOrReconnect()
    {
        FakeStore store;
        FakeMls mls;
        FakeTransport transport;
        qint64 now = 1000;
        SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), [&] { return now; });
        engine.start();
        engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("hello"));
        now += 2000;
        QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 2, 2500);
        QCOMPARE(mls.encryptCount, 1);
        QCOMPARE(transport.sent.first().ciphertext, transport.sent.last().ciphertext);
    }
    void offlineSendQueuesAndLeavesOnReconnect();
    void offlineSendSurvivesEngineRestart();
    void offlineGroupSendQueuesForEveryMember();
    void conversationEnvelopesOutliveCallSignals();
    void sendQueuedPastItsExpiryFailsWithoutSending();
    void neverReEncryptsOnResend();
    void duplicateIncomingIsAckedNotReprocessed();
    void staleMessageIsDroppedWithoutAckOrCallback();
    void forgedSenderIsDroppedNotAttributed();
    void expiredIncomingIsAckedNotProcessed();
    void retryExhaustionMarksFailed();
    void commitFailureFailsClosed();
    void sendHandshakeShipsWelcomeWithoutEncrypting();
    void sendHandshakeAcceptanceCancelsRetry();
    void sendHandshakeCommitFailureFailsClosed();
    void inboundHandshakeStashedAcksAndSurfacesWithoutProcess();
    void inboundHandshakeAlreadySeenAckedNotSurfaced();
    void inboundHandshakeDroppedBlockedAckedNotSurfaced();
    void expiredHandshakeAckedNotStashed();
    void handshakeReceiveCommitFailureFailsClosedNoAck();
    void acceptHandshakeAuthenticatesJoinsAndCommits();
    void acceptHandshakeRejectsMismatchedCredential();
    void acceptHandshakeCommitFailureFailsClosed();
    void sendEmitsQueuedRowBeforeRelayAcceptance();
    void contactAcceptIsControlSentAndSurfacedOnReceive();
    void callSignalIsControlSentAndSurfacedOnReceive();
    void profileUpdateIsControlSentAndSurfacedOnReceive();
    void callMediaBypassesTheStoreAndTheRatchet();
    void inboundDatagramsAreSurfacedWithoutTouchingTheStore();
    void groupTextIsEncryptedOnceAndFannedOutUnderOneMessage();
    void groupTextFailsOnlyWhenNoRecipientTookIt();
    void groupControlAndChangesFanOut();
    void groupWelcomeFromAcceptedContactIsJoinedAndSurfaced();
    void groupWelcomeFromStrangerOrForKnownGroupIsConsumedNotJoined();
    void groupWelcomeNotNamingTheSenderIsRefused();
    void groupControlIsSurfacedOnReceive();
    void textsAreFiledUnderTheirCiphertextOnBothEnds();
    void replyCarriesItsQuoteBothWays();
    void editChangesASentMessageAndTellsEveryRecipient();
    void editOfAQueuedOrUnknownMessageDoesNothing();
    void inboundEditAppliesOnlyToTheSendersNewerEdit();
    void unreadableTaggedMessageIsConsumedWithoutARow();
    void drainPausesWhileTheLinkIsBacklogged();
    void drainResumesInOrder();
    void callMediaAloneNeverHoldsTheDrainBack();
    void unknownBacklogNeverGates();
    void retryOfALargeEnvelopeWaitsForItsUpload();
    void linkUpIsSignalled();
    void attachmentIsEncryptedOnceAndStoredWithItsDescriptor();
    void groupAttachmentFansOutUnderOneMessage();
    void attachmentRefusalsNeverEncryptAndNeverFailClosed();
    void attachmentFramesBypassTheRatchet();
    void attachmentFrameRefusalsQueueNothing();
    void conversationTrafficLeavesBeforeWaitingFrames();
    void inboundFramesBypassMlsStateAndAreSignalledOnce();
    void replayedFramesAreNotReSignalled();
    void malformedFramesAreConsumedSilently();
    void aFrameTheStoreCannotTakeNeverFailsClosed();
    void attachmentMessageArrivesWithItsDescriptorAndQuote();
    void attachmentFromAnUnexpectedCredentialIsDroppedLikeText();

private:
    qint64 m_now = 1'700'000'000'000;
    SyncEngine::Clock clock() { return [this] { return m_now; }; }
};

void SyncEngineTest::sendEncryptsPersistsAndReportsQueued()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy stateSpy(&engine, &SyncEngine::messageStateChanged);
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    engine.enqueueText(conversation, DeviceId::generate(), QStringLiteral("hello"));

    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.messages.size(), 1);
    QCOMPARE(store.outboxes.size(), 1);
    QCOMPARE(store.lastMlsState, QByteArray("state-1"));
    QVERIFY(stateSpy.count() >= 1);
    QCOMPARE(stateSpy.first().at(1).value<DeliveryState>(), DeliveryState::Queued);
    QCOMPARE(transport.sent.size(), 1); // drained while connected
}

void SyncEngineTest::relayAcceptanceMarksSent()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy stateSpy(&engine, &SyncEngine::messageStateChanged);
    engine.start();

    engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("hi"));
    QCOMPARE(transport.sent.size(), 1);
    const EnvelopeId envelopeId = transport.sent.first().envelopeId;
    const MessageId messageId = store.messages.first().id;

    transport.onRelayAccepted(envelopeId, 5);

    QCOMPARE(store.deliveryStates.value(messageId.bytes()), DeliveryState::Sent);
    bool sawSent = false;
    for (const auto &args : stateSpy)
        sawSent = sawSent || args.at(1).value<DeliveryState>() == DeliveryState::Sent;
    QVERIFY(sawSent);
}

void SyncEngineTest::offlineSendQueuesAndLeavesOnReconnect()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    transport.connected = false;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy stateSpy(&engine, &SyncEngine::messageStateChanged);
    engine.start();

    engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("later"));
    QCOMPARE(store.outboxes.size(), 1);
    QCOMPARE(store.messages.first().deliveryState, DeliveryState::Queued);
    QCOMPARE(store.outboxes.first().record.state, OutboxState::Pending);
    QCOMPARE(stateSpy.first().at(1).value<DeliveryState>(), DeliveryState::Queued);
    QCOMPARE(transport.sent.size(), 0); // nothing leaves while offline

    // The link comes back: the queued send leaves without a manual retry, as
    // the ciphertext made offline, and the relay's acceptance marks it Sent.
    transport.connected = true;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(mls.encryptCount, 1);
    transport.onRelayAccepted(transport.sent.first().envelopeId, 1);
    QCOMPARE(store.deliveryStates.value(store.messages.first().id.bytes()), DeliveryState::Sent);
}

void SyncEngineTest::offlineSendSurvivesEngineRestart()
{
    m_now = 1'700'000'000'000;
    FakeStore store; // shared durable store across engine instances
    FakeMls mls;
    FakeTransport transport;
    transport.connected = false;

    {
        SyncEngine first(makeConfig(), store, mls, transport, okSigner(), clock());
        first.start();
        first.enqueueText(ConversationId::generate(), DeviceId::generate(),
                          QStringLiteral("durable"));
        first.stop();
    }
    QCOMPARE(store.outboxes.size(), 1);
    QCOMPARE(transport.sent.size(), 0);

    // The app was closed before the message left and is opened days later,
    // online: the stored envelope goes out as it is, never re-encrypted.
    m_now += 3LL * 24 * 60 * 60 * 1000;
    transport.connected = true;
    SyncEngine second(makeConfig(), store, mls, transport, okSigner(), clock());
    second.start();
    QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(mls.encryptCount, 1);
    transport.onRelayAccepted(transport.sent.first().envelopeId, 1);
    QCOMPARE(store.deliveryStates.value(store.messages.first().id.bytes()), DeliveryState::Sent);
}

void SyncEngineTest::offlineGroupSendQueuesForEveryMember()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    transport.connected = false;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    engine.enqueueGroupText(ConversationId::generate(),
                            {DeviceId::generate(), DeviceId::generate()},
                            QStringLiteral("see you all later"));
    QCOMPARE(store.messages.first().deliveryState, DeliveryState::Queued);
    QCOMPARE(store.outboxes.size(), 2);
    for (const StoredOutbox &outbox : std::as_const(store.outboxes))
        QCOMPARE(outbox.record.state, OutboxState::Pending);
    QCOMPARE(transport.sent.size(), 0);

    transport.connected = true;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 2);
    transport.onRelayAccepted(transport.sent.first().envelopeId, 1);
    QCOMPARE(store.deliveryStates.value(store.messages.first().id.bytes()), DeliveryState::Sent);
}

void SyncEngineTest::conversationEnvelopesOutliveCallSignals()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    // The relay holds an envelope for an offline recipient until it expires, so
    // a message may wait as long as the wire allows; a call offer must not ring
    // a device that reappears days after the call.
    const ConversationId conversation = ConversationId::generate();
    engine.enqueueText(conversation, DeviceId::generate(), QStringLiteral("whenever"));
    engine.sendCallSignal(conversation, DeviceId::generate(), QByteArray("OFFER"));
    QCOMPARE(transport.sent.size(), 2);
    const auto lifetime = [](const CiphertextEnvelopeV1 &envelope) {
        return envelope.expiresAtMs - envelope.createdAtMs;
    };
    QCOMPARE(lifetime(transport.sent.at(0)), maxEnvelopeLifetimeMs);
    QCOMPARE(lifetime(transport.sent.at(1)), 24LL * 60 * 60 * 1000);
    // Both are still valid wire envelopes.
    for (const CiphertextEnvelopeV1 &envelope : std::as_const(transport.sent))
        QVERIFY(decodeEnvelope(encodeCanonical(envelope)).hasValue());
}

void SyncEngineTest::sendQueuedPastItsExpiryFailsWithoutSending()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    transport.connected = false;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    engine.enqueueText(conversation, DeviceId::generate(), QStringLiteral("two days late"));
    engine.sendCallSignal(conversation, DeviceId::generate(), QByteArray("OFFER"));
    engine.enqueueText(conversation, DeviceId::generate(), QStringLiteral("a month late"));
    QCOMPARE(store.outboxes.size(), 3);

    // Two days offline: the call offer has expired and is dropped unsent, the
    // messages still go.
    m_now += 2LL * 24 * 60 * 60 * 1000;
    transport.connected = true;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 2);
    for (const CiphertextEnvelopeV1 &envelope : std::as_const(transport.sent))
        QCOMPARE(envelope.messageKind, EnvelopeMessageKind::MlsPrivateMessage);
    QCOMPARE(store.outboxes.at(1).record.state, OutboxState::Failed);
    transport.onRelayAccepted(transport.sent.at(0).envelopeId, 1);

    // A queued message the relay would refuse as expired fails at once rather
    // than being retried until the attempts run out.
    m_now += maxEnvelopeLifetimeMs;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 2);
    QCOMPARE(store.deliveryStates.value(store.messages.at(1).id.bytes()), DeliveryState::Failed);
    QCOMPARE(store.deliveryStates.value(store.messages.at(0).id.bytes()), DeliveryState::Sent);
}

void SyncEngineTest::neverReEncryptsOnResend()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("once"));
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(transport.sent.size(), 1);

    // Not accepted: advance past the backoff and drain again (resend).
    m_now += 5'000;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 2);
    QCOMPARE(mls.encryptCount, 1); // resend used the persisted envelope, no re-encrypt
}

void SyncEngineTest::duplicateIncomingIsAckedNotReprocessed()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy receivedSpy(&engine, &SyncEngine::messageReceived);
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    // Envelope's claimed sender matches the MLS-authenticated credential.
    const CiphertextEnvelopeV1 envelope =
        incomingEnvelope(conversation, mls.senderDevice, QByteArray("ENC:world"));

    engine.handleEnvelope(envelope, 9);
    QCOMPARE(receivedSpy.count(), 1);
    QCOMPARE(store.received.size(), 1);
    QCOMPARE(store.received.first().body, QStringLiteral("world"));
    QCOMPARE(store.watermarkValue, quint64(9));
    QCOMPARE(transport.acks.size(), 1);
    QCOMPARE(mls.processCount, 1);

    // Redeliver the same envelope: acked again, never reprocessed.
    engine.handleEnvelope(envelope, 9);
    QCOMPARE(receivedSpy.count(), 1);   // no second message
    QCOMPARE(mls.processCount, 1);      // ratchet not touched again
    QCOMPARE(transport.acks.size(), 2); // but still acknowledged
}

void SyncEngineTest::staleMessageIsDroppedWithoutAckOrCallback()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy receivedSpy(&engine, &SyncEngine::messageReceived);
    engine.start();

    // Payload without the "ENC:" prefix -> process() reports invalid/stale.
    const CiphertextEnvelopeV1 envelope =
        incomingEnvelope(ConversationId::generate(), DeviceId::generate(), QByteArray("garbage"));
    engine.handleEnvelope(envelope, 3);

    QCOMPARE(receivedSpy.count(), 0);
    QCOMPARE(store.received.size(), 0);
    QCOMPARE(transport.acks.size(), 0); // not acknowledged; nothing durably applied
}

void SyncEngineTest::forgedSenderIsDroppedNotAttributed()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy receivedSpy(&engine, &SyncEngine::messageReceived);
    engine.start();

    // The relay claims a different sender device than the MLS credential names.
    const CiphertextEnvelopeV1 envelope = incomingEnvelope(
        ConversationId::generate(), DeviceId::generate(), QByteArray("ENC:forged"));
    QVERIFY(envelope.senderDeviceId != mls.senderDevice);

    engine.handleEnvelope(envelope, 7);

    QCOMPARE(receivedSpy.count(), 0);   // never surfaced under a forged sender
    QCOMPARE(store.received.size(), 0); // nothing durably applied
    QCOMPARE(transport.acks.size(), 0); // and not acknowledged
}

void SyncEngineTest::expiredIncomingIsAckedNotProcessed()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy receivedSpy(&engine, &SyncEngine::messageReceived);
    engine.start();

    const CiphertextEnvelopeV1 envelope =
        incomingEnvelope(ConversationId::generate(), DeviceId::generate(), QByteArray("ENC:old"));
    m_now = envelope.expiresAtMs + 1; // withheld past expiry, then (re)delivered

    engine.handleEnvelope(envelope, 12);

    QCOMPARE(receivedSpy.count(), 0);
    QCOMPARE(store.received.size(), 0);
    QCOMPARE(mls.processCount, 0);      // ratchet never touched for an expired envelope
    QCOMPARE(transport.acks.size(), 1); // acknowledged so the relay stops redelivering
}

void SyncEngineTest::retryExhaustionMarksFailed()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(3), store, mls, transport, okSigner(), clock());
    QSignalSpy stateSpy(&engine, &SyncEngine::messageStateChanged);
    engine.start();

    engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("retry"));
    const MessageId messageId = store.messages.first().id;

    // Drive drains past the backoff without any acceptance until exhausted.
    for (int i = 0; i < 6; ++i) {
        m_now += 600'000; // beyond the capped backoff
        transport.onConnected();
    }

    QCOMPARE(store.deliveryStates.value(messageId.bytes()), DeliveryState::Failed);
    QCOMPARE(transport.sent.size(), 3); // exactly maxSendAttempts sends, then failed
    bool sawFailed = false;
    for (const auto &args : stateSpy)
        sawFailed = sawFailed || args.at(1).value<DeliveryState>() == DeliveryState::Failed;
    QVERIFY(sawFailed);
}

void SyncEngineTest::commitFailureFailsClosed()
{
    FakeStore store;
    store.failSendCommit = true;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy failedSpy(&engine, &SyncEngine::failedClosed);
    QSignalSpy stateSpy(&engine, &SyncEngine::messageStateChanged);
    engine.start();

    engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("boom"));

    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(engine.isFailedClosed());
    QCOMPARE(stateSpy.count(), 0);      // never reported Queued
    QCOMPARE(transport.sent.size(), 0); // nothing sent

    // Further operations are inert once failed closed.
    engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("again"));
    QCOMPARE(failedSpy.count(), 1);
}

void SyncEngineTest::sendHandshakeShipsWelcomeWithoutEncrypting()
{
    FakeStore store;
    FakeMls mls;
    // A pending MLS snapshot the caller captured out-of-band via createGroup +
    // addMembers on the shared client; sendHandshake must surrender and commit it.
    mls.stateVersion = 5;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy stateSpy(&engine, &SyncEngine::messageStateChanged);
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const DeviceId recipient = DeviceId::generate();
    const QByteArray welcome("mls-welcome-bytes");
    engine.sendHandshake(conversation, recipient, welcome);

    // The ciphertext is the raw Welcome: mls.encrypt is never called.
    QCOMPARE(mls.encryptCount, 0);
    // Committed as a control send (outbox row, no visible message row) carrying the
    // pending MLS state the engine took from takePendingState().
    QCOMPARE(store.outboxes.size(), 1);
    QCOMPARE(store.messages.size(), 0);
    QCOMPARE(store.lastMlsState, QByteArray("state-5"));
    QCOMPARE(stateSpy.count(), 0); // no Queued state reported for a control send

    // Drained while connected: exactly one MlsHandshake envelope whose ciphertext
    // is the Welcome verbatim and whose signature is present.
    QCOMPARE(transport.sent.size(), 1);
    const CiphertextEnvelopeV1 &sent = transport.sent.first();
    QCOMPARE(sent.messageKind, EnvelopeMessageKind::MlsHandshake);
    QCOMPARE(sent.ciphertext, welcome);
    QCOMPARE(sent.recipientDeviceId, recipient);
    QCOMPARE(sent.conversationId, conversation);
    QCOMPARE(sent.senderSignature, QByteArray(64, 'S'));
}

void SyncEngineTest::sendHandshakeAcceptanceCancelsRetry()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    engine.sendHandshake(ConversationId::generate(), DeviceId::generate(),
                         QByteArray("welcome"));
    QCOMPARE(transport.sent.size(), 1);
    const EnvelopeId envelopeId = transport.sent.first().envelopeId;

    // Relay acceptance marks the outbox Accepted, cancelling any scheduled retry.
    transport.onRelayAccepted(envelopeId, 5);

    // Advance well past the backoff and drain again: nothing is resent.
    m_now += 600'000;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(mls.encryptCount, 0); // never encrypts on the handshake path
}

void SyncEngineTest::sendHandshakeCommitFailureFailsClosed()
{
    FakeStore store;
    store.failSendCommit = true;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy failedSpy(&engine, &SyncEngine::failedClosed);
    engine.start();

    engine.sendHandshake(ConversationId::generate(), DeviceId::generate(),
                         QByteArray("welcome"));

    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(engine.isFailedClosed());
    QCOMPARE(transport.sent.size(), 0); // nothing left the device
}

void SyncEngineTest::inboundHandshakeStashedAcksAndSurfacesWithoutProcess()
{
    m_now = 1'700'000'000'000; // reset: earlier slots advance the shared clock
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());

    int received = 0;
    std::optional<AccountId> sawSender;
    std::optional<DeviceId> sawDevice;
    std::optional<ConversationId> sawConversation;
    qint64 sawReceivedAt = 0;
    connect(&engine, &SyncEngine::handshakeReceived, &engine,
            [&](const AccountId &s, const DeviceId &d, const ConversationId &c, qint64 at) {
                ++received;
                sawSender = s;
                sawDevice = d;
                sawConversation = c;
                sawReceivedAt = at;
            });
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const AccountId senderAccount = AccountId::generate();
    const DeviceId senderDevice = DeviceId::generate();
    const QByteArray welcome("welcome-bytes");
    const CiphertextEnvelopeV1 envelope =
        incomingHandshakeEnvelope(conversation, senderAccount, senderDevice, welcome);

    engine.handleEnvelope(envelope, 11);

    // Stashed via commitHandshakeReceive with exactly the envelope's fields.
    QCOMPARE(store.commitHandshakeReceiveCount, 1);
    QVERIFY(store.handshakeReceiveEnvelopeId.has_value());
    QCOMPARE(store.handshakeReceiveEnvelopeId->bytes(), envelope.envelopeId.bytes());
    QCOMPARE(store.handshakeReceiveSenderAccount->bytes(), senderAccount.bytes());
    QCOMPARE(store.handshakeReceiveSenderDevice->bytes(), senderDevice.bytes());
    QCOMPARE(store.handshakeReceiveConversation->bytes(), conversation.bytes());
    QCOMPARE(store.handshakeReceiveWelcome, welcome);
    QCOMPARE(store.handshakeReceiveReceivedAtMs, envelope.createdAtMs);
    QCOMPARE(store.handshakeReceiveWatermark, quint64(11));

    // Acknowledged only after the durable stash, at the relay sequence.
    QCOMPARE(transport.acks.size(), 1);
    QCOMPARE(transport.acks.first().second, quint64(11));

    // Surfaced exactly once with the envelope's sender / device / conversation / time.
    QCOMPARE(received, 1);
    QCOMPARE(sawSender->bytes(), senderAccount.bytes());
    QCOMPARE(sawDevice->bytes(), senderDevice.bytes());
    QCOMPARE(sawConversation->bytes(), conversation.bytes());
    QCOMPARE(sawReceivedAt, envelope.createdAtMs);

    // A handshake is NEVER fed to mls.process: a Welcome names an unjoined group.
    QCOMPARE(mls.processCount, 0);
}

void SyncEngineTest::inboundHandshakeAlreadySeenAckedNotSurfaced()
{
    m_now = 1'700'000'000'000; // reset: earlier slots advance the shared clock
    FakeStore store;
    store.handshakeReceiveOutcome = HandshakeReceiveOutcome::AlreadySeen;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    int received = 0;
    connect(&engine, &SyncEngine::handshakeReceived, &engine,
            [&](const AccountId &, const DeviceId &, const ConversationId &, qint64) { ++received; });
    engine.start();

    const CiphertextEnvelopeV1 envelope = incomingHandshakeEnvelope(
        ConversationId::generate(), AccountId::generate(), DeviceId::generate(),
        QByteArray("welcome"));
    engine.handleEnvelope(envelope, 4);

    QCOMPARE(store.commitHandshakeReceiveCount, 1);
    QCOMPARE(transport.acks.size(), 1); // consumed (idempotent redelivery)
    QCOMPARE(received, 0);              // but nothing surfaced
    QCOMPARE(mls.processCount, 0);
}

void SyncEngineTest::inboundHandshakeDroppedBlockedAckedNotSurfaced()
{
    m_now = 1'700'000'000'000; // reset: earlier slots advance the shared clock
    FakeStore store;
    store.handshakeReceiveOutcome = HandshakeReceiveOutcome::DroppedBlocked;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    int received = 0;
    connect(&engine, &SyncEngine::handshakeReceived, &engine,
            [&](const AccountId &, const DeviceId &, const ConversationId &, qint64) { ++received; });
    engine.start();

    const CiphertextEnvelopeV1 envelope = incomingHandshakeEnvelope(
        ConversationId::generate(), AccountId::generate(), DeviceId::generate(),
        QByteArray("welcome"));
    engine.handleEnvelope(envelope, 5);

    QCOMPARE(store.commitHandshakeReceiveCount, 1);
    QCOMPARE(transport.acks.size(), 1); // still consumed so the relay stops redelivering
    QCOMPARE(received, 0);              // a blocked sender surfaces nothing
    QCOMPARE(mls.processCount, 0);
}

void SyncEngineTest::expiredHandshakeAckedNotStashed()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    int received = 0;
    connect(&engine, &SyncEngine::handshakeReceived, &engine,
            [&](const AccountId &, const DeviceId &, const ConversationId &, qint64) { ++received; });
    engine.start();

    const CiphertextEnvelopeV1 envelope = incomingHandshakeEnvelope(
        ConversationId::generate(), AccountId::generate(), DeviceId::generate(),
        QByteArray("welcome"));
    m_now = envelope.expiresAtMs + 1; // withheld past expiry, then (re)delivered

    engine.handleEnvelope(envelope, 8);

    QCOMPARE(store.commitHandshakeReceiveCount, 0); // never stashed
    QCOMPARE(received, 0);
    QCOMPARE(transport.acks.size(), 1); // acknowledged so the relay stops redelivering
}

void SyncEngineTest::handshakeReceiveCommitFailureFailsClosedNoAck()
{
    m_now = 1'700'000'000'000; // reset: earlier slots advance the shared clock
    FakeStore store;
    store.failHandshakeReceive = true;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy failedSpy(&engine, &SyncEngine::failedClosed);
    int received = 0;
    connect(&engine, &SyncEngine::handshakeReceived, &engine,
            [&](const AccountId &, const DeviceId &, const ConversationId &, qint64) { ++received; });
    engine.start();

    const CiphertextEnvelopeV1 envelope = incomingHandshakeEnvelope(
        ConversationId::generate(), AccountId::generate(), DeviceId::generate(),
        QByteArray("welcome"));
    engine.handleEnvelope(envelope, 6);

    QCOMPARE(store.commitHandshakeReceiveCount, 1);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(engine.isFailedClosed());
    QCOMPARE(transport.acks.size(), 0); // fail closed: NOT acked -> relay redelivers
    QCOMPARE(received, 0);
}

void SyncEngineTest::acceptHandshakeAuthenticatesJoinsAndCommits()
{
    FakeStore store;
    FakeMls mls;
    mls.stateVersion = 7; // a pending snapshot to surrender after the join
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    std::optional<ConversationId> accepted;
    std::optional<AccountId> acceptedSender;
    int authFailed = 0;
    connect(&engine, &SyncEngine::handshakeAccepted, &engine,
            [&](const ConversationId &c, const AccountId &s) {
                accepted = c;
                acceptedSender = s;
            });
    connect(&engine, &SyncEngine::handshakeAuthFailed, &engine,
            [&](const ConversationId &, const AccountId &) { ++authFailed; });
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const AccountId senderAccount = AccountId::generate();
    const DeviceId claimedDevice = DeviceId::generate();
    const QByteArray welcome("real-welcome");
    // inspectWelcome returns exactly one credential naming the claimed device.
    mls.inspectMembers = {credentialBytes(claimedDevice)};

    engine.acceptHandshake(conversation, senderAccount, claimedDevice, welcome);

    // Authenticated read-only, then joined, then committed the post-join state.
    QCOMPARE(mls.inspectWelcomeCount, 1);
    QCOMPARE(mls.lastInspectedWelcome, welcome);
    QCOMPARE(mls.joinGroupCount, 1);
    QCOMPARE(mls.lastJoinedWelcome, welcome);
    QCOMPARE(store.commitHandshakeAcceptCount, 1);
    QCOMPARE(store.handshakeAcceptAccount->bytes(), senderAccount.bytes());
    QCOMPARE(store.handshakeAcceptConversation->bytes(), conversation.bytes());
    QCOMPARE(store.handshakeAcceptMlsState, QByteArray("state-8")); // join advanced the ratchet
    // The engine forwarded the authenticated front-member credential's identity
    // bytes [17,49) as the peer signing key.
    QCOMPARE(store.handshakeAcceptPeerKey, credentialBytes(claimedDevice).sliced(17, 32));
    QCOMPARE(authFailed, 0);
    QVERIFY(accepted.has_value());
    QCOMPARE(accepted->bytes(), conversation.bytes());
    QCOMPARE(acceptedSender->bytes(), senderAccount.bytes());
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::acceptHandshakeRejectsMismatchedCredential()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    std::optional<ConversationId> authFailedConversation;
    std::optional<AccountId> authFailedSender;
    int accepted = 0;
    connect(&engine, &SyncEngine::handshakeAccepted, &engine,
            [&](const ConversationId &, const AccountId &) { ++accepted; });
    connect(&engine, &SyncEngine::handshakeAuthFailed, &engine,
            [&](const ConversationId &c, const AccountId &s) {
                authFailedConversation = c;
                authFailedSender = s;
            });
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const AccountId senderAccount = AccountId::generate();
    const DeviceId claimedDevice = DeviceId::generate();
    const DeviceId otherDevice = DeviceId::generate();
    QVERIFY(claimedDevice != otherDevice);
    // The Welcome's sole member names a DIFFERENT device than the relay claims.
    mls.inspectMembers = {credentialBytes(otherDevice)};

    engine.acceptHandshake(conversation, senderAccount, claimedDevice, QByteArray("welcome"));

    QCOMPARE(mls.inspectWelcomeCount, 1);
    QCOMPARE(mls.joinGroupCount, 0);               // NEVER joined -- auth ran first
    QCOMPARE(store.commitHandshakeAcceptCount, 0); // NEVER committed
    QCOMPARE(accepted, 0);
    QVERIFY(authFailedConversation.has_value());
    QCOMPARE(authFailedConversation->bytes(), conversation.bytes());
    QCOMPARE(authFailedSender->bytes(), senderAccount.bytes());
    QVERIFY(!engine.isFailedClosed()); // an auth failure is not a fail-closed

    // A membership size != 1 is likewise rejected without any join or commit.
    mls.inspectMembers = {credentialBytes(claimedDevice), credentialBytes(otherDevice)};
    engine.acceptHandshake(conversation, senderAccount, claimedDevice, QByteArray("welcome2"));
    QCOMPARE(mls.joinGroupCount, 0);
    QCOMPARE(store.commitHandshakeAcceptCount, 0);
}

void SyncEngineTest::acceptHandshakeCommitFailureFailsClosed()
{
    FakeStore store;
    store.failHandshakeAccept = true;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy failedSpy(&engine, &SyncEngine::failedClosed);
    int accepted = 0;
    int authFailed = 0;
    connect(&engine, &SyncEngine::handshakeAccepted, &engine,
            [&](const ConversationId &, const AccountId &) { ++accepted; });
    connect(&engine, &SyncEngine::handshakeAuthFailed, &engine,
            [&](const ConversationId &, const AccountId &) { ++authFailed; });
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const DeviceId claimedDevice = DeviceId::generate();
    mls.inspectMembers = {credentialBytes(claimedDevice)};

    engine.acceptHandshake(conversation, AccountId::generate(), claimedDevice,
                           QByteArray("welcome"));

    // Auth passed and the group was joined, but the atomic accept commit failed:
    // fail closed so the just-joined ratchet is discarded on restart.
    QCOMPARE(mls.joinGroupCount, 1);
    QCOMPARE(store.commitHandshakeAcceptCount, 1);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(engine.isFailedClosed());
    QCOMPARE(accepted, 0);
    QCOMPARE(authFailed, 0);
}

void SyncEngineTest::sendEmitsQueuedRowBeforeRelayAcceptance()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    std::optional<MessageRecord> queued;
    connect(&engine, &SyncEngine::messageQueued, &engine,
            [&](const MessageRecord &record) { queued = record; });
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    engine.enqueueText(conversation, DeviceId::generate(), QStringLiteral("hello"));

    // The queued row is exactly the committed store row, so a UI can render it
    // (with its stable id for later state updates) before the relay accepts.
    QVERIFY(queued.has_value());
    QCOMPARE(store.messages.size(), 1);
    QCOMPARE(queued->id.bytes(), store.messages.first().id.bytes());
    QCOMPARE(queued->body, QStringLiteral("hello"));
    QCOMPARE(queued->flow, MessageFlow::Outgoing);
    QCOMPARE(queued->deliveryState, DeliveryState::Queued);
    QCOMPARE(queued->conversationId.bytes(), conversation.bytes());
}

void SyncEngineTest::contactAcceptIsControlSentAndSurfacedOnReceive()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    std::optional<ConversationId> acceptedConversation;
    std::optional<DeviceId> acceptedDevice;
    int visibleMessages = 0;
    connect(&engine, &SyncEngine::contactAcceptReceived, &engine,
            [&](const ConversationId &c, const DeviceId &d) {
                acceptedConversation = c;
                acceptedDevice = d;
            });
    connect(&engine, &SyncEngine::messageReceived, &engine,
            [&](const MessageRecord &) { ++visibleMessages; });
    engine.start();

    // --- Send: encrypted under the group ratchet, shipped as ContactAccept with
    //     no visible message row. ---
    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    engine.sendContactAccept(conversation, peer);
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.messages.size(), 0); // control send: no message row
    QCOMPARE(store.outboxes.size(), 1);
    QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(transport.sent.first().messageKind, EnvelopeMessageKind::ContactAccept);
    QCOMPARE(transport.sent.first().recipientDeviceId.bytes(), peer.bytes());
    QCOMPARE(transport.sent.first().conversationId.bytes(), conversation.bytes());
    QVERIFY(transport.sent.first().ciphertext.startsWith("ENC:"));

    // --- Receive: an authenticated ContactAccept is consumed as a control
    //     receive (acked, replay-guarded, never a message) and surfaced typed. ---
    CiphertextEnvelopeV1 inbound =
        incomingEnvelope(conversation, mls.senderDevice, QByteArray("ENC:ACCEPT"));
    inbound.messageKind = EnvelopeMessageKind::ContactAccept;
    engine.handleEnvelope(inbound, 21);

    QVERIFY(acceptedConversation.has_value());
    QCOMPARE(acceptedConversation->bytes(), conversation.bytes());
    QCOMPARE(acceptedDevice->bytes(), mls.senderDevice.bytes());
    QCOMPARE(visibleMessages, 0);
    QCOMPARE(store.received.size(), 0);
    QCOMPARE(transport.acks.size(), 1);
    QCOMPARE(transport.acks.first().second, quint64(21));
    QVERIFY(store.seen.contains(inbound.envelopeId.bytes()));

    // A forged sender (credential names another device) is dropped silently.
    CiphertextEnvelopeV1 forged =
        incomingEnvelope(conversation, DeviceId::generate(), QByteArray("ENC:ACCEPT"));
    forged.messageKind = EnvelopeMessageKind::ContactAccept;
    acceptedConversation.reset();
    engine.handleEnvelope(forged, 22);
    QVERIFY(!acceptedConversation.has_value());
    QCOMPARE(transport.acks.size(), 1);

    // A redelivery of the consumed accept is acked but not surfaced twice.
    engine.handleEnvelope(inbound, 23);
    QVERIFY(!acceptedConversation.has_value());
    QCOMPARE(transport.acks.size(), 2);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::groupTextIsEncryptedOnceAndFannedOutUnderOneMessage()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy queued(&engine, &SyncEngine::messageQueued);
    QSignalSpy stateSpy(&engine, &SyncEngine::messageStateChanged);
    engine.start();

    const ConversationId group = ConversationId::generate();
    const QList<DeviceId> members{DeviceId::generate(), DeviceId::generate(), DeviceId::generate()};
    engine.enqueueGroupText(group, members, QStringLiteral("hello all"));

    // One ratchet step, one visible row, one durable commit holding every
    // envelope; each envelope carries the same ciphertext and message id but
    // its own envelope id, addressed to its own member.
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.commitGroupSendCount, 1);
    QCOMPARE(store.messages.size(), 1);
    QCOMPARE(store.outboxes.size(), 3);
    QCOMPARE(queued.count(), 1);
    QCOMPARE(transport.sent.size(), 3);
    QSet<QByteArray> envelopeIds;
    QSet<QByteArray> recipients;
    for (const CiphertextEnvelopeV1 &sent : transport.sent) {
        QCOMPARE(sent.messageKind, EnvelopeMessageKind::MlsPrivateMessage);
        QCOMPARE(sent.ciphertext, QByteArray("ENC:hello all"));
        QCOMPARE(sent.conversationId, group);
        envelopeIds.insert(sent.envelopeId.bytes());
        recipients.insert(sent.recipientDeviceId.bytes());
    }
    QCOMPARE(envelopeIds.size(), 3);
    for (const DeviceId &member : members)
        QVERIFY(recipients.contains(member.bytes()));
    for (const auto &outbox : store.outboxes)
        QCOMPARE(outbox.record.messageId, store.messages.first().id);
    // The row is Sent on the first acceptance and stays Sent.
    transport.onRelayAccepted(transport.sent.at(1).envelopeId, 5);
    QCOMPARE(store.deliveryStates.value(store.messages.first().id.bytes()), DeliveryState::Sent);
    transport.onRelayAccepted(transport.sent.at(0).envelopeId, 6);
    transport.onRelayAccepted(transport.sent.at(2).envelopeId, 7);
    QCOMPARE(store.deliveryStates.value(store.messages.first().id.bytes()), DeliveryState::Sent);
    QCOMPARE(store.failEnvelopeCount, 0);
    // Nobody to send to: nothing happens.
    engine.enqueueGroupText(group, {}, QStringLiteral("nobody"));
    QCOMPARE(store.messages.size(), 1);
}

void SyncEngineTest::groupTextFailsOnlyWhenNoRecipientTookIt()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    const ConversationId group = ConversationId::generate();
    engine.enqueueGroupText(group, {DeviceId::generate(), DeviceId::generate()},
                            QStringLiteral("anyone there?"));
    QCOMPARE(transport.sent.size(), 2);
    const MessageId messageId = store.messages.first().id;

    // One member is offline: only their envelope is retired, the row is not
    // failed, because the other member may still get it.
    transport.onRecipientUnavailable(transport.sent.at(0).envelopeId);
    QCOMPARE(store.failEnvelopeCount, 1);
    QVERIFY(store.deliveryStates.value(messageId.bytes()) != DeliveryState::Failed);
    QCOMPARE(store.outboxes.at(0).record.state, OutboxState::Failed);
    QVERIFY(store.outboxes.at(1).record.state != OutboxState::Failed);
    // The other one is accepted: the row is Sent.
    transport.onRelayAccepted(transport.sent.at(1).envelopeId, 9);
    QCOMPARE(store.deliveryStates.value(messageId.bytes()), DeliveryState::Sent);

    // A second message nobody can receive fails as a whole, once, when the
    // last envelope is retired.
    engine.enqueueGroupText(group, {DeviceId::generate(), DeviceId::generate()},
                            QStringLiteral("hello?"));
    const MessageId second = store.messages.at(1).id;
    QSignalSpy stateSpy(&engine, &SyncEngine::messageStateChanged);
    transport.onRecipientUnavailable(transport.sent.at(2).envelopeId);
    QVERIFY(store.deliveryStates.value(second.bytes()) != DeliveryState::Failed);
    QCOMPARE(stateSpy.count(), 0);
    transport.onRecipientUnavailable(transport.sent.at(3).envelopeId);
    QCOMPARE(store.deliveryStates.value(second.bytes()), DeliveryState::Failed);
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(store.failEnvelopeCount, 3);
    QVERIFY(!engine.isFailedClosed());

    // An ordinary one-to-one send still fails the message outright.
    engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("direct"));
    const MessageId direct = store.messages.at(2).id;
    transport.onRecipientUnavailable(transport.sent.at(4).envelopeId);
    QCOMPARE(store.deliveryStates.value(direct.bytes()), DeliveryState::Failed);
    QCOMPARE(store.failEnvelopeCount, 3);
}

void SyncEngineTest::groupControlAndChangesFanOut()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();
    const ConversationId group = ConversationId::generate();
    const DeviceId existingA = DeviceId::generate();
    const DeviceId existingB = DeviceId::generate();
    const DeviceId newcomer = DeviceId::generate();

    // A control message is encrypted once and shipped to everyone, with no
    // visible row.
    engine.sendGroupControl(group, {existingA, existingB}, QByteArray("INFO"));
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.messages.size(), 0);
    QCOMPARE(store.commitControlSendManyCount, 1);
    QCOMPARE(transport.sent.size(), 2);
    QCOMPARE(transport.sent.at(0).messageKind, EnvelopeMessageKind::GroupControl);
    QCOMPARE(transport.sent.at(0).ciphertext, QByteArray("ENC:INFO"));

    // A membership change ships the Commit to the existing members and the
    // Welcome to the newcomer, neither encrypted again, with the caller's
    // pending MLS snapshot committed alongside all three envelopes.
    mls.stateVersion = 42;
    engine.sendGroupChange(group, {existingA, existingB}, QByteArray("COMMIT"), {newcomer},
                           QByteArray("WELCOME"));
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.commitControlSendManyCount, 2);
    QCOMPARE(store.lastMlsState, QByteArray("state-42"));
    QCOMPARE(transport.sent.size(), 5);
    int commits = 0;
    int welcomes = 0;
    for (int i = 2; i < 5; ++i) {
        const CiphertextEnvelopeV1 &sent = transport.sent.at(i);
        if (sent.messageKind == EnvelopeMessageKind::MlsCommit) {
            ++commits;
            QCOMPARE(sent.ciphertext, QByteArray("COMMIT"));
            QVERIFY(sent.recipientDeviceId == existingA || sent.recipientDeviceId == existingB);
        } else if (sent.messageKind == EnvelopeMessageKind::GroupWelcome) {
            ++welcomes;
            QCOMPARE(sent.ciphertext, QByteArray("WELCOME"));
            QCOMPARE(sent.recipientDeviceId, newcomer);
        }
    }
    QCOMPARE(commits, 2);
    QCOMPARE(welcomes, 1);
    // The Welcome is queued after the Commits, so the newcomer's roster (sent
    // next, under the new epoch) always follows the join.
    QCOMPARE(transport.sent.at(4).messageKind, EnvelopeMessageKind::GroupWelcome);

    // A change with nobody left to tell still persists the epoch.
    mls.stateVersion = 43;
    engine.sendGroupChange(group, {}, QByteArray("COMMIT"), {}, QByteArray());
    QCOMPARE(store.commitMlsStateOnlyCount, 1);
    QCOMPARE(store.lastMlsState, QByteArray("state-43"));
    QCOMPARE(transport.sent.size(), 5);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::groupWelcomeFromAcceptedContactIsJoinedAndSurfaced()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    struct Seen final {
        std::optional<ConversationId> conversation;
        std::optional<AccountId> account;
        std::optional<DeviceId> device;
        QList<QByteArray> members;
        int count = 0;
    } seen;
    connect(&engine, &SyncEngine::groupWelcomeReceived, &engine,
            [&](const ConversationId &c, const AccountId &a, const DeviceId &d,
                const QList<QByteArray> &m) {
                seen.conversation = c;
                seen.account = a;
                seen.device = d;
                seen.members = m;
                ++seen.count;
            });
    QSignalSpy handshakes(&engine, &SyncEngine::handshakeReceived);
    engine.start();

    const ConversationId group = ConversationId::generate();
    const AccountId inviter = AccountId::generate();
    store.acceptedAccounts.insert(inviter.bytes());
    // The Welcome's membership names the inviter's device and one more.
    const DeviceId third = DeviceId::generate();
    mls.inspectMembers = {credentialBytes(mls.senderDevice), credentialBytes(third)};

    CiphertextEnvelopeV1 welcome =
        incomingHandshakeEnvelope(group, inviter, mls.senderDevice, QByteArray("welcome-bytes"));
    welcome.messageKind = EnvelopeMessageKind::GroupWelcome;
    engine.handleEnvelope(welcome, 21);

    // Inspected, joined, committed with the joined state, acked, surfaced —
    // and never stashed as a contact request.
    QCOMPARE(mls.inspectWelcomeCount, 1);
    QCOMPARE(mls.joinGroupCount, 1);
    QCOMPARE(mls.lastJoinedWelcome, QByteArray("welcome-bytes"));
    QCOMPARE(store.groupWelcomeJoinCount, 1);
    QCOMPARE(store.commitHandshakeReceiveCount, 0);
    QCOMPARE(handshakes.count(), 0);
    QCOMPARE(transport.acks.size(), 1);
    QCOMPARE(seen.count, 1);
    QCOMPARE(seen.conversation->bytes(), group.bytes());
    QCOMPARE(seen.account->bytes(), inviter.bytes());
    QCOMPARE(seen.device->bytes(), mls.senderDevice.bytes());
    QCOMPARE(seen.members.size(), 2);
    QVERIFY(store.knownConversations.contains(group.bytes()));

    // A redelivery is acked and joins nothing twice.
    engine.handleEnvelope(welcome, 22);
    QCOMPARE(mls.joinGroupCount, 1);
    QCOMPARE(seen.count, 1);
    QCOMPARE(transport.acks.size(), 2);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::groupWelcomeFromStrangerOrForKnownGroupIsConsumedNotJoined()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy joined(&engine, &SyncEngine::groupWelcomeReceived);
    engine.start();

    // A stranger (not an Accepted contact) cannot put this device into a group.
    CiphertextEnvelopeV1 fromStranger = incomingHandshakeEnvelope(
        ConversationId::generate(), AccountId::generate(), mls.senderDevice, QByteArray("w"));
    fromStranger.messageKind = EnvelopeMessageKind::GroupWelcome;
    engine.handleEnvelope(fromStranger, 31);
    QCOMPARE(mls.inspectWelcomeCount, 0);
    QCOMPARE(mls.joinGroupCount, 0);
    QCOMPARE(store.groupWelcomeRefuseCount, 1);
    QCOMPARE(transport.acks.size(), 1); // consumed, so it is not redelivered forever
    QCOMPARE(joined.count(), 0);

    // A Welcome for a group this device already holds would overwrite it.
    const AccountId contact = AccountId::generate();
    store.acceptedAccounts.insert(contact.bytes());
    const ConversationId known = ConversationId::generate();
    store.knownConversations.insert(known.bytes());
    CiphertextEnvelopeV1 forKnown =
        incomingHandshakeEnvelope(known, contact, mls.senderDevice, QByteArray("w2"));
    forKnown.messageKind = EnvelopeMessageKind::GroupWelcome;
    engine.handleEnvelope(forKnown, 32);
    QCOMPARE(mls.joinGroupCount, 0);
    QCOMPARE(store.groupWelcomeRefuseCount, 2);
    QCOMPARE(transport.acks.size(), 2);

    // A store failure on the trust check fails closed without acking.
    store.failCanJoinGroup = true;
    CiphertextEnvelopeV1 another = incomingHandshakeEnvelope(ConversationId::generate(), contact,
                                                             mls.senderDevice, QByteArray("w3"));
    another.messageKind = EnvelopeMessageKind::GroupWelcome;
    engine.handleEnvelope(another, 33);
    QVERIFY(engine.isFailedClosed());
    QCOMPARE(transport.acks.size(), 2);
    QCOMPARE(joined.count(), 0);
}

void SyncEngineTest::groupWelcomeNotNamingTheSenderIsRefused()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy joined(&engine, &SyncEngine::groupWelcomeReceived);
    engine.start();
    const AccountId contact = AccountId::generate();
    store.acceptedAccounts.insert(contact.bytes());

    // The relay claims the contact's device sent it, but the Welcome's own
    // membership names a different device: a misattribution, refused before
    // any join.
    mls.inspectMembers = {credentialBytes(DeviceId::generate())};
    CiphertextEnvelopeV1 forged = incomingHandshakeEnvelope(ConversationId::generate(), contact,
                                                            mls.senderDevice, QByteArray("w"));
    forged.messageKind = EnvelopeMessageKind::GroupWelcome;
    engine.handleEnvelope(forged, 41);
    QCOMPARE(mls.inspectWelcomeCount, 1);
    QCOMPARE(mls.joinGroupCount, 0);
    QCOMPARE(store.groupWelcomeRefuseCount, 1);
    QCOMPARE(joined.count(), 0);
    QCOMPARE(transport.acks.size(), 1);

    // A Welcome that will not even inspect is consumed the same way.
    mls.failInspect = true;
    CiphertextEnvelopeV1 broken = incomingHandshakeEnvelope(ConversationId::generate(), contact,
                                                            mls.senderDevice, QByteArray("x"));
    broken.messageKind = EnvelopeMessageKind::GroupWelcome;
    engine.handleEnvelope(broken, 42);
    QCOMPARE(store.groupWelcomeRefuseCount, 2);
    QCOMPARE(transport.acks.size(), 2);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::groupControlIsSurfacedOnReceive()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QByteArray payload;
    std::optional<DeviceId> from;
    int visible = 0;
    connect(&engine, &SyncEngine::groupControlReceived, &engine,
            [&](const ConversationId &, const DeviceId &sender, const QByteArray &bytes) {
                from = sender;
                payload = bytes;
            });
    connect(&engine, &SyncEngine::messageReceived, &engine, [&](const MessageRecord &) { ++visible; });
    engine.start();

    const ConversationId group = ConversationId::generate();
    CiphertextEnvelopeV1 inbound = incomingEnvelope(group, mls.senderDevice, QByteArray("ENC:RENAME"));
    inbound.messageKind = EnvelopeMessageKind::GroupControl;
    engine.handleEnvelope(inbound, 51);
    QCOMPARE(payload, QByteArray("RENAME"));
    QCOMPARE(from->bytes(), mls.senderDevice.bytes());
    QCOMPARE(visible, 0);
    QCOMPARE(store.received.size(), 0);
    QCOMPARE(transport.acks.size(), 1);

    // A forged sender is dropped; a redelivery is acked once more but not
    // surfaced again.
    CiphertextEnvelopeV1 forged = incomingEnvelope(group, DeviceId::generate(), QByteArray("ENC:LEAVE"));
    forged.messageKind = EnvelopeMessageKind::GroupControl;
    payload.clear();
    engine.handleEnvelope(forged, 52);
    QVERIFY(payload.isEmpty());
    engine.handleEnvelope(inbound, 53);
    QVERIFY(payload.isEmpty());
    QCOMPARE(transport.acks.size(), 2);
}

void SyncEngineTest::callSignalIsControlSentAndSurfacedOnReceive()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QByteArray receivedPayload;
    std::optional<DeviceId> receivedFrom;
    int visibleMessages = 0;
    connect(&engine, &SyncEngine::callSignalReceived, &engine,
            [&](const ConversationId &, const DeviceId &sender, const QByteArray &payload) {
                receivedFrom = sender;
                receivedPayload = payload;
            });
    connect(&engine, &SyncEngine::messageReceived, &engine,
            [&](const MessageRecord &) { ++visibleMessages; });
    engine.start();

    // Call control travels the DURABLE path: encrypted under the group ratchet
    // and queued in the outbox, so an offer or a hangup is retried rather than
    // lost. It never becomes a visible message row.
    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    engine.sendCallSignal(conversation, peer, QByteArray("OFFER"));
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.messages.size(), 0);
    QCOMPARE(store.outboxes.size(), 1);
    QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(transport.datagrams.size(), 0);
    QCOMPARE(transport.sent.first().messageKind, EnvelopeMessageKind::CallSignal);
    QCOMPARE(transport.sent.first().ciphertext, QByteArray("ENC:OFFER"));

    // Receiving one is a control receive: acked, replay-guarded, and surfaced as
    // plaintext for the call layer rather than as conversation.
    CiphertextEnvelopeV1 inbound =
        incomingEnvelope(conversation, mls.senderDevice, QByteArray("ENC:ANSWER"));
    inbound.messageKind = EnvelopeMessageKind::CallSignal;
    engine.handleEnvelope(inbound, 31);
    QCOMPARE(receivedPayload, QByteArray("ANSWER"));
    QCOMPARE(receivedFrom->bytes(), mls.senderDevice.bytes());
    QCOMPARE(visibleMessages, 0);
    QCOMPARE(store.received.size(), 0);
    QCOMPARE(transport.acks.size(), 1);

    // A signal whose MLS credential names a different device is a relay trying
    // to misattribute call control; it is dropped without being surfaced.
    CiphertextEnvelopeV1 forged =
        incomingEnvelope(conversation, DeviceId::generate(), QByteArray("ENC:HANGUP"));
    forged.messageKind = EnvelopeMessageKind::CallSignal;
    receivedPayload.clear();
    engine.handleEnvelope(forged, 32);
    QVERIFY(receivedPayload.isEmpty());
    QCOMPARE(transport.acks.size(), 1);

    // A redelivery is acked but surfaced only once, so a retried offer does not
    // ring twice.
    engine.handleEnvelope(inbound, 33);
    QVERIFY(receivedPayload.isEmpty());
    QCOMPARE(transport.acks.size(), 2);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::profileUpdateIsControlSentAndSurfacedOnReceive()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QByteArray receivedPayload;
    std::optional<DeviceId> receivedFrom;
    int visibleMessages = 0;
    connect(&engine, &SyncEngine::profileUpdateReceived, &engine,
            [&](const ConversationId &, const DeviceId &sender, const QByteArray &payload) {
                receivedFrom = sender;
                receivedPayload = payload;
            });
    connect(&engine, &SyncEngine::messageReceived, &engine,
            [&](const MessageRecord &) { ++visibleMessages; });
    engine.start();

    // A profile update travels the DURABLE path: encrypted under the group
    // ratchet and queued in the outbox, so a change made while the contact is
    // away still reaches them. It never becomes a visible message row.
    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    engine.sendProfileUpdate(conversation, peer, QByteArray("PROFILE"));
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.messages.size(), 0);
    QCOMPARE(store.outboxes.size(), 1);
    QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(transport.datagrams.size(), 0);
    QCOMPARE(transport.sent.first().messageKind, EnvelopeMessageKind::ProfileUpdate);
    QCOMPARE(transport.sent.first().recipientDeviceId.bytes(), peer.bytes());
    QCOMPARE(transport.sent.first().ciphertext, QByteArray("ENC:PROFILE"));

    // Receiving one is a control receive: acked, replay-guarded, and surfaced
    // as plaintext for the roster rather than as conversation.
    CiphertextEnvelopeV1 inbound =
        incomingEnvelope(conversation, mls.senderDevice, QByteArray("ENC:THEIRS"));
    inbound.messageKind = EnvelopeMessageKind::ProfileUpdate;
    engine.handleEnvelope(inbound, 41);
    QCOMPARE(receivedPayload, QByteArray("THEIRS"));
    QCOMPARE(receivedFrom->bytes(), mls.senderDevice.bytes());
    QCOMPARE(visibleMessages, 0);
    QCOMPARE(store.received.size(), 0);
    QCOMPARE(transport.acks.size(), 1);

    // One whose MLS credential names a different device is a relay trying to
    // put words (or a picture) in someone else's mouth; dropped unsurfaced.
    CiphertextEnvelopeV1 forged =
        incomingEnvelope(conversation, DeviceId::generate(), QByteArray("ENC:FORGED"));
    forged.messageKind = EnvelopeMessageKind::ProfileUpdate;
    receivedPayload.clear();
    engine.handleEnvelope(forged, 42);
    QVERIFY(receivedPayload.isEmpty());
    QCOMPARE(transport.acks.size(), 1);

    // A redelivery is acked but surfaced only once.
    engine.handleEnvelope(inbound, 43);
    QVERIFY(receivedPayload.isEmpty());
    QCOMPARE(transport.acks.size(), 2);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::callMediaBypassesTheStoreAndTheRatchet()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    const QByteArray sealedFrame("already-sealed-audio");
    engine.sendCallMedia(conversation, peer, sealedFrame);

    // The whole point of this path: no durable write, no outbox entry, no
    // ratchet advance, and no retry. Fifty of these a second must cost nothing
    // but a signature and a socket write.
    QCOMPARE(store.messages.size(), 0);
    QCOMPARE(store.outboxes.size(), 0);
    QCOMPARE(mls.encryptCount, 0);
    QCOMPARE(transport.sent.size(), 0);
    QCOMPARE(transport.datagrams.size(), 1);

    const CiphertextEnvelopeV1 &sent = transport.datagrams.first();
    QCOMPARE(sent.messageKind, EnvelopeMessageKind::CallMedia);
    QCOMPARE(sent.recipientDeviceId.bytes(), peer.bytes());
    QCOMPARE(sent.conversationId.bytes(), conversation.bytes());
    // The payload is shipped verbatim: it is already sealed under the call's own
    // media key, so re-encrypting it would be wrong as well as expensive.
    QCOMPARE(sent.ciphertext, sealedFrame);
    // It is still signed, so the relay and the recipient authenticate its origin
    // exactly as they do a durable envelope.
    QCOMPARE(sent.senderSignature.size(), 64);

    // A signing failure drops the frame rather than failing the whole session:
    // one lost 20 ms of audio must not end the call.
    SyncEngine unsignable(makeConfig(), store, mls, transport,
                          [](QByteArrayView) { return QByteArray(); }, clock());
    unsignable.start();
    unsignable.sendCallMedia(conversation, peer, sealedFrame);
    QCOMPARE(transport.datagrams.size(), 1);
    QVERIFY(!unsignable.isFailedClosed());
}

void SyncEngineTest::inboundDatagramsAreSurfacedWithoutTouchingTheStore()
{
    m_now = 1'700'000'000'000;
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QByteArray media;
    connect(&engine, &SyncEngine::callMediaReceived, &engine,
            [&](const ConversationId &, const DeviceId &, const QByteArray &payload) {
                media = payload;
            });
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    CiphertextEnvelopeV1 inbound =
        incomingEnvelope(conversation, mls.senderDevice, QByteArray("sealed-audio"));
    inbound.messageKind = EnvelopeMessageKind::CallMedia;
    QVERIFY(transport.onDatagram);
    transport.onDatagram(inbound);

    QCOMPARE(media, QByteArray("sealed-audio"));
    // Nothing was decrypted, stored, deduplicated or acknowledged: a datagram
    // has no sequence to acknowledge and the engine holds no call keys.
    QCOMPARE(mls.processCount, 0);
    QCOMPARE(store.received.size(), 0);
    QCOMPARE(transport.acks.size(), 0);
    QVERIFY(store.seen.isEmpty());

    // Any other kind arriving on the unreliable path is dropped rather than
    // interpreted: a durable message must not be honoured without its guards.
    media.clear();
    CiphertextEnvelopeV1 smuggled =
        incomingEnvelope(conversation, mls.senderDevice, QByteArray("ENC:hello"));
    smuggled.messageKind = EnvelopeMessageKind::MlsPrivateMessage;
    transport.onDatagram(smuggled);
    QVERIFY(media.isEmpty());
    QCOMPARE(mls.processCount, 0);
    QCOMPARE(store.received.size(), 0);

    // And after stop() the callback is uninstalled entirely.
    engine.stop();
    QVERIFY(!transport.onDatagram);
}

void SyncEngineTest::textsAreFiledUnderTheirCiphertextOnBothEnds()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    // A plain text still travels as its bare UTF-8, so older clients read it,
    // and the sender files it under the id its ciphertext names.
    const ConversationId conversation = ConversationId::generate();
    engine.enqueueText(conversation, DeviceId::generate(), QStringLiteral("hello"));
    QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(transport.sent.first().ciphertext, QByteArray("ENC:hello"));
    QCOMPARE(store.messages.first().id, messageIdForCiphertext(transport.sent.first().ciphertext));
    QVERIFY(store.messages.first().sharedId);

    // The recipient arrives at the same id from the same bytes.
    const auto incoming = incomingEnvelope(conversation, mls.senderDevice, "ENC:hi there");
    engine.handleEnvelope(incoming, 3);
    QCOMPARE(store.received.size(), 1);
    QCOMPARE(store.received.first().id, messageIdForCiphertext(incoming.ciphertext));
    QVERIFY(store.received.first().sharedId);
    QCOMPARE(store.received.first().body, QStringLiteral("hi there"));
    QVERIFY(!store.received.first().replyToId);
}

void SyncEngineTest::replyCarriesItsQuoteBothWays()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const MessageQuote quote{MessageId::generate(), DeviceId::generate(),
                             QStringLiteral("Are you free on Saturday?")};
    engine.enqueueText(conversation, DeviceId::generate(), QStringLiteral("Yes!"), quote);
    QCOMPARE(transport.sent.size(), 1);
    const auto sent = decodeMessageContent(transport.sent.first().ciphertext.mid(4));
    QVERIFY(sent.has_value());
    QCOMPARE(sent->type, MessageContent::Type::Reply);
    QCOMPARE(*sent->target, quote.target);
    QCOMPARE(*sent->quotedSender, quote.sender);
    const MessageRecord &mine = store.messages.first();
    QCOMPARE(mine.body, QStringLiteral("Yes!"));
    QCOMPARE(*mine.replyToId, quote.target);
    QCOMPARE(*mine.quotedSenderDeviceId, quote.sender);
    QCOMPARE(mine.quotedBody, quote.body);

    QSignalSpy received(&engine, &SyncEngine::messageReceived);
    const QByteArray reply = encodeMessageContent(
        MessageContent::reply(QStringLiteral("See you then"), mine.id, mine.senderDeviceId,
                              mine.body));
    engine.handleEnvelope(incomingEnvelope(conversation, mls.senderDevice, "ENC:" + reply), 4);
    QCOMPARE(received.count(), 1);
    const MessageRecord &theirs = store.received.first();
    QCOMPARE(theirs.body, QStringLiteral("See you then"));
    QCOMPARE(*theirs.replyToId, mine.id);
    QCOMPARE(*theirs.quotedSenderDeviceId, mine.senderDeviceId);
    QCOMPARE(theirs.quotedBody, QStringLiteral("Yes!"));
}

void SyncEngineTest::editChangesASentMessageAndTellsEveryRecipient()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    EditLog edited(engine);
    QVector<MessageId> stateIds;
    connect(&engine, &SyncEngine::messageStateChanged, &engine,
            [&stateIds](const MessageId &id, DeliveryState) { stateIds.append(id); });
    engine.start();

    const ConversationId group = ConversationId::generate();
    const QList<DeviceId> members{DeviceId::generate(), DeviceId::generate()};
    engine.enqueueGroupText(group, members, QStringLiteral("See you at 7"));
    const MessageId id = store.messages.first().id;
    transport.onRelayAccepted(transport.sent.at(0).envelopeId, 1);
    transport.onRelayAccepted(transport.sent.at(1).envelopeId, 2);
    QCOMPARE(store.deliveryStates.value(id.bytes()), DeliveryState::Sent);

    m_now += 5'000;
    engine.enqueueEdit(group, members, id, QStringLiteral("See you at 8"));
    QCOMPARE(edited.entries.size(), 1);
    QCOMPARE(edited.entries.first().conversation, group);
    QCOMPARE(edited.entries.first().id, id);
    QCOMPARE(edited.entries.first().body, QStringLiteral("See you at 8"));
    QCOMPARE(edited.entries.first().editedAtMs, m_now);
    QCOMPARE(store.messages.first().body, QStringLiteral("See you at 8"));
    QCOMPARE(store.messages.first().editedAtMs, m_now);

    // One envelope per member, all one encrypted edit, none of them a row.
    QCOMPARE(mls.encryptCount, 2);
    QCOMPARE(store.messages.size(), 1);
    QCOMPARE(transport.sent.size(), 4);
    for (qsizetype i = 2; i < transport.sent.size(); ++i) {
        const auto &envelope = transport.sent.at(i);
        QCOMPARE(envelope.messageKind, EnvelopeMessageKind::MlsPrivateMessage);
        const auto content = decodeMessageContent(envelope.ciphertext.mid(4));
        QVERIFY(content.has_value());
        QCOMPARE(content->type, MessageContent::Type::Edit);
        QCOMPARE(*content->target, id);
        QCOMPARE(content->body, QStringLiteral("See you at 8"));
    }
    // Its envelopes keep their own bookkeeping: the relay taking them does
    // not report anything about the edited message.
    const qsizetype before = stateIds.size();
    transport.onRelayAccepted(transport.sent.at(2).envelopeId, 3);
    for (qsizetype i = before; i < stateIds.size(); ++i)
        QVERIFY(stateIds.at(i) != id);
    QCOMPARE(store.deliveryStates.value(id.bytes()), DeliveryState::Sent);
}

void SyncEngineTest::editOfAQueuedOrUnknownMessageDoesNothing()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    transport.connected = false;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    EditLog edited(engine);
    QSignalSpy failed(&engine, &SyncEngine::failedClosed);
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    engine.enqueueText(conversation, peer, QStringLiteral("typo"));
    const MessageId id = store.messages.first().id;

    // Still queued: the relay has not taken it, so an edit could overtake it.
    engine.enqueueEdit(conversation, {peer}, id, QStringLiteral("fixed"));
    // Not a message of this conversation, or not one at all.
    engine.enqueueEdit(ConversationId::generate(), {peer}, id, QStringLiteral("fixed"));
    engine.enqueueEdit(conversation, {peer}, MessageId::generate(), QStringLiteral("fixed"));

    // Refused before the ratchet moved: nothing encrypted, nothing failed.
    QCOMPARE(mls.encryptCount, 1);
    QVERIFY(edited.entries.isEmpty());
    QCOMPARE(failed.count(), 0);
    QVERIFY(!engine.isFailedClosed());
    QCOMPARE(store.messages.first().body, QStringLiteral("typo"));
    QCOMPARE(store.outboxes.size(), 1);
}

void SyncEngineTest::inboundEditAppliesOnlyToTheSendersNewerEdit()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    EditLog edited(engine);
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const DeviceId author = mls.senderDevice;
    const auto original = incomingEnvelope(conversation, author, "ENC:See you at 7");
    engine.handleEnvelope(original, 1);
    const MessageId id = messageIdForCiphertext(original.ciphertext);
    const auto editEnvelope = [&](const QString &text, qint64 createdAtMs) {
        auto envelope = incomingEnvelope(
            conversation, mls.senderDevice,
            "ENC:" + encodeMessageContent(MessageContent::edit(id, text)));
        envelope.createdAtMs = createdAtMs;
        return envelope;
    };

    // The author's edit lands.
    engine.handleEnvelope(editEnvelope(QStringLiteral("See you at 8"), m_now + 1'000), 2);
    QCOMPARE(edited.entries.size(), 1);
    QCOMPARE(edited.entries.first().id, id);
    QCOMPARE(edited.entries.first().body, QStringLiteral("See you at 8"));
    QCOMPARE(store.received.first().body, QStringLiteral("See you at 8"));
    QCOMPARE(store.received.first().editedAtMs, m_now + 1'000);

    // An older edit arriving late does not undo a newer one.
    engine.handleEnvelope(editEnvelope(QStringLiteral("See you at 6"), m_now + 500), 3);
    QCOMPARE(edited.entries.size(), 1);
    QCOMPARE(store.received.first().body, QStringLiteral("See you at 8"));

    // Another member cannot rewrite the author's words.
    mls.senderDevice = DeviceId::generate();
    engine.handleEnvelope(editEnvelope(QStringLiteral("forged"), m_now + 2'000), 4);
    QCOMPARE(edited.entries.size(), 1);
    QCOMPARE(store.received.first().body, QStringLiteral("See you at 8"));

    // Every one of them was consumed, and none became a row of its own.
    QCOMPARE(transport.acks.size(), 4);
    QCOMPARE(store.received.size(), 1);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::unreadableTaggedMessageIsConsumedWithoutARow()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy received(&engine, &SyncEngine::messageReceived);
    engine.start();

    engine.handleEnvelope(incomingEnvelope(ConversationId::generate(), mls.senderDevice,
                                           QByteArray("ENC:\xFF\x01\x02", 7)),
                          1);
    QCOMPARE(received.count(), 0);
    QVERIFY(store.received.isEmpty());
    QCOMPARE(transport.acks.size(), 1);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::drainPausesWhileTheLinkIsBacklogged()
{
    // About one page envelope of headroom, and more than call media alone
    // can leave unsent (callMediaAloneNeverHoldsTheDrainBack). makeConfig
    // leaves it at its default.
    const qint64 gate = SyncEngine::defaultMaxDrainBacklogBytes;
    QCOMPARE(gate, qint64(256 * 1024));
    QCOMPARE(makeConfig().maxDrainBacklogBytes, gate);

    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    transport.connected = false;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();
    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    engine.enqueueText(conversation, peer, QStringLiteral("one"));
    engine.enqueueText(conversation, peer, QStringLiteral("two"));
    QCOMPARE(store.outboxes.size(), 2);

    // The link is back but more than the gate is still unsent: nothing is
    // claimed or sent, and no attempt is spent.
    transport.connected = true;
    transport.backlog = gate + 1;
    transport.onConnected();
    // A send made meanwhile is stored and waits with the rest.
    engine.enqueueText(conversation, peer, QStringLiteral("three"));
    QCOMPARE(store.outboxes.size(), 3);
    QCOMPARE(transport.sent.size(), 0);
    for (const StoredOutbox &outbox : std::as_const(store.outboxes)) {
        QCOMPARE(outbox.record.state, OutboxState::Pending);
        QCOMPARE(outbox.record.attemptCount, 0);
    }

    // Exactly at the limit the link takes one more envelope. That envelope
    // pushes the backlog over, and the check before the next claim holds the
    // other two back: the backlog is looked at per envelope, not per drain.
    transport.backlog = gate;
    transport.backlogGrowsOnSend = true;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(transport.sent.first().ciphertext, QByteArray("ENC:one"));
    QCOMPARE(store.outboxes.at(0).record.attemptCount, 1);
    for (int i = 1; i < store.outboxes.size(); ++i) {
        QCOMPARE(store.outboxes.at(i).record.state, OutboxState::Pending);
        QCOMPARE(store.outboxes.at(i).record.attemptCount, 0);
    }
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::drainResumesInOrder()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    transport.connected = false;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();
    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    engine.enqueueText(conversation, peer, QStringLiteral("first"));
    engine.sendCallSignal(conversation, peer, QByteArray("OFFER"));
    engine.enqueueText(conversation, peer, QStringLiteral("third"));

    // A large upload is still leaving: the first envelope goes, the rest wait.
    transport.connected = true;
    transport.backlog = SyncEngine::defaultMaxDrainBacklogBytes - 1;
    transport.backlogGrowsOnSend = true;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 1);

    // The socket empties. With no reconnect and no new send, the one-second
    // retry timer resumes the drain where it stopped, in the order queued.
    transport.backlog = 0;
    transport.backlogGrowsOnSend = false;
    QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 3, 2500);
    QCOMPARE(transport.sent.at(0).ciphertext, QByteArray("ENC:first"));
    QCOMPARE(transport.sent.at(1).ciphertext, QByteArray("ENC:OFFER"));
    QCOMPARE(transport.sent.at(2).ciphertext, QByteArray("ENC:third"));
    // Each left exactly once: the clock did not move, so no retry was due.
    for (const StoredOutbox &outbox : std::as_const(store.outboxes))
        QCOMPARE(outbox.record.attemptCount, 1);
    QCOMPARE(mls.encryptCount, 3);
}

void SyncEngineTest::callMediaAloneNeverHoldsTheDrainBack()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    transport.backlog = 0;
    transport.backlogGrowsOnSend = true;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();
    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();

    // Camera video on an uplink slower than the camera: unpaced frames of up
    // to 96 KiB (plus the call packet's header and tag) are admitted while
    // at most 128 KiB is unsent, and the rest are dropped. The backlog
    // settles above 128 KiB, far over 64 KiB, and stays there for the call.
    const QByteArray frame(96 * 1024 + 38, 'v');
    for (int i = 0; i < 20; ++i)
        engine.sendCallMedia(conversation, peer, frame);
    QVERIFY(transport.datagrams.size() < 20);
    QVERIFY2(transport.backlog > RelayClient::maxDatagramBacklogBytes,
             qPrintable(QString::number(transport.backlog)));

    // A text and a call signal still leave at once, ahead of the video:
    // durable traffic is never held back by media that can be dropped.
    engine.enqueueText(conversation, peer, QStringLiteral("can you hear me?"));
    engine.sendCallSignal(conversation, peer, QByteArray("ANSWER"));
    QCOMPARE(transport.sent.size(), 2);
    QCOMPARE(transport.sent.at(0).ciphertext, QByteArray("ENC:can you hear me?"));
    QCOMPARE(transport.sent.at(1).ciphertext, QByteArray("ENC:ANSWER"));
    for (const StoredOutbox &outbox : std::as_const(store.outboxes))
        QCOMPARE(outbox.record.attemptCount, 1);
}

void SyncEngineTest::unknownBacklogNeverGates()
{
    const auto queueThreeOffline = [](SyncEngine &engine) {
        const ConversationId conversation = ConversationId::generate();
        const DeviceId peer = DeviceId::generate();
        for (const QString &text : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")})
            engine.enqueueText(conversation, peer, text);
    };

    // A transport that cannot count its unsent bytes answers -1, and the
    // drain behaves exactly as it did before it had a gate, however small
    // the limit.
    {
        FakeStore store;
        FakeMls mls;
        FakeTransport transport;
        transport.connected = false;
        SyncEngine::Config config = makeConfig();
        config.maxDrainBacklogBytes = 1;
        SyncEngine engine(config, store, mls, transport, okSigner(), clock());
        engine.start();
        queueThreeOffline(engine);
        transport.connected = true;
        QCOMPARE(engine.pendingSendBytes(), qint64(-1));
        transport.onConnected();
        QCOMPARE(transport.sent.size(), 3);
    }

    // A limit of zero or below turns the gate off, whatever the link reports.
    for (const qint64 disabled : {qint64(0), qint64(-1)}) {
        FakeStore store;
        FakeMls mls;
        FakeTransport transport;
        transport.connected = false;
        SyncEngine::Config config = makeConfig();
        config.maxDrainBacklogBytes = disabled;
        SyncEngine engine(config, store, mls, transport, okSigner(), clock());
        engine.start();
        queueThreeOffline(engine);
        transport.connected = true;
        transport.backlog = 16LL * 1024 * 1024;
        transport.onConnected();
        QCOMPARE(transport.sent.size(), 3);
    }

    // With nothing holding it back, one drain still hands over at most
    // drainBatch envelopes; the rest stay Pending for the next one.
    {
        FakeStore store;
        FakeMls mls;
        FakeTransport transport;
        transport.connected = false;
        SyncEngine::Config config = makeConfig();
        config.drainBatch = 2;
        SyncEngine engine(config, store, mls, transport, okSigner(), clock());
        engine.start();
        queueThreeOffline(engine);
        transport.connected = true;
        transport.onConnected();
        QCOMPARE(transport.sent.size(), 2);
        QCOMPARE(store.outboxes.at(2).record.state, OutboxState::Pending);
        QCOMPARE(store.outboxes.at(2).record.attemptCount, 0);
        transport.onConnected();
        QCOMPARE(transport.sent.size(), 3);
        QCOMPARE(transport.sent.at(2).ciphertext, QByteArray("ENC:c"));
    }
}

void SyncEngineTest::retryOfALargeEnvelopeWaitsForItsUpload()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    // The largest payload the profile page sync hands the engine: 240 KiB.
    const QByteArray payload(245'760, 'p');
    const qint64 sentAt = m_now;
    engine.sendProfileUpdate(ConversationId::generate(), DeviceId::generate(), payload);
    QCOMPARE(transport.sent.size(), 1);
    const qsizetype envelopeBytes = store.outboxes.first().record.envelope.size();
    QVERIFY(envelopeBytes > payload.size());

    // The first backoff (1 s) plus the envelope's own upload at 16 KB/s,
    // about 15 s for this one.
    const qint64 upload = envelopeBytes / 16;
    QVERIFY(upload >= 15'000);
    QCOMPARE(store.outboxes.first().record.nextAttemptMs, sentAt + 1'000 + upload);

    // Past the plain backoff but not past the upload: it is not sent twice
    // and spends no second attempt.
    m_now = sentAt + 1'000 + upload - 1;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(store.outboxes.first().record.attemptCount, 1);

    // Once that time has passed it is re-sent as stored, and the next attempt
    // again waits its (doubled) backoff plus the upload.
    m_now = sentAt + 1'000 + upload;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 2);
    QCOMPARE(transport.sent.last().ciphertext, transport.sent.first().ciphertext);
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.outboxes.first().record.attemptCount, 2);
    QCOMPARE(store.outboxes.first().record.nextAttemptMs, m_now + 2'000 + upload);

    // A text's allowance is a fraction of a second, so a lost text is still
    // retried about a second later.
    engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("hi"));
    const OutboxRecord text = store.outboxes.last().record;
    QCOMPARE(text.attemptCount, 1);
    QVERIFY(text.nextAttemptMs > m_now + 1'000);
    QVERIFY(text.nextAttemptMs <= m_now + 1'100);
}

void SyncEngineTest::linkUpIsSignalled()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    transport.connected = false;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy linkUp(&engine, &SyncEngine::linkUp);
    qsizetype sentWhenSignalled = -1;
    connect(&engine, &SyncEngine::linkUp, &engine,
            [&] { sentWhenSignalled = transport.sent.size(); });
    engine.start();
    QVERIFY(!engine.isLinkUp());
    QCOMPARE(linkUp.count(), 0); // starting is not a connection

    engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("waiting"));
    QCOMPARE(transport.sent.size(), 0);

    // isLinkUp follows the transport itself, not the signal.
    transport.connected = true;
    QVERIFY(engine.isLinkUp());
    QCOMPARE(linkUp.count(), 0);

    transport.onConnected();
    QCOMPARE(linkUp.count(), 1);
    // Emitted after the outbox resumed: what was waiting is already on the
    // link, so whatever a listener sends in response leaves behind it.
    QCOMPARE(sentWhenSignalled, qsizetype(1));

    // Every reconnect is signalled.
    transport.connected = false;
    QVERIFY(!engine.isLinkUp());
    transport.connected = true;
    transport.onConnected();
    QCOMPARE(linkUp.count(), 2);

    // A stopped engine no longer listens for the link.
    engine.stop();
    QVERIFY(!transport.onConnected);
    QCOMPARE(linkUp.count(), 2);
}

void SyncEngineTest::attachmentIsEncryptedOnceAndStoredWithItsDescriptor()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy queued(&engine, &SyncEngine::messageQueued);
    QSignalSpy stateSpy(&engine, &SyncEngine::messageStateChanged);
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    const AttachmentDescriptor photo = photoDescriptor();
    QCOMPARE(engine.enqueueAttachment(conversation, {peer}, false, QStringLiteral("The harbour"), photo),
             AttachmentSendRefusal::None);

    // One ratchet step, one row of kind Attachment whose body is the caption,
    // and the descriptor with the recipient committed beside it.
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.commitAttachmentSendCount, 1);
    QCOMPARE(store.messages.size(), 1);
    const MessageRecord &row = store.messages.first();
    QCOMPARE(row.kind, ContentKind::Attachment);
    QCOMPARE(row.flow, MessageFlow::Outgoing);
    QCOMPARE(row.body, QStringLiteral("The harbour"));
    QCOMPARE(*row.attachment, photo);
    QVERIFY(row.sharedId);
    QVERIFY(!row.replyToId);
    QCOMPARE(store.lastAttachmentRecipients, peer.bytes());
    QCOMPARE(store.lastMlsState, QByteArray("state-1"));
    QCOMPARE(queued.count(), 1);
    QCOMPARE(stateSpy.first().at(1).value<DeliveryState>(), DeliveryState::Queued);

    // The envelope is an ordinary conversation message the peer decodes as
    // an attachment, filed under the id its ciphertext names.
    QCOMPARE(transport.sent.size(), 1);
    const CiphertextEnvelopeV1 &sent = transport.sent.first();
    QCOMPARE(sent.messageKind, EnvelopeMessageKind::MlsPrivateMessage);
    QCOMPARE(sent.recipientDeviceId, peer);
    QCOMPARE(row.id, messageIdForCiphertext(sent.ciphertext));
    const auto content = decodeMessageContent(sent.ciphertext.mid(4));
    QVERIFY(content.has_value());
    QCOMPARE(content->type, MessageContent::Type::Attachment);
    QCOMPARE(content->body, QStringLiteral("The harbour"));
    QCOMPARE(*content->attachment, photo);

    // Its delivery is a text's: Sent on acceptance.
    transport.onRelayAccepted(sent.envelopeId, 4);
    QCOMPARE(store.deliveryStates.value(row.id.bytes()), DeliveryState::Sent);

    // A reply carries its quote in the message and on the row.
    const MessageQuote quote{MessageId::generate(), DeviceId::generate(), QStringLiteral("Where?")};
    AttachmentDescriptor second = photoDescriptor();
    QCOMPARE(engine.enqueueAttachment(conversation, {peer}, false, QString(), second, quote),
             AttachmentSendRefusal::None);
    const MessageRecord &reply = store.messages.at(1);
    QVERIFY(reply.body.isEmpty());
    QCOMPARE(*reply.replyToId, quote.target);
    QCOMPARE(*reply.quotedSenderDeviceId, quote.sender);
    QCOMPARE(reply.quotedBody, quote.body);
    const auto quoted = decodeMessageContent(transport.sent.at(1).ciphertext.mid(4));
    QVERIFY(quoted.has_value());
    QCOMPARE(*quoted->target, quote.target);

    // A one-to-one attachment nobody takes fails as its text would.
    transport.onRecipientUnavailable(transport.sent.at(1).envelopeId);
    QCOMPARE(store.deliveryStates.value(reply.id.bytes()), DeliveryState::Failed);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::groupAttachmentFansOutUnderOneMessage()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    const ConversationId group = ConversationId::generate();
    const QList<DeviceId> members{DeviceId::generate(), DeviceId::generate(), DeviceId::generate()};
    QCOMPARE(engine.enqueueAttachment(group, members, true, QStringLiteral("for everyone"),
                                      photoDescriptor()),
             AttachmentSendRefusal::None);
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.messages.size(), 1);
    QCOMPARE(store.outboxes.size(), 3);
    QCOMPARE(transport.sent.size(), 3);
    QByteArray expected;
    for (const DeviceId &member : members)
        expected.append(member.bytes());
    QCOMPARE(store.lastAttachmentRecipients, expected);
    for (const StoredOutbox &outbox : std::as_const(store.outboxes)) {
        QCOMPARE(outbox.record.messageId, store.messages.first().id);
        QCOMPARE(outbox.record.priority, 0);
    }

    // One member offline does not fail it; the first acceptance sends it.
    const MessageId id = store.messages.first().id;
    transport.onRecipientUnavailable(transport.sent.at(0).envelopeId);
    QVERIFY(store.deliveryStates.value(id.bytes()) != DeliveryState::Failed);
    transport.onRelayAccepted(transport.sent.at(1).envelopeId, 9);
    QCOMPARE(store.deliveryStates.value(id.bytes()), DeliveryState::Sent);
}

void SyncEngineTest::attachmentRefusalsNeverEncryptAndNeverFailClosed()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy failedSpy(&engine, &SyncEngine::failedClosed);
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    AttachmentDescriptor invalid = photoDescriptor();
    invalid.partCount += 1;
    AttachmentDescriptor keyless = photoDescriptor();
    keyless.key.clear();

    QCOMPARE(engine.enqueueAttachment(conversation, {}, true, QString(), photoDescriptor()),
             AttachmentSendRefusal::NoRecipients);
    QCOMPARE(engine.enqueueAttachment(conversation, {peer}, false, QString(), invalid),
             AttachmentSendRefusal::Invalid);
    QCOMPARE(engine.enqueueAttachment(conversation, {peer}, false, QString(), keyless),
             AttachmentSendRefusal::Invalid);
    // A caption no receiver would take: longer than any composer allows.
    QCOMPARE(engine.enqueueAttachment(conversation, {peer}, false, QString(65537, u'x'), photoDescriptor()),
             AttachmentSendRefusal::TooLarge);
    store.failCanEnqueueAttachment = true;
    QCOMPARE(engine.enqueueAttachment(conversation, {peer}, false, QString(), photoDescriptor()),
             AttachmentSendRefusal::StoreError);
    store.failCanEnqueueAttachment = false;
    QCOMPARE(mls.encryptCount, 0);
    QVERIFY(store.messages.isEmpty());
    QVERIFY(transport.sent.isEmpty());

    // An id already used in the conversation (a retry that forgot to mint a
    // new one) is refused before encrypting, where a commit refused after
    // encrypting would have stopped the engine.
    const AttachmentDescriptor photo = photoDescriptor();
    QCOMPARE(engine.enqueueAttachment(conversation, {peer}, false, QString(), photo),
             AttachmentSendRefusal::None);
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(engine.enqueueAttachment(conversation, {peer}, false, QStringLiteral("again"), photo),
             AttachmentSendRefusal::Duplicate);
    QCOMPARE(mls.encryptCount, 1);
    QCOMPARE(store.messages.size(), 1);
    // The same id in another conversation is another attachment.
    QCOMPARE(engine.enqueueAttachment(ConversationId::generate(), {peer}, false, QString(), photo),
             AttachmentSendRefusal::None);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(!engine.isFailedClosed());

    // Once the engine has stopped, nothing is attempted.
    store.failSendCommit = true;
    engine.enqueueText(conversation, peer, QStringLiteral("boom"));
    QVERIFY(engine.isFailedClosed());
    const int encrypted = mls.encryptCount;
    QCOMPARE(engine.enqueueAttachment(conversation, {peer}, false, QString(), photoDescriptor()),
             AttachmentSendRefusal::FailedClosed);
    QCOMPARE(mls.encryptCount, encrypted);
}

void SyncEngineTest::attachmentFramesBypassTheRatchet()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy stateSpy(&engine, &SyncEngine::messageStateChanged);
    engine.start();

    const ConversationId group = ConversationId::generate();
    const QList<DeviceId> members{DeviceId::generate(), DeviceId::generate()};
    const QByteArray frame = partFrame();
    QVERIFY(engine.sendAttachmentFrame(group, members, frame));

    // Nothing encrypted, no state written (an empty blob keeps the stored
    // one), no row: one envelope per member at the lowest priority, carrying
    // the caller's bytes as they are.
    QCOMPARE(mls.encryptCount, 0);
    QCOMPARE(store.commitControlSendManyCount, 1);
    QVERIFY(store.lastMlsState.isEmpty());
    QVERIFY(store.messages.isEmpty());
    QCOMPARE(store.outboxes.size(), 2);
    QCOMPARE(store.outboxes.at(0).record.messageId, store.outboxes.at(1).record.messageId);
    for (const StoredOutbox &outbox : std::as_const(store.outboxes))
        QCOMPARE(outbox.record.priority, 1);
    QCOMPARE(transport.sent.size(), 2);
    QSet<QByteArray> recipients;
    for (const CiphertextEnvelopeV1 &sent : std::as_const(transport.sent)) {
        QCOMPARE(sent.messageKind, EnvelopeMessageKind::AttachmentControl);
        QCOMPARE(sent.ciphertext, frame);
        QCOMPARE(sent.conversationId, group);
        recipients.insert(sent.recipientDeviceId.bytes());
    }
    QCOMPARE(recipients.size(), 2);
    // Sending a frame reports no delivery state (it has no row).
    QCOMPARE(stateSpy.count(), 0);

    // Waiting until the relay takes them, then gone.
    QCOMPARE(engine.pendingAttachmentFrames(), 2);
    transport.onRelayAccepted(transport.sent.at(0).envelopeId, 1);
    QCOMPARE(engine.pendingAttachmentFrames(), 1);
    transport.onRelayAccepted(transport.sent.at(1).envelopeId, 2);
    QCOMPARE(engine.pendingAttachmentFrames(), 0);

    // A part is sent again byte for byte, under new envelopes.
    QVERIFY(engine.sendAttachmentFrame(group, {members.first()}, frame));
    QCOMPARE(transport.sent.size(), 3);
    QCOMPARE(transport.sent.last().ciphertext, frame);
    QVERIFY(transport.sent.last().envelopeId != transport.sent.first().envelopeId);
    QCOMPARE(mls.encryptCount, 0);
}

void SyncEngineTest::attachmentFrameRefusalsQueueNothing()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy failedSpy(&engine, &SyncEngine::failedClosed);
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    const QByteArray header = attachmentFrameHeader(AttachmentFrameType::Part, AttachmentId::generate(), 0);
    QVERIFY(!engine.sendAttachmentFrame(conversation, {}, partFrame()));
    QVERIFY(!engine.sendAttachmentFrame(
        conversation, {peer},
        header + QByteArray(AttachmentLimits::maxFrameBytes - header.size() + 1, 'x')));
    QVERIFY(!engine.sendAttachmentFrame(conversation, {peer}, QByteArray("not a frame at all, no")));
    QVERIFY(!engine.sendAttachmentFrame(conversation, {peer}, QByteArray()));
    QVERIFY(store.outboxes.isEmpty());
    QVERIFY(transport.sent.isEmpty());
    QCOMPARE(engine.pendingAttachmentFrames(), 0);
    // A frame the store could not take is not sent, and stops nothing.
    store.failSendCommit = true;
    QVERIFY(!engine.sendAttachmentFrame(conversation, {peer}, partFrame()));
    QVERIFY(transport.sent.isEmpty());
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(!engine.isFailedClosed());
    // Nor does the largest frame the attachment layer makes get refused.
    store.failSendCommit = false;
    QVERIFY(engine.sendAttachmentFrame(
        conversation, {peer},
        header + QByteArray(AttachmentLimits::partBytes + AttachmentLimits::sealOverhead, 'x')));
    QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(mls.encryptCount, 0);
}

void SyncEngineTest::conversationTrafficLeavesBeforeWaitingFrames()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    transport.connected = false;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const DeviceId peer = DeviceId::generate();
    QVERIFY(engine.sendAttachmentFrame(conversation, {peer}, partFrame()));
    QVERIFY(engine.sendAttachmentFrame(conversation, {peer}, partFrame()));
    engine.enqueueText(conversation, peer, QStringLiteral("queued after the frames"));
    engine.sendCallSignal(conversation, peer, QByteArray("OFFER"));

    // Whatever was queued first, the text and the call signal leave before
    // any frame.
    transport.connected = true;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 4);
    QCOMPARE(transport.sent.at(0).messageKind, EnvelopeMessageKind::MlsPrivateMessage);
    QCOMPARE(transport.sent.at(1).messageKind, EnvelopeMessageKind::CallSignal);
    QCOMPARE(transport.sent.at(2).messageKind, EnvelopeMessageKind::AttachmentControl);
    QCOMPARE(transport.sent.at(3).messageKind, EnvelopeMessageKind::AttachmentControl);
}

void SyncEngineTest::inboundFramesBypassMlsStateAndAreSignalledOnce()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    FrameLog frames(engine);
    QSignalSpy received(&engine, &SyncEngine::messageReceived);
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const DeviceId sender = DeviceId::generate();
    const QByteArray frame = partFrame(AttachmentId::generate(), 7);
    const CiphertextEnvelopeV1 envelope = incomingFrame(conversation, sender, frame);
    engine.handleEnvelope(envelope, 11);

    // Never near the ratchet: no process, no state, and the relay's
    // sender (there is no MLS credential to name one) scopes it.
    QCOMPARE(mls.processCount, 0);
    QVERIFY(store.lastMlsState.isEmpty());
    QCOMPARE(store.watermarkValue, quint64(11));
    QCOMPARE(transport.acks.size(), 1);
    QCOMPARE(transport.acks.first().first, envelope.envelopeId);
    QCOMPARE(frames.entries.size(), 1);
    QCOMPARE(frames.entries.first().conversation, conversation);
    QCOMPARE(frames.entries.first().sender, sender);
    QCOMPARE(frames.entries.first().frame, frame);
    QCOMPARE(received.count(), 0);
    QVERIFY(store.received.isEmpty());

    // A part sent again arrives in a new envelope, and is surfaced again: the
    // attachment layer knows it already holds it.
    engine.handleEnvelope(incomingFrame(conversation, sender, frame), 12);
    QCOMPARE(frames.entries.size(), 2);
    QCOMPARE(mls.processCount, 0);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::replayedFramesAreNotReSignalled()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    FrameLog frames(engine);
    engine.start();

    const CiphertextEnvelopeV1 envelope =
        incomingFrame(ConversationId::generate(), DeviceId::generate(), partFrame());
    engine.handleEnvelope(envelope, 3);
    QCOMPARE(frames.entries.size(), 1);

    // Redelivered after a reconnect: acknowledged again, surfaced never.
    engine.handleEnvelope(envelope, 3);
    QCOMPARE(frames.entries.size(), 1);
    QCOMPARE(transport.acks.size(), 2);

    // Past the early check (a redelivery racing its first commit), the
    // store's replay guard still decides: consumed and acknowledged, not
    // surfaced twice.
    store.hideSeen = true;
    engine.handleEnvelope(envelope, 3);
    QCOMPARE(frames.entries.size(), 1);
    QCOMPARE(transport.acks.size(), 3);
    QCOMPARE(mls.processCount, 0);
    store.hideSeen = false;

    // An expired frame is acknowledged without being surfaced.
    const CiphertextEnvelopeV1 stale =
        incomingFrame(ConversationId::generate(), DeviceId::generate(), partFrame());
    m_now = stale.expiresAtMs + 1;
    engine.handleEnvelope(stale, 4);
    QCOMPARE(frames.entries.size(), 1);
}

void SyncEngineTest::malformedFramesAreConsumedSilently()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    FrameLog frames(engine);
    QSignalSpy failedSpy(&engine, &SyncEngine::failedClosed);
    engine.start();

    const QByteArray good = partFrame();
    const QList<QByteArray> malformed{
        QByteArray(),
        QByteArray("ENC:an MLS message in the wrong kind"),
        QByteArray(1, '\xAC') + good.mid(1, 22), // a header and no body
        QByteArray(1, '\xAD') + good.mid(1),     // another magic
        good.left(2) + QByteArray(1, '\x09') + good.mid(3), // an unknown type
        attachmentFrameHeader(AttachmentFrameType::Part, AttachmentId::generate(),
                              quint32(AttachmentLimits::maxParts))
            + QByteArray(64, 'p'),
        attachmentFrameHeader(AttachmentFrameType::Cancel, AttachmentId::generate(), 1)
            + QByteArray(16, 't'),
        good + QByteArray(AttachmentLimits::maxFrameBytes, 'x'),
    };
    quint64 sequence = 20;
    for (const QByteArray &frame : malformed)
        engine.handleEnvelope(incomingFrame(ConversationId::generate(), DeviceId::generate(), frame),
                              ++sequence);

    // Every one consumed and acknowledged, so the relay stops redelivering
    // it; none surfaced; the ratchet never asked.
    QCOMPARE(transport.acks.size(), malformed.size());
    QCOMPARE(store.watermarkValue, sequence);
    QVERIFY(frames.entries.isEmpty());
    QCOMPARE(mls.processCount, 0);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(!engine.isFailedClosed());
}

void SyncEngineTest::aFrameTheStoreCannotTakeNeverFailsClosed()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    FrameLog frames(engine);
    QSignalSpy failedSpy(&engine, &SyncEngine::failedClosed);
    engine.start();

    const CiphertextEnvelopeV1 envelope =
        incomingFrame(ConversationId::generate(), DeviceId::generate(), partFrame());
    store.failReceive = true;
    engine.handleEnvelope(envelope, 5);
    // Not consumed, so not acknowledged: the relay delivers it again.
    QVERIFY(transport.acks.isEmpty());
    QVERIFY(frames.entries.isEmpty());
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(!engine.isFailedClosed());

    store.failReceive = false;
    engine.handleEnvelope(envelope, 5);
    QCOMPARE(transport.acks.size(), 1);
    QCOMPARE(frames.entries.size(), 1);

    // Conversation traffic still flows.
    engine.handleEnvelope(incomingEnvelope(envelope.conversationId, mls.senderDevice, "ENC:still here"), 6);
    QCOMPARE(store.received.size(), 1);
}

void SyncEngineTest::attachmentMessageArrivesWithItsDescriptorAndQuote()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy received(&engine, &SyncEngine::messageReceived);
    engine.start();

    const ConversationId conversation = ConversationId::generate();
    const AttachmentDescriptor photo = photoDescriptor();
    const QByteArray payload =
        encodeMessageContent(MessageContent::attachmentMessage(QStringLiteral("Look"), photo));
    const auto envelope = incomingEnvelope(conversation, mls.senderDevice, "ENC:" + payload);
    engine.handleEnvelope(envelope, 8);

    QCOMPARE(received.count(), 1);
    QCOMPARE(store.received.size(), 1);
    const MessageRecord &row = store.received.first();
    QCOMPARE(row.kind, ContentKind::Attachment);
    QCOMPARE(row.flow, MessageFlow::Incoming);
    QCOMPARE(row.body, QStringLiteral("Look"));
    QCOMPARE(row.senderDeviceId, mls.senderDevice);
    QCOMPARE(row.id, messageIdForCiphertext(envelope.ciphertext));
    QVERIFY(row.sharedId);
    QVERIFY(row.attachment.has_value());
    QCOMPARE(*row.attachment, photo);
    QVERIFY(!row.replyToId);
    QCOMPARE(transport.acks.size(), 1);

    // One that answers another message keeps the quote.
    const MessageQuote quote{MessageId::generate(), DeviceId::generate(), QStringLiteral("Which one?")};
    const QByteArray reply = encodeMessageContent(
        MessageContent::attachmentMessage(QString(), photoDescriptor(), quote));
    engine.handleEnvelope(incomingEnvelope(conversation, mls.senderDevice, "ENC:" + reply), 9);
    QCOMPARE(store.received.size(), 2);
    const MessageRecord &answer = store.received.at(1);
    QCOMPARE(answer.kind, ContentKind::Attachment);
    QVERIFY(answer.body.isEmpty());
    QCOMPARE(*answer.replyToId, quote.target);
    QCOMPARE(*answer.quotedSenderDeviceId, quote.sender);
    QCOMPARE(answer.quotedBody, quote.body);

    // An id the conversation already used gets no descriptor stored, and
    // the message surfaced says so too: it would otherwise show a transfer
    // that can never move, until the chat was reopened.
    std::optional<bool> surfacedWithDescriptor;
    QObject::connect(&engine, &SyncEngine::messageReceived, &engine, [&](const MessageRecord &message) {
        surfacedWithDescriptor = message.attachment.has_value();
    });
    const QByteArray again = encodeMessageContent(MessageContent::attachmentMessage(QStringLiteral("Again"), photo));
    engine.handleEnvelope(incomingEnvelope(conversation, mls.senderDevice, "ENC:" + again), 10);
    QCOMPARE(store.received.size(), 3);
    QCOMPARE(store.received.at(2).kind, ContentKind::Attachment);
    QVERIFY(!store.received.at(2).attachment);
    QCOMPARE(surfacedWithDescriptor, std::optional<bool>(false));
}

void SyncEngineTest::attachmentFromAnUnexpectedCredentialIsDroppedLikeText()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    QSignalSpy received(&engine, &SyncEngine::messageReceived);
    engine.start();

    // The relay names a device the MLS credential does not.
    const QByteArray payload =
        encodeMessageContent(MessageContent::attachmentMessage(QString(), photoDescriptor()));
    const auto envelope =
        incomingEnvelope(ConversationId::generate(), DeviceId::generate(), "ENC:" + payload);
    QVERIFY(envelope.senderDeviceId != mls.senderDevice);
    engine.handleEnvelope(envelope, 7);
    QCOMPARE(received.count(), 0);
    QVERIFY(store.received.isEmpty());
    QVERIFY(transport.acks.isEmpty());
    QVERIFY(!engine.isFailedClosed());
}

QTEST_MAIN(SyncEngineTest)
#include "tst_syncengine.moc"
