#pragma once

#include <QHash>
#include <QObject>

#include "models/ContactListModel.h"
#include "models/MessageListModel.h"

namespace OpenChat {

class ChatController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(ContactListModel *contacts READ contacts CONSTANT)
    Q_PROPERTY(MessageListModel *messages READ messages CONSTANT)
    Q_PROPERTY(QString currentContactName READ currentContactName NOTIFY currentContactChanged)
    Q_PROPERTY(QString currentStatusText READ currentStatusText NOTIFY currentContactChanged)
    Q_PROPERTY(QString currentAvatarKey READ currentAvatarKey NOTIFY currentContactChanged)
    Q_PROPERTY(QString composerText READ composerText WRITE setComposerText NOTIFY composerTextChanged)
    Q_PROPERTY(bool canSend READ canSend NOTIFY canSendChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)

public:
    explicit ChatController(QObject *parent = nullptr);

    [[nodiscard]] ContactListModel *contacts();
    [[nodiscard]] MessageListModel *messages();
    [[nodiscard]] QString currentContactName() const;
    [[nodiscard]] QString currentStatusText() const;
    [[nodiscard]] QString currentAvatarKey() const;
    [[nodiscard]] QString composerText() const;
    [[nodiscard]] bool canSend() const;
    [[nodiscard]] QString searchQuery() const;

    Q_INVOKABLE bool selectContact(const QString &id);
    Q_INVOKABLE void setSearchQuery(const QString &query);
    Q_INVOKABLE void setComposerText(const QString &text);
    Q_INVOKABLE bool sendMessage();

signals:
    void currentContactChanged();
    void composerTextChanged();
    void canSendChanged();
    void searchQueryChanged();

private:
    [[nodiscard]] std::optional<Contact> currentContact() const;

    ContactListModel m_contacts;
    MessageListModel m_messages;
    QHash<QString, QVector<Message>> m_messagesByContact;
    QString m_currentContactId;
    QString m_composerText;
    QString m_searchQuery;
};

} // namespace OpenChat
