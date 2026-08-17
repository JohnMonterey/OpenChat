#include "network/SyncEngine.h"

#include "protocol/CanonicalCborCodec.h"
#include "repositories/OutboxRepository.h" // for retryDelayMs

#include <QCryptographicHash>

#include <utility>

namespace OpenChat {

namespace {

constexpr qint64 envelopeLifetimeMs = 24LL * 60 * 60 * 1000; // 24h

// DevicePublicCredential serializes as: version(1) || deviceId(16) || key(32).
constexpr char credentialVersion = 1;
constexpr qsizetype credentialDeviceIdOffset = 1;

// True iff the MLS-authenticated credential names exactly the device the
// envelope claims as its sender. A relay cannot forge the credential (MLS binds
// it), so a mismatch means the relay is trying to misattribute the plaintext.
[[nodiscard]] bool credentialNamesDevice(QByteArrayView credential, const DeviceId &claimed)
{
    if (credential.size() < credentialDeviceIdOffset + DeviceId::byteCount)
        return false;
    if (credential.at(0) != credentialVersion)
        return false;
    return credential.sliced(credentialDeviceIdOffset, DeviceId::byteCount)
        == QByteArrayView(claimed.bytes());
}

} // namespace

class SyncEngine::Private
{
public:
    Private(Config config, SyncStore &store, SyncMlsSession &mls, SyncTransport &transport,
            Signer signer, Clock clock, SyncEngine *owner)
        : q(owner)
        , config(std::move(config))
        , store(store)
        , mls(mls)
        , transport(transport)
        , signer(std::move(signer))
        , clock(std::move(clock))
    {
    }

    [[nodiscard]] qint64 now() const { return clock ? clock() : 0; }

    [[nodiscard]] OutboxRecord makeOutbox(const EnvelopeId &envelopeId, const MessageId &messageId,
                                          const ConversationId &conversation,
                                          const QByteArray &envelopeBytes) const
    {
        return OutboxRecord{envelopeId, messageId, conversation, envelopeBytes,
                            0,          now(),     0,            OutboxState::Pending};
    }

    void failClosed()
    {
        if (failed)
            return;
        failed = true;
        emit q->failedClosed();
    }

    // Builds and signs a canonical envelope for a ciphertext payload.
    [[nodiscard]] std::optional<CiphertextEnvelopeV1>
    buildEnvelope(const ConversationId &conversation, const DeviceId &recipient,
                  const QByteArray &ciphertext, EnvelopeMessageKind kind)
    {
        const qint64 created = now();
        CiphertextEnvelopeV1 envelope{
            1,
            EnvelopeId::generate(),
            config.localAccountId,
            config.localDeviceId,
            recipient,
            conversation,
            kind,
            created,
            created + envelopeLifetimeMs,
            EnvelopeId::generate(),
            ciphertext,
            QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256),
            QByteArray()};

        // Sign the canonical encoding with the signature field cleared — the same
        // input the relay and recipients verify, derived from the one shared
        // codec definition so signer and verifier cannot drift apart.
        const QByteArray signingInput = encodeForSignature(envelope);
        if (signingInput.isEmpty() || !signer)
            return std::nullopt;
        const QByteArray signature = signer(signingInput);
        if (signature.size() != 64)
            return std::nullopt;
        envelope.senderSignature = signature;
        return envelope;
    }

