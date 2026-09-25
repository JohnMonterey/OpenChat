#include "domain/MessageContent.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QCryptographicHash>

#include <algorithm>

namespace OpenChat {

namespace {

// Reply:      0xFF [version, 1, body, target, quotedSender, quotedBody]
// Edit:       0xFF [version, 2, body, target]
// Attachment: 0xFF [version, 3, caption, attachmentId, descriptor, reply]
//   descriptor: a map with integer keys (DescriptorKey); keys this version
//   does not know are ignored. reply: null, or [target, quotedSender,
//   quotedBody] as a Reply carries them.
// A later version may append fields; this one reads the ones it knows.
constexpr char structuredTag = '\xFF';
constexpr quint64 wireVersion = 1;
constexpr qsizetype replyFieldCount = 6;
constexpr qsizetype editFieldCount = 4;
constexpr qsizetype attachmentFieldCount = 6;
constexpr qsizetype quoteFieldCount = 3;

// The composer's limit is 65536 UTF-16 units, at most three UTF-8 bytes each,
// plus a quote and framing.
constexpr qsizetype maxBodyLength = 65536;
constexpr qsizetype maxEncodedBytes = 3 * (maxBodyLength + maxQuoteLength) + 1024;
// Nothing larger is even parsed; each type then applies its own cap.
constexpr qsizetype maxParsedBytes = std::max(maxEncodedBytes, AttachmentLimits::maxMessageBytes);

enum DescriptorKey : qint64 {
    KindKey = 1,
    ByteCountKey = 2,
    Sha256Key = 3,
    PartCountKey = 4,
    KeyKey = 5,
    MimeKey = 6,
    FileNameKey = 7,
    WidthKey = 8,
    HeightKey = 9,
    DurationKey = 10,
    PreviewKey = 11,
    PeaksKey = 12,
};

// A newer sender's kinds (voice notes, say) are still files to this version:
// they can be saved, if not shown.
constexpr qint64 lastForwardKind = 15;

// A caption of nothing but whitespace is no caption.
[[nodiscard]] QString captionOf(const QString &text)
{
    return text.trimmed().isEmpty() ? QString() : text;
}

[[nodiscard]] QCborMap encodeDescriptor(const AttachmentDescriptor &descriptor)
{
    QCborMap map;
    map.insert(KindKey, qint64(descriptor.kind));
    map.insert(ByteCountKey, descriptor.byteCount);
    map.insert(Sha256Key, descriptor.sha256);
    map.insert(PartCountKey, qint64(descriptor.partCount));
    map.insert(KeyKey, descriptor.key);
    if (!descriptor.mimeType.isEmpty())
        map.insert(MimeKey, descriptor.mimeType);
    if (!descriptor.fileName.isEmpty())
        map.insert(FileNameKey, descriptor.fileName);
    if (descriptor.width != 0)
        map.insert(WidthKey, qint64(descriptor.width));
    if (descriptor.height != 0)
        map.insert(HeightKey, qint64(descriptor.height));
    if (descriptor.durationMs != 0)
        map.insert(DurationKey, descriptor.durationMs);
    if (descriptor.hasPreview)
        map.insert(PreviewKey, true);
    if (!descriptor.peaks.isEmpty())
        map.insert(PeaksKey, descriptor.peaks);
    return map;
}

// The integer under `key`, `fallback` when it is absent, nothing when it is
// there but not an integer.
[[nodiscard]] std::optional<qint64> integerAt(const QCborMap &map, qint64 key,
                                             std::optional<qint64> fallback)
{
    const QCborValue value = map.value(key);
    if (value.isUndefined())
        return fallback;
    if (!value.isInteger())
        return std::nullopt;
    return value.toInteger();
}

// The descriptor a peer sent, held to isValidDescriptor. What only decorates
// the bubble (the MIME type, the waveform, the preview flag) is dropped when
// malformed rather than costing the whole message; the name is cleaned
// again whatever the sender did; everything the transfer relies on (sizes,
// hash, key, dimensions) must be right or nothing is shown.
[[nodiscard]] std::optional<AttachmentDescriptor> decodeDescriptor(const QCborMap &map,
                                                                   const AttachmentId &attachmentId)
{
    const auto kind = integerAt(map, KindKey, std::nullopt);
    const auto byteCount = integerAt(map, ByteCountKey, std::nullopt);
    const auto partCount = integerAt(map, PartCountKey, std::nullopt);
    const QCborValue sha256 = map.value(Sha256Key);
    const QCborValue key = map.value(KeyKey);
    if (!kind || *kind < 1 || *kind > lastForwardKind || !byteCount || !partCount
        || !sha256.isByteArray() || !key.isByteArray())
        return std::nullopt;
    const auto width = integerAt(map, WidthKey, 0);
    const auto height = integerAt(map, HeightKey, 0);
    const auto durationMs = integerAt(map, DurationKey, 0);
    if (!width || !height || !durationMs)
        return std::nullopt;
    // Checked before narrowing into the descriptor's ints.
    if (*partCount < 1 || *partCount > AttachmentLimits::maxParts || *width < 0
        || *width > AttachmentLimits::maxImageDimension || *height < 0
        || *height > AttachmentLimits::maxImageDimension)
        return std::nullopt;

    AttachmentDescriptor descriptor;
    descriptor.attachmentId = attachmentId;
    descriptor.key = key.toByteArray();
    descriptor.byteCount = *byteCount;
    descriptor.sha256 = sha256.toByteArray();
    descriptor.partCount = int(*partCount);
    const QString mimeType = map.value(MimeKey).toString();
    descriptor.mimeType = isValidMimeType(mimeType) ? mimeType : QString();
    descriptor.fileName = sanitizeAttachmentFileName(map.value(FileNameKey).toString());
    descriptor.hasPreview = map.value(PreviewKey).isTrue();

    if (*kind > qint64(AttachmentKind::File)) {
        // Only what every file has; the rest meant something to its sender.
        descriptor.kind = AttachmentKind::File;
        descriptor.hasPreview = false;
        return isValidDescriptor(descriptor) ? std::optional(descriptor) : std::nullopt;
    }
    descriptor.kind = AttachmentKind(*kind);
    if (descriptor.kind == AttachmentKind::Image || descriptor.kind == AttachmentKind::Video) {
        descriptor.width = int(*width);
        descriptor.height = int(*height);
    }
    if (descriptor.kind == AttachmentKind::Video || descriptor.kind == AttachmentKind::Audio)
        descriptor.durationMs = *durationMs;
    const QCborValue peaks = map.value(PeaksKey);
    if (descriptor.kind == AttachmentKind::Audio && peaks.isByteArray()
        && peaks.toByteArray().size() <= AttachmentLimits::maxPeaks)
        descriptor.peaks = peaks.toByteArray();
    return isValidDescriptor(descriptor) ? std::optional(descriptor) : std::nullopt;
}

[[nodiscard]] std::optional<MessageContent> decodeAttachment(const QCborArray &fields)
{
    if (fields.size() < attachmentFieldCount || !fields.at(2).isString() || !fields.at(3).isByteArray()
        || !fields.at(4).isMap())
        return std::nullopt;
    const QString caption = fields.at(2).toString();
    const auto attachmentId = AttachmentId::fromBytes(fields.at(3).toByteArray());
    if (!attachmentId || caption.size() > maxBodyLength)
        return std::nullopt;
    const auto descriptor = decodeDescriptor(fields.at(4).toMap(), *attachmentId);
    if (!descriptor)
        return std::nullopt;

    const QCborValue reply = fields.at(5);
    if (reply.isNull())
        return MessageContent::attachmentMessage(caption, *descriptor);
    if (!reply.isArray())
        return std::nullopt;
    const QCborArray quote = reply.toArray();
    if (quote.size() < quoteFieldCount || !quote.at(0).isByteArray() || !quote.at(1).isByteArray()
        || !quote.at(2).isString())
        return std::nullopt;
    const auto target = MessageId::fromBytes(quote.at(0).toByteArray());
    const auto quotedSender = DeviceId::fromBytes(quote.at(1).toByteArray());
    const QString quotedBody = quote.at(2).toString();
    if (!target || !quotedSender || quotedBody.size() > maxQuoteLength)
        return std::nullopt;
    return MessageContent::attachmentMessage(caption, *descriptor,
                                             MessageQuote{*target, *quotedSender, quotedBody});
}

[[nodiscard]] QByteArray encodeAttachment(const MessageContent &content)
{
    // Everything decodeAttachment would refuse is refused here, so a message
    // no receiver would show is never encrypted and sent.
    if (!content.attachment || !isValidDescriptor(*content.attachment)
        || content.body.size() > maxBodyLength
        || content.target.has_value() != content.quotedSender.has_value())
        return {};

    QCborArray fields;
    fields.append(qint64(wireVersion));
    fields.append(qint64(MessageContent::Type::Attachment));
    fields.append(captionOf(content.body));
    fields.append(content.attachment->attachmentId.bytes());
    fields.append(encodeDescriptor(*content.attachment));
    if (content.target)
        fields.append(QCborArray{content.target->bytes(), content.quotedSender->bytes(),
                                 quoteExcerpt(content.quotedBody)});
    else
        fields.append(QCborValue(nullptr));
    QByteArray bytes(1, structuredTag);
    bytes.append(QCborValue(fields).toCbor());
    if (bytes.size() > AttachmentLimits::maxMessageBytes)
        return {};
    return bytes;
}

} // namespace

QString quoteExcerpt(const QString &body)
{
    qsizetype length = std::min<qsizetype>(body.size(), maxQuoteLength);
    if (length > 0 && length < body.size() && body.at(length - 1).isHighSurrogate())
        --length;
    return body.left(length);
}

MessageContent MessageContent::text(const QString &body)
{
    MessageContent content;
    content.body = body;
    return content;
}

MessageContent MessageContent::reply(const QString &body, const MessageId &target,
                                     const DeviceId &quotedSender, const QString &quotedBody)
{
    MessageContent content;
    content.type = Type::Reply;
    content.body = body;
    content.target = target;
    content.quotedSender = quotedSender;
    content.quotedBody = quoteExcerpt(quotedBody);
    return content;
}

MessageContent MessageContent::edit(const MessageId &target, const QString &body)
{
    MessageContent content;
    content.type = Type::Edit;
    content.body = body;
    content.target = target;
    return content;
}

MessageContent MessageContent::attachmentMessage(const QString &caption,
                                                 const AttachmentDescriptor &attachment,
                                                 const std::optional<MessageQuote> &quote)
{
    MessageContent content;
    content.type = Type::Attachment;
    content.body = captionOf(caption);
    content.attachment = attachment;
    if (quote) {
        content.target = quote->target;
        content.quotedSender = quote->sender;
        content.quotedBody = quoteExcerpt(quote->body);
    }
    return content;
}

QByteArray encodeMessageContent(const MessageContent &content)
{
    if (content.type == MessageContent::Type::Attachment)
        return encodeAttachment(content);
    if (content.type == MessageContent::Type::Text || !content.target)
        return content.body.toUtf8();

    QCborArray fields;
    fields.append(qint64(wireVersion));
    fields.append(qint64(content.type));
    fields.append(content.body);
    fields.append(content.target->bytes());
    if (content.type == MessageContent::Type::Reply) {
        fields.append(content.quotedSender ? content.quotedSender->bytes() : QByteArray());
        fields.append(quoteExcerpt(content.quotedBody));
    }
    QByteArray bytes(1, structuredTag);
    bytes.append(QCborValue(fields).toCbor());
    return bytes;
}

std::optional<MessageContent> decodeMessageContent(QByteArrayView bytes)
{
    if (bytes.isEmpty() || bytes.front() != structuredTag)
        return MessageContent::text(QString::fromUtf8(bytes));
    if (bytes.size() > maxParsedBytes)
        return std::nullopt;

    QCborParserError error;
    const QCborValue root = QCborValue::fromCbor(bytes.sliced(1).toByteArray(), &error);
    if (error.error != QCborError::NoError || !root.isArray())
        return std::nullopt;
    const QCborArray fields = root.toArray();
    if (fields.size() < 2 || !fields.at(0).isInteger() || fields.at(0).toInteger() != qint64(wireVersion)
        || !fields.at(1).isInteger())
        return std::nullopt;
    const qint64 type = fields.at(1).toInteger();
    if (type == qint64(MessageContent::Type::Attachment)) {
        if (bytes.size() > AttachmentLimits::maxMessageBytes)
            return std::nullopt;
        return decodeAttachment(fields);
    }

    // Replies and edits, held to the same rules as in every earlier version.
    if (bytes.size() > maxEncodedBytes || fields.size() < editFieldCount || !fields.at(2).isString()
        || !fields.at(3).isByteArray())
        return std::nullopt;

    const QString body = fields.at(2).toString();
    const auto target = MessageId::fromBytes(fields.at(3).toByteArray());
    if (!target || body.trimmed().isEmpty() || body.size() > maxBodyLength)
        return std::nullopt;

    if (type == qint64(MessageContent::Type::Edit))
        return MessageContent::edit(*target, body);
    if (type != qint64(MessageContent::Type::Reply) || fields.size() < replyFieldCount
        || !fields.at(4).isByteArray() || !fields.at(5).isString())
        return std::nullopt;
    const auto quotedSender = DeviceId::fromBytes(fields.at(4).toByteArray());
    const QString quotedBody = fields.at(5).toString();
    if (!quotedSender || quotedBody.size() > maxQuoteLength)
        return std::nullopt;
    return MessageContent::reply(body, *target, *quotedSender, quotedBody);
}

MessageId messageIdForCiphertext(QByteArrayView ciphertext)
{
    const QByteArray digest = QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256);
    // An all-zero prefix is not a valid id; SHA-256 will not produce one, but
    // a fresh id is the safe answer if it ever did.
    return MessageId::fromBytes(QByteArrayView(digest).first(MessageId::byteCount))
        .value_or(MessageId::generate());
}

} // namespace OpenChat
