#include "protocol/CanonicalCborCodec.h"

#include <QCryptographicHash>
#include <QSet>

#include <array>
#include <limits>
#include <optional>

namespace OpenChat {
namespace {

constexpr quint64 criticalFieldLimit = 128;
constexpr qint64 maxEnvelopeLifetimeMs = 30LL * 24 * 60 * 60 * 1000;
constexpr quint64 requiredFieldCount = 13;

enum Field : quint64 {
    Version = 0,
    EnvelopeIdentifier = 1,
    SenderAccount = 2,
    SenderDevice = 3,
    RecipientDevice = 4,
    Conversation = 5,
    MessageKind = 6,
    CreatedAt = 7,
    ExpiresAt = 8,
    Idempotency = 9,
    Ciphertext = 10,
    CiphertextHash = 11,
    SenderSignature = 12,
};

void appendTypeValue(QByteArray &output, quint8 major, quint64 value)
{
    const char prefix = static_cast<char>(major << 5);
    if (value < 24) {
        output.append(static_cast<char>(prefix | static_cast<char>(value)));
    } else if (value <= std::numeric_limits<quint8>::max()) {
        output.append(static_cast<char>(prefix | 24));
        output.append(static_cast<char>(value));
    } else if (value <= std::numeric_limits<quint16>::max()) {
        output.append(static_cast<char>(prefix | 25));
        output.append(static_cast<char>((value >> 8) & 0xff));
        output.append(static_cast<char>(value & 0xff));
    } else if (value <= std::numeric_limits<quint32>::max()) {
        output.append(static_cast<char>(prefix | 26));
        for (int shift = 24; shift >= 0; shift -= 8)
            output.append(static_cast<char>((value >> shift) & 0xff));
    } else {
        output.append(static_cast<char>(prefix | 27));
        for (int shift = 56; shift >= 0; shift -= 8)
            output.append(static_cast<char>((value >> shift) & 0xff));
    }
}

void appendSigned(QByteArray &output, qint64 value)
{
    if (value >= 0)
        appendTypeValue(output, 0, static_cast<quint64>(value));
    else
        appendTypeValue(output, 1, static_cast<quint64>(-(value + 1)));
}

void appendBytes(QByteArray &output, QByteArrayView value)
{
    appendTypeValue(output, 2, static_cast<quint64>(value.size()));
    output.append(value.data(), value.size());
}

struct Head final {
    quint8 major = 0;
    quint64 value = 0;
};

class Reader final
{
public:
    explicit Reader(QByteArrayView input, DecodeLimits limits)
        : m_input(input)
        , m_limits(limits)
    {
    }

    [[nodiscard]] qsizetype position() const noexcept { return m_position; }
    [[nodiscard]] bool atEnd() const noexcept { return m_position == m_input.size(); }
    [[nodiscard]] std::optional<DecodeError> error() const noexcept { return m_error; }

    std::optional<Head> readHead()
    {
        if (m_error)
            return std::nullopt;
        if (m_position >= m_input.size()) {
            fail(DecodeError::Truncated);
            return std::nullopt;
        }

        const quint8 initial = static_cast<quint8>(m_input[m_position++]);
        const quint8 major = initial >> 5;
        const quint8 additional = initial & 0x1f;
        if (additional == 31) {
            fail(DecodeError::NonCanonical);
            return std::nullopt;
        }
        if (additional >= 28) {
            fail(DecodeError::Malformed);
            return std::nullopt;
        }
        if (additional < 24)
            return Head{major, additional};

        const int bytes = additional == 24 ? 1 : additional == 25 ? 2 : additional == 26 ? 4 : 8;
        if (m_input.size() - m_position < bytes) {
            fail(DecodeError::Truncated);
            return std::nullopt;
        }
        quint64 value = 0;
        for (int index = 0; index < bytes; ++index)
            value = (value << 8) | static_cast<quint8>(m_input[m_position++]);

        if ((additional == 24 && value < 24) || (additional == 25 && value <= 0xff)
            || (additional == 26 && value <= 0xffff)
            || (additional == 27 && value <= 0xffffffffULL)) {
            fail(DecodeError::NonCanonical);
            return std::nullopt;
        }
        return Head{major, value};
    }

    std::optional<quint64> readUnsigned()
    {
        const auto head = readHead();
        if (!head)
            return std::nullopt;
        if (head->major != 0) {
            fail(DecodeError::InvalidFieldType);
            return std::nullopt;
        }
        return head->value;
    }

