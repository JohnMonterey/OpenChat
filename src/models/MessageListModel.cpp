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
    case EditedRole:
        return message.edited;
    case EditableRole:
        return message.isEditable();
    case ReplyToIdRole:
        return message.replyToId;
    case QuotedSenderRole:
        return message.quotedSender;
    case QuotedBodyRole:
        return message.quotedBody;
    case SenderAccountRole:
        return message.senderAccount;
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
        {EditedRole, "edited"},
        {EditableRole, "editable"},
        {ReplyToIdRole, "replyToId"},
        {QuotedSenderRole, "quotedSender"},
        {QuotedBodyRole, "quotedBody"},
        {SenderAccountRole, "senderAccount"},
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
    const int row = rowOf(stableId);
    if (row < 0)
        return false;
    Message &message = m_messages[row];
    if (message.deliveryState == state && message.failureReason == failureReason)
        return true;
    message.deliveryState = state;
    message.failureReason = failureReason;
    // Whether it can be edited follows from whether the relay took it.
    emit dataChanged(index(row), index(row), {DeliveryStateRole, FailureReasonRole, EditableRole});
    return true;
}

bool MessageListModel::updateBody(const QString &stableId, const QString &body)
{
    const int row = rowOf(stableId);
    if (row < 0)
        return false;
    Message &message = m_messages[row];
    if (message.body == body && message.edited)
        return true;
    message.body = body;
    message.edited = true;
    emit dataChanged(index(row), index(row), {BodyRole, EditedRole});
    return true;
}

std::optional<Message> MessageListModel::messageAt(int row) const
{
    if (row < 0 || row >= m_messages.size())
        return std::nullopt;
    return m_messages.at(row);
}

std::optional<Message> MessageListModel::messageById(const QString &stableId) const
{
    return messageAt(rowOf(stableId));
}

int MessageListModel::rowOf(const QString &stableId) const
{
    if (stableId.isEmpty())
        return -1;
    // Recent rows are the ones acted on, so search from the end.
    for (int row = m_messages.size() - 1; row >= 0; --row) {
        if (m_messages.at(row).stableId == stableId)
            return row;
    }
    return -1;
}

} // namespace OpenChat
