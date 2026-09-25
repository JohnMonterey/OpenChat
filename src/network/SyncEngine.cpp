#include "network/SyncEngine.h"

#include "domain/Attachment.h"
#include "domain/MessageContent.h"
#include "protocol/CanonicalCborCodec.h"
#include "repositories/OutboxRepository.h" // for retryDelayMs

#include <QCryptographicHash>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace OpenChat {

namespace {

// How long an envelope may wait in the relay for a recipient that is offline.
// Conversation traffic (messages, receipts, contact and group handshakes,
// profile and group updates) gets the longest span the wire allows, so a
// recipient who opens the app days later still receives it. Call signalling
// stays short: an offer is meaningless long after the call, and must not reach
// a device that only reappears days later.
constexpr qint64 callEnvelopeLifetimeMs = 24LL * 60 * 60 * 1000; // 24h
// Attachment frames are short-lived too: a device away longer asks for what
// it lacks (the sender keeps every part), and a device that cannot read them
// (0.2.x and older never acknowledge one) is not handed a month of them on
// every reconnect.
constexpr qint64 attachmentFrameLifetimeMs = 24LL * 60 * 60 * 1000; // 24h

[[nodiscard]] constexpr qint64 envelopeLifetimeMs(EnvelopeMessageKind kind) noexcept
{
    switch (kind) {
    case EnvelopeMessageKind::CallSignal:
    case EnvelopeMessageKind::CallMedia:
        return callEnvelopeLifetimeMs;
    case EnvelopeMessageKind::AttachmentControl:
        return attachmentFrameLifetimeMs;
    default:
        return maxEnvelopeLifetimeMs;
    }
}

// The slowest uplink the retry schedule plans for: 16 KB/s, i.e. 16 bytes a
// millisecond. "Sent" only means handed to the socket; a 240 KiB page envelope
// can still be leaving the TLS buffer 15 s later on such a link, long after
// the first 1 s backoff. Re-sending it then would upload it twice and burn an
// attempt on every retry, so each attempt also waits for the envelope's own
// upload at this rate (a tenth of a second for a text).
constexpr qint64 slowestPlannedUplinkBytesPerMs = 16;

[[nodiscard]] constexpr qint64 uploadAllowanceMs(qsizetype envelopeBytes) noexcept
{
    return envelopeBytes > 0 ? envelopeBytes / slowestPlannedUplinkBytesPerMs : 0;
}

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

    [[nodiscard]] bool isFailed() const noexcept { return failed; }

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
            created + envelopeLifetimeMs(kind),
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

    // What a text this device sends encrypts: its bare UTF-8, or, answering
    // another message, the tagged reply that carries the quote.
    [[nodiscard]] static QByteArray textPayload(const QString &text,
                                                const std::optional<MessageQuote> &quote)
    {
        return encodeMessageContent(
            quote ? MessageContent::reply(text, quote->target, quote->sender, quote->body)
                  : MessageContent::text(text));
    }

    // The visible row of a text (or of an attachment's message, whose body is
    // the caption) this device sends. It is named after its ciphertext, so
    // every recipient files it under the same id and a later reply or edit can
    // refer to it.
    [[nodiscard]] MessageRecord outgoingText(const ConversationId &conversation,
                                             const QByteArray &ciphertext, const QString &text,
                                             const std::optional<MessageQuote> &quote) const
    {
        MessageRecord message{messageIdForCiphertext(ciphertext),
                              conversation,
                              config.localDeviceId,
                              MessageFlow::Outgoing,
                              ContentKind::Text,
                              text,
                              now(),
                              DeliveryState::Queued,
                              std::nullopt,
                              std::nullopt};
        message.sharedId = true;
        if (quote) {
            message.replyToId = quote->target;
            message.quotedSenderDeviceId = quote->sender;
            message.quotedBody = quoteExcerpt(quote->body);
        }
        return message;
    }

