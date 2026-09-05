#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>
#include <QMap>

namespace OpenChat {

enum class EnvelopeMessageKind : quint8 {
    MlsPrivateMessage = 0,
    MlsHandshake = 1,
    Receipt = 2,
    AttachmentControl = 3,
    // A contact-request acceptance: an MLS application message (encrypted
    // under the freshly joined group's ratchet) whose delivery proves to the
    // requester that the peer joined the 2-party group. Carries no user
    // content; the engine consumes it as a control receive and surfaces it as
    // contactAcceptReceived rather than as a visible message.
    ContactAccept = 4,
    // Voice-call control: an MLS application message carrying an offer, ringing
    // notice, answer or hangup. Reliable and ratcheted like any other control
    // message, and consumed by the call engine rather than shown as a row.
    CallSignal = 5,
    // One 20 ms slice of call audio, sealed under the call's own media key
    // rather than the group ratchet. Unlike every other kind this one travels
    // the datagram path: it is never stored, never retried, and dropped when the
    // recipient is offline, because a late voice frame is worse than none.
    CallMedia = 6,
    // A profile update: the sender's chosen presence, status text and (JPEG)
    // profile picture, as an MLS application message under the conversation's
    // ratchet. Durable and retried like a call signal, consumed as a control
    // receive, and never a visible row. The relay sees only ciphertext, so a
    // picture or status never leaves the two devices in the clear.
    ProfileUpdate = 7,
};

// The highest EnvelopeMessageKind the codec accepts on decode. Kept next to the
// enum so a new kind cannot be added without widening the wire bound.
inline constexpr quint8 maxEnvelopeMessageKind =
    static_cast<quint8>(EnvelopeMessageKind::ProfileUpdate);

struct CiphertextEnvelopeV1 final {
    quint8 version = 1;
    EnvelopeId envelopeId;
    AccountId senderAccountId;
    DeviceId senderDeviceId;
    DeviceId recipientDeviceId;
    ConversationId conversationId;
    EnvelopeMessageKind messageKind = EnvelopeMessageKind::MlsPrivateMessage;
    qint64 createdAtMs = 0;
    qint64 expiresAtMs = 0;
    EnvelopeId idempotencyKey;
    QByteArray ciphertext;
    QByteArray ciphertextSha256;
    QByteArray senderSignature;
    QMap<quint64, QByteArray> noncriticalExtensions;

    friend bool operator==(const CiphertextEnvelopeV1 &,
                           const CiphertextEnvelopeV1 &) = default;
};

} // namespace OpenChat
