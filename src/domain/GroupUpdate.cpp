#include "domain/GroupUpdate.h"

#include <QCborArray>
#include <QCborValue>

namespace OpenChat {

namespace {

// [version, type, title, [[account, device, name], ...]]
constexpr qsizetype fieldCount = 4;
constexpr qsizetype memberFieldCount = 3;
constexpr quint64 wireVersion = 1;

// The largest encoding the decoder will even parse: every member at the name
// bound plus generous framing.
constexpr qsizetype maxEncodedBytes =
    maxGroupMembers * (2 * 16 + 4 * maxGroupMemberNameLength + 16) + 1024;

[[nodiscard]] bool isKnownType(qint64 value) noexcept
{
    return value >= 0 && value <= static_cast<qint64>(GroupUpdateType::Leave);
}

} // namespace

QString normalizeGroupTitle(const QString &title)
{
    return title.trimmed().left(maxGroupTitleLength);
}

GroupUpdateMessage GroupUpdateMessage::info(const QString &title,
                                            const QVector<GroupMemberInfo> &members)
{
    GroupUpdateMessage message;
    message.type = GroupUpdateType::Info;
    message.title = normalizeGroupTitle(title);
    message.members = members;
    return message;
}

GroupUpdateMessage GroupUpdateMessage::rename(const QString &title)
{
    GroupUpdateMessage message;
    message.type = GroupUpdateType::Rename;
    message.title = normalizeGroupTitle(title);
    return message;
}

GroupUpdateMessage GroupUpdateMessage::leave()
{
    GroupUpdateMessage message;
    message.type = GroupUpdateType::Leave;
    return message;
}

QByteArray encodeGroupUpdate(const GroupUpdateMessage &message)
{
    QCborArray members;
    if (message.type == GroupUpdateType::Info) {
        for (const GroupMemberInfo &member : message.members) {
            QCborArray fields;
            fields.append(member.account.bytes());
            fields.append(member.device.bytes());
            fields.append(member.name.trimmed().left(maxGroupMemberNameLength));
            members.append(fields);
        }
    }
    QCborArray fields;
    fields.append(qint64(wireVersion));
    fields.append(qint64(message.type));
    fields.append(message.type == GroupUpdateType::Leave ? QString()
                                                         : normalizeGroupTitle(message.title));
    fields.append(members);
    return QCborValue(fields).toCbor();
}

std::optional<GroupUpdateMessage> decodeGroupUpdate(QByteArrayView bytes)
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
    if (!fields.at(1).isInteger() || !isKnownType(fields.at(1).toInteger()))
        return std::nullopt;
    if (!fields.at(2).isString() || !fields.at(3).isArray())
        return std::nullopt;

    GroupUpdateMessage message;
    message.type = static_cast<GroupUpdateType>(fields.at(1).toInteger());
    const QString title = fields.at(2).toString();
    if (title.size() > maxGroupTitleLength)
        return std::nullopt;
    message.title = title.trimmed();

    const QCborArray members = fields.at(3).toArray();
    if (members.size() > maxGroupMembers)
        return std::nullopt;
    // Only an Info carries a roster; any other type with one is malformed.
    if (message.type != GroupUpdateType::Info && !members.isEmpty())
        return std::nullopt;
    if (message.type == GroupUpdateType::Leave && !message.title.isEmpty())
        return std::nullopt;
    for (const QCborValue &value : members) {
        if (!value.isArray())
            return std::nullopt;
        const QCborArray member = value.toArray();
        if (member.size() != memberFieldCount || !member.at(0).isByteArray()
            || !member.at(1).isByteArray() || !member.at(2).isString())
            return std::nullopt;
        const auto account = AccountId::fromBytes(member.at(0).toByteArray());
        const auto device = DeviceId::fromBytes(member.at(1).toByteArray());
        const QString name = member.at(2).toString();
        if (!account || !device || name.size() > maxGroupMemberNameLength)
            return std::nullopt;
        // A roster names each device once.
        for (const GroupMemberInfo &existing : message.members)
            if (existing.device == *device)
                return std::nullopt;
        message.members.append(GroupMemberInfo{*account, *device, name.trimmed()});
    }
    return message;
}

} // namespace OpenChat
