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
    Q_PROPERTY(SessionState sessionState READ sessionState NOTIFY sessionStateChanged)
    Q_PROPERTY(QString sessionStateText READ sessionStateText NOTIFY sessionStateChanged)
    Q_PROPERTY(bool plaintextVisible READ plaintextVisible NOTIFY sessionStateChanged)
    Q_PROPERTY(QString securityNoticeText READ securityNoticeText NOTIFY sessionStateChanged)
    Q_PROPERTY(NavSection navSection READ navSection NOTIFY navSectionChanged)
    Q_PROPERTY(int chatUnreadCount READ chatUnreadCount CONSTANT)
    Q_PROPERTY(int callMissedCount READ callMissedCount CONSTANT)

public:
    // Connection/security posture of the conversation surface. Ready is the only
    // state that renders the approved chat interface unchanged; every other
    // state adds an explanatory banner and, where trust is unresolved, withholds
    // message plaintext entirely (see plaintextVisible).
    enum class SessionState {
        Ready,
        Locked,
        Offline,
        Reconnecting,
        Quarantined,
        DeviceChanged,
        StorageFull,
    };
    Q_ENUM(SessionState)

    // Which top-level section the sidebar navigation is showing. Chat is the
    // default and renders the approved conversation interface unchanged; Call
    // and Settings swap in placeholder panes while the chat pane is hidden.
    enum class NavSection {
        Chat,
        Call,
        Settings,
    };
    Q_ENUM(NavSection)

    explicit ChatController(QObject *parent = nullptr);

    [[nodiscard]] ContactListModel *contacts();
    [[nodiscard]] MessageListModel *messages();
    [[nodiscard]] QString currentContactName() const;
    [[nodiscard]] QString currentStatusText() const;
    [[nodiscard]] QString currentAvatarKey() const;
    [[nodiscard]] QString composerText() const;
    [[nodiscard]] bool canSend() const;
    [[nodiscard]] QString searchQuery() const;
    [[nodiscard]] SessionState sessionState() const;
    [[nodiscard]] QString sessionStateText() const;
    [[nodiscard]] bool plaintextVisible() const;
    [[nodiscard]] QString securityNoticeText() const;
    [[nodiscard]] NavSection navSection() const;
    [[nodiscard]] int chatUnreadCount() const;
    [[nodiscard]] int callMissedCount() const;

    Q_INVOKABLE bool selectContact(const QString &id);
    Q_INVOKABLE void setSearchQuery(const QString &query);
    Q_INVOKABLE void setComposerText(const QString &text);
    Q_INVOKABLE bool sendMessage();
    Q_INVOKABLE void setSessionState(SessionState state);
    Q_INVOKABLE void setNavSection(NavSection section);

signals:
    void currentContactChanged();
    void composerTextChanged();
    void canSendChanged();
    void searchQueryChanged();
    void sessionStateChanged();
    void navSectionChanged();

private:
    [[nodiscard]] std::optional<Contact> currentContact() const;
    // True only in states whose already-verified, locally-stored history is safe
    // to display. Locked/Quarantined/DeviceChanged withhold plaintext.
    [[nodiscard]] static bool statePermitsPlaintext(SessionState state);
    // Reflects the current conversation into the exposed model, clearing it when
    // the current state withholds plaintext.
    void refreshVisibleMessages();

    ContactListModel m_contacts;
    MessageListModel m_messages;
    QHash<QString, QVector<Message>> m_messagesByContact;
    QString m_currentContactId;
    QString m_composerText;
    QString m_searchQuery;
    SessionState m_sessionState = SessionState::Ready;
    NavSection m_navSection = NavSection::Chat;
};

} // namespace OpenChat
