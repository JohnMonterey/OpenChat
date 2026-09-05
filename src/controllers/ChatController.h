#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>

#include <optional>

#include "domain/ChatTypes.h"
#include "domain/Identifiers.h"
#include "models/ContactListModel.h"
#include "models/MessageListModel.h"

namespace OpenChat {

class ContactRequestService;
class ProfileSession;
class SyncEngine;

// The QML-facing bridge for the conversation surface: the chat list, the open
// conversation's history, and the composer.
//
// Dual-world, like ContactController: constructed with no services it runs the
// reference mock roster and conversation the --capture path and the QML tests
// render; setLiveServices() swaps in the real profile (Accepted contacts from
// the durable roster, history from the message store) and routes sends and
// receives through the SyncEngine. The live seam is never touched under
// --capture, so the approved default rendering is unchanged.
class ChatController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(ContactListModel *contacts READ contacts CONSTANT)
    Q_PROPERTY(MessageListModel *messages READ messages CONSTANT)
    Q_PROPERTY(QString localUserName READ localUserName NOTIFY localUserNameChanged)
    Q_PROPERTY(bool hasCurrentContact READ hasCurrentContact NOTIFY currentContactChanged)
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
    Q_PROPERTY(int chatUnreadCount READ chatUnreadCount NOTIFY chatUnreadCountChanged)
    Q_PROPERTY(int callMissedCount READ callMissedCount CONSTANT)
    Q_PROPERTY(int callCount READ callCount CONSTANT)
    Q_PROPERTY(QStringList settingsCategories READ settingsCategories CONSTANT)
    Q_PROPERTY(int currentSettingsCategory READ currentSettingsCategory WRITE
                   setCurrentSettingsCategory NOTIFY currentSettingsCategoryChanged)
    Q_PROPERTY(QString currentSettingsCategoryName READ currentSettingsCategoryName NOTIFY
                   currentSettingsCategoryChanged)
    Q_PROPERTY(QStringList currentSettingsElements READ currentSettingsElements NOTIFY
                   currentSettingsCategoryChanged)

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
    ~ChatController() override;

    [[nodiscard]] ContactListModel *contacts();
    [[nodiscard]] MessageListModel *messages();
    [[nodiscard]] QString localUserName() const;
    [[nodiscard]] bool hasCurrentContact() const;
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
    [[nodiscard]] int callCount() const;
    [[nodiscard]] QStringList settingsCategories() const;
    [[nodiscard]] int currentSettingsCategory() const;
    [[nodiscard]] QString currentSettingsCategoryName() const;
    [[nodiscard]] QStringList currentSettingsElements() const;
    [[nodiscard]] bool isLive() const noexcept { return m_live; }

    Q_INVOKABLE bool selectContact(const QString &id);
    Q_INVOKABLE void setSearchQuery(const QString &query);
    Q_INVOKABLE void setLocalUserName(const QString &name);
    Q_INVOKABLE void setComposerText(const QString &text);
    Q_INVOKABLE bool sendMessage();
    Q_INVOKABLE void setSessionState(SessionState state);
    Q_INVOKABLE void setNavSection(NavSection section);
    Q_INVOKABLE void setCurrentSettingsCategory(int index);

    // C++-only live seam. Replaces the mock roster with the profile's Accepted
    // contacts, loads history from the message store on selection, sends through
    // the engine, and appends inbound/outbound rows from the engine's signals. A
    // contact that becomes Accepted later (either side of a request) appears as
    // a chat through the request service's contactAccepted. All three are
    // borrowed and must outlive this controller.
    void setLiveServices(ProfileSession *session, SyncEngine *engine,
                         ContactRequestService *requests);

    // Re-reads one contact's roster row (e.g. after its handle was resolved) and
    // refreshes its list row. Live mode only; hex is the AccountId hex.
    void refreshContact(const QString &accountHex);

    // Everything a voice call needs to address a contact: the shared MLS
    // conversation, the device its envelopes go to, and how to render it while
    // ringing. Empty when the contact is unknown or has no resolved device yet
    // (an accepted contact whose device is still unknown cannot be called), and
    // always empty in mock mode, where no conversation exists to call over.
    struct CallRoute final {
        ConversationId conversation;
        DeviceId device;
        QString contactId;
        QString displayName;
        QString avatarKey;
    };
    [[nodiscard]] std::optional<CallRoute> callRouteFor(const QString &contactId) const;

    // The contact currently open in the conversation pane, or empty when none
    // is. This is what the header's call button acts on.
    [[nodiscard]] QString currentContactId() const { return m_currentContactId; }

    // The identity to show for this user on the call screen.
    [[nodiscard]] QString localAvatarKey() const;

signals:
    void currentContactChanged();
    void localUserNameChanged();
    void composerTextChanged();
    void canSendChanged();
    void searchQueryChanged();
    void sessionStateChanged();
    void navSectionChanged();
    void chatUnreadCountChanged();
    void currentSettingsCategoryChanged();

private:
    // A live chat's routing: the peer account, the 2-party MLS conversation, and
    // the peer device every envelope is addressed to.
    struct LiveChat final {
        AccountId account;
        ConversationId conversation;
        std::optional<DeviceId> peerDevice;
        QString handle;
        int unread = 0;
    };

    [[nodiscard]] std::optional<Contact> currentContact() const;
    // True only in states whose already-verified, locally-stored history is safe
    // to display. Locked/Quarantined/DeviceChanged withhold plaintext.
    [[nodiscard]] static bool statePermitsPlaintext(SessionState state);
    // Reflects the current conversation into the exposed model, clearing it when
    // the current state withholds plaintext.
    void refreshVisibleMessages();
    // Live: whether the current chat can be addressed at all.
    [[nodiscard]] bool currentLiveChatSendable() const;

    // Live roster and history.
    void loadRoster();
    [[nodiscard]] static Contact contactRowFor(const LiveChat &chat);
    [[nodiscard]] QVector<Message> loadHistory(const ConversationId &conversation) const;
    [[nodiscard]] static Message toMessage(const MessageRecord &record);
    [[nodiscard]] QString contactForConversation(const ConversationId &conversation) const;
    void onMessageQueued(const MessageRecord &record);
    void onMessageReceived(const MessageRecord &record);
    void onMessageStateChanged(const MessageId &messageId, DeliveryState state);
    void onContactAccepted(const AccountId &account);
    void updateCanSend(bool wasSendable);

    ContactListModel m_contacts;
    MessageListModel m_messages;
    QHash<QString, QVector<Message>> m_messagesByContact;
    QString m_currentContactId;
    QString m_localUserName;
    QString m_composerText;
    QString m_searchQuery;
    SessionState m_sessionState = SessionState::Ready;
    NavSection m_navSection = NavSection::Chat;
    int m_currentSettingsCategory = 0;

    // Live seam (null in mock mode). Borrowed; owned by the app runtime and kept
    // alive past this controller.
    bool m_live = false;
    ProfileSession *m_session = nullptr;
    SyncEngine *m_engine = nullptr;
    ContactRequestService *m_requests = nullptr;
    QHash<QString, LiveChat> m_liveChats;          // keyed by AccountId hex (== Contact.id)
    QHash<QByteArray, QString> m_contactByConversation; // ConversationId bytes -> Contact.id
};

} // namespace OpenChat
