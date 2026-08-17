#include "network/SyncEngine.h"

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
    Result<void, RepositoryError> commitSend(const MessageRecord &message,
                                             const OutboxRecord &outbox,
                                             QByteArrayView mlsState) override
    {
        if (failSend)
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
        if (failSend)
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
                                                        QByteArrayView mlsState) override
    {
        ++commitHandshakeAcceptCount;
        handshakeAcceptAccount = accountId;
        handshakeAcceptConversation = conversationId;
        handshakeAcceptUpdatedAtMs = updatedAtMs;
        handshakeAcceptMlsState = mlsState.toByteArray();
        if (failHandshakeAccept)
            return err();
        return ok();
    }

    Result<bool, RepositoryError> hasSeen(const EnvelopeId &envelopeId) override
    {
        return Result<bool, RepositoryError>::success(seen.contains(envelopeId.bytes()));
    }

    Result<QVector<OutboxRecord>, RepositoryError> claimDue(qint64 nowMs, int limit,
                                                            qint64 leaseUntilMs) override
    {
        QVector<OutboxRecord> due;
        for (StoredOutbox &item : outboxes) {
            if (item.record.state == OutboxState::Accepted)
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

    QVector<MessageRecord> messages;
    QVector<MessageRecord> received;
    QVector<StoredOutbox> outboxes;
    QSet<QByteArray> seen;
    QHash<QByteArray, DeliveryState> deliveryStates;
    QByteArray lastMlsState;
    quint64 watermarkValue = 0;
    int commitSendCount = 0;
    bool failSend = false;
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
    void sendEnvelope(const CiphertextEnvelopeV1 &envelope) override { sent.append(envelope); }
    void acknowledge(const EnvelopeId &envelopeId, quint64 watermark) override
    {
        acks.append({envelopeId, watermark});
    }

    bool connected = true;
    QVector<CiphertextEnvelopeV1> sent;
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

    void sendEncryptsPersistsAndReportsQueued();
    void relayAcceptanceMarksSent();
    void offlineSendStaysQueuedThenDrainsOnConnect();
    void offlineSendSurvivesEngineRestart();
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

void SyncEngineTest::offlineSendStaysQueuedThenDrainsOnConnect()
{
    FakeStore store;
    FakeMls mls;
    FakeTransport transport;
    transport.connected = false;
    SyncEngine engine(makeConfig(), store, mls, transport, okSigner(), clock());
    engine.start();

    engine.enqueueText(ConversationId::generate(), DeviceId::generate(), QStringLiteral("later"));
    QCOMPARE(store.outboxes.size(), 1); // durably queued
    QCOMPARE(transport.sent.size(), 0); // but not sent while offline

    transport.connected = true;
    transport.onConnected(); // link comes up -> drain
    QCOMPARE(transport.sent.size(), 1);
}

void SyncEngineTest::offlineSendSurvivesEngineRestart()
{
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

    transport.connected = true;
    SyncEngine second(makeConfig(), store, mls, transport, okSigner(), clock());
    second.start(); // initial drain resends the durable item
    QCOMPARE(transport.sent.size(), 1);
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
    store.failSend = true;
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
    store.failSend = true;
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

QTEST_MAIN(SyncEngineTest)
#include "tst_syncengine.moc"
