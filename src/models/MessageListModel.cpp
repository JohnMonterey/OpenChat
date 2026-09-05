#include "models/MessageListModel.h"

#include <QLocale>

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
    case DateLabelRole:
        return message.date.isValid()
            ? QLocale(QLocale::English).toString(message.date, QStringLiteral("MMMM d, yyyy"))
            : QString();
    case ShowDateDividerRole:
        return message.date.isValid()
            && (index.row() == 0 || m_messages.at(index.row() - 1).date != message.date);
    case StableIdRole:
        return message.stableId;
    case DeliveryStateRole:
        return static_cast<int>(message.deliveryState);
    case FailureReasonRole:
        return static_cast<int>(message.failureReason);
    case SenderDeviceRole:
        return message.senderDevice;
    case SecurityEventRole:
        return static_cast<int>(message.securityEvent);
    case SenderNameRole:
        return message.senderName;
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
        {DateLabelRole, "dateLabel"},
        {ShowDateDividerRole, "showDateDivider"},
        {StableIdRole, "stableId"},
        {DeliveryStateRole, "deliveryState"},
        {FailureReasonRole, "failureReason"},
        {SenderDeviceRole, "senderDevice"},
        {SecurityEventRole, "securityEvent"},
        {SenderNameRole, "senderName"},
    };
}

int MessageListModel::count() const
{
    return m_messages.size();
}

void MessageListModel::setMessages(QVector<Message> messages)
{
    const int previousCount = m_messages.size();
    beginResetModel();
    m_messages = std::move(messages);
    endResetModel();
    if (previousCount != m_messages.size())
        emit countChanged();
}

bool MessageListModel::appendOutgoing(const QString &body, const QTime &timestamp)
{
    return appendOutgoing(body, QDateTime(QDate::currentDate(), timestamp));
}

bool MessageListModel::appendOutgoing(const QString &body, const QDateTime &sentAt)
{
    const QString trimmedBody = body.trimmed();
    if (trimmedBody.isEmpty() || !sentAt.isValid())
        return false;

    const int row = m_messages.size();
    beginInsertRows({}, row, row);
    m_messages.append({MessageDirection::Outgoing, trimmedBody, sentAt.time(), MessageKind::Text,
                       sentAt.date()});
    endInsertRows();
    emit countChanged();
    return true;
}

void MessageListModel::appendMessage(Message message)
{
    const int row = m_messages.size();
    beginInsertRows({}, row, row);
    m_messages.append(std::move(message));
    endInsertRows();
    emit countChanged();
}

bool MessageListModel::updateDeliveryState(const QString &stableId, MessageDeliveryState state,
                                           MessageFailureReason failureReason)
{
    if (stableId.isEmpty())
        return false;
    for (int row = m_messages.size() - 1; row >= 0; --row) {
        Message &message = m_messages[row];
        if (message.stableId != stableId)
            continue;
        if (message.deliveryState == state && message.failureReason == failureReason)
            return true;
        message.deliveryState = state;
        message.failureReason = failureReason;
        emit dataChanged(index(row), index(row), {DeliveryStateRole, FailureReasonRole});
        return true;
    }
    return false;
}

std::optional<Message> MessageListModel::messageAt(int row) const
{
    if (row < 0 || row >= m_messages.size())
        return std::nullopt;
    return m_messages.at(row);
}

} // namespace OpenChat
