#include "domain/MessageContent.h"

#include <QCborArray>
#include <QCborValue>
#include <QCryptographicHash>

#include <algorithm>

namespace OpenChat {

namespace {

// Reply: 0xFF [version, 1, body, target, quotedSender, quotedBody]
// Edit:  0xFF [version, 2, body, target]
// A later version may append fields; this one reads the ones it knows.
constexpr char structuredTag = '\xFF';
constexpr quint64 wireVersion = 1;
constexpr qsizetype replyFieldCount = 6;
constexpr qsizetype editFieldCount = 4;

// The composer's limit is 65536 UTF-16 units, at most three UTF-8 bytes each,
// plus a quote and framing.
constexpr qsizetype maxBodyLength = 65536;
constexpr qsizetype maxEncodedBytes = 3 * (maxBodyLength + maxQuoteLength) + 1024;

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

QByteArray encodeMessageContent(const MessageContent &content)
{
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
    if (bytes.size() > maxEncodedBytes)
        return std::nullopt;

    QCborParserError error;
    const QCborValue root = QCborValue::fromCbor(bytes.sliced(1).toByteArray(), &error);
    if (error.error != QCborError::NoError || !root.isArray())
        return std::nullopt;
    const QCborArray fields = root.toArray();
    if (fields.size() < editFieldCount || !fields.at(0).isInteger()
        || fields.at(0).toInteger() != qint64(wireVersion) || !fields.at(1).isInteger()
        || !fields.at(2).isString() || !fields.at(3).isByteArray())
        return std::nullopt;

    const qint64 type = fields.at(1).toInteger();
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
