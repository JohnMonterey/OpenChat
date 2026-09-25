#include "models/MessageListModel.h"

#include <QLocale>
#include <QVariantList>

#include <algorithm>

namespace OpenChat {

namespace {

// Every role an attachment's transfer moves (updateTransfer).
const QList<int> transferRoles{
    MessageListModel::TransferStateRole, MessageListModel::TransferReasonRole,
    MessageListModel::TransferProgressRole, MessageListModel::TransferTextRole,
    MessageListModel::CanCancelRole, MessageListModel::CanRetryRole,
    MessageListModel::CanSaveRole,
};

// The stored failure reasons (AttachmentFailure) the bubble tells apart.
constexpr int reasonNoSpace = 2;
constexpr int reasonSenderCancelled = 3;

// What a failed incoming attachment is called in "Couldn't receive this …".
QString receivedNoun(int attachmentKind)
{
    switch (attachmentKind) {
    case 1:
        return QStringLiteral("photo");
    case 2:
        return QStringLiteral("video");
    case 3:
        return QStringLiteral("audio file");
    default:
        return QStringLiteral("file");
    }
}

} // namespace

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
    case AttachmentKindRole:
        return message.attachmentKind;
    case FileNameRole:
        return message.fileName;
    case MimeTypeRole:
        return message.mimeType;
    case ByteCountRole:
        return double(message.byteCount);
    case SizeTextRole:
        return message.attachmentKind == 0 ? QString() : sizeText(message.byteCount);
    case MediaWidthRole:
        return message.mediaWidth;
    case MediaHeightRole:
        return message.mediaHeight;
    case DurationMsRole:
        return double(message.durationMs);
    case PeaksRole: {
        QVariantList peaks;
        peaks.reserve(message.peaks.size());
        for (const int peak : message.peaks)
            peaks.append(peak);
        return peaks;
    }
    case TransferStateRole:
        return static_cast<int>(message.transferState);
    case TransferReasonRole:
        return message.transferReason;
    case TransferProgressRole:
        if (message.transferState == AttachmentTransferState::Ready)
            return 1.0;
        return message.transferTotal > 0
            ? std::clamp(qreal(message.transferDone) / qreal(message.transferTotal), 0.0, 1.0)
            : 0.0;
    case TransferTextRole:
        return transferText(message);
    case HasPreviewRole:
        return message.hasPreview;
    case PreviewRevisionRole:
        return message.previewRevision;
    case CanCancelRole:
        return message.canCancelTransfer();
    case CanRetryRole:
        return message.canRetryTransfer();
    case CanSaveRole:
        return message.canSaveAttachment();
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
        {AttachmentKindRole, "attachmentKind"},
        {FileNameRole, "fileName"},
        {MimeTypeRole, "mimeType"},
        {ByteCountRole, "byteCount"},
        {SizeTextRole, "sizeText"},
        {MediaWidthRole, "mediaWidth"},
        {MediaHeightRole, "mediaHeight"},
        {DurationMsRole, "durationMs"},
        {PeaksRole, "peaks"},
        {TransferStateRole, "transferState"},
        {TransferReasonRole, "transferReason"},
        {TransferProgressRole, "transferProgress"},
        {TransferTextRole, "transferText"},
        {HasPreviewRole, "hasPreview"},
        {PreviewRevisionRole, "previewRevision"},
        {CanCancelRole, "canCancel"},
        {CanRetryRole, "canRetry"},
        {CanSaveRole, "canSave"},
    };
}

int MessageListModel::count() const
{
    return m_messages.size();
}

QString MessageListModel::sizeText(qint64 bytes)
{
    constexpr qint64 kib = 1024;
    constexpr qint64 mib = 1024 * kib;
    const QLocale english(QLocale::English);
    if (bytes < kib)
        return bytes == 1 ? QStringLiteral("1 byte") : QStringLiteral("%1 bytes").arg(std::max<qint64>(bytes, 0));
    if (bytes < mib)
        return QStringLiteral("%1 KB").arg(std::max<qint64>(1, (bytes + kib / 2) / kib));
    const double megabytes = double(bytes) / double(mib);
    // One decimal while it says something ("2.4 MB"), none from ten up.
    if (megabytes < 9.95)
        return QStringLiteral("%1 MB").arg(english.toString(megabytes, 'f', 1));
    return QStringLiteral("%1 MB").arg(qint64(megabytes + 0.5));
}