    void doEnqueueText(const ConversationId &conversation, const DeviceId &recipient,
                       const QString &text)
    {
        if (failed)
            return;
        const auto ciphertext = mls.encrypt(conversation, text.toUtf8());
        if (!ciphertext.hasValue()) {
            failClosed();
            return;
        }
        const auto envelope =
            buildEnvelope(conversation, recipient, ciphertext.value(),
                          EnvelopeMessageKind::MlsPrivateMessage);
        if (!envelope) {
            failClosed();
            return;
        }
        const QByteArray envelopeBytes = encodeCanonical(*envelope);

        const MessageRecord message{MessageId::generate(),
                                    conversation,
                                    config.localDeviceId,
                                    MessageFlow::Outgoing,
                                    ContentKind::Text,
                                    text,
                                    now(),
                                    DeliveryState::Queued,
                                    std::nullopt,
                                    std::nullopt};

        const OutboxRecord outbox =
            makeOutbox(envelope->envelopeId, message.id, conversation, envelopeBytes);

        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitSend(message, outbox, mlsState).hasValue()) {
            failClosed(); // in-memory ratchet may be ahead of the store; stop.
            return;
        }
        emit q->messageStateChanged(message.id, DeliveryState::Queued);
        drainOutbox();
    }

    void doAcknowledgeRead(const ConversationId &conversation, const DeviceId &recipient,
                           const MessageId &messageId)
    {
        if (failed)
            return;
        // Receipt payload is bounded and contains only the message id being
        // acknowledged; it is an ordinary MLS application message to the relay.
        QByteArray payload("R", 1);
        payload.append(messageId.bytes());
        const auto ciphertext = mls.encrypt(conversation, payload);
        if (!ciphertext.hasValue()) {
            failClosed();
            return;
        }
        const auto envelope =
            buildEnvelope(conversation, recipient, ciphertext.value(), EnvelopeMessageKind::Receipt);
        if (!envelope) {
            failClosed();
            return;
        }

        // No visible message row for a receipt.
        const OutboxRecord outbox = makeOutbox(envelope->envelopeId, MessageId::generate(),
                                               conversation, encodeCanonical(*envelope));

        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitControlSend(outbox, mlsState).hasValue()) {
            failClosed();
            return;
        }
        drainOutbox();
    }

    void doSendHandshake(const ConversationId &conversation, const DeviceId &recipient,
                         const QByteArray &welcome)
    {
        if (failed)
            return;
        // Deliberately NO mls.encrypt here: the ciphertext IS the caller-supplied
        // Welcome. A Welcome is already HPKE-sealed to the recipient's claimed
        // KeyPackage, so it is confidential to that device as-is; re-encrypting it
        // through this group's application ratchet would be wrong (the recipient is
        // not yet a member and could never derive that key), which is why this path
        // diverges from doAcknowledgeRead by skipping the encrypt step. The pending
        // MLS state taken below is the createGroup + addMembers snapshot the caller
        // captured on the shared MlsClient just before invoking us; it is committed
        // atomically with the Welcome outbox by commitControlSend.
        const auto envelope =
            buildEnvelope(conversation, recipient, welcome, EnvelopeMessageKind::MlsHandshake);
        if (!envelope) {
            failClosed();
            return;
        }

        // No visible message row for a handshake control send.
        const OutboxRecord outbox = makeOutbox(envelope->envelopeId, MessageId::generate(),
                                               conversation, encodeCanonical(*envelope));

        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitControlSend(outbox, mlsState).hasValue()) {
            failClosed();
            return;
        }
        drainOutbox();
    }

    void drainOutbox()
    {
        if (failed || !transport.isConnected())
            return;
        const qint64 nowMs = now();
        const auto due = store.claimDue(nowMs, config.drainBatch, nowMs + config.leaseMs);
        if (!due.hasValue())
            return;
        for (const OutboxRecord &record : due.value()) {
            if (record.attemptCount >= config.maxSendAttempts) {
                (void)store.advanceDeliveryState(record.messageId, DeliveryState::Failed);
                emit q->messageStateChanged(record.messageId, DeliveryState::Failed);
                // Park so it is not re-claimed.
                (void)store.scheduleRetry(record.envelopeId, record.attemptCount,
                                          nowMs + envelopeLifetimeMs);
                continue;
            }
            const auto decoded = decodeEnvelope(record.envelope);
            if (!decoded.hasValue()) {
                (void)store.advanceDeliveryState(record.messageId, DeliveryState::Failed);
                (void)store.scheduleRetry(record.envelopeId, record.attemptCount,
                                          nowMs + envelopeLifetimeMs);
                continue;
            }
            inflight.insert(record.envelopeId.bytes(), record.messageId);
            transport.sendEnvelope(decoded.value());
            (void)store.advanceDeliveryState(record.messageId, DeliveryState::Sending);
            // Schedule the next attempt; relay acceptance cancels it via markAccepted.
            const qint64 nextMs = nowMs + retryDelayMs(record.attemptCount, 0);
            (void)store.scheduleRetry(record.envelopeId, record.attemptCount + 1, nextMs);
        }
    }

    void onAccepted(const EnvelopeId &envelopeId, quint64 /*serverSequence*/)
    {
        (void)store.markAccepted(envelopeId);
        const auto it = inflight.constFind(envelopeId.bytes());
        if (it != inflight.cend()) {
            const MessageId messageId = it.value();
            inflight.erase(it);
            if (store.advanceDeliveryState(messageId, DeliveryState::Sent).hasValue())
                emit q->messageStateChanged(messageId, DeliveryState::Sent);
        }
    }

    void doHandleEnvelope(const CiphertextEnvelopeV1 &envelope, quint64 serverSequence)
    {
        if (failed)
            return;
        // Idempotent redelivery (live + catch-up): ack and skip without touching
        // the ratchet — reprocessing a consumed message would fail decryption.
        const auto seen = store.hasSeen(envelope.envelopeId);
        if (seen.hasValue() && seen.value()) {
            transport.acknowledge(envelope.envelopeId, serverSequence);
            return;
        }

        // Expired in transit (e.g. a relay that withheld then replayed a very old
        // envelope): acknowledge so it stops being redelivered, but never advance
        // the ratchet for it. Bounds how far back a relay can resurrect traffic.
        if (envelope.expiresAtMs <= now()) {
            transport.acknowledge(envelope.envelopeId, serverSequence);
            return;
        }

        // A contact-handshake Welcome names a group we have not joined; feeding it
        // to mls.process would fail. Stash it durably instead and never auto-join.
        if (envelope.messageKind == EnvelopeMessageKind::MlsHandshake) {
            doHandleHandshake(envelope, serverSequence);
            return;
        }

        const auto processed = mls.process(envelope.conversationId, envelope.ciphertext);
        if (!processed.hasValue()) {
            // Stale/invalid or unbuffered future epoch: drop without surfacing
            // anything. (Bounded future-epoch buffering is a follow-up.)
            return;
        }

        if (processed.value().kind == SyncProcessOutcome::Kind::Application) {
            // The envelope's senderDeviceId is untrusted relay metadata. Only
            // accept the plaintext if the MLS-authenticated credential names the
            // same device; otherwise a relay is misattributing a decrypted
            // message. Drop without surfacing or acknowledging.
            if (!credentialNamesDevice(processed.value().senderIdentity, envelope.senderDeviceId))
                return;

            const MessageRecord message{MessageId::generate(),
                                        envelope.conversationId,
                                        envelope.senderDeviceId,
                                        MessageFlow::Incoming,
                                        ContentKind::Text,
                                        QString::fromUtf8(processed.value().applicationData),
                                        envelope.createdAtMs,
                                        DeliveryState::Delivered,
                                        std::optional<quint64>(serverSequence),
                                        std::nullopt};

            const QByteArray mlsState = mls.takePendingState();
            const auto committed =
                store.commitReceive(message, envelope.envelopeId, serverSequence, mlsState);
            if (!committed.hasValue()) {
                failClosed();
                return;
            }
            transport.acknowledge(envelope.envelopeId, serverSequence);
            if (committed.value())
                emit q->messageReceived(message);
        } else {
            const QByteArray mlsState = mls.takePendingState();
            const auto committed = store.commitControlReceive(
                envelope.envelopeId, envelope.senderDeviceId, serverSequence, mlsState);
            if (!committed.hasValue()) {
                failClosed();
                return;
            }
            transport.acknowledge(envelope.envelopeId, serverSequence);
        }
    }

    void doHandleHandshake(const CiphertextEnvelopeV1 &envelope, quint64 serverSequence)
    {
        if (failed)
            return;
        // Atomic replay-guard + is-Blocked drop + stash insert + watermark advance.
        // The ciphertext IS the raw Welcome; nothing is decrypted or joined here.
        const auto outcome = store.commitHandshakeReceive(
            envelope.envelopeId, envelope.senderAccountId, envelope.senderDeviceId,
            envelope.conversationId, /*welcome=*/envelope.ciphertext,
            /*receivedAtMs=*/envelope.createdAtMs, /*watermark=*/serverSequence);
        if (!outcome.hasValue()) {
            // Fail closed: do NOT acknowledge, so the relay redelivers and a later
            // attempt (or restart) can stash the Welcome durably.
            failClosed();
            return;
        }
        // Only acknowledge once the stash (or the durable drop/dedup) committed.
        transport.acknowledge(envelope.envelopeId, serverSequence);
        // A fresh stash is the only outcome the user must decide on. AlreadySeen and
        // DroppedBlocked are consumed silently (already acked above).
        if (outcome.value() == HandshakeReceiveOutcome::Stashed)
            emit q->handshakeReceived(envelope.senderAccountId, envelope.senderDeviceId,
                                      envelope.conversationId, envelope.createdAtMs);
    }

    void doAcceptHandshake(const ConversationId &conversation, const AccountId &senderAccount,
                           const DeviceId &claimedSenderDevice, const QByteArray &welcome)
    {
        if (failed)
            return;
        // Authenticate BEFORE joining. inspectWelcome is READ-ONLY (no ratchet
        // change), so any rejection below leaves NO MLS state to roll back.
        const auto members = mls.inspectWelcome(welcome);
        if (!members.hasValue()) {
            emit q->handshakeAuthFailed(conversation, senderAccount);
            return;
        }
        // A contact handshake is a 2-party group: exactly one other member, whose
        // MLS-authenticated credential must name the relay-claimed sender device.
        // credentialNamesDevice is the engine's single source of truth for this
        // check (reused from the application-message receive path).
        //
        // Trust boundary: the MLS credential authenticates the DEVICE only; the
        // device->account binding is asserted by the authenticated relay directory
        // that produced this request (the same boundary as the 8a/8b accept paths).
        if (members.value().size() != 1
            || !credentialNamesDevice(members.value().front(), claimedSenderDevice)) {
            emit q->handshakeAuthFailed(conversation, senderAccount); // no MLS mutation happened
            return;
        }
        // Auth passed: join now. joinGroup advances the ratchet and captures the
        // pending state; a failed join captured no state to discard.
        if (!mls.joinGroup(conversation, welcome).hasValue()) {
            emit q->handshakeAuthFailed(conversation, senderAccount);
            return;
        }
        const QByteArray mlsState = mls.takePendingState();
        // Forward the ALREADY-authenticated credential's 32 identity bytes so the
        // store binds the peer signing key atomically with the accept. The engine
        // only slices the front member's credential (version(1) || deviceId(16) ||
        // signingKey(32)); it never inspects application plaintext, staying
        // content-blind. A short credential yields an empty key (bound NULL).
        const QByteArray peerKey =
            members.value().front().size() >= 49 ? members.value().front().sliced(17, 32)
                                                 : QByteArray();
        // Persist the just-joined ratchet atomically with the PendingIncoming->
        // Accepted flip and the stash delete. On failure, fail closed so the
        // in-memory join is discarded on restart rather than left ahead of the store.
        if (!store.commitHandshakeAccept(senderAccount, conversation, now(), mlsState, peerKey)
                 .hasValue()) {
            failClosed();
            return;
        }
        emit q->handshakeAccepted(conversation, senderAccount);
    }

    SyncEngine *q;
    Config config;
    SyncStore &store;
    SyncMlsSession &mls;
    SyncTransport &transport;
    Signer signer;
    Clock clock;
    MlsTransactionCoordinator coordinator;
    QHash<QByteArray, MessageId> inflight;
    bool failed = false;
    bool started = false;
};

