#include "models/ContactListModel.h"

#include <algorithm>

namespace OpenChat {

ContactListModel::ContactListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ContactListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_visibleRows.size();
}

QVariant ContactListModel::data(const QModelIndex &index, int role) const
{
    const Contact *contact = visibleContact(index.row());
    if (!contact || index.column() != 0)
        return {};

    switch (role) {
    case IdRole:
        return contact->id;
    case NameRole:
        return contact->name;
    case StatusTextRole:
        return presenceText(contact->presence);
    case PresenceRole:
        return static_cast<int>(contact->presence);
    case FavoriteRole:
        return contact->favorite;
    case SelectedRole:
        return contact->id == m_selectedId;
    case AvatarKeyRole:
        return contact->avatarKey;
    default:
        return {};
    }
}

QHash<int, QByteArray> ContactListModel::roleNames() const
{
    return {
        {IdRole, "contactId"},
        {NameRole, "name"},
        {StatusTextRole, "statusText"},
        {PresenceRole, "presence"},
        {FavoriteRole, "favorite"},
        {SelectedRole, "selected"},
        {AvatarKeyRole, "avatarKey"},
    };
}

void ContactListModel::setContacts(QVector<Contact> contacts)
{
    beginResetModel();
    m_contacts = std::move(contacts);
    m_visibleRows.clear();
    for (int row = 0; row < m_contacts.size(); ++row) {
        if (m_query.isEmpty()
            || m_contacts.at(row).name.contains(m_query, Qt::CaseInsensitive)) {
            m_visibleRows.append(row);
        }
    }
    if (contactById(m_selectedId) == std::nullopt)
        m_selectedId.clear();
    endResetModel();
    emit countsChanged();
}

void ContactListModel::setQuery(const QString &query)
{
    const QString normalized = query.trimmed();
    if (m_query == normalized)
        return;

    m_query = normalized;
    rebuildVisibleRows();
}

bool ContactListModel::selectContact(const QString &id)
{
    if (!contactById(id).has_value())
        return false;
    if (m_selectedId == id)
        return true;

    m_selectedId = id;
    if (!m_visibleRows.isEmpty())
        emit dataChanged(index(0), index(m_visibleRows.size() - 1), {SelectedRole});
    return true;
}

int ContactListModel::favoriteCount() const
{
    return static_cast<int>(std::count_if(
        m_visibleRows.cbegin(), m_visibleRows.cend(), [this](int row) {
            return m_contacts.at(row).favorite;
        }));
}

int ContactListModel::regularCount() const
{
    return m_visibleRows.size() - favoriteCount();
}

std::optional<Contact> ContactListModel::contactAt(int row) const
{
    if (const Contact *contact = visibleContact(row))
        return *contact;
    return std::nullopt;
}

std::optional<Contact> ContactListModel::contactById(const QString &id) const
{
    const auto found = std::find_if(m_contacts.cbegin(), m_contacts.cend(),
                                    [&id](const Contact &contact) { return contact.id == id; });
    if (found == m_contacts.cend())
        return std::nullopt;
    return *found;
}

void ContactListModel::rebuildVisibleRows()
{
    beginResetModel();
    m_visibleRows.clear();
    for (int row = 0; row < m_contacts.size(); ++row) {
        if (m_query.isEmpty()
            || m_contacts.at(row).name.contains(m_query, Qt::CaseInsensitive)) {
            m_visibleRows.append(row);
        }
    }
    endResetModel();
    emit countsChanged();
}

const Contact *ContactListModel::visibleContact(int row) const
{
    if (row < 0 || row >= m_visibleRows.size())
        return nullptr;
    return &m_contacts.at(m_visibleRows.at(row));
}

} // namespace OpenChat
