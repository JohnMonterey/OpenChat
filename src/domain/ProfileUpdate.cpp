#include "domain/ProfileUpdate.h"

#include "models/Contact.h"

#include <QCborArray>
#include <QCborValue>

namespace OpenChat {

namespace {

// [version, presence, statusText, avatarJpeg]
constexpr qsizetype fieldCount = 4;
constexpr quint64 wireVersion = 1;

// The largest encoding the decoder will even parse: the picture bound plus
// generous room for the other fields and CBOR framing.
constexpr qsizetype maxEncodedBytes = maxAvatarJpegBytes + 1024;

[[nodiscard]] bool looksLikeJpeg(const QByteArray &bytes)
{
    return bytes.size() >= 4 && static_cast<quint8>(bytes[0]) == 0xFF
        && static_cast<quint8>(bytes[1]) == 0xD8;
}

} // namespace

QByteArray encodeProfileUpdate(const ProfileUpdateMessage &message)
{
    QCborArray fields;
    fields.append(qint64(wireVersion));
    fields.append(qint64(message.presence));
    fields.append(message.statusText.trimmed().left(maxStatusTextLength));
    fields.append(message.avatarJpeg);
    return QCborValue(fields).toCbor();
}

std::optional<ProfileUpdateMessage> decodeProfileUpdate(QByteArrayView bytes)
{
    if (bytes.isEmpty() || bytes.size() > maxEncodedBytes)
        return std::nullopt;
    QCborParserError error;
    const QCborValue root = QCborValue::fromCbor(bytes.toByteArray(), &error);
    if (error.error != QCborError::NoError || !root.isArray())
        return std::nullopt;
    const QCborArray fields = root.toArray();
    if (fields.size() != fieldCount)
        return std::nullopt;
    if (!fields.at(0).isInteger() || fields.at(0).toInteger() != qint64(wireVersion))
        return std::nullopt;
    if (!fields.at(1).isInteger() || !fields.at(2).isString() || !fields.at(3).isByteArray())
        return std::nullopt;
    const qint64 presence = fields.at(1).toInteger();
    if (presence < 0 || presence >= presenceCount)
        return std::nullopt;
    const QString statusText = fields.at(2).toString();
    if (statusText.size() > maxStatusTextLength)
        return std::nullopt;
    const QByteArray avatarJpeg = fields.at(3).toByteArray();
    if (avatarJpeg.size() > maxAvatarJpegBytes)
        return std::nullopt;
    if (!avatarJpeg.isEmpty() && !looksLikeJpeg(avatarJpeg))
        return std::nullopt;

    ProfileUpdateMessage message;
    message.presence = static_cast<int>(presence);
    message.statusText = statusText.trimmed();
    message.avatarJpeg = avatarJpeg;
    return message;
}

} // namespace OpenChat