SyncEngine::SyncEngine(Config config, SyncStore &store, SyncMlsSession &mls,
                       SyncTransport &transport, Signer signer, Clock clock, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(std::move(config), store, mls, transport, std::move(signer),
                                  std::move(clock), this))
{
}

SyncEngine::~SyncEngine() = default;

void SyncEngine::start()
{
    if (d->started)
        return;
    d->started = true;
    d->transport.onRelayAccepted = [this](const EnvelopeId &id, quint64 seq) {
        d->onAccepted(id, seq);
    };
    d->transport.onEnvelope = [this](const CiphertextEnvelopeV1 &envelope, quint64 seq) {
        handleEnvelope(envelope, seq);
    };
    d->transport.onConnected = [this] {
        // Resume: drain the durable outbox once the link is up.
        d->drainOutbox();
    };
    // Kick an initial drain (e.g. offline items queued before start / on restart).
    d->drainOutbox();
}

void SyncEngine::stop()
{
    d->started = false;
    d->transport.onRelayAccepted = nullptr;
    d->transport.onEnvelope = nullptr;
    d->transport.onConnected = nullptr;
}

void SyncEngine::enqueueText(const ConversationId &conversation, const DeviceId &recipientDevice,
                            const QString &text)
{
    d->coordinator.run(conversation,
                       [this, conversation, recipientDevice, text] {
                           d->doEnqueueText(conversation, recipientDevice, text);
                       });
}