QString MessageListModel::durationText(qint64 durationMs)
{
    const qint64 seconds = std::max<qint64>(durationMs, 0) / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString MessageListModel::transferText(const Message &message) const
{
    if (message.kind != MessageKind::Attachment)
        return {};
    const bool outgoing = message.direction == MessageDirection::Outgoing;
    const QString sender = message.transferPeer.isEmpty() ? QStringLiteral("The sender")
                                                          : message.transferPeer;
    switch (message.transferState) {
    case AttachmentTransferState::Ready:
        return {};
    case AttachmentTransferState::Unavailable:
        return QStringLiteral("Couldn't show this attachment");
    case AttachmentTransferState::Failed:
        if (outgoing)
            return QStringLiteral("Couldn't send");
        if (message.transferReason == reasonNoSpace)
            return QStringLiteral("Not enough space to receive this");
        return QStringLiteral("Couldn't receive this %1").arg(receivedNoun(message.attachmentKind));
    case AttachmentTransferState::Cancelled:
        if (message.transferReason != reasonSenderCancelled)
            return outgoing ? QStringLiteral("Couldn't send")
                            : QStringLiteral("Couldn't receive this %1").arg(receivedNoun(message.attachmentKind));
        return outgoing ? QStringLiteral("You stopped sending this")
                        : QStringLiteral("%1 stopped sending this").arg(sender);
    case AttachmentTransferState::Transferring:
        break;
    }
    if (outgoing) {
        if (message.deliveryState == MessageDeliveryState::Failed)
            return QStringLiteral("Couldn't send");
        if (m_callActive)
            return QStringLiteral("Waiting for the call to end");
        const int percent = message.transferTotal > 0
            ? int(std::clamp<qint64>(qint64(message.transferDone) * 100 / message.transferTotal, 0, 100))
            : 0;
        return QStringLiteral("Sending… %1%").arg(percent);
    }
    // Nothing has come yet: the sender has not started (or is offline).
    if (message.transferDone <= 0 && !message.transferPeer.isEmpty())
        return QStringLiteral("Waiting for %1").arg(message.transferPeer);
    return QStringLiteral("Receiving… %1 of %2").arg(message.transferDone).arg(message.transferTotal);
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
    // Whether it can be edited follows from whether the relay took it; an
    // attachment's own actions and status line follow from it too.
    QList<int> roles{DeliveryStateRole, FailureReasonRole, EditableRole};
    if (message.kind == MessageKind::Attachment)
        roles << TransferTextRole << CanCancelRole << CanRetryRole;
    emit dataChanged(index(row), index(row), roles);
    return true;
}

bool MessageListModel::updateTransfer(const QString &stableId, AttachmentTransferState state,
                                      int reason, int done, int total)
{
    const int row = rowOf(stableId);
    if (row < 0)
        return false;
    Message &message = m_messages[row];
    if (message.transferState == state && message.transferReason == reason
        && message.transferDone == done && message.transferTotal == total)
        return true;
    message.transferState = state;
    message.transferReason = reason;
    message.transferDone = done;
    message.transferTotal = total;
    emit dataChanged(index(row), index(row), transferRoles);
    return true;
}

bool MessageListModel::bumpPreview(const QString &stableId)
{
    const int row = rowOf(stableId);
    if (row < 0)
        return false;
    Message &message = m_messages[row];
    message.hasPreview = true;
    ++message.previewRevision;
    emit dataChanged(index(row), index(row), {HasPreviewRole, PreviewRevisionRole});
    return true;
}

void MessageListModel::setCallActive(bool active)
{
    if (m_callActive == active)
        return;
    m_callActive = active;
    // Only this device's transferring attachments read differently.
    for (int row = 0; row < m_messages.size(); ++row) {
        const Message &message = m_messages.at(row);
        if (message.kind == MessageKind::Attachment && message.direction == MessageDirection::Outgoing
            && message.transferState == AttachmentTransferState::Transferring)
            emit dataChanged(index(row), index(row), {TransferTextRole});
    }
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
