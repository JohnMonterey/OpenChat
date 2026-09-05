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
};

// The highest EnvelopeMessageKind the codec accepts on decode. Kept next to the
// enum so a new kind cannot be added without widening the wire bound.
inline constexpr quint8 maxEnvelopeMessageKind =
    static_cast<quint8>(EnvelopeMessageKind::ContactAccept);

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
