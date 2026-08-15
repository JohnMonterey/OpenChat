#include "network/SyncEngine.h"

#include "protocol/CiphertextEnvelope.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <optional>

using namespace OpenChat;

Q_DECLARE_METATYPE(OpenChat::DeliveryState)

namespace {

// Deterministic MLS stand-in: encrypt prefixes "ENC:", process strips it (and
// reports Application), and an unknown payload is a stale/invalid message.
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
        return Result<SyncProcessOutcome, MlsError>::success(outcome);
    }

    QByteArray takePendingState() override
    {
        return QByteArray("state-") + QByteArray::number(stateVersion);
    }

    int encryptCount = 0;
    int processCount = 0;
    int stateVersion = 0;
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
    void expiredIncomingIsAckedNotProcessed();
    void retryExhaustionMarksFailed();
    void commitFailureFailsClosed();

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
    const CiphertextEnvelopeV1 envelope =
        incomingEnvelope(conversation, DeviceId::generate(), QByteArray("ENC:world"));

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

QTEST_MAIN(SyncEngineTest)
#include "tst_syncengine.moc"
