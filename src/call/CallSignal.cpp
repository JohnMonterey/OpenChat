#include "call/CallSignal.h"

#include <QCborArray>
#include <QCborValue>

#include <openssl/rand.h>

namespace OpenChat {

namespace {

// A signal is a fixed-arity array so the decoder can reject on size alone:
// [type, callId, secret, codec, accepted, reason]. Fields not used by a type are
// written as their zero value; the decoder ignores them for that type.
constexpr qsizetype signalFieldCount = 6;

// An upper bound on a decoded signal. The MLS layer already bounds the
// plaintext, but this keeps the CBOR parser from being handed something absurd
// if that ever changes.
constexpr qsizetype maxSignalBytes = 512;

[[nodiscard]] bool isKnownSignalType(quint64 value) noexcept
{
    return value <= static_cast<quint64>(CallSignalType::Hangup);
}

[[nodiscard]] bool isKnownCodec(quint64 value) noexcept
{
    return value <= static_cast<quint64>(AudioCodecKind::Opus);
}

[[nodiscard]] bool isKnownEndReason(quint64 value) noexcept
{
    return value <= static_cast<quint64>(CallEndReason::Superseded);
}

} // namespace

CallSignalMessage CallSignalMessage::offer(const CallId &callId, const QByteArray &secret,
                                           AudioCodecKind codec)
{
    CallSignalMessage message;
    message.type = CallSignalType::Offer;
    message.callId = callId;
    message.secret = secret;
    message.codec = codec;
    return message;
}

CallSignalMessage CallSignalMessage::ringing(const CallId &callId)
{
    CallSignalMessage message;
    message.type = CallSignalType::Ringing;
    message.callId = callId;
    return message;
}

CallSignalMessage CallSignalMessage::answer(const CallId &callId, bool accepted,
                                            AudioCodecKind codec)
{
    CallSignalMessage message;
    message.type = CallSignalType::Answer;
    message.callId = callId;
    message.accepted = accepted;
    message.codec = codec;
    return message;
}

CallSignalMessage CallSignalMessage::hangup(const CallId &callId, CallEndReason reason)
{
    CallSignalMessage message;
    message.type = CallSignalType::Hangup;
    message.callId = callId;
    message.reason = reason;
    return message;
}

QByteArray encodeCallSignal(const CallSignalMessage &message)
{
    QCborArray fields;
    fields.append(static_cast<qint64>(message.type));
    fields.append(message.callId.bytes());
    fields.append(message.type == CallSignalType::Offer ? message.secret : QByteArray());
    fields.append(static_cast<qint64>(message.codec));
    fields.append(message.accepted);
    fields.append(static_cast<qint64>(message.reason));
    return fields.toCborValue().toCbor();
}

std::optional<CallSignalMessage> decodeCallSignal(QByteArrayView bytes)
{
    if (bytes.isEmpty() || bytes.size() > maxSignalBytes)
        return std::nullopt;
    QCborParserError error{};
    const QCborValue value = QCborValue::fromCbor(bytes.toByteArray(), &error);
    if (error.error != QCborError::NoError || !value.isArray())
        return std::nullopt;
    const QCborArray fields = value.toArray();
    if (fields.size() != signalFieldCount)
        return std::nullopt;

    if (!fields.at(0).isInteger() || fields.at(0).toInteger() < 0)
        return std::nullopt;
    const auto rawType = static_cast<quint64>(fields.at(0).toInteger());
    if (!isKnownSignalType(rawType))
        return std::nullopt;

    if (!fields.at(1).isByteArray())
        return std::nullopt;
    const std::optional<CallId> callId = CallId::fromBytes(fields.at(1).toByteArray());
    if (!callId)
        return std::nullopt;

    if (!fields.at(2).isByteArray() || !fields.at(3).isInteger() || !fields.at(4).isBool()
        || !fields.at(5).isInteger())
        return std::nullopt;
    if (fields.at(3).toInteger() < 0 || fields.at(5).toInteger() < 0)
        return std::nullopt;
    const auto rawCodec = static_cast<quint64>(fields.at(3).toInteger());
    const auto rawReason = static_cast<quint64>(fields.at(5).toInteger());
    if (!isKnownCodec(rawCodec) || !isKnownEndReason(rawReason))
        return std::nullopt;

    CallSignalMessage message;
    message.type = static_cast<CallSignalType>(rawType);
    message.callId = *callId;
    message.secret = fields.at(2).toByteArray();
    message.codec = static_cast<AudioCodecKind>(rawCodec);
    message.accepted = fields.at(4).toBool();
    message.reason = static_cast<CallEndReason>(rawReason);

    // An offer without a full-length secret cannot key a call; rejecting it here
    // means no later stage has to cope with a short or absent key.
    if (message.type == CallSignalType::Offer && message.secret.size() != callSecretBytes)
        return std::nullopt;
    return message;
}

QByteArray generateCallSecret()
{
    QByteArray secret(callSecretBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(secret.data()), callSecretBytes) != 1)
        return {};
    return secret;
}

} // namespace OpenChat
