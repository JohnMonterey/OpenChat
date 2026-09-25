#include "models/StagedAttachmentModel.h"

#include "models/MessageListModel.h"

#include <algorithm>
#include <cmath>

namespace OpenChat {

StagedAttachmentModel::StagedAttachmentModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int StagedAttachmentModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant StagedAttachmentModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.column() != 0 || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const StagedAttachment &item = m_items.at(index.row());
    switch (role) {
    case StagedIdRole:
        return item.id;
    case KindRole:
        return item.kind;
    case NameRole:
        return item.name;
    case SizeTextRole:
        return item.byteCount > 0 ? MessageListModel::sizeText(item.byteCount) : QString();
    case ProgressRole:
        return item.ready ? 1.0 : std::clamp<qreal>(item.progress, 0.0, 1.0);
    case ReadyRole:
        return item.ready;
    case FailedRole:
        return item.failed;
    case ErrorRole:
        return item.error;
    case NoticeRole:
        return item.notice;
    case PreviewKeyRole:
        return item.previewKey;
    case DurationTextRole:
        return item.durationMs > 0 ? MessageListModel::durationText(item.durationMs) : QString();
    default:
        return {};
    }
}

QHash<int, QByteArray> StagedAttachmentModel::roleNames() const
{
    return {
        {StagedIdRole, "stagedId"},
        {KindRole, "kind"},
        {NameRole, "name"},
        {SizeTextRole, "sizeText"},
        {ProgressRole, "progress"},
        {ReadyRole, "ready"},
        {FailedRole, "failed"},
        {ErrorRole, "error"},
        {NoticeRole, "notice"},
        {PreviewKeyRole, "previewKey"},
        {DurationTextRole, "durationText"},
    };
}

void StagedAttachmentModel::append(const StagedAttachment &item)
{
    const int row = int(m_items.size());
    beginInsertRows({}, row, row);
    m_items.append(item);
    endInsertRows();
    emit countChanged();
}

bool StagedAttachmentModel::remove(const QString &id)
{
    const int row = rowOf(id);
    if (row < 0)
        return false;
    beginRemoveRows({}, row, row);
    m_items.removeAt(row);
    endRemoveRows();
    emit countChanged();
    return true;
}

void StagedAttachmentModel::clear()
{
    if (m_items.isEmpty())
        return;
    beginResetModel();
    m_items.clear();
    endResetModel();
    emit countChanged();
}

bool StagedAttachmentModel::update(const StagedAttachment &item)
{
    const int row = rowOf(item.id);
    if (row < 0)
        return false;
    StagedAttachment &current = m_items[row];
    QList<int> roles;
    if (current.kind != item.kind)
        roles << KindRole;
    if (current.name != item.name)
        roles << NameRole;
    if (current.byteCount != item.byteCount)
        roles << SizeTextRole;
    if (current.progress != item.progress || current.ready != item.ready)
        roles << ProgressRole;
    if (current.ready != item.ready)
        roles << ReadyRole;
    if (current.failed != item.failed)
        roles << FailedRole;
    if (current.error != item.error)
        roles << ErrorRole;
    if (current.notice != item.notice)
        roles << NoticeRole;
    if (current.previewKey != item.previewKey)
        roles << PreviewKeyRole;
    if (current.durationMs != item.durationMs)
        roles << DurationTextRole;
    current = item;
    if (!roles.isEmpty())
        emit dataChanged(index(row), index(row), roles);
    return true;
}

bool StagedAttachmentModel::setProgress(const QString &id, qreal progress)
{
    const int row = rowOf(id);
    if (row < 0)
        return false;
    StagedAttachment &current = m_items[row];
    progress = std::clamp<qreal>(progress, 0.0, 1.0);
    const bool moved = std::abs(progress - current.progress) >= 0.01
        || (progress >= 1.0 && current.progress < 1.0);
    if (!moved)
        return true;
    current.progress = progress;
    emit dataChanged(index(row), index(row), {ProgressRole});
    return true;
}

std::optional<StagedAttachment> StagedAttachmentModel::item(const QString &id) const
{
    const int row = rowOf(id);
    if (row < 0)
        return std::nullopt;
    return m_items.at(row);
}

int StagedAttachmentModel::rowOf(const QString &id) const
{
    if (id.isEmpty())
        return -1;
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).id == id)
            return row;
    }
    return -1;
}

} // namespace OpenChat