    std::optional<qint64> readSigned()
    {
        const auto head = readHead();
        if (!head)
            return std::nullopt;
        if (head->major == 0) {
            if (head->value > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
                fail(DecodeError::InvalidFieldValue);
                return std::nullopt;
            }
            return static_cast<qint64>(head->value);
        }
        if (head->major == 1
            && head->value <= static_cast<quint64>(std::numeric_limits<qint64>::max()))
            return -1 - static_cast<qint64>(head->value);
        fail(DecodeError::InvalidFieldType);
        return std::nullopt;
    }

    std::optional<QByteArrayView> readBytes()
    {
        const auto head = readHead();
        if (!head)
            return std::nullopt;
        if (head->major != 2) {
            fail(DecodeError::InvalidFieldType);
            return std::nullopt;
        }
        if (head->value > static_cast<quint64>(m_input.size() - m_position)) {
            fail(DecodeError::Truncated);
            return std::nullopt;
        }
        const qsizetype size = static_cast<qsizetype>(head->value);
        const QByteArrayView result = m_input.sliced(m_position, size);
        m_position += size;
        return result;
    }

    bool skipValue(int depth)
    {
        if (depth > m_limits.cborDepth) {
            fail(DecodeError::DepthLimitExceeded);
            return false;
        }
        const auto head = readHead();
        if (!head)
            return false;
        if (head->major == 0 || head->major == 1)
            return true;
        if (head->major == 2 || head->major == 3) {
            if (head->value > static_cast<quint64>(m_input.size() - m_position)) {
                fail(DecodeError::Truncated);
                return false;
            }
            m_position += static_cast<qsizetype>(head->value);
            return true;
        }
        if (head->major == 4) {
            if (head->value > 1024) {
                fail(DecodeError::Malformed);
                return false;
            }
            for (quint64 index = 0; index < head->value; ++index) {
                if (!skipValue(depth + 1))
                    return false;
            }
            return true;
        }
        if (head->major == 5) {
            if (head->value > 1024) {
                fail(DecodeError::Malformed);
                return false;
            }
            QByteArrayView previousKey;
            bool havePrevious = false;
            for (quint64 index = 0; index < head->value; ++index) {
                const qsizetype keyStart = m_position;
                if (!skipValue(depth + 1))
                    return false;
                const QByteArrayView key = m_input.sliced(keyStart, m_position - keyStart);
                // Track "have a previous key" explicitly rather than inferring it
                // from a non-empty slice: an encoded key is never zero length, but
                // the intent (skip the check only on the first key) should not
                // depend on that.
                if (havePrevious && !isCanonicalKeyOrder(previousKey, key)) {
                    fail(previousKey == key ? DecodeError::DuplicateField
                                            : DecodeError::NonCanonical);
                    return false;
                }
                previousKey = key;
                havePrevious = true;
                if (!skipValue(depth + 1))
                    return false;
            }
            return true;
        }
        if (head->major == 6)
            return skipValue(depth + 1);
        if (head->major == 7 && (head->value == 20 || head->value == 21 || head->value == 22))
            return true;
        fail(DecodeError::Malformed);
        return false;
    }

    void fail(DecodeError error)
    {
        if (!m_error)
            m_error = error;
    }

private:
    static bool isCanonicalKeyOrder(QByteArrayView left, QByteArrayView right)
    {
        if (left.size() != right.size())
            return left.size() < right.size();
        return left < right;
    }

