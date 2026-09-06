#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QStringList>
#include <QUrl>

#include <optional>

#include "call/CallEngine.h"
#include "domain/ChatTypes.h"
#include "domain/Identifiers.h"
#include "domain/ProfileUpdate.h"
#include "models/ContactListModel.h"
#include "models/MessageListModel.h"

#include <QVariantList>

namespace OpenChat {

class ContactRequestService;
class GroupService;
class ProfileSession;
class SyncEngine;
class RelayClient;

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
    Q_PROPERTY(bool conversationVisible READ conversationVisible WRITE setConversationVisible NOTIFY conversationVisibleChanged)
    Q_PROPERTY(ContactListModel *contacts READ contacts CONSTANT)
    Q_PROPERTY(MessageListModel *messages READ messages CONSTANT)
    Q_PROPERTY(bool localOnline READ localOnline NOTIFY localOnlineChanged)
    Q_PROPERTY(QString localUserName READ localUserName NOTIFY localUserNameChanged)
    // The local user's self-published profile. localStatusLine is what the
    // sidebar prints under the name: the custom status text, or the name of the
    // presence contacts currently see (Offline while the relay link is down).
    Q_PROPERTY(QString localStatusText READ localStatusText NOTIFY localProfileChanged)
    Q_PROPERTY(QString localStatusLine READ localStatusLine NOTIFY localProfileChanged)
    Q_PROPERTY(int localPresence READ localPresence NOTIFY localProfileChanged)
    Q_PROPERTY(QString localAvatarKey READ localAvatarKey NOTIFY localProfileChanged)
    // A one-line explanation of why a profile change was refused (e.g. an
    // unusable picture file); empty when there is nothing to say.
    Q_PROPERTY(QString profileNotice READ profileNotice NOTIFY profileNoticeChanged)
    Q_PROPERTY(bool hasCurrentContact READ hasCurrentContact NOTIFY currentContactChanged)
    Q_PROPERTY(QString currentContactName READ currentContactName NOTIFY currentContactChanged)
    Q_PROPERTY(QString currentStatusText READ currentStatusText NOTIFY currentContactChanged)
    Q_PROPERTY(QString currentAvatarKey READ currentAvatarKey NOTIFY currentContactChanged)
    Q_PROPERTY(int currentPresence READ currentPresence NOTIFY currentContactChanged)
    // Group chats. currentIsGroup says whether the open chat is a group; the
    // title is what the members named it (empty until someone does, in which
    // case currentContactName lists the members instead); currentGroupMembers
    // is the one-line roster shown under the title ("You, Alice, Bob").
    Q_PROPERTY(bool currentIsGroup READ currentIsGroup NOTIFY currentContactChanged)
    Q_PROPERTY(QString currentGroupTitle READ currentGroupTitle NOTIFY currentContactChanged)
    Q_PROPERTY(QString currentGroupMembers READ currentGroupMembers NOTIFY currentContactChanged)
    Q_PROPERTY(int currentGroupMemberCount READ currentGroupMemberCount NOTIFY currentContactChanged)
    // A one-line explanation of why a group change was refused; empty when
    // there is nothing to say.
    Q_PROPERTY(QString groupNotice READ groupNotice NOTIFY groupNoticeChanged)
    // Contacts the "+" next to the current chat's name can add: everyone
    // accepted who is not already in it (for a one-to-one chat, everyone but
    // the person it is with). Each entry is {contactId, name, avatarKey}.
    Q_PROPERTY(QVariantList groupCandidates READ groupCandidates NOTIFY groupCandidatesChanged)
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
    bool conversationVisible() const { return m_conversationVisible; }
    void setConversationVisible(bool visible);
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
    [[nodiscard]] int currentPresence() const;
    [[nodiscard]] bool currentIsGroup() const;
    [[nodiscard]] QString currentGroupTitle() const;
    [[nodiscard]] QString currentGroupMembers() const;
    [[nodiscard]] int currentGroupMemberCount() const;
    [[nodiscard]] QString groupNotice() const { return m_groupNotice; }
    [[nodiscard]] QVariantList groupCandidates() const;
    [[nodiscard]] QString localStatusText() const { return m_localStatusText; }
    [[nodiscard]] QString localStatusLine() const;
    [[nodiscard]] int localPresence() const { return m_localPresence; }
    [[nodiscard]] QString profileNotice() const { return m_profileNotice; }
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
    void setPresenceRelay(RelayClient *relay);
    [[nodiscard]] bool localOnline() const { return m_localOnline; }
    [[nodiscard]] bool isLive() const noexcept { return m_live; }