void SyncEngine::acknowledgeRead(const ConversationId &conversation, const DeviceId &recipientDevice,
                                 const MessageId &messageId)
{
    d->coordinator.run(conversation, [this, conversation, recipientDevice, messageId] {
        d->doAcknowledgeRead(conversation, recipientDevice, messageId);
    });
}

void SyncEngine::sendHandshake(const ConversationId &conversation, const DeviceId &recipientDevice,
                               const QByteArray &welcome)
{
    d->coordinator.run(conversation, [this, conversation, recipientDevice, welcome] {
        d->doSendHandshake(conversation, recipientDevice, welcome);
    });
}

void SyncEngine::acceptHandshake(const ConversationId &conversation, const AccountId &senderAccount,
                                 const DeviceId &claimedSenderDevice, const QByteArray &welcome)
{
    d->coordinator.run(conversation, [this, conversation, senderAccount, claimedSenderDevice,
                                      welcome] {
        d->doAcceptHandshake(conversation, senderAccount, claimedSenderDevice, welcome);
    });
}

void SyncEngine::handleEnvelope(const CiphertextEnvelopeV1 &envelope, quint64 serverSequence)
{
    d->coordinator.run(envelope.conversationId, [this, envelope, serverSequence] {
        d->doHandleEnvelope(envelope, serverSequence);
    });
}

bool SyncEngine::isFailedClosed() const noexcept
{
    return d->failed;
}

} // namespace OpenChat