    void doEnqueueText(const ConversationId &conversation, const DeviceId &recipient,
                       const QString &text, const std::optional<MessageQuote> &quote)
    {
        if (failed)
            return;
        const auto ciphertext = mls.encrypt(conversation, textPayload(text, quote));
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

        // Queued even with no relay link: the outbox is durable, so the send
        // leaves when the link comes back, including after a restart.
        const MessageRecord message = outgoingText(conversation, ciphertext.value(), text, quote);

        const OutboxRecord outbox =
            makeOutbox(envelope->envelopeId, message.id, conversation, envelopeBytes);

        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitSend(message, outbox, mlsState).hasValue()) {
            failClosed(); // in-memory ratchet may be ahead of the store; stop.
            return;
        }
        emit q->messageQueued(message);
        emit q->messageStateChanged(message.id, DeliveryState::Queued);
        drainOutbox();
    }

    void doSendContactAccept(const ConversationId &conversation, const DeviceId &recipient)
    {
        if (failed)
            return;
        // A fixed, content-free payload: the proof is the ratchet, not the bytes.
        const auto ciphertext = mls.encrypt(conversation, QByteArrayView("ACCEPT"));
        if (!ciphertext.hasValue()) {
            failClosed();
            return;
        }
        const auto envelope = buildEnvelope(conversation, recipient, ciphertext.value(),
                                            EnvelopeMessageKind::ContactAccept);
        if (!envelope) {
            failClosed();
            return;
        }
        const OutboxRecord outbox = makeOutbox(envelope->envelopeId, MessageId::generate(),
                                               conversation, encodeCanonical(*envelope));
        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitControlSend(outbox, mlsState).hasValue()) {
            failClosed();
            return;
        }
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

    void doSendCallSignal(const ConversationId &conversation, const DeviceId &recipient,
                          const QByteArray &payload)
    {
        if (failed)
            return;
        const auto ciphertext = mls.encrypt(conversation, payload);
        if (!ciphertext.hasValue()) {
            failClosed();
            return;
        }
        const auto envelope = buildEnvelope(conversation, recipient, ciphertext.value(),
                                            EnvelopeMessageKind::CallSignal);
        if (!envelope) {
            failClosed();
            return;
        }
        // No visible message row: a call's control traffic is not conversation.
        const OutboxRecord outbox = makeOutbox(envelope->envelopeId, MessageId::generate(),
                                               conversation, encodeCanonical(*envelope));
        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitControlSend(outbox, mlsState).hasValue()) {
            failClosed();
            return;
        }
        emit q->callSignalSent(conversation, config.localDeviceId, payload);
        drainOutbox();
    }

    void doSendProfileUpdate(const ConversationId &conversation, const DeviceId &recipient,
                             const QByteArray &payload)
    {
        if (failed)
            return;
        const auto ciphertext = mls.encrypt(conversation, payload);
        if (!ciphertext.hasValue()) {
            failClosed();
            return;
        }
        const auto envelope = buildEnvelope(conversation, recipient, ciphertext.value(),
                                            EnvelopeMessageKind::ProfileUpdate);
        if (!envelope) {
            failClosed();
            return;
        }
        // No visible message row: a profile change is not conversation.
        const OutboxRecord outbox = makeOutbox(envelope->envelopeId, MessageId::generate(),
                                               conversation, encodeCanonical(*envelope));
        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitControlSend(outbox, mlsState).hasValue()) {
            failClosed();
            return;
        }
        drainOutbox();
    }

    // One envelope per recipient over the same ciphertext, each with its own
    // envelope id, idempotency key and signature.
    [[nodiscard]] std::optional<QVector<OutboxRecord>>
    buildFanOut(const ConversationId &conversation, const QList<DeviceId> &recipients,
                const QByteArray &ciphertext, EnvelopeMessageKind kind, const MessageId &messageId)
    {
        QVector<OutboxRecord> outboxes;
        outboxes.reserve(recipients.size());
        for (const DeviceId &recipient : recipients) {
            const auto envelope = buildEnvelope(conversation, recipient, ciphertext, kind);
            if (!envelope)
                return std::nullopt;
            outboxes.append(makeOutbox(envelope->envelopeId, messageId, conversation,
                                       encodeCanonical(*envelope)));
        }
        return outboxes;
    }

    void doEnqueueGroupText(const ConversationId &conversation, const QList<DeviceId> &recipients,
                            const QString &text, const std::optional<MessageQuote> &quote)
    {
        if (failed || recipients.isEmpty())
            return;
        const auto ciphertext = mls.encrypt(conversation, textPayload(text, quote));
        if (!ciphertext.hasValue()) {
            failClosed();
            return;
        }
        // Queued with or without a relay link, as in doEnqueueText.
        const MessageRecord message = outgoingText(conversation, ciphertext.value(), text, quote);
        const auto outboxes = buildFanOut(conversation, recipients, ciphertext.value(),
                                          EnvelopeMessageKind::MlsPrivateMessage, message.id);
        if (!outboxes) {
            failClosed();
            return;
        }

        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitGroupSend(message, *outboxes, mlsState).hasValue()) {
            failClosed();
            return;
        }
        fanOut.insert(message.id.bytes(), FanOutProgress{outboxes->size(), false});
        emit q->messageQueued(message);
        emit q->messageStateChanged(message.id, DeliveryState::Queued);
        drainOutbox();
    }

    // Everything that would make an attachment message fail once encrypted,
    // or reach nobody who could show it, found while nothing is encrypted yet.
    [[nodiscard]] AttachmentSendRefusal attachmentRefusal(const ConversationId &conversation,
                                                          const QList<DeviceId> &recipients,
                                                          const MessageContent &content,
                                                          const QByteArray &payload)
    {
        if (failed)
            return AttachmentSendRefusal::FailedClosed;
        if (recipients.isEmpty())
            return AttachmentSendRefusal::NoRecipients;
        if (!content.attachment || !isValidDescriptor(*content.attachment))
            return AttachmentSendRefusal::Invalid;
        // The encoder refuses exactly what every receiver's decoder would, and
        // the cap keeps the plaintext well inside MLS's own.
        if (payload.isEmpty() || payload.size() > AttachmentLimits::maxMessageBytes)
            return AttachmentSendRefusal::TooLarge;
        const auto unused =
            store.canEnqueueAttachment(conversation, content.attachment->attachmentId);
        if (!unused.hasValue())
            return AttachmentSendRefusal::StoreError;
        if (!unused.value())
            return AttachmentSendRefusal::Duplicate;
        return AttachmentSendRefusal::None;
    }

    void doEnqueueAttachment(const ConversationId &conversation, const QList<DeviceId> &recipients,
                             bool group, const MessageContent &content,
                             const std::optional<MessageQuote> &quote)
    {
        const QByteArray payload = encodeMessageContent(content);
        // Asked again here: the lane may have run another send since the
        // caller was told None, and a commit refused after encrypting would
        // stop the engine.
        if (attachmentRefusal(conversation, recipients, content, payload)
            != AttachmentSendRefusal::None)
            return;
        const auto ciphertext = mls.encrypt(conversation, payload);
        if (!ciphertext.hasValue()) {
            failClosed();
            return;
        }
        // One row and its descriptor, one envelope per recipient, as a group
        // text is sent; queued with or without a relay link.
        MessageRecord message = outgoingText(conversation, ciphertext.value(), content.body, quote);
        message.kind = ContentKind::Attachment;
        message.attachment = content.attachment;
        const auto outboxes = buildFanOut(conversation, recipients, ciphertext.value(),
                                          EnvelopeMessageKind::MlsPrivateMessage, message.id);
        if (!outboxes) {
            failClosed();
            return;
        }
        // Who the message went to, so the bytes that follow go to them alone.
        QByteArray recipientBytes;
        for (const DeviceId &recipient : recipients)
            recipientBytes.append(recipient.bytes());

        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitAttachmentSend(message, *outboxes, recipientBytes, mlsState).hasValue()) {
            failClosed(); // in-memory ratchet may be ahead of the store; stop.
            return;
        }
        if (group || outboxes->size() > 1)
            fanOut.insert(message.id.bytes(), FanOutProgress{outboxes->size(), false});
        emit q->messageQueued(message);
        emit q->messageStateChanged(message.id, DeliveryState::Queued);
        drainOutbox();
    }

    // False when nothing was queued. Never fails the engine: nothing here
    // touches the ratchet, so there is nothing a lost frame could leave
    // inconsistent (a receiver asks for a missing part again).
    [[nodiscard]] bool doSendAttachmentFrame(const ConversationId &conversation,
                                             const QList<DeviceId> &recipients,
                                             const QByteArray &frame)
    {
        if (failed)
            return false;
        // Deliberately NO mls.encrypt: the frame is already sealed under the
        // attachment's own key (security/AttachmentSeal.h), which only the
        // recipients of its MLS-encrypted descriptor hold. Outside the ratchet
        // it uses no generation, so a part can be sent again byte for byte and
        // parts may arrive in any order. Its envelopes carry a fresh id, as an
        // edit's do, so they leave the outbox once settled.
        auto outboxes = buildFanOut(conversation, recipients, frame,
                                    EnvelopeMessageKind::AttachmentControl, MessageId::generate());
        if (!outboxes)
            return false;
        for (OutboxRecord &outbox : *outboxes)
            outbox.priority = 1;
        // An EMPTY state: the store keeps the stored ratchet snapshot as it
        // is, and anything an MLS operation in this lane captured stays
        // pending for the send it belongs to.
        if (!store.commitControlSendMany(*outboxes, QByteArrayView()).hasValue())
            return false;
        drainOutbox();
        return true;
    }

    // An inbound attachment frame. Consumed (replay guard and cursor, no
    // ratchet state) and acknowledged whatever it holds; only a well-formed
    // one is surfaced, once, for the holder of the key to open. A frame can
    // never stop the engine: one the store could not take is left
    // unacknowledged and comes again.
    void doHandleAttachmentFrame(const CiphertextEnvelopeV1 &envelope, quint64 serverSequence)
    {
        const bool wellFormed = splitAttachmentFrame(envelope.ciphertext).has_value();
        const auto committed = store.commitControlReceive(
            envelope.envelopeId, envelope.senderDeviceId, serverSequence, QByteArrayView());
        if (!committed.hasValue())
            return;
        transport.acknowledge(envelope.envelopeId, serverSequence);
        if (committed.value() && wellFormed)
            emit q->attachmentFrameReceived(envelope.conversationId, envelope.senderDeviceId,
                                            envelope.ciphertext);
    }

    void doEnqueueEdit(const ConversationId &conversation, const QList<DeviceId> &recipients,
                       const MessageId &target, const QString &text)
    {
        if (failed || recipients.isEmpty() || text.trimmed().isEmpty())
            return;
        // Asked before encrypting, so an edit the store would refuse (a message
        // still queued, or one that is not ours) never moves the ratchet.
        const auto editable = store.canEditSent(conversation, target);
        if (!editable.hasValue() || !editable.value())
            return;
        const auto ciphertext =
            mls.encrypt(conversation, encodeMessageContent(MessageContent::edit(target, text)));
        if (!ciphertext.hasValue()) {
            failClosed();
            return;
        }
        // An edit is a conversation message with no row of its own: its
        // envelopes take a fresh id, so their delivery bookkeeping never
        // touches the message they change.
        const auto outboxes = buildFanOut(conversation, recipients, ciphertext.value(),
                                          EnvelopeMessageKind::MlsPrivateMessage,
                                          MessageId::generate());
        if (!outboxes) {
            failClosed();
            return;
        }
        // 0 means "never edited", so an edit is stamped no earlier than 1.
        const qint64 editedAtMs = std::max<qint64>(now(), 1);
        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitEditSend(conversation, target, text, editedAtMs, *outboxes, mlsState)
                 .hasValue()) {
            failClosed();
            return;
        }
        emit q->messageEdited(conversation, target, text, editedAtMs);
        drainOutbox();
    }

    void doSendGroupControl(const ConversationId &conversation, const QList<DeviceId> &recipients,
                            const QByteArray &payload)
    {
        if (failed || recipients.isEmpty())
            return;
        const auto ciphertext = mls.encrypt(conversation, payload);
        if (!ciphertext.hasValue()) {
            failClosed();
            return;
        }
        auto outboxes = buildFanOut(conversation, recipients, ciphertext.value(),
                                    EnvelopeMessageKind::GroupControl, MessageId::generate());
        if (!outboxes) {
            failClosed();
            return;
        }
        const QByteArray mlsState = mls.takePendingState();
        if (!store.commitControlSendMany(*outboxes, mlsState).hasValue()) {
            failClosed();
            return;
        }
        drainOutbox();
    }

    void doSendGroupChange(const ConversationId &conversation,
                           const QList<DeviceId> &existingRecipients, const QByteArray &commit,
                           const QList<DeviceId> &newRecipients, const QByteArray &welcome)
    {
        if (failed)
            return;
        // Deliberately NO mls.encrypt: a Commit is group handshake traffic the
        // members' ratchets process as-is, and a Welcome is already sealed to
        // its recipients' KeyPackages (see doSendHandshake).
        QVector<OutboxRecord> outboxes;
        if (!existingRecipients.isEmpty() && !commit.isEmpty()) {
            auto commits = buildFanOut(conversation, existingRecipients, commit,
                                       EnvelopeMessageKind::MlsCommit, MessageId::generate());
            if (!commits) {
                failClosed();
                return;
            }
            outboxes += *commits;
        }
        if (!newRecipients.isEmpty() && !welcome.isEmpty()) {
            auto welcomes = buildFanOut(conversation, newRecipients, welcome,
                                        EnvelopeMessageKind::GroupWelcome, MessageId::generate());
            if (!welcomes) {
                failClosed();
                return;
            }
            outboxes += *welcomes;
        }
        const QByteArray mlsState = mls.takePendingState();
        if (outboxes.isEmpty()) {
            // Nobody else to tell (a group created with members who all failed
            // to claim, or the last member removed): persist the epoch alone.
            if (!mlsState.isEmpty() && !store.commitMlsStateOnly(mlsState).hasValue())
                failClosed();
            return;
        }
        if (!store.commitControlSendMany(outboxes, mlsState).hasValue()) {
            failClosed();
            return;
        }
        drainOutbox();
    }

    void doSendCallMedia(const ConversationId &conversation, const DeviceId &recipient,
                         const QByteArray &payload)
    {
        if (failed)
            return;
        // Deliberately NO mls.encrypt and NO durable write. The payload arrives
        // already sealed under the call's media key, and a media frame that
        // needed persisting or retrying would be stale long before it was
        // replayed. The envelope is still signed, so the relay and the recipient
        // authenticate its origin exactly as they do a durable one.
        const auto envelope = buildEnvelope(conversation, recipient, payload,
                                            EnvelopeMessageKind::CallMedia);
        if (!envelope)
            return; // a signing failure drops this frame, it does not fail the session
        transport.sendDatagram(*envelope);
    }

    // True while the link already holds more unsent bytes than the drain may
    // add to. A transport that cannot tell (-1) never holds the drain back, so
    // one without a backlog figure drains exactly as it always has.
    [[nodiscard]] bool linkIsBacklogged() const
    {
        if (config.maxDrainBacklogBytes <= 0)
            return false;
        return transport.pendingSendBytes() > config.maxDrainBacklogBytes;
    }

    // Hands due envelopes to the link one at a time, in the store's order
    // (earliest due, then first queued), up to drainBatch per call. The
    // backlog is looked at again before every claim: one large envelope can
    // fill the socket on its own, and a batch claimed up front would pile
    // everything behind it (a chat message, a call offer) into the TLS buffer
    // too. A row that is not claimed stays Pending and spends no attempt; the
    // retry timer comes back for it within a second.
    void drainOutbox()
    {
        const qint64 nowMs = now();
        for (int claimed = 0; claimed < config.drainBatch; ++claimed) {
            if (failed || !transport.isConnected() || linkIsBacklogged())
                return;
            const auto due = store.claimDue(nowMs, 1, nowMs + config.leaseMs);
            if (!due.hasValue() || due.value().isEmpty())
                return;
            sendClaimed(due.value().constFirst(), nowMs);
        }
    }

    void sendClaimed(const OutboxRecord &record, qint64 nowMs)
    {
        const auto decoded = decodeEnvelope(record.envelope);
        if (record.attemptCount >= config.maxSendAttempts) {
            inflight.remove(record.envelopeId.bytes());
            failOne(record.envelopeId, record.messageId);
            // A frame is only handed over while the link is up, so a relay
            // that took none of its attempts will not take it for this device.
            if (decoded.hasValue() && decoded.value().messageKind == EnvelopeMessageKind::AttachmentControl)
                emit q->recipientUnreachable(decoded.value().conversationId, decoded.value().recipientDeviceId);
            return;
        }
        if (!decoded.hasValue()) {
            (void)store.advanceDeliveryState(record.messageId, DeliveryState::Failed);
            (void)store.scheduleRetry(record.envelopeId, record.attemptCount,
                                      nowMs + maxEnvelopeLifetimeMs);
            return;
        }
        // Queued offline for longer than its lifetime: the relay would
        // refuse it on every attempt, so give up now rather than retrying.
        if (decoded.value().expiresAtMs <= nowMs) {
            inflight.remove(record.envelopeId.bytes());
            failOne(record.envelopeId, record.messageId);
            return;
        }
        inflight.insert(record.envelopeId.bytes(),
                        {record.messageId, decoded.value().conversationId, decoded.value().recipientDeviceId});
        if (store.advanceDeliveryState(record.messageId, DeliveryState::Sending).hasValue()
            && decoded.value().messageKind == EnvelopeMessageKind::MlsPrivateMessage)
            emit q->messageStateChanged(record.messageId, DeliveryState::Sending);
        // Schedule the next attempt; relay acceptance cancels it via markAccepted.
        // The allowance keeps a large envelope from being re-sent while its
        // first copy may still be uploading.
        const qint64 nextMs = nowMs + retryDelayMs(record.attemptCount, 0)
                              + uploadAllowanceMs(record.envelope.size());
        (void)store.scheduleRetry(record.envelopeId, record.attemptCount + 1, nextMs);
        transport.sendEnvelope(decoded.value());
    }

    // Gives up on one envelope. For an ordinary send that fails the message; for
    // a multi-recipient message it only retires this recipient's envelope, and
    // the message fails only once every envelope has failed without a single
    // relay acceptance.
    void failOne(const EnvelopeId &envelopeId, const MessageId &messageId)
    {
        const auto progress = fanOut.find(messageId.bytes());
        if (progress == fanOut.end()) {
            if (store.failSend(envelopeId, messageId).hasValue())
                emit q->messageStateChanged(messageId, DeliveryState::Failed);
            return;
        }
        if (!store.failEnvelope(envelopeId).hasValue())
            return;
        if (--progress->pending > 0 || progress->accepted)
            return;
        fanOut.erase(progress);
        if (store.advanceDeliveryState(messageId, DeliveryState::Failed).hasValue())
            emit q->messageStateChanged(messageId, DeliveryState::Failed);
    }

    // The relay will never hold this envelope: its recipient device does not
    // exist or was retired (a login elsewhere). Older relays said this for
    // any recipient that was not connected. Either way the send has nowhere
    // to wait.
    void onUnavailable(const EnvelopeId &envelopeId)
    {
        const auto it = inflight.constFind(envelopeId.bytes());
        if (it == inflight.cend())
            return;
        const InflightSend send = it.value();
        inflight.remove(envelopeId.bytes());
        failOne(envelopeId, send.messageId);
        emit q->recipientUnreachable(send.conversation, send.recipient);
    }

    void onAccepted(const EnvelopeId &envelopeId, quint64 /*serverSequence*/)
    {
        (void)store.markAccepted(envelopeId);
        const auto it = inflight.constFind(envelopeId.bytes());
        if (it != inflight.cend()) {
            const MessageId messageId = it.value().messageId;
            inflight.erase(it);
            const auto progress = fanOut.find(messageId.bytes());
            if (progress != fanOut.end()) {
                progress->accepted = true;
                if (--progress->pending <= 0)
                    fanOut.erase(progress);
            }
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
        // A group Welcome likewise names a group we have not joined, but it is
        // joined outright (from a trusted sender) rather than stashed.
        if (envelope.messageKind == EnvelopeMessageKind::GroupWelcome) {
            doHandleGroupWelcome(envelope, serverSequence);
            return;
        }
        // An attachment frame is no MLS message at all (see
        // doSendAttachmentFrame), so it never reaches mls.process. Its sender
        // is the device the relay authenticated; what it carries is only
        // trusted once it opens under a descriptor's key and the assembled
        // bytes match that descriptor's MLS-authenticated hash.
        if (envelope.messageKind == EnvelopeMessageKind::AttachmentControl) {
            doHandleAttachmentFrame(envelope, serverSequence);
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

            // Application-layer control traffic (a receipt, a contact-accept) never
            // becomes a visible message row: consume it as a control receive and
            // surface only the typed outcome.
            if (envelope.messageKind != EnvelopeMessageKind::MlsPrivateMessage) {
                const QByteArray mlsState = mls.takePendingState();
                const auto committed = store.commitControlReceive(
                    envelope.envelopeId, envelope.senderDeviceId, serverSequence, mlsState);
                if (!committed.hasValue()) {
                    failClosed();
                    return;
                }
                transport.acknowledge(envelope.envelopeId, serverSequence);
                if (!committed.value())
                    return; // an idempotent redelivery; already surfaced once
                if (envelope.messageKind == EnvelopeMessageKind::ContactAccept)
                    emit q->contactAcceptReceived(envelope.conversationId, envelope.senderDeviceId);
                else if (envelope.messageKind == EnvelopeMessageKind::CallSignal)
                    emit q->callSignalReceived(envelope.conversationId, envelope.senderDeviceId,
                                               processed.value().applicationData);
                else if (envelope.messageKind == EnvelopeMessageKind::ProfileUpdate)
                    emit q->profileUpdateReceived(envelope.conversationId,
                                                  envelope.senderDeviceId,
                                                  processed.value().applicationData);
                else if (envelope.messageKind == EnvelopeMessageKind::GroupControl)
                    emit q->groupControlReceived(envelope.conversationId,
                                                 envelope.senderDeviceId,
                                                 processed.value().applicationData);
                return;
            }

            const auto content = decodeMessageContent(processed.value().applicationData);
            const QByteArray mlsState = mls.takePendingState();

            // Tagged but unreadable here (malformed, or from a later version):
            // consumed like control traffic, with nothing shown.
            if (!content) {
                const auto consumed = store.commitControlReceive(
                    envelope.envelopeId, envelope.senderDeviceId, serverSequence, mlsState);
                if (!consumed.hasValue()) {
                    failClosed();
                    return;
                }
                transport.acknowledge(envelope.envelopeId, serverSequence);
                return;
            }

            // The store applies an edit only to a message this sender sent
            // into this conversation, so an edit of anyone else's is consumed
            // and changes nothing.
            if (content->type == MessageContent::Type::Edit) {
                const auto outcome = store.commitEditReceive(
                    envelope.envelopeId, envelope.senderDeviceId, envelope.conversationId,
                    *content->target, content->body, envelope.createdAtMs, serverSequence,
                    mlsState);
                if (!outcome.hasValue()) {
                    failClosed();
                    return;
                }
                transport.acknowledge(envelope.envelopeId, serverSequence);
                if (outcome.value() == EditReceiveOutcome::Applied)
                    emit q->messageEdited(envelope.conversationId, *content->target,
                                          content->body, envelope.createdAtMs);
                return;
            }

            // Filed under the id the sender gave it: both derive it from the
            // ciphertext they share. An attachment is a row like a text's,
            // its body the caption, stored with its descriptor; its bytes
            // follow as frames.
            const bool isAttachment = content->type == MessageContent::Type::Attachment;
            MessageRecord message{messageIdForCiphertext(envelope.ciphertext),
                                  envelope.conversationId,
                                  envelope.senderDeviceId,
                                  MessageFlow::Incoming,
                                  isAttachment ? ContentKind::Attachment : ContentKind::Text,
                                  content->body,
                                  envelope.createdAtMs,
                                  DeliveryState::Delivered,
                                  std::optional<quint64>(serverSequence),
                                  std::nullopt};
            message.sharedId = true;
            if (content->type == MessageContent::Type::Reply || (isAttachment && content->target)) {
                message.replyToId = content->target;
                message.quotedSenderDeviceId = content->quotedSender;
                message.quotedBody = content->quotedBody;
            }
            // The store keeps no descriptor for an id this conversation has
            // already used (the message would borrow another attachment's
            // bytes), so the message surfaced now says the same as the one
            // read back later: it has none, and shows as unavailable.
            if (isAttachment) {
                const auto fresh = store.canEnqueueAttachment(envelope.conversationId,
                                                              content->attachment->attachmentId);
                if (!fresh.hasValue() || fresh.value())
                    message.attachment = content->attachment;
            }

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

    void doHandleGroupWelcome(const CiphertextEnvelopeV1 &envelope, quint64 serverSequence)
    {
        if (failed)
            return;
        // Consumes the envelope without joining: replay-guard + watermark only,
        // then acknowledge so the relay stops redelivering it.
        const auto refuse = [&] {
            const auto consumed = store.commitGroupWelcome(
                envelope.envelopeId, envelope.senderDeviceId, envelope.conversationId,
                serverSequence, envelope.createdAtMs, QByteArrayView(), /*joined=*/false);
            if (!consumed.hasValue()) {
                failClosed();
                return;
            }
            transport.acknowledge(envelope.envelopeId, serverSequence);
        };

        // Only an Accepted contact may put this device into a group, and only
        // into one it does not already hold: a Welcome for a known group would
        // overwrite the ratchet state this device already has for it.
        const auto allowed = store.canJoinGroup(envelope.senderAccountId, envelope.conversationId);
        if (!allowed.hasValue()) {
            failClosed();
            return;
        }
        if (!allowed.value()) {
            refuse();
            return;
        }
        // Authenticate BEFORE joining: the Welcome's membership must name the
        // relay-claimed sender device, so a relay cannot attribute a contact's
        // group to a stranger. inspectWelcome is read-only.
        const auto members = mls.inspectWelcome(envelope.ciphertext);
        if (!members.hasValue()) {
            refuse();
            return;
        }
        const bool namesSender = std::any_of(
            members.value().cbegin(), members.value().cend(), [&](const QByteArray &credential) {
                return credentialNamesDevice(credential, envelope.senderDeviceId);
            });
        if (!namesSender || !mls.joinGroup(envelope.conversationId, envelope.ciphertext).hasValue()) {
            refuse();
            return;
        }
        const QByteArray mlsState = mls.takePendingState();
        const auto committed = store.commitGroupWelcome(
            envelope.envelopeId, envelope.senderDeviceId, envelope.conversationId, serverSequence,
            envelope.createdAtMs, mlsState, /*joined=*/true);
        if (!committed.hasValue()) {
            failClosed();
            return;
        }
        transport.acknowledge(envelope.envelopeId, serverSequence);
        if (committed.value())
            emit q->groupWelcomeReceived(envelope.conversationId, envelope.senderAccountId,
                                         envelope.senderDeviceId, members.value());
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
    // Envelopes handed to the link and not yet settled, by envelope id.
    struct InflightSend final {
        MessageId messageId;
        ConversationId conversation;
        DeviceId recipient;
    };
    QHash<QByteArray, InflightSend> inflight;
    // Multi-recipient messages still waiting on some envelope, keyed by message
    // id: how many envelopes are unresolved and whether any was accepted.
    struct FanOutProgress final {
        qsizetype pending = 0;
        bool accepted = false;
    };
    QHash<QByteArray, FanOutProgress> fanOut;
    QTimer retryTimer;
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

SyncEngine::~SyncEngine() { stop(); }

void SyncEngine::start()
{
    if (d->started)
        return;
    d->started = true;
    d->retryTimer.setInterval(1000);
    connect(&d->retryTimer, &QTimer::timeout, this, [this] { d->drainOutbox(); });
    d->retryTimer.start();
    d->transport.onRecipientUnavailable = [this](const EnvelopeId &id) { d->onUnavailable(id); };
    d->transport.onRelayAccepted = [this](const EnvelopeId &id, quint64 seq) {
        d->onAccepted(id, seq);
    };
    d->transport.onEnvelope = [this](const CiphertextEnvelopeV1 &envelope, quint64 seq) {
        handleEnvelope(envelope, seq);
    };
    d->transport.onDatagram = [this](const CiphertextEnvelopeV1 &envelope) {
        handleDatagram(envelope);
    };
    d->transport.onConnected = [this] {
        // Resume: drain the durable outbox once the link is up, then tell the
        // callers that pace their own sends, which queue behind what drained.
        d->drainOutbox();
        emit linkUp();
    };
    // Kick an initial drain (e.g. offline items queued before start / on restart).
    d->drainOutbox();
}

void SyncEngine::stop()
{
    d->started = false;
    d->retryTimer.stop();
    disconnect(&d->retryTimer, nullptr, this, nullptr);
    d->transport.onRecipientUnavailable = nullptr;
    d->transport.onRelayAccepted = nullptr;
    d->transport.onEnvelope = nullptr;
    d->transport.onDatagram = nullptr;
    d->transport.onConnected = nullptr;
}

void SyncEngine::enqueueText(const ConversationId &conversation, const DeviceId &recipientDevice,
                            const QString &text, const std::optional<MessageQuote> &quote)
{
    d->coordinator.run(conversation,
                       [this, conversation, recipientDevice, text, quote] {
                           d->doEnqueueText(conversation, recipientDevice, text, quote);
                       });
}

void SyncEngine::enqueueEdit(const ConversationId &conversation, const QList<DeviceId> &recipients,
                            const MessageId &target, const QString &text)
{
    d->coordinator.run(conversation, [this, conversation, recipients, target, text] {
        d->doEnqueueEdit(conversation, recipients, target, text);
    });
}

void SyncEngine::sendContactAccept(const ConversationId &conversation,
                                   const DeviceId &recipientDevice)
{
    d->coordinator.run(conversation, [this, conversation, recipientDevice] {
        d->doSendContactAccept(conversation, recipientDevice);
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

void SyncEngine::sendCallSignal(const ConversationId &conversation,
                                const DeviceId &recipientDevice, const QByteArray &payload)
{
    d->coordinator.run(conversation, [this, conversation, recipientDevice, payload] {
        d->doSendCallSignal(conversation, recipientDevice, payload);
    });
}

void SyncEngine::sendProfileUpdate(const ConversationId &conversation,
                                   const DeviceId &recipientDevice, const QByteArray &payload)
{
    d->coordinator.run(conversation, [this, conversation, recipientDevice, payload] {
        d->doSendProfileUpdate(conversation, recipientDevice, payload);
    });
}

void SyncEngine::enqueueGroupText(const ConversationId &conversation,
                                  const QList<DeviceId> &recipients, const QString &text,
                                  const std::optional<MessageQuote> &quote)
{
    d->coordinator.run(conversation, [this, conversation, recipients, text, quote] {
        d->doEnqueueGroupText(conversation, recipients, text, quote);
    });
}

void SyncEngine::sendGroupControl(const ConversationId &conversation,
                                  const QList<DeviceId> &recipients, const QByteArray &payload)
{
    d->coordinator.run(conversation, [this, conversation, recipients, payload] {
        d->doSendGroupControl(conversation, recipients, payload);
    });
}

void SyncEngine::sendGroupChange(const ConversationId &conversation,
                                 const QList<DeviceId> &existingRecipients,
                                 const QByteArray &commit, const QList<DeviceId> &newRecipients,
                                 const QByteArray &welcome)
{
    d->coordinator.run(conversation, [this, conversation, existingRecipients, commit,
                                      newRecipients, welcome] {
        d->doSendGroupChange(conversation, existingRecipients, commit, newRecipients, welcome);
    });
}

AttachmentSendRefusal SyncEngine::enqueueAttachment(const ConversationId &conversation,
                                                     const QList<DeviceId> &recipients, bool group,
                                                     const QString &caption,
                                                     const AttachmentDescriptor &attachment,
                                                     const std::optional<MessageQuote> &quote)
{
    // Checked here, before the lane runs the send, so the caller learns of a
    // refusal while it can still say so; the task checks again when it runs.
    const MessageContent content = MessageContent::attachmentMessage(caption, attachment, quote);
    const auto refusal =
        d->attachmentRefusal(conversation, recipients, content, encodeMessageContent(content));
    if (refusal != AttachmentSendRefusal::None)
        return refusal;
    d->coordinator.run(conversation, [this, conversation, recipients, group, content, quote] {
        d->doEnqueueAttachment(conversation, recipients, group, content, quote);
    });
    return AttachmentSendRefusal::None;
}

bool SyncEngine::sendAttachmentFrame(const ConversationId &conversation,
                                     const QList<DeviceId> &recipients, const QByteArray &frame)
{
    if (d->isFailed() || recipients.isEmpty() || frame.size() > AttachmentLimits::maxFrameBytes
        || !splitAttachmentFrame(frame))
        return false;
    // Through the conversation's lane, like every send, so a frame never
    // overtakes a send already under way there (the message it follows).
    // When the lane is busy the frame is queued behind it and counts as sent;
    // the pump sees the outbox either way.
    const auto sent = std::make_shared<std::optional<bool>>();
    d->coordinator.run(conversation, [this, conversation, recipients, frame, sent] {
        *sent = d->doSendAttachmentFrame(conversation, recipients, frame);
    });
    return sent->value_or(true);
}

int SyncEngine::pendingAttachmentFrames() const
{
    const auto pending = d->store.pendingLowPriorityCount();
    return pending.hasValue() ? pending.value() : 0;
}

qint64 SyncEngine::pendingSendBytes() const
{
    return d->transport.pendingSendBytes();
}

bool SyncEngine::isLinkUp() const
{
    return d->transport.isConnected();
}

void SyncEngine::sendCallMedia(const ConversationId &conversation,
                               const DeviceId &recipientDevice, const QByteArray &payload)
{
    // Deliberately NOT serialized through the coordinator: it exists to keep MLS
    // ratchet mutations for one conversation from interleaving, and this path
    // touches neither the ratchet nor the store. Queueing media behind an
    // in-flight durable send would add exactly the latency it must avoid.
    d->doSendCallMedia(conversation, recipientDevice, payload);
}

void SyncEngine::handleDatagram(const CiphertextEnvelopeV1 &envelope)
{
    if (d->isFailed())
        return;
    // A datagram is not stored, sequenced or acknowledged, so there is no replay
    // guard to consult and nothing to commit. The only kind that may arrive this
    // way is call media; anything else is dropped rather than interpreted.
    if (envelope.messageKind != EnvelopeMessageKind::CallMedia)
        return;
    emit callMediaReceived(envelope.conversationId, envelope.senderDeviceId, envelope.ciphertext);
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