    Q_INVOKABLE bool selectContact(const QString &id);
    Q_INVOKABLE void setSearchQuery(const QString &query);
    Q_INVOKABLE void setLocalUserName(const QString &name);
    // Profile edits. Each persists to the profile (live mode), updates the
    // sidebar, and publishes the whole profile to every accepted contact as an
    // end-to-end encrypted ProfileUpdate. The status text is trimmed and capped
    // at maxStatusTextLength; an empty one shows the presence name instead. The
    // presence must be a Presence value (Offline means "appear offline").
    Q_INVOKABLE void setLocalStatusText(const QString &text);
    Q_INVOKABLE void setLocalPresence(int presence);
    // Reads the chosen image file, refuses anything unrealistic (see
    // render/ProfileImage.h), scales and JPEG-compresses it locally, then stores
    // and publishes it. Returns false and sets profileNotice on refusal.
    Q_INVOKABLE bool setLocalAvatarFromFile(const QUrl &file);
    Q_INVOKABLE void clearProfileNotice();
    // The "+" next to the current chat's name. In a one-to-one chat it starts
    // a new group with that person and `contactId`; in a group it adds
    // `contactId` to it. Live mode claims KeyPackages first, so the new group
    // appears (and is opened) a moment later, or groupNotice says why not.
    Q_INVOKABLE void addToGroup(const QString &contactId);
    // Renames the open group the way the status line is edited: trimmed,
    // capped, told to every member. Returns false when no group is open.
    Q_INVOKABLE bool renameCurrentGroup(const QString &title);
    // Leaves the open group: the others are told and the chat disappears here.
    Q_INVOKABLE void leaveCurrentGroup();
    Q_INVOKABLE void clearGroupNotice();
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
                         ContactRequestService *requests, GroupService *groups = nullptr);

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
    // Resolve an incoming caller from the conversation and authenticated sender,
    // independently of the selected chat or roster search filter.
    [[nodiscard]] std::optional<CallRoute> callRouteFor(const ConversationId &conversation,
                                                      const DeviceId &device) const;
    // The same for a group chat: every member as a call peer, so a group call
    // can ring them all. Empty for anything but an open live group with at
    // least one member.
    [[nodiscard]] std::optional<CallEngine::GroupCallRoute>
    groupCallRouteFor(const QString &chatId) const;
    [[nodiscard]] std::optional<CallEngine::GroupCallRoute>
    groupCallRouteFor(const ConversationId &conversation) const;
    // The chat id ("group:" + conversation hex) of a live group, or empty.
    [[nodiscard]] QString groupChatIdFor(const ConversationId &conversation) const;
    [[nodiscard]] static bool isGroupChatId(const QString &chatId);

    // The contact currently open in the conversation pane, or empty when none
    // is. This is what the header's call button acts on.
    [[nodiscard]] QString currentContactId() const { return m_currentContactId; }

    // The identity to show for this user on the call screen and in the sidebar:
    // the content key of the chosen picture, or the neutral artwork.
    [[nodiscard]] QString localAvatarKey() const { return m_localAvatarKey; }
    // The local profile as it would be published (also what tests decode).
    [[nodiscard]] ProfileUpdateMessage localProfile() const;

signals:
    void conversationVisibleChanged();
    void currentContactChanged();
    void localUserNameChanged();
    void localProfileChanged();
    void profileNoticeChanged();
    void composerTextChanged();
    void canSendChanged();
    void searchQueryChanged();
    void sessionStateChanged();
    void navSectionChanged();
    void localOnlineChanged();
    void chatUnreadCountChanged();
    void currentSettingsCategoryChanged();
    void groupNoticeChanged();
    void groupCandidatesChanged();

    // A message arrived that the desktop should announce: the chat it belongs
    // to (an AccountId hex, the same id selectContact takes), the sender's
    // display name, what they wrote, and their avatar key.
    //
    // Emitted for every inbound message, including one in the conversation
    // already on screen: whether the user is actually looking at it is a
    // question about window focus, which the notification service answers.
    // `body` carries the message text only in states whose plaintext the
    // interface itself would show; the states that withhold plaintext
    // (Locked, Quarantined, DeviceChanged) announce that a message arrived
    // without repeating it to the desktop's notification service, which is a
    // different trust domain from the encrypted store it came out of.
    void messageNotificationRequested(const QString &contactId, const QString &senderName,
                                      const QString &body, const QString &avatarKey);

