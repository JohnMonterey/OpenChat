#pragma once

#include "core/Result.h"
#include "crypto/MlsClient.h" // for MlsError
#include "domain/ChatTypes.h"
#include "domain/Identifiers.h"
#include "network/MlsTransactionCoordinator.h"
#include "protocol/CiphertextEnvelope.h"
#include "repositories/RepositoryError.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QObject>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

namespace OpenChat {

// Outcome of processing an inbound MLS message.
struct SyncProcessOutcome final {
    enum class Kind { Application, Control } kind = Kind::Application;
    QByteArray applicationData; // plaintext, only for Application
};

// Narrow MLS surface the engine needs, adapting MlsClient plus a capturing state
// store. encrypt/process advance the in-memory ratchet; takePendingState()
// returns the serialized state to persist atomically with the durable write, and
// clears the pending marker.
class SyncMlsSession
{
public:
    virtual ~SyncMlsSession() = default;
    [[nodiscard]] virtual Result<QByteArray, MlsError>
    encrypt(const ConversationId &conversation, QByteArrayView plaintext) = 0;
    [[nodiscard]] virtual Result<SyncProcessOutcome, MlsError>
    process(const ConversationId &conversation, QByteArrayView mlsMessage) = 0;
    [[nodiscard]] virtual QByteArray takePendingState() = 0;
};

// Durable persistence with the atomic combinations the engine requires. Each
// commit* persists the new MLS state blob in the SAME transaction as the message
// / outbox / watermark, so a crash never leaves the ratchet ahead of the store.
class SyncStore
{
public:
    virtual ~SyncStore() = default;

    [[nodiscard]] virtual Result<void, RepositoryError>
    commitSend(const MessageRecord &message, const OutboxRecord &outbox,
               QByteArrayView mlsState) = 0;
    // Outbox-only send (e.g. an encrypted receipt with no visible message).
    [[nodiscard]] virtual Result<void, RepositoryError>
    commitControlSend(const OutboxRecord &outbox, QByteArrayView mlsState) = 0;

    // Returns true if newly applied, false if the envelope was already seen.
    [[nodiscard]] virtual Result<bool, RepositoryError>
    commitReceive(const MessageRecord &message, const EnvelopeId &envelopeId, quint64 watermark,
                  QByteArrayView mlsState) = 0;
    [[nodiscard]] virtual Result<bool, RepositoryError>
    commitControlReceive(const EnvelopeId &envelopeId, const DeviceId &senderDeviceId,
                         quint64 watermark, QByteArrayView mlsState) = 0;

    [[nodiscard]] virtual Result<bool, RepositoryError> hasSeen(const EnvelopeId &envelopeId) = 0;
    [[nodiscard]] virtual Result<QVector<OutboxRecord>, RepositoryError>
    claimDue(qint64 nowMs, int limit, qint64 leaseUntilMs) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError> markAccepted(const EnvelopeId &envelopeId) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
    scheduleRetry(const EnvelopeId &envelopeId, int attemptCount, qint64 nextAttemptMs) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
    advanceDeliveryState(const MessageId &messageId, DeliveryState state) = 0;
};

// Ciphertext transport the engine drives. The engine sets the callbacks; a real
// adapter wires them to RelayClient signals. No plaintext ever crosses it.
class SyncTransport
{
public:
    virtual ~SyncTransport() = default;
    [[nodiscard]] virtual bool isConnected() const = 0;
    virtual void sendEnvelope(const CiphertextEnvelopeV1 &envelope) = 0;
    virtual void acknowledge(const EnvelopeId &envelopeId, quint64 watermark) = 0;

    // Set by the engine before use.
    std::function<void(const EnvelopeId &, quint64 serverSequence)> onRelayAccepted;
    std::function<void(const CiphertextEnvelopeV1 &, quint64 serverSequence)> onEnvelope;
    std::function<void()> onConnected;
};

// Durable send/receive coordination over MLS + a ciphertext relay.
class SyncEngine final : public QObject
{
    Q_OBJECT

public:
    struct Config final {
        AccountId localAccountId;
        DeviceId localDeviceId;
        int maxSendAttempts = 8;
        int drainBatch = 32;
        qint64 leaseMs = 30'000;
    };

    // signer produces the Ed25519 signature over the canonical envelope signing
    // input (the caller owns the device private key). clock returns UTC ms.
    using Signer = std::function<QByteArray(QByteArrayView signingInput)>;
    using Clock = std::function<qint64()>;

    SyncEngine(Config config, SyncStore &store, SyncMlsSession &mls, SyncTransport &transport,
               Signer signer, Clock clock, QObject *parent = nullptr);
    ~SyncEngine() override;

    void start();
    void stop();

    // Encrypts, durably persists, and queues a text message to a recipient
    // device. UI state reaches Queued only after the durable commit.
    void enqueueText(const ConversationId &conversation, const DeviceId &recipientDevice,
                     const QString &text);

    // Sends an encrypted read receipt (MLS application message, no visible row).
    void acknowledgeRead(const ConversationId &conversation, const DeviceId &recipientDevice,
                         const MessageId &messageId);

    // Processes an inbound envelope with its relay sequence.
    void handleEnvelope(const CiphertextEnvelopeV1 &envelope, quint64 serverSequence);

    [[nodiscard]] bool isFailedClosed() const noexcept;

signals:
    void messageStateChanged(const MessageId &messageId, DeliveryState state);
    void messageReceived(const MessageRecord &message);
    void failedClosed();

private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace OpenChat
