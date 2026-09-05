#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QHashFunctions>
#include <QString>

#include <optional>

namespace OpenChat {

namespace Detail {

[[nodiscard]] QByteArray generateIdentifierBytes();

} // namespace Detail

template<typename Tag>
class StrongId final
{
public:
    static constexpr qsizetype byteCount = 16;

    [[nodiscard]] static StrongId generate()
    {
        return StrongId(Detail::generateIdentifierBytes());
    }

    [[nodiscard]] static std::optional<StrongId> fromBytes(QByteArrayView bytes)
    {
        if (bytes.size() != byteCount)
            return std::nullopt;

        bool hasNonZeroByte = false;
        for (const char byte : bytes) {
            if (byte != '\0') {
                hasNonZeroByte = true;
                break;
            }
        }
        if (!hasNonZeroByte)
            return std::nullopt;

        return StrongId(bytes.toByteArray());
    }

    [[nodiscard]] QByteArray bytes() const
    {
        return m_bytes;
    }

    [[nodiscard]] QString toHex() const
    {
        return QString::fromLatin1(m_bytes.toHex());
    }

    friend bool operator==(const StrongId &, const StrongId &) = default;

    friend bool operator<(const StrongId &left, const StrongId &right) noexcept
    {
        return left.m_bytes < right.m_bytes;
    }

    friend size_t qHash(const StrongId &id, size_t seed = 0) noexcept
    {
        return qHashBits(id.m_bytes.constData(), static_cast<size_t>(id.m_bytes.size()), seed);
    }

private:
    explicit StrongId(QByteArray bytes)
        : m_bytes(std::move(bytes))
    {
    }

    QByteArray m_bytes;
};

struct ProfileIdTag final { };
struct AccountIdTag final { };
struct DeviceIdTag final { };
struct ConversationIdTag final { };
struct MessageIdTag final { };
struct EnvelopeIdTag final { };
struct AttachmentIdTag final { };
struct CallIdTag final { };

using ProfileId = StrongId<ProfileIdTag>;
using AccountId = StrongId<AccountIdTag>;
using DeviceId = StrongId<DeviceIdTag>;
using ConversationId = StrongId<ConversationIdTag>;
using MessageId = StrongId<MessageIdTag>;
using EnvelopeId = StrongId<EnvelopeIdTag>;
using AttachmentId = StrongId<AttachmentIdTag>;
// Names one voice call end to end. Both peers use the caller's value, so it is
// what binds an answer, a hangup and every media packet to the same call.
using CallId = StrongId<CallIdTag>;

} // namespace OpenChat