    QByteArrayView m_input;
    DecodeLimits m_limits;
    qsizetype m_position = 0;
    std::optional<DecodeError> m_error;
};

template <typename Id>
std::optional<Id> decodeId(Reader &reader)
{
    const auto bytes = reader.readBytes();
    if (!bytes)
        return std::nullopt;
    if (bytes->size() != Id::byteCount) {
        reader.fail(DecodeError::InvalidFieldLength);
        return std::nullopt;
    }
    const auto id = Id::fromBytes(*bytes);
    if (!id)
        reader.fail(DecodeError::InvalidFieldValue);
    return id;
}

Result<CiphertextEnvelopeV1, DecodeError> failure(const Reader &reader,
                                                  DecodeError fallback)
{
    return Result<CiphertextEnvelopeV1, DecodeError>::failure(reader.error().value_or(fallback));
}

} // namespace

QByteArray encodeCanonical(const CiphertextEnvelopeV1 &envelope)
{
    for (auto extension = envelope.noncriticalExtensions.cbegin();
         extension != envelope.noncriticalExtensions.cend(); ++extension) {
        if (extension.key() < criticalFieldLimit || extension.value().isEmpty())
            return {};

        // Validate the extension value at the SAME nesting depth the decoder
        // starts from (a map value sits one level inside the envelope map, i.e.
        // depth 2). Using depth 1 here would accept one extra level that every
        // decoder then rejects, letting a sender emit envelopes no peer can read.
        Reader reader(extension.value(), {});
        if (!reader.skipValue(2) || !reader.atEnd())
            return {};
    }

    QByteArray output;
    output.reserve(256 + envelope.ciphertext.size());
    appendTypeValue(output, 5,
                    requiredFieldCount + static_cast<quint64>(envelope.noncriticalExtensions.size()));

    appendTypeValue(output, 0, Version);
    appendTypeValue(output, 0, envelope.version);
    appendTypeValue(output, 0, EnvelopeIdentifier);
    appendBytes(output, envelope.envelopeId.bytes());
    appendTypeValue(output, 0, SenderAccount);
    appendBytes(output, envelope.senderAccountId.bytes());
    appendTypeValue(output, 0, SenderDevice);
    appendBytes(output, envelope.senderDeviceId.bytes());
    appendTypeValue(output, 0, RecipientDevice);
    appendBytes(output, envelope.recipientDeviceId.bytes());
    appendTypeValue(output, 0, Conversation);
    appendBytes(output, envelope.conversationId.bytes());
    appendTypeValue(output, 0, MessageKind);
    appendTypeValue(output, 0, static_cast<quint64>(envelope.messageKind));
    appendTypeValue(output, 0, CreatedAt);
    appendSigned(output, envelope.createdAtMs);
    appendTypeValue(output, 0, ExpiresAt);
    appendSigned(output, envelope.expiresAtMs);
    appendTypeValue(output, 0, Idempotency);
    appendBytes(output, envelope.idempotencyKey.bytes());
    appendTypeValue(output, 0, Ciphertext);
    appendBytes(output, envelope.ciphertext);
    appendTypeValue(output, 0, CiphertextHash);
    appendBytes(output, envelope.ciphertextSha256);
    appendTypeValue(output, 0, SenderSignature);
    appendBytes(output, envelope.senderSignature);

    for (auto extension = envelope.noncriticalExtensions.cbegin();
         extension != envelope.noncriticalExtensions.cend(); ++extension) {
        if (extension.key() < criticalFieldLimit)
            continue;
        appendTypeValue(output, 0, extension.key());
        output.append(extension.value());
    }
    return output;
}

QByteArray encodeForSignature(const CiphertextEnvelopeV1 &envelope)
{
    CiphertextEnvelopeV1 unsignedEnvelope = envelope;
    unsignedEnvelope.senderSignature.clear();
    return encodeCanonical(unsignedEnvelope);
}

Result<CiphertextEnvelopeV1, DecodeError> decodeEnvelope(QByteArrayView encoded,
                                                         DecodeLimits limits)
{
    if (limits.envelopeBytes < 0 || limits.ciphertextBytes < 0 || limits.cborDepth < 1)
        return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::Malformed);
    if (encoded.size() > limits.envelopeBytes)
        return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::FrameTooLarge);

    Reader reader(encoded, limits);
    const auto map = reader.readHead();
    if (!map)
        return failure(reader, DecodeError::Malformed);
    if (map->major != 5)
        return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::InvalidFieldType);
    if (map->value > 64)
        return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::Malformed);

    std::array<bool, requiredFieldCount> seen{};
    std::optional<quint64> previousKey;
    quint8 version = 0;
    std::optional<EnvelopeId> envelopeId;
    std::optional<AccountId> senderAccountId;
    std::optional<DeviceId> senderDeviceId;
    std::optional<DeviceId> recipientDeviceId;
    std::optional<ConversationId> conversationId;
    EnvelopeMessageKind messageKind = EnvelopeMessageKind::MlsPrivateMessage;
    qint64 createdAtMs = 0;
    qint64 expiresAtMs = 0;
    std::optional<EnvelopeId> idempotencyKey;
    QByteArray ciphertext;
    QByteArray ciphertextHash;
    QByteArray senderSignature;
    QMap<quint64, QByteArray> extensions;

    for (quint64 index = 0; index < map->value; ++index) {
        const auto key = reader.readUnsigned();
        if (!key)
            return failure(reader, DecodeError::Malformed);
        if (previousKey && *key <= *previousKey)
            return Result<CiphertextEnvelopeV1, DecodeError>::failure(
                *key == *previousKey ? DecodeError::DuplicateField : DecodeError::NonCanonical);
        previousKey = key;

        if (*key >= criticalFieldLimit) {
            const qsizetype valueStart = reader.position();
            if (!reader.skipValue(2))
                return failure(reader, DecodeError::Malformed);
            extensions.insert(*key, encoded.sliced(valueStart, reader.position() - valueStart).toByteArray());
            continue;
        }
        if (*key >= requiredFieldCount)
            return Result<CiphertextEnvelopeV1, DecodeError>::failure(
                DecodeError::UnknownCriticalField);
        if (seen[*key])
            return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::DuplicateField);
        seen[*key] = true;

        switch (*key) {
        case Version: {
            const auto value = reader.readUnsigned();
            if (!value || *value > std::numeric_limits<quint8>::max())
                return failure(reader, DecodeError::InvalidFieldValue);
            version = static_cast<quint8>(*value);
            break;
        }
        case EnvelopeIdentifier:
            envelopeId = decodeId<EnvelopeId>(reader);
            break;
        case SenderAccount:
            senderAccountId = decodeId<AccountId>(reader);
            break;
        case SenderDevice:
            senderDeviceId = decodeId<DeviceId>(reader);
            break;
        case RecipientDevice:
            recipientDeviceId = decodeId<DeviceId>(reader);
            break;
        case Conversation:
            conversationId = decodeId<ConversationId>(reader);
            break;
        case MessageKind: {
            const auto value = reader.readUnsigned();
            if (!value || *value > static_cast<quint64>(maxEnvelopeMessageKind))
                return failure(reader, DecodeError::InvalidFieldValue);
            messageKind = static_cast<EnvelopeMessageKind>(*value);
            break;
        }
        case CreatedAt: {
            const auto value = reader.readSigned();
            if (!value)
                return failure(reader, DecodeError::InvalidFieldValue);
            createdAtMs = *value;
            break;
        }
        case ExpiresAt: {
            const auto value = reader.readSigned();
            if (!value)
                return failure(reader, DecodeError::InvalidFieldValue);
            expiresAtMs = *value;
            break;
        }
        case Idempotency:
            idempotencyKey = decodeId<EnvelopeId>(reader);
            break;
        case Ciphertext: {
            const auto value = reader.readBytes();
            if (!value)
                return failure(reader, DecodeError::InvalidFieldType);
            if (value->size() > limits.ciphertextBytes)
                return Result<CiphertextEnvelopeV1, DecodeError>::failure(
                    DecodeError::CiphertextTooLarge);
            ciphertext = value->toByteArray();
            break;
        }
        case CiphertextHash: {
            const auto value = reader.readBytes();
            if (!value)
                return failure(reader, DecodeError::InvalidFieldType);
            if (value->size() != 32)
                return Result<CiphertextEnvelopeV1, DecodeError>::failure(
                    DecodeError::InvalidFieldLength);
            ciphertextHash = value->toByteArray();
            break;
        }
        case SenderSignature: {
            const auto value = reader.readBytes();
            if (!value)
                return failure(reader, DecodeError::InvalidFieldType);
            if (value->size() != 64)
                return Result<CiphertextEnvelopeV1, DecodeError>::failure(
                    DecodeError::InvalidFieldLength);
            senderSignature = value->toByteArray();
            break;
        }
        default:
            return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::Malformed);
        }
        if (reader.error())
            return failure(reader, DecodeError::Malformed);
    }

    if (!reader.atEnd())
        return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::TrailingData);
    for (bool fieldSeen : seen) {
        if (!fieldSeen)
            return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::MissingField);
    }
    if (version != 1)
        return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::UnsupportedVersion);
    if (!envelopeId || !senderAccountId || !senderDeviceId || !recipientDeviceId
        || !conversationId || !idempotencyKey)
        return failure(reader, DecodeError::InvalidFieldValue);
    if (createdAtMs < 0 || expiresAtMs <= createdAtMs
        || expiresAtMs - createdAtMs > maxEnvelopeLifetimeMs)
        return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::InvalidExpiry);
    if (QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256) != ciphertextHash)
        return Result<CiphertextEnvelopeV1, DecodeError>::failure(DecodeError::HashMismatch);

    return Result<CiphertextEnvelopeV1, DecodeError>::success(
        CiphertextEnvelopeV1{version,
                             *envelopeId,
                             *senderAccountId,
                             *senderDeviceId,
                             *recipientDeviceId,
                             *conversationId,
                             messageKind,
                             createdAtMs,
                             expiresAtMs,
                             *idempotencyKey,
                             std::move(ciphertext),
                             std::move(ciphertextHash),
                             std::move(senderSignature),
                             std::move(extensions)});
}

} // namespace OpenChat
