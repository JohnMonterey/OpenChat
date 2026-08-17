#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>

#include <optional>

#include "domain/Identifiers.h"

namespace OpenChat {

// The list of inbound contact requests awaiting the local user's decision, exposed
// to QML by ContactController. Mirrors ContactListModel / MessageListModel: a
// QAbstractListModel with a Role enum, roleNames(), data(), and a count property.
// Each row is a durable inbound request keyed by its ConversationId; the QML never
// sees raw ids beyond the hex `requestId`/`accountId` string roles.
class RequestListModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        AccountIdRole,
        NameRole,
        SubtitleRole,
        HandleRole,
    };
    Q_ENUM(Role)

    // One inbound request. conversation is the row's stable identity (the sender
    // chose it); account is the peer whose acceptance produces an Accepted contact.
    struct RequestEntry {
        ConversationId conversation;
        AccountId account;
        QString handle;
        QString displayName;
        QString subtitle;
    };

    explicit RequestListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const;

    // Replace every row (a durable roster seed on startup).
    void setRequests(QVector<RequestEntry> requests);
    // Append one request, ignoring a conversation that is already present so a
    // redelivered handshake never duplicates a row.
    void appendRequest(RequestEntry entry);
    // Remove the row for a conversation / account. Returns whether a row was removed.
    bool removeByConversation(const ConversationId &conversation);
    bool removeByAccount(const AccountId &account);
    // Look a row up by its hex `requestId` (the ConversationId hex the QML holds).
    [[nodiscard]] std::optional<RequestEntry> byConversationHex(const QString &hex) const;

signals:
    void countChanged();

private:
    QVector<RequestEntry> m_requests;
};

} // namespace OpenChat
