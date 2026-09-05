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
    // MLS-authenticated sender credential (version || deviceId || signingKey).
    // The engine binds the message to this identity, not to the relay-supplied
    // envelope sender field.
    QByteArray senderIdentity;
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
    // Read-only inspection of a Welcome's other members (no ratchet change), used
    // to authenticate the claimed sender before any durable join.
    [[nodiscard]] virtual Result<QList<QByteArray>, MlsError>
    inspectWelcome(QByteArrayView welcome) = 0;
    // Joins the group named by the Welcome, advancing the ratchet. Like
    // encrypt/process, the resulting state is captured for the caller to surrender
    // via takePendingState() and commit atomically; this call does not take it.
    [[nodiscard]] virtual Result<void, MlsError>
    joinGroup(const ConversationId &conversation, QByteArrayView welcome) = 0;
    [[nodiscard]] virtual QByteArray takePendingState() = 0;
};

// Outcome of stashing an inbound contact-handshake Welcome: newly stashed, an
// idempotent redelivery of an already-seen envelope, or dropped because the
// sender is blocked (the envelope is still consumed).
enum class HandshakeReceiveOutcome { Stashed, AlreadySeen, DroppedBlocked };

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

    // Atomic: replay-guard (idempotency) + minimal is-Blocked guard + stash insert
    // + watermark advance. Writes NO mls state (receive never joins).
    [[nodiscard]] virtual Result<HandshakeReceiveOutcome, RepositoryError>
    commitHandshakeReceive(const EnvelopeId &envelopeId, const AccountId &senderAccountId,
                           const DeviceId &senderDeviceId, const ConversationId &conversationId,
                           QByteArrayView welcome, qint64 receivedAtMs, quint64 watermark) = 0;

    // Atomic: upsert mls state + PendingIncoming->Accepted (guarded, also binds the
    // authenticated peer signing key) + delete stash. peerSigningKey is the peer
    // credential's 32 identity bytes, or empty to bind NULL.
    [[nodiscard]] virtual Result<void, RepositoryError>
    commitHandshakeAccept(const AccountId &accountId, const ConversationId &conversationId,
                          qint64 updatedAtMs, QByteArrayView mlsState,
                          QByteArrayView peerSigningKey) = 0;

    [[nodiscard]] virtual Result<bool, RepositoryError> hasSeen(const EnvelopeId &envelopeId) = 0;
    [[nodiscard]] virtual Result<QVector<OutboxRecord>, RepositoryError>
    claimDue(qint64 nowMs, int limit, qint64 leaseUntilMs) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError> markAccepted(const EnvelopeId &envelopeId) = 0;
    // Atomically stop retrying a rejected send and persist the visible failure.
    [[nodiscard]] virtual Result<void, RepositoryError>
    failSend(const EnvelopeId &envelopeId, const MessageId &messageId) = 0;
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

    // Best-effort, live-only delivery: the relay forwards the envelope to a
    // connected recipient and drops it otherwise. Nothing is stored, sequenced
    // or acknowledged, so there is no watermark to advance and no retry. Real-
    // time media is the only traffic that wants this; everything else wants the
    // durable path above.
    virtual void sendDatagram(const CiphertextEnvelopeV1 &envelope) = 0;

    // Set by the engine before use.
    std::function<void(const EnvelopeId &, quint64 serverSequence)> onRelayAccepted;
    std::function<void(const EnvelopeId &)> onRecipientUnavailable;
    std::function<void(const CiphertextEnvelopeV1 &, quint64 serverSequence)> onEnvelope;
    std::function<void(const CiphertextEnvelopeV1 &)> onDatagram;
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
    // device. Offline attempts are stored as Failed for manual retry. Online
    // attempts reach Queued only after the durable commit.
    void enqueueText(const ConversationId &conversation, const DeviceId &recipientDevice,
                     const QString &text);

    // Sends an encrypted read receipt (MLS application message, no visible row).
    void acknowledgeRead(const ConversationId &conversation, const DeviceId &recipientDevice,
                         const MessageId &messageId);

    // Tells the peer that their contact request was accepted: an MLS application
    // message encrypted under the just-joined group's ratchet, shipped with
    // EnvelopeMessageKind::ContactAccept and no visible row. Only a member of the
    // group can produce it, so its arrival is the requester's proof of acceptance.
    void sendContactAccept(const ConversationId &conversation, const DeviceId &recipientDevice);

    // Sends an MLS Welcome to a peer device as a content-blind handshake control
    // send (no visible message row). Unlike acknowledgeRead the ciphertext is NOT
    // produced by the group ratchet: `welcome` is the already-sealed Welcome the
    // caller obtained from MlsClient::addMembers and is shipped verbatim.
    void sendHandshake(const ConversationId &conversation, const DeviceId &recipientDevice,
                       const QByteArray &welcome);

    // Accepts a previously stashed inbound contact-handshake Welcome. Authenticates
    // the Welcome's sole other member against `claimedSenderDevice` BEFORE any
    // durable join (inspectWelcome is side-effect-free), then joins the group and
    // commits the ratchet advance atomically with the PendingIncoming->Accepted
    // flip and the stash delete. `welcome` is the exact stashed Welcome bytes.
    void acceptHandshake(const ConversationId &conversation, const AccountId &senderAccount,
                         const DeviceId &claimedSenderDevice, const QByteArray &welcome);

    // Sends a voice-call control message: an ordinary MLS application message on
    // the durable path, so an offer or a hangup is retried until the relay takes
    // it. Carries no visible message row.
    void sendCallSignal(const ConversationId &conversation, const DeviceId &recipientDevice,
                        const QByteArray &payload);

    // Sends the local user's self-published profile (presence, status line,
    // picture) as an MLS application message on the durable path, so a change
    // made while a contact is offline still reaches them. `payload` is an
    // encoded ProfileUpdateMessage. Carries no visible message row.
    void sendProfileUpdate(const ConversationId &conversation, const DeviceId &recipientDevice,
                           const QByteArray &payload);

    // Sends one sealed media frame on the unreliable datagram path. `payload` is
    // already encrypted under the call's media key, so this deliberately skips
    // the group ratchet: 50 frames a second through MLS would mean 50 durable
    // state commits a second, and a frame that needs retrying is already too old
    // to play. Dropped silently when the link is down.
    void sendCallMedia(const ConversationId &conversation, const DeviceId &recipientDevice,
                       const QByteArray &payload);

    // Processes an inbound envelope with its relay sequence.
    void handleEnvelope(const CiphertextEnvelopeV1 &envelope, quint64 serverSequence);

    // Processes an inbound datagram. Unlike handleEnvelope this touches neither
    // the store nor the ratchet; it only surfaces the payload.
    void handleDatagram(const CiphertextEnvelopeV1 &envelope);

    [[nodiscard]] bool isFailedClosed() const noexcept;