private:
    bool m_conversationVisible = true;
    void refreshChatActivity(bool markCurrentRead = true);
    // A live chat's routing: the peer account, the 2-party MLS conversation, and
    // the peer device every envelope is addressed to.
    struct LiveChat final {
        AccountId account;
        ConversationId conversation;
        std::optional<DeviceId> peerDevice;
        QString handle;
        int unread = 0;
        // The peer's last published profile (see ContactRecord).
        int presence = 0;
        QString statusText;
        QString avatarKey;
    };

    // A group chat's routing and roster. In mock mode the same shape carries a
    // group made from the reference contacts, so the surface can be exercised
    // without a session.
    struct GroupMember final {
        AccountId account = AccountId::generate();
        DeviceId device = DeviceId::generate();
        QString contactId; // the roster row when the member is a contact
        QString name;
        QString avatarKey;
    };
    struct LiveGroup final {
        ConversationId conversation = ConversationId::generate();
        QString title;
        QVector<GroupMember> members; // everyone but this device
        int unread = 0;
    };

    [[nodiscard]] std::optional<Contact> currentContact() const;
    [[nodiscard]] const LiveGroup *currentGroup() const;
    [[nodiscard]] Contact groupRowFor(const QString &id, const LiveGroup &group) const;
    [[nodiscard]] QString groupDisplayName(const LiveGroup &group) const;
    [[nodiscard]] QString memberName(const GroupMember &member) const;
    void loadGroups(QVector<Contact> &rows, QHash<QByteArray, QString> &byConversation);
    void onGroupCreated(const ConversationId &conversation);
    void onGroupChanged(const ConversationId &conversation);
    void onGroupLeft(const ConversationId &conversation);
    void setGroupNotice(const QString &notice);
    [[nodiscard]] Message messageFor(const MessageRecord &record) const;
    // Mock-mode group operations, so the QML surface works in previews.
    void mockAddToGroup(const QString &contactId);
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
    void refreshPresence();
    void setDevicePresence(const DeviceId &device, bool online);
    [[nodiscard]] Contact contactRowFor(const LiveChat &chat) const;
    [[nodiscard]] QVector<Message> loadHistory(const ConversationId &conversation) const;
    [[nodiscard]] static Message toMessage(const MessageRecord &record);
    [[nodiscard]] QString contactForConversation(const ConversationId &conversation) const;
    void onMessageQueued(const MessageRecord &record);
    void onMessageReceived(const MessageRecord &record);
    void onMessageStateChanged(const MessageId &messageId, DeliveryState state);
    void onContactAccepted(const AccountId &account);
    void onProfileUpdateReceived(const ConversationId &conversation, const DeviceId &senderDevice,
                                 const QByteArray &payload);
    // What a contact's row shows: their chosen presence while their device is
    // reachable, Offline otherwise.
    [[nodiscard]] Presence displayedPresence(const LiveChat &chat) const;
    // Publishes the local profile to one chat / every chat with a known device.
    void sendProfileTo(const LiveChat &chat);
    void broadcastProfile();
    void setProfileNotice(const QString &notice);
    void updateCanSend(bool wasSendable);

    ContactListModel m_contacts;
    MessageListModel m_messages;
    QHash<QString, QVector<Message>> m_messagesByContact;
    QString m_currentContactId;
    QString m_localUserName;
    QString m_localStatusText;
    int m_localPresence = 0;
    QByteArray m_localAvatarJpeg;
    QString m_localAvatarKey = QStringLiteral("userpfp_none");
    QString m_profileNotice;
    QString m_groupNotice;
    QString m_composerText;
    QString m_searchQuery;
    SessionState m_sessionState = SessionState::Ready;
    NavSection m_navSection = NavSection::Chat;
    int m_currentSettingsCategory = 0;

    // Live seam (null in mock mode). Borrowed; owned by the app runtime and kept
    // alive past this controller.
    bool m_live = false;
    bool m_localOnline = true; // reference rendering
    RelayClient *m_presenceRelay = nullptr;
    QTimer m_presenceTimer;
    QHash<QByteArray, qint64> m_onlineDevices;
    ProfileSession *m_session = nullptr;
    SyncEngine *m_engine = nullptr;
    ContactRequestService *m_requests = nullptr;
    GroupService *m_groups = nullptr;
    QHash<QString, LiveChat> m_liveChats;          // keyed by AccountId hex (== Contact.id)
    QHash<QString, LiveGroup> m_liveGroups;        // keyed by "group:" + ConversationId hex
    QHash<QByteArray, QString> m_contactByConversation; // ConversationId bytes -> Contact.id
    int m_mockGroupCounter = 0;
};

} // namespace OpenChat
