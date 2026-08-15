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
};

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