signals:
    // An outgoing text was durably committed (Queued or Failed) and is the exact row the
    // store holds, so the UI can show it before relay acceptance.
    void messageQueued(const MessageRecord &message);
    void messageStateChanged(const MessageId &messageId, DeliveryState state);
    void messageReceived(const MessageRecord &message);
    // A ContactAccept control message from `senderDevice` was authenticated and
    // consumed: the peer joined the conversation's group.
    void contactAcceptReceived(const OpenChat::ConversationId &conversation,
                               const OpenChat::DeviceId &senderDevice);
    // A CallSignal control message was decrypted, authenticated against the MLS
    // sender credential, and durably consumed. `payload` is its plaintext.
    void callSignalReceived(const OpenChat::ConversationId &conversation,
                            const OpenChat::DeviceId &senderDevice, const QByteArray &payload);
    // A CallMedia datagram arrived. It has NOT been decrypted here (the engine
    // holds no call keys); the payload is the sealed packet for the call layer
    // to authenticate and open.
    void callMediaReceived(const OpenChat::ConversationId &conversation,
                           const OpenChat::DeviceId &senderDevice, const QByteArray &payload);
    // A ProfileUpdate control message was decrypted, authenticated against the
    // MLS sender credential, and durably consumed. `payload` is the encoded
    // ProfileUpdateMessage plaintext.
    void profileUpdateReceived(const OpenChat::ConversationId &conversation,
                               const OpenChat::DeviceId &senderDevice, const QByteArray &payload);
    void failedClosed();
    // An inbound contact-handshake Welcome was durably stashed (not auto-joined).
    void handshakeReceived(const OpenChat::AccountId &sender, const OpenChat::DeviceId &senderDevice,
                           const OpenChat::ConversationId &conversation, qint64 receivedAtMs);
    // A stashed handshake was authenticated, joined and committed as Accepted.
    void handshakeAccepted(const OpenChat::ConversationId &conversation,
                           const OpenChat::AccountId &sender);
    // A stashed handshake failed authentication (or its join/inspect failed); no
    // MLS state was mutated. The caller treats this as a decline.
    void handshakeAuthFailed(const OpenChat::ConversationId &conversation,
                             const OpenChat::AccountId &sender);

private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace OpenChat
