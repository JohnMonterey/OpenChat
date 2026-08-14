#include "models/MessageListModel.h"

namespace OpenChat {

MessageListModel::MessageListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int MessageListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_messages.size();
}

QVariant MessageListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.column() != 0 || index.row() < 0
        || index.row() >= m_messages.size()) {
        return {};
    }

    const Message &message = m_messages.at(index.row());
    switch (role) {
    case DirectionRole:
        return static_cast<int>(message.direction);
    case BodyRole:
        return message.body;
    case TimestampRole:
        return message.timestamp.toString(QStringLiteral("h:mm AP"));
    case KindRole:
        return static_cast<int>(message.kind);
    default:
        return {};
    }
}

QHash<int, QByteArray> MessageListModel::roleNames() const
{
    return {
        {DirectionRole, "direction"},
        {BodyRole, "body"},
        {TimestampRole, "timestamp"},
        {KindRole, "kind"},
    };
}

void MessageListModel::setMessages(QVector<Message> messages)
{
    beginResetModel();
    m_messages = std::move(messages);
    endResetModel();
}

bool MessageListModel::appendOutgoing(const QString &body, const QTime &timestamp)
{
    const QString trimmedBody = body.trimmed();
    if (trimmedBody.isEmpty())
        return false;

    const int row = m_messages.size();
    beginInsertRows({}, row, row);
    m_messages.append({MessageDirection::Outgoing, trimmedBody, timestamp, MessageKind::Text});
    endInsertRows();
    return true;
}

std::optional<Message> MessageListModel::messageAt(int row) const
{
    if (row < 0 || row >= m_messages.size())
        return std::nullopt;
    return m_messages.at(row);
}

} // namespace OpenChat
