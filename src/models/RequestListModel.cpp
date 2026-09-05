#include "models/RequestListModel.h"

#include <algorithm>
#include <utility>

namespace OpenChat {

RequestListModel::RequestListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int RequestListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_requests.size();
}

QVariant RequestListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.column() != 0 || index.row() < 0
        || index.row() >= m_requests.size()) {
        return {};
    }

    const RequestEntry &entry = m_requests.at(index.row());
    switch (role) {
    case IdRole:
        return entry.conversation.toHex();
    case AccountIdRole:
        return entry.account.toHex();
    case NameRole:
        return entry.displayName;
    case SubtitleRole:
        return entry.subtitle;
    case HandleRole:
        return entry.handle;
    default:
        return {};
    }
}

QHash<int, QByteArray> RequestListModel::roleNames() const
{
    return {
        {IdRole, "requestId"},
        {AccountIdRole, "accountId"},
        {NameRole, "displayName"},
        {SubtitleRole, "subtitle"},
        {HandleRole, "handle"},
    };
}

int RequestListModel::count() const
{
    return m_requests.size();
}

void RequestListModel::setRequests(QVector<RequestEntry> requests)
{
    const int previousCount = m_requests.size();
    beginResetModel();
    m_requests = std::move(requests);
    endResetModel();
    if (previousCount != m_requests.size())
        emit countChanged();
}

void RequestListModel::appendRequest(RequestEntry entry)
{
    const auto existing = std::find_if(
        m_requests.cbegin(), m_requests.cend(),
        [&entry](const RequestEntry &row) { return row.conversation == entry.conversation; });
    if (existing != m_requests.cend())
        return; // dedupe: a redelivered request never doubles a row

    const int row = m_requests.size();
    beginInsertRows({}, row, row);
    m_requests.append(std::move(entry));
    endInsertRows();
    emit countChanged();
}

bool RequestListModel::removeByConversation(const ConversationId &conversation)
{
    for (int row = 0; row < m_requests.size(); ++row) {
        if (m_requests.at(row).conversation != conversation)
            continue;
        beginRemoveRows({}, row, row);
        m_requests.removeAt(row);
        endRemoveRows();
        emit countChanged();
        return true;
    }
    return false;
}

bool RequestListModel::removeByAccount(const AccountId &account)
{
    for (int row = 0; row < m_requests.size(); ++row) {
        if (m_requests.at(row).account != account)
            continue;
        beginRemoveRows({}, row, row);
        m_requests.removeAt(row);
        endRemoveRows();
        emit countChanged();
        return true;
    }
    return false;
}

bool RequestListModel::updateHandle(const AccountId &account, const QString &handle)
{
    bool updated = false;
    for (int row = 0; row < m_requests.size(); ++row) {
        RequestEntry &entry = m_requests[row];
        if (entry.account != account)
            continue;
        entry.handle = handle;
        entry.displayName = displayNameForHandle(handle);
        entry.subtitle = subtitleForHandle(handle, account);
        emit dataChanged(index(row), index(row), {NameRole, SubtitleRole, HandleRole});
        updated = true;
    }
    return updated;
}

QString RequestListModel::displayNameForHandle(const QString &handle)
{
    return handle.isEmpty() ? QStringLiteral("New contact request")
                            : QStringLiteral("@") + handle;
}

QString RequestListModel::subtitleForHandle(const QString &handle, const AccountId &account)
{
    return handle.isEmpty() ? QStringLiteral("ID ") + account.toHex().left(10)
                            : QStringLiteral("wants to chat with you");
}

std::optional<RequestListModel::RequestEntry>
RequestListModel::byConversationHex(const QString &hex) const
{
    const auto found = std::find_if(
        m_requests.cbegin(), m_requests.cend(),
        [&hex](const RequestEntry &row) { return row.conversation.toHex() == hex; });
    if (found == m_requests.cend())
        return std::nullopt;
    return *found;
}

} // namespace OpenChat
