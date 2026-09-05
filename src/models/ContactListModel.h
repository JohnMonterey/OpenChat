#pragma once

#include <QAbstractListModel>
#include <QVector>

#include <optional>

#include "models/Contact.h"

namespace OpenChat {

class ContactListModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int favoriteCount READ favoriteCount NOTIFY countsChanged)
    Q_PROPERTY(int regularCount READ regularCount NOTIFY countsChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        StatusTextRole,
        PresenceRole,
        FavoriteRole,
        SelectedRole,
        AvatarKeyRole,
        IsGroupRole,
    };
    Q_ENUM(Role)

    explicit ContactListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setContacts(QVector<Contact> contacts);
    void setQuery(const QString &query);
    bool selectContact(const QString &id);
    void setPresence(const QString &id, Presence presence);

    [[nodiscard]] int favoriteCount() const;
    [[nodiscard]] int regularCount() const;
    [[nodiscard]] std::optional<Contact> contactAt(int row) const;
    [[nodiscard]] std::optional<Contact> contactById(const QString &id) const;

signals:
    void countsChanged();

private:
    void rebuildVisibleRows();
    [[nodiscard]] const Contact *visibleContact(int row) const;

    QVector<Contact> m_contacts;
    QVector<int> m_visibleRows;
    QString m_query;
    QString m_selectedId;
};

} // namespace OpenChat
