#include "controllers/ChatController.h"
#include "network/RelayClient.h"

#include "app/ContactRequestService.h"
#include "app/GroupService.h"
#include "app/ProfileSession.h"
#include "domain/Contact.h"
#include "domain/GroupUpdate.h"
#include "network/SyncEngine.h"
#include "render/AvatarStore.h"
#include "render/ProfileImage.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"

#include <QDateTime>

#include <algorithm>

namespace OpenChat {

namespace {

constexpr int historyPageSize = 200;

QVector<Contact> referenceContacts()
{
    return {
        {"michael", "Michael", Presence::Available, true, "michael"},
        {"sarah", "Sarah", Presence::Away, true, "sarah"},
        {"alex", "Alex", Presence::Available, false, "alex"},
        {"jessica", "Jessica", Presence::Available, false, "jessica"},
        {"ryan", "Ryan", Presence::Away, false, "ryan"},
        {"tom", "Tom", Presence::Offline, false, "mono"},
    };
}

QVector<Message> michaelConversation()
{
    const QDate referenceDate(2010, 5, 24);
    return {
        {MessageDirection::Incoming, "Hey Daniel!", QTime(10, 15), MessageKind::Text,
         referenceDate},
        {MessageDirection::Outgoing, "Hey Michael, how’s it going?", QTime(10, 16),
         MessageKind::Text, referenceDate},
        {MessageDirection::Incoming, "Pretty good, just working on some stuff. You?",
         QTime(10, 17), MessageKind::Text, referenceDate},
        {MessageDirection::Outgoing, "Same here. Almost done for the day thankfully.",
         QTime(10, 18), MessageKind::Text, referenceDate},
        {MessageDirection::Incoming, QString::fromUtf8("🙂"), QTime(10, 18),
         MessageKind::Emoji, referenceDate},
    };
}

QStringList settingsCategoryNames()
{
    return {
        QStringLiteral("General"),        QStringLiteral("Account & Profile"),
        QStringLiteral("Privacy"),        QStringLiteral("Notifications"),
        QStringLiteral("Audio & Video"),  QStringLiteral("Appearance"),
        QStringLiteral("About"),
    };
}

// Element labels shown in the detail pane for each settings category, indexed
// to match settingsCategoryNames(). These are presentation stubs; wiring the
// individual controls to real preferences comes later.
QStringList settingsElementsForCategory(int index)
{
    switch (index) {
    case 0:
        return {QStringLiteral("Language"), QStringLiteral("Show in taskbar"),
                QStringLiteral("Launch on startup"), QStringLiteral("On close, keep running")};
    case 1:
        return {QStringLiteral("Display name"), QStringLiteral("Presence status"),
                QStringLiteral("Profile picture"), QStringLiteral("Manage account")};
    case 2:
        return {QStringLiteral("Read receipts"), QStringLiteral("Who can contact me"),
                QStringLiteral("Blocked contacts"), QStringLiteral("Typing indicators")};
    case 3:
        return {QStringLiteral("Message notifications"), QStringLiteral("Call notifications"),
                QStringLiteral("Notification sounds"), QStringLiteral("Do not disturb")};
    case 4:
        return {QStringLiteral("Microphone"), QStringLiteral("Speakers"),
                QStringLiteral("Camera"), QStringLiteral("Ringtone")};
    case 5:
        return {QStringLiteral("Theme"), QStringLiteral("Chat font size"),
                QStringLiteral("Bubble style"), QStringLiteral("Compact contact list")};
    case 6:
        return {QStringLiteral("Version"), QStringLiteral("What's new"),
                QStringLiteral("Licenses"), QStringLiteral("Check for updates")};
    default:
        return {};
    }
}

MessageDeliveryState toModelState(DeliveryState state)
{
    switch (state) {
    case DeliveryState::Draft:
        return MessageDeliveryState::None;
    case DeliveryState::Queued:
        return MessageDeliveryState::Queued;
    case DeliveryState::Sending:
        return MessageDeliveryState::Sending;
    case DeliveryState::Sent:
        return MessageDeliveryState::Sent;
    case DeliveryState::Delivered:
        return MessageDeliveryState::Delivered;
    case DeliveryState::Read:
        return MessageDeliveryState::Read;
    case DeliveryState::Failed:
        return MessageDeliveryState::Failed;
    }
    return MessageDeliveryState::None;
}

// A short, id-free label for a contact whose handle is not (yet) known.
QString shortIdLabel(const AccountId &account)
{
    return QStringLiteral("ID ") + account.toHex().left(10);
}

const QString groupIdPrefix = QStringLiteral("group:");
const QString groupAvatarKey = QStringLiteral("group");

} // namespace

ChatController::ChatController(QObject *parent)
    : QObject(parent)
{
    m_contacts.setContacts(referenceContacts());
    m_messagesByContact.insert(QStringLiteral("michael"), michaelConversation());
    for (const QString &id : {QStringLiteral("sarah"), QStringLiteral("alex"),
                              QStringLiteral("jessica"), QStringLiteral("ryan"),
                              QStringLiteral("tom")}) {
        m_messagesByContact.insert(id, {});
    }

    m_currentContactId = QStringLiteral("michael");
    m_contacts.selectContact(m_currentContactId);
    refreshVisibleMessages();
}

ChatController::~ChatController() = default;

ContactListModel *ChatController::contacts()
{
    return &m_contacts;
}

MessageListModel *ChatController::messages()
{
    return &m_messages;
}

QString ChatController::localUserName() const
{
    return m_localUserName;
}

bool ChatController::hasCurrentContact() const
{
    return currentContact().has_value();
}

QString ChatController::currentContactName() const
{
    const auto contact = currentContact();
    return contact ? contact->name : QString();
}

QString ChatController::currentStatusText() const
{
    const auto contact = currentContact();
    if (!contact)
        return QString();
    if (contact->isGroup)
        return currentGroupMembers();
    return contact->statusText.isEmpty() ? presenceText(contact->presence) : contact->statusText;
}

bool ChatController::isGroupChatId(const QString &chatId)
{
    return chatId.startsWith(groupIdPrefix);
}

const ChatController::LiveGroup *ChatController::currentGroup() const
{
    const auto group = m_liveGroups.constFind(m_currentContactId);
    return group == m_liveGroups.cend() ? nullptr : &*group;
}

bool ChatController::currentIsGroup() const
{
    return currentGroup() != nullptr;
}

QString ChatController::currentGroupTitle() const
{
    const LiveGroup *group = currentGroup();
    return group ? group->title : QString();
}

QString ChatController::memberName(const GroupMember &member) const
{
    // A member who is also a contact goes by the roster's name (a resolved
    // handle beats whatever name their inviter knew them by).
    if (!member.contactId.isEmpty()) {
        if (const auto contact = m_contacts.contactById(member.contactId))
            return contact->name;
    }
    return member.name.isEmpty() ? shortIdLabel(member.account) : member.name;
}

QString ChatController::currentGroupMembers() const
{
    const LiveGroup *group = currentGroup();
    if (group == nullptr)
        return QString();
    QStringList names;
    for (const GroupMember &member : group->members)
        names.append(memberName(member));
    // Alphabetical, so every device lists the same group the same way.
    std::sort(names.begin(), names.end(),
              [](const QString &a, const QString &b) { return a.localeAwareCompare(b) < 0; });
    names.prepend(QStringLiteral("You"));
    return names.join(QStringLiteral(", "));
}

int ChatController::currentGroupMemberCount() const
{
    const LiveGroup *group = currentGroup();
    return group ? group->members.size() + 1 : 0;
}

QString ChatController::groupDisplayName(const LiveGroup &group) const
{
    if (!group.title.isEmpty())
        return group.title;
    // Untitled: the members name it, the way a phone names a group thread.
    QStringList names;
    for (const GroupMember &member : group.members)
        names.append(memberName(member));
    std::sort(names.begin(), names.end(),
              [](const QString &a, const QString &b) { return a.localeAwareCompare(b) < 0; });
    return names.isEmpty() ? QStringLiteral("New group") : names.join(QStringLiteral(", "));
}

Contact ChatController::groupRowFor(const QString &id, const LiveGroup &group) const
{
    Contact row;
    row.id = id;
    row.name = groupDisplayName(group);
    row.presence = Presence::Available;
    row.favorite = false;
    row.avatarKey = groupAvatarKey;
    const int count = group.members.size() + 1;
    row.statusText = count == 1 ? QStringLiteral("Just you")
                                : QStringLiteral("%1 members").arg(count);
    row.isGroup = true;
    return row;
}

QVariantList ChatController::groupCandidates() const
{
    QVariantList candidates;
    if (m_currentContactId.isEmpty())
        return candidates;
    const LiveGroup *group = currentGroup();
    for (int row = 0; row < m_contacts.rowCount(); ++row) {
        const auto contact = m_contacts.contactAt(row);
        if (!contact || contact->isGroup || contact->id == m_currentContactId)
            continue;
        if (m_live) {
            // Only a contact with a known device can be put into a group.
            const auto chat = m_liveChats.constFind(contact->id);
            if (chat == m_liveChats.cend() || !chat->peerDevice)
                continue;
        }
        bool member = false;
        if (group != nullptr)
            for (const GroupMember &existing : group->members)
                member = member || existing.contactId == contact->id;
        if (member)
            continue;
        candidates.append(QVariantMap{{QStringLiteral("contactId"), contact->id},
                                      {QStringLiteral("name"), contact->name},
                                      {QStringLiteral("avatarKey"), contact->avatarKey}});
    }
    return candidates;
}

void ChatController::addToGroup(const QString &contactId)
{
    if (contactId.isEmpty() || contactId == m_currentContactId || m_currentContactId.isEmpty())
        return;
    if (!m_live) {
        mockAddToGroup(contactId);
        return;
    }
    if (m_groups == nullptr) {
        setGroupNotice(QStringLiteral("Group chats are not available right now."));
        return;
    }
    const auto added = m_liveChats.constFind(contactId);
    if (added == m_liveChats.cend()) {
        setGroupNotice(QStringLiteral("Only your contacts can be added to a group."));
        return;
    }
    setGroupNotice({});
    if (const LiveGroup *group = currentGroup()) {
        m_groups->addMember(group->conversation, added->account);
        return;
    }
    const auto current = m_liveChats.constFind(m_currentContactId);
    if (current == m_liveChats.cend())
        return;
    m_groups->createGroup({current->account, added->account}, QString());
}

bool ChatController::renameCurrentGroup(const QString &title)
{
    const auto group = m_liveGroups.find(m_currentContactId);
    if (group == m_liveGroups.end())
        return false;
    const QString normalized = normalizeGroupTitle(title);
    if (m_live) {
        if (m_groups == nullptr || !m_groups->rename(group->conversation, normalized)) {
            setGroupNotice(QStringLiteral("The group could not be renamed."));
            return false;
        }
        // groupChanged re-reads the roster; nothing more to do here.
        return true;
    }
    group->title = normalized;
    QVector<Contact> rows;
    for (int row = 0; row < m_contacts.rowCount(); ++row)
        if (auto contact = m_contacts.contactAt(row); contact) {
            if (contact->id == m_currentContactId)
                *contact = groupRowFor(contact->id, *group);
            rows.append(*contact);
        }
    m_contacts.setContacts(rows);
    m_contacts.selectContact(m_currentContactId);
    emit currentContactChanged();
    return true;
}

void ChatController::leaveCurrentGroup()
{
    const auto group = m_liveGroups.constFind(m_currentContactId);
    if (group == m_liveGroups.cend())
        return;
    if (m_live) {
        if (m_groups != nullptr && !m_groups->leave(group->conversation))
            setGroupNotice(QStringLiteral("The group could not be left."));
        return;
    }
    const QString id = m_currentContactId;
    m_liveGroups.remove(id);
    m_messagesByContact.remove(id);
    QVector<Contact> rows;
    for (int row = 0; row < m_contacts.rowCount(); ++row)
        if (const auto contact = m_contacts.contactAt(row); contact && contact->id != id)
            rows.append(*contact);
    m_contacts.setContacts(rows);
    m_currentContactId = m_contacts.contactAt(0) ? m_contacts.contactAt(0)->id : QString();
    if (!m_currentContactId.isEmpty())
        m_contacts.selectContact(m_currentContactId);
    refreshVisibleMessages();
    emit currentContactChanged();
    emit groupCandidatesChanged();
}

void ChatController::mockAddToGroup(const QString &contactId)
{
    const auto added = m_contacts.contactById(contactId);
    if (!added || added->isGroup)
        return;
    const auto toMember = [](const Contact &contact) {
        GroupMember member{AccountId::generate(), DeviceId::generate(), contact.id, contact.name,
                           contact.avatarKey};
        return member;
    };
    QString id = m_currentContactId;
    if (const auto group = m_liveGroups.find(id); group != m_liveGroups.end()) {
        for (const GroupMember &member : group->members)
            if (member.contactId == contactId)
                return;
        group->members.append(toMember(*added));
    } else {
        const auto current = m_contacts.contactById(id);
        if (!current)
            return;
        id = groupIdPrefix + QStringLiteral("mock-%1").arg(++m_mockGroupCounter);
        LiveGroup created;
        created.members = {toMember(*current), toMember(*added)};
        m_liveGroups.insert(id, created);
        m_messagesByContact.insert(id, {});
    }
    QVector<Contact> rows;
    bool present = false;
    for (int row = 0; row < m_contacts.rowCount(); ++row)
        if (auto contact = m_contacts.contactAt(row); contact) {
            if (contact->id == id) {
                *contact = groupRowFor(id, m_liveGroups[id]);
                present = true;
            }
            rows.append(*contact);
        }
    if (!present)
        rows.append(groupRowFor(id, m_liveGroups[id]));
    m_contacts.setContacts(rows);
    selectContact(id);
    emit currentContactChanged();
    emit groupCandidatesChanged();
}

void ChatController::clearGroupNotice()
{
    setGroupNotice({});
}

void ChatController::setGroupNotice(const QString &notice)
{
    if (m_groupNotice == notice)
        return;
    m_groupNotice = notice;
    emit groupNoticeChanged();
}

QString ChatController::currentAvatarKey() const
{
    const auto contact = currentContact();
    return contact ? contact->avatarKey : QStringLiteral("neutral");
}

int ChatController::currentPresence() const
{
    const auto contact = currentContact();
    return static_cast<int>(contact ? contact->presence : Presence::Offline);
}

QString ChatController::localStatusLine() const
{
    if (!m_localStatusText.isEmpty())
        return m_localStatusText;
    if (!m_localOnline)
        return presenceText(Presence::Offline);
    return presenceText(static_cast<Presence>(m_localPresence));
}

ProfileUpdateMessage ChatController::localProfile() const
{
    ProfileUpdateMessage profile;
    profile.presence = m_localPresence;
    profile.statusText = m_localStatusText;
    profile.avatarJpeg = m_localAvatarJpeg;
    return profile;
}

QString ChatController::composerText() const
{
    return m_composerText;
}

bool ChatController::canSend() const
{
    if (m_sessionState != SessionState::Ready || m_composerText.trimmed().isEmpty())
        return false;
    return !m_live || currentLiveChatSendable();
}

bool ChatController::currentLiveChatSendable() const
{
    if (m_engine == nullptr)
        return false;
    if (const LiveGroup *group = currentGroup())
        return !group->members.isEmpty();
    const auto chat = m_liveChats.constFind(m_currentContactId);
    return chat != m_liveChats.cend() && chat->peerDevice.has_value();
}

QString ChatController::searchQuery() const
{
    return m_searchQuery;
}

ChatController::SessionState ChatController::sessionState() const
{
    return m_sessionState;
}

QString ChatController::sessionStateText() const
{
    switch (m_sessionState) {
    case SessionState::Ready:
        return {};
    case SessionState::Locked:
        return QStringLiteral("Locked");
    case SessionState::Offline:
        return QStringLiteral("Offline");
    case SessionState::Reconnecting:
        return QStringLiteral("Reconnecting…");
    case SessionState::Quarantined:
        return QStringLiteral("Conversation paused for a security review");
    case SessionState::DeviceChanged:
        return QStringLiteral("This contact's device changed — verification required");
    case SessionState::StorageFull:
        return QStringLiteral("Storage full — free space to send messages");
    }
    return {};
}

bool ChatController::plaintextVisible() const
{
    return statePermitsPlaintext(m_sessionState);
}

QString ChatController::securityNoticeText() const
{
    switch (m_sessionState) {
    case SessionState::Locked:
        return QStringLiteral("Messages are locked. Unlock OpenChat to view this conversation.");
    case SessionState::Quarantined:
        return QStringLiteral("This conversation is paused pending a security review.");
    case SessionState::DeviceChanged:
        return QStringLiteral(
            "This contact's device changed. Verify their identity to view messages.");
    case SessionState::Ready:
    case SessionState::Offline:
    case SessionState::Reconnecting:
    case SessionState::StorageFull:
        return {};
    }
    return {};
}

bool ChatController::selectContact(const QString &id)
{
    if (id == m_currentContactId)
        return true;

    const auto contact = m_contacts.contactById(id);
    if (!contact)
        return false;

    const bool wasSendable = canSend();
    m_currentContactId = id;
    m_contacts.selectContact(id);
    if (m_live) {
        // History is read on demand: the open conversation is the only one whose
        // plaintext sits in a model. Opening also clears its unread count.
        const auto chat = m_liveChats.find(id);
        if (chat != m_liveChats.end()) {
            m_messagesByContact.insert(id, loadHistory(chat->conversation));
            if (chat->unread != 0) {
                chat->unread = 0;
                emit chatUnreadCountChanged();
            }
        }
        const auto group = m_liveGroups.find(id);
        if (group != m_liveGroups.end()) {
            m_messagesByContact.insert(id, loadHistory(group->conversation));
            if (group->unread != 0) {
                group->unread = 0;
                emit chatUnreadCountChanged();
            }
        }
    }
    refreshVisibleMessages();
    emit currentContactChanged();
    emit groupCandidatesChanged();
    updateCanSend(wasSendable);
    return true;
}

void ChatController::setSearchQuery(const QString &query)
{
    const QString normalized = query.trimmed();
    if (m_searchQuery == normalized)
        return;

    m_searchQuery = normalized;
    m_contacts.setQuery(normalized);
    emit searchQueryChanged();
}

void ChatController::setLocalUserName(const QString &name)
{
    const QString normalized = name.trimmed();
    if (m_localUserName == normalized)
        return;
    m_localUserName = normalized;
    emit localUserNameChanged();
}

void ChatController::setLocalStatusText(const QString &text)
{
    const QString normalized = text.trimmed().left(maxStatusTextLength);
    if (m_localStatusText == normalized)
        return;
    if (m_session && !m_session->setStatusText(normalized).hasValue()) {
        setProfileNotice(QStringLiteral("Your status could not be saved."));
        return;
    }
    m_localStatusText = normalized;
    emit localProfileChanged();
    broadcastProfile();
}

void ChatController::setLocalPresence(int presence)
{
    if (!isSelectablePresence(presence) || m_localPresence == presence)
        return;
    if (m_session && !m_session->setPresence(presence).hasValue()) {
        setProfileNotice(QStringLiteral("Your status could not be saved."));
        return;
    }
    m_localPresence = presence;
    emit localProfileChanged();
    broadcastProfile();
}

bool ChatController::setLocalAvatarFromFile(const QUrl &file)
{
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    const auto processed = processProfileImageFile(path);
    if (!processed.hasValue()) {
        setProfileNotice(profileImageErrorText(processed.error()));
        return false;
    }
    const QByteArray jpeg = processed.value();
    const QString key = AvatarStore::instance().registerJpeg(jpeg);
    if (key.isEmpty()) {
        setProfileNotice(profileImageErrorText(ProfileImageError::EncodeFailed));
        return false;
    }
    if (m_session && !m_session->setAvatarJpeg(jpeg).hasValue()) {
        setProfileNotice(QStringLiteral("Your picture could not be saved."));
        return false;
    }
    m_localAvatarJpeg = jpeg;
    m_localAvatarKey = key;
    setProfileNotice({});
    emit localProfileChanged();
    broadcastProfile();
    return true;
}

void ChatController::clearProfileNotice()
{
    setProfileNotice({});
}

void ChatController::setProfileNotice(const QString &notice)
{
    if (m_profileNotice == notice)
        return;
    m_profileNotice = notice;
    emit profileNoticeChanged();
}

void ChatController::setComposerText(const QString &text)
{
    if (m_composerText == text)
        return;

    const bool wasSendable = canSend();
    m_composerText = text;
    emit composerTextChanged();
    updateCanSend(wasSendable);
}

bool ChatController::sendMessage()
{
    // Sending is only offered once the session is Ready; other states either
    // withhold plaintext or lack a durable send path, and must keep the
    // composer text intact rather than silently drop it.
    if (m_sessionState != SessionState::Ready)
        return false;

    const QString body = m_composerText.trimmed();
    if (body.isEmpty())
        return false;

    if (m_live) {
        if (m_engine == nullptr)
            return false;
        if (const LiveGroup *group = currentGroup()) {
            if (group->members.isEmpty())
                return false;
            // One row, one ciphertext, an envelope per member.
            QList<DeviceId> recipients;
            for (const GroupMember &member : group->members)
                recipients.append(member.device);
            m_engine->enqueueGroupText(group->conversation, recipients, body);
            setComposerText({});
            return true;
        }
        const auto chat = m_liveChats.constFind(m_currentContactId);
        if (chat == m_liveChats.cend() || !chat->peerDevice)
            return false;
        // The engine encrypts, commits the row durably and reports it back through
        // messageQueued, which is where the visible row is appended: the model only
        // ever shows what the store holds.
        m_engine->enqueueText(chat->conversation, *chat->peerDevice, body);
        setComposerText({});
        return true;
    }

    const QDateTime sentAt = QDateTime::currentDateTime();
    if (!m_messages.appendOutgoing(body, sentAt))
        return false;

    m_messagesByContact[m_currentContactId].append(
        {MessageDirection::Outgoing, body, sentAt.time(), MessageKind::Text, sentAt.date()});
    setComposerText({});
    return true;
}

void ChatController::setSessionState(SessionState state)
{
    if (m_sessionState == state)
        return;

    const bool wasSendable = canSend();
    const bool visibilityChanged =
        statePermitsPlaintext(m_sessionState) != statePermitsPlaintext(state);
    m_sessionState = state;
    if (visibilityChanged)
        refreshVisibleMessages();
    emit sessionStateChanged();
    updateCanSend(wasSendable);
}

ChatController::NavSection ChatController::navSection() const
{
    return m_navSection;
}

int ChatController::chatUnreadCount() const
{
    if (!m_live)
        return 3; // static mock for the reference rendering
    int total = 0;
    for (const LiveChat &chat : m_liveChats)
        total += chat.unread;
    for (const LiveGroup &group : m_liveGroups)
        total += group.unread;
    return total;
}

int ChatController::callMissedCount() const
{
    // Static mock until call history is wired in.
    return 1;
}

int ChatController::callCount() const
{
    // No call history exists yet, so the call list is empty. A real
    // CallListModel will replace this once call data is available.
    return 0;
}

QStringList ChatController::settingsCategories() const
{
    return settingsCategoryNames();
}

int ChatController::currentSettingsCategory() const
{
    return m_currentSettingsCategory;
}

QString ChatController::currentSettingsCategoryName() const
{
    return settingsCategoryNames().value(m_currentSettingsCategory);
}

QStringList ChatController::currentSettingsElements() const
{
    return settingsElementsForCategory(m_currentSettingsCategory);
}

void ChatController::setCurrentSettingsCategory(int index)
{
    // Ignore anything outside the fixed category range so the selection stays
    // valid; only a genuine change notifies.
    if (index < 0 || index >= settingsCategoryNames().size())
        return;
    if (index == m_currentSettingsCategory)
        return;

    m_currentSettingsCategory = index;
    emit currentSettingsCategoryChanged();
}

void ChatController::setNavSection(NavSection section)
{
    if (m_navSection == section)
        return;

    m_navSection = section;
    emit navSectionChanged();
}

// ---------------------------------------------------------------------------
// Live seam
// ---------------------------------------------------------------------------

void ChatController::setLiveServices(ProfileSession *session, SyncEngine *engine,
                                     ContactRequestService *requests, GroupService *groups)
{
    if (session == nullptr || engine == nullptr)
        return;
    m_session = session;
    m_engine = engine;
    m_requests = requests;
    m_groups = groups;
    m_live = true;

    // Drop the reference mock entirely; the profile's roster replaces it.
    m_messagesByContact.clear();
    m_liveChats.clear();
    m_liveGroups.clear();
    m_contactByConversation.clear();
    m_currentContactId.clear();
    m_contacts.setContacts({});
    m_messages.setMessages({});

    connect(m_engine, &SyncEngine::messageQueued, this, &ChatController::onMessageQueued);
    connect(m_engine, &SyncEngine::messageReceived, this, &ChatController::onMessageReceived);
    connect(m_engine, &SyncEngine::messageStateChanged, this,
            &ChatController::onMessageStateChanged);
    connect(m_engine, &SyncEngine::profileUpdateReceived, this,
            &ChatController::onProfileUpdateReceived);
    if (m_requests != nullptr) {
        connect(m_requests, &ContactRequestService::contactAccepted, this,
                &ChatController::onContactAccepted);
    }
    if (m_groups != nullptr) {
        connect(m_groups, &GroupService::groupCreated, this, &ChatController::onGroupCreated);
        connect(m_groups, &GroupService::groupJoined, this, &ChatController::onGroupChanged);
        connect(m_groups, &GroupService::groupChanged, this, &ChatController::onGroupChanged);
        connect(m_groups, &GroupService::groupLeft, this, &ChatController::onGroupLeft);
        connect(m_groups, &GroupService::groupActionFailed, this,
                &ChatController::setGroupNotice);
    }

    // The profile as last saved. A picture that no longer decodes is dropped
    // rather than shown broken.
    m_localPresence = isSelectablePresence(m_session->presence()) ? m_session->presence() : 0;
    m_localStatusText = m_session->statusText();
    m_localAvatarJpeg = m_session->avatarJpeg();
    m_localAvatarKey = AvatarStore::instance().registerJpeg(m_localAvatarJpeg);
    if (m_localAvatarKey.isEmpty()) {
        m_localAvatarJpeg.clear();
        m_localAvatarKey = QStringLiteral("userpfp_none");
    }
    emit localProfileChanged();

    loadRoster();
    emit chatUnreadCountChanged();
}

void ChatController::setPresenceRelay(RelayClient *relay)
{
    if (m_presenceRelay == relay)
        return;
    if (m_presenceRelay)
        disconnect(m_presenceRelay, nullptr, this, nullptr);
    m_presenceRelay = relay;
    m_onlineDevices.clear();
    disconnect(&m_presenceTimer, nullptr, this, nullptr);
    connect(&m_presenceTimer, &QTimer::timeout, this, &ChatController::refreshPresence);
    if (relay) {
        connect(relay, &RelayClient::connected, this, &ChatController::refreshPresence);
        connect(relay, &RelayClient::disconnected, this, &ChatController::refreshPresence);
        connect(relay, &RelayClient::devicePresenceChanged, this, &ChatController::setDevicePresence);
        m_presenceTimer.start(5000);
    } else {
        m_presenceTimer.stop();
    }
    refreshPresence();
}

void ChatController::refreshPresence()
{
    const bool online = m_presenceRelay && m_presenceRelay->isConnected();
    if (m_localOnline != online) {
        m_localOnline = online;
        emit localOnlineChanged();
        emit localProfileChanged(); // the status line follows the link
    }
    QList<DeviceId> devices;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto &chat : m_liveChats) {
        if (!chat.peerDevice)
            continue;
        devices.append(*chat.peerDevice);
        if (!online || now - m_onlineDevices.value(chat.peerDevice->bytes(), 0) > 15'000)
            setDevicePresence(*chat.peerDevice, false);
    }
    if (online)
        m_presenceRelay->requestPresence(devices);
}

void ChatController::setDevicePresence(const DeviceId &device, bool online)
{
    for (const auto &chat : m_liveChats) {
        if (!chat.peerDevice || *chat.peerDevice != device)
            continue;
        if (online && m_presenceRelay && m_presenceRelay->isConnected())
            m_onlineDevices.insert(device.bytes(), QDateTime::currentMSecsSinceEpoch());
        else
            m_onlineDevices.remove(device.bytes());
        m_contacts.setPresence(chat.account.toHex(), displayedPresence(chat));
        if (chat.account.toHex() == m_currentContactId)
            emit currentContactChanged();
    }
}

void ChatController::loadRoster()
{
    if (m_session == nullptr)
        return;
    SqlCipherContactRepository *contacts = m_session->contacts();
    if (contacts == nullptr)
        return;
    auto all = contacts->contacts();
    if (!all.hasValue())
        return;

    QVector<Contact> rows;
    QHash<QString, LiveChat> chats;
    QHash<QByteArray, QString> byConversation;
    for (const ContactRecord &record : all.value()) {
        // Only a mutual contact backed by an MLS group is a chat.
        if (record.state != ContactState::Accepted || !record.conversationId)
            continue;
        const QString id = record.accountId.toHex();
        int unread = 0;
        if (const auto existing = m_liveChats.constFind(id); existing != m_liveChats.cend())
            unread = existing->unread;
        LiveChat chat{record.accountId, *record.conversationId, record.peerDeviceId,
                      record.handle, unread};
        chat.presence = isSelectablePresence(record.presence) ? record.presence : 0;
        chat.statusText = record.statusText;
        chat.avatarKey = AvatarStore::instance().registerJpeg(record.avatarJpeg);
        if (chat.avatarKey.isEmpty())
            chat.avatarKey = QStringLiteral("userpfp_none");
        chats.insert(id, chat);
        byConversation.insert(record.conversationId->bytes(), id);
        rows.append(contactRowFor(chat));
    }
    m_liveChats = std::move(chats);
    m_contactByConversation = std::move(byConversation);
    // Groups come after the people, and need the people in place first: a
    // member who is a contact is named from that contact's row.
    m_contacts.setContacts(rows);
    loadGroups(rows, m_contactByConversation);
    m_contacts.setContacts(std::move(rows));

    const bool wasSendable = canSend();
    if (!m_liveChats.contains(m_currentContactId) && !m_liveGroups.contains(m_currentContactId))
        m_currentContactId.clear();
    if (m_currentContactId.isEmpty() && m_contacts.contactAt(0)) {
        // Open the first chat so the surface is never blank while one exists.
        const QString first = m_contacts.contactAt(0)->id;
        m_currentContactId = first;
        m_contacts.selectContact(first);
        if (const auto chat = m_liveChats.constFind(first); chat != m_liveChats.cend())
            m_messagesByContact.insert(first, loadHistory(chat->conversation));
        if (const auto group = m_liveGroups.constFind(first); group != m_liveGroups.cend())
            m_messagesByContact.insert(first, loadHistory(group->conversation));
    } else if (!m_currentContactId.isEmpty()) {
        m_contacts.selectContact(m_currentContactId);
    }
    refreshVisibleMessages();
    emit currentContactChanged();
    emit groupCandidatesChanged();
    updateCanSend(wasSendable);
    if (m_presenceRelay)
        refreshPresence();
}

void ChatController::loadGroups(QVector<Contact> &rows, QHash<QByteArray, QString> &byConversation)
{
    QHash<QString, LiveGroup> groups;
    if (m_groups != nullptr) {
        for (const GroupService::Group &record : m_groups->groups()) {
            const QString id = groupIdPrefix + record.conversation.toHex();
            LiveGroup group{record.conversation, record.title, {}, 0};
            if (const auto existing = m_liveGroups.constFind(id); existing != m_liveGroups.cend())
                group.unread = existing->unread;
            for (const GroupMemberRecord &member : record.members) {
                GroupMember row{member.accountId, member.deviceId, QString(), member.displayName,
                                QStringLiteral("userpfp_none")};
                const QString contactId = member.accountId.toHex();
                if (const auto chat = m_liveChats.constFind(contactId); chat != m_liveChats.cend()) {
                    row.contactId = contactId;
                    row.avatarKey = chat->avatarKey;
                }
                group.members.append(row);
            }
            groups.insert(id, group);
            byConversation.insert(record.conversation.bytes(), id);
            rows.append(groupRowFor(id, group));
        }
    }
    m_liveGroups = std::move(groups);
}

QString ChatController::groupChatIdFor(const ConversationId &conversation) const
{
    const QString id = m_contactByConversation.value(conversation.bytes());
    return m_liveGroups.contains(id) ? id : QString();
}

std::optional<CallEngine::GroupCallRoute>
ChatController::groupCallRouteFor(const QString &chatId) const
{
    const auto group = m_liveGroups.constFind(chatId);
    if (!m_live || group == m_liveGroups.cend() || group->members.isEmpty())
        return std::nullopt;
    CallEngine::GroupCallRoute route;
    route.conversation = group->conversation;
    route.title = groupDisplayName(*group);
    for (const GroupMember &member : group->members) {
        CallEngine::CallPeer peer;
        peer.conversation = group->conversation;
        peer.device = member.device;
        peer.contactId = member.contactId;
        peer.displayName = memberName(member);
        peer.avatarKey = member.avatarKey;
        route.members.append(peer);
    }
    return route;
}

std::optional<CallEngine::GroupCallRoute>
ChatController::groupCallRouteFor(const ConversationId &conversation) const
{
    return groupCallRouteFor(groupChatIdFor(conversation));
}

void ChatController::onGroupCreated(const ConversationId &conversation)
{
    loadRoster();
    // The group the user just made is what they want to look at.
    const QString id = groupChatIdFor(conversation);
    if (!id.isEmpty())
        selectContact(id);
}

void ChatController::onGroupChanged(const ConversationId &conversation)
{
    loadRoster();
    (void)conversation;
}

void ChatController::onGroupLeft(const ConversationId &conversation)
{
    const QString id = groupChatIdFor(conversation);
    if (!id.isEmpty())
        m_messagesByContact.remove(id);
    loadRoster();
}

std::optional<ChatController::CallRoute>
ChatController::callRouteFor(const QString &contactId) const
{
    const auto chat = m_liveChats.constFind(contactId);
    if (chat == m_liveChats.cend() || !chat->peerDevice)
        return std::nullopt;
    const Contact row = contactRowFor(*chat);
    return CallRoute{chat->conversation, *chat->peerDevice, contactId, row.name, row.avatarKey};
}

std::optional<ChatController::CallRoute>
ChatController::callRouteFor(const ConversationId &conversation, const DeviceId &device) const
{
    const auto route = callRouteFor(contactForConversation(conversation));
    if (!route || route->device != device)
        return std::nullopt;
    return route;
}

Presence ChatController::displayedPresence(const LiveChat &chat) const
{
    const bool reachable = chat.peerDevice && m_onlineDevices.contains(chat.peerDevice->bytes());
    return reachable ? static_cast<Presence>(chat.presence) : Presence::Offline;
}

Contact ChatController::contactRowFor(const LiveChat &chat) const
{
    Contact row;
    row.id = chat.account.toHex();
    row.name = chat.handle.isEmpty() ? shortIdLabel(chat.account) : chat.handle;
    row.presence = displayedPresence(chat);
    row.favorite = false;
    row.avatarKey = chat.avatarKey.isEmpty() ? QStringLiteral("userpfp_none") : chat.avatarKey;
    row.statusText = chat.statusText;
    return row;
}

void ChatController::sendProfileTo(const LiveChat &chat)
{
    if (m_engine == nullptr || !chat.peerDevice)
        return;
    m_engine->sendProfileUpdate(chat.conversation, *chat.peerDevice,
                                encodeProfileUpdate(localProfile()));
}

void ChatController::broadcastProfile()
{
    if (!m_live)
        return;
    for (const LiveChat &chat : m_liveChats)
        sendProfileTo(chat);
}

void ChatController::onProfileUpdateReceived(const ConversationId &conversation,
                                             const DeviceId &senderDevice,
                                             const QByteArray &payload)
{
    (void)senderDevice; // the engine already bound the plaintext to the MLS credential
    QString contactId = contactForConversation(conversation);
    if (contactId.isEmpty()) {
        // A profile can arrive before we learn of the acceptance that created
        // the chat, exactly like a first message; re-read the roster first.
        loadRoster();
        contactId = contactForConversation(conversation);
        if (contactId.isEmpty())
            return;
    }
    const auto profile = decodeProfileUpdate(payload);
    if (!profile || m_session == nullptr)
        return;
    const auto chat = m_liveChats.constFind(contactId);
    if (chat == m_liveChats.cend())
        return;
    SqlCipherContactRepository *contacts = m_session->contacts();
    if (contacts == nullptr
        || !contacts
                ->setProfile(chat->account, profile->presence, profile->statusText,
                             profile->avatarJpeg, QDateTime::currentMSecsSinceEpoch())
                .hasValue())
        return;
    // The row, the header and any ringing call screen all read the roster.
    loadRoster();
}

QVector<Message> ChatController::loadHistory(const ConversationId &conversation) const
{
    QVector<Message> history;
    if (m_session == nullptr)
        return history;
    SqlCipherChatRepository *chats = m_session->chats();
    if (chats == nullptr)
        return history;
    auto page = chats->messages(conversation, historyPageSize, std::nullopt);
    if (!page.hasValue())
        return history;
    // The store returns newest-first; the view reads oldest-first.
    const QVector<MessageRecord> &records = page.value();
    history.reserve(records.size());
    for (auto it = records.crbegin(); it != records.crend(); ++it)
        history.append(messageFor(*it));
    return history;
}

Message ChatController::messageFor(const MessageRecord &record) const
{
    Message message = toMessage(record);
    // In a group the bubble alone does not say who spoke.
    const auto group = m_liveGroups.constFind(contactForConversation(record.conversationId));
    if (group != m_liveGroups.cend() && record.flow == MessageFlow::Incoming) {
        for (const GroupMember &member : group->members)
            if (member.device == record.senderDeviceId)
                message.senderName = memberName(member);
        if (message.senderName.isEmpty())
            message.senderName = QStringLiteral("Former member");
    }
    return message;
}

Message ChatController::toMessage(const MessageRecord &record)
{
    const QDateTime sentAt = QDateTime::fromMSecsSinceEpoch(record.sentAtMs).toLocalTime();
    Message message;
    message.direction = record.flow == MessageFlow::Outgoing ? MessageDirection::Outgoing
                                                             : MessageDirection::Incoming;
    message.body = record.body;
    message.timestamp = sentAt.time();
    message.kind = record.kind == ContentKind::Emoji ? MessageKind::Emoji : MessageKind::Text;
    message.date = sentAt.date();
    message.stableId = record.id.toHex();
    message.deliveryState = toModelState(record.deliveryState);
    message.failureReason = record.deliveryState == DeliveryState::Failed
        ? MessageFailureReason::Network
        : MessageFailureReason::None;
    message.senderDevice = record.senderDeviceId.toHex();
    return message;
}

QString ChatController::contactForConversation(const ConversationId &conversation) const
{
    return m_contactByConversation.value(conversation.bytes());
}

void ChatController::onMessageQueued(const MessageRecord &record)
{
    const QString contactId = contactForConversation(record.conversationId);
    if (contactId.isEmpty())
        return;
    m_messagesByContact[contactId].append(messageFor(record));
    if (contactId == m_currentContactId && statePermitsPlaintext(m_sessionState))
        m_messages.appendMessage(messageFor(record));
}

void ChatController::onMessageReceived(const MessageRecord &record)
{
    QString contactId = contactForConversation(record.conversationId);
    if (contactId.isEmpty()) {
        // A first message from a peer whose acceptance we learned only now: the
        // request service has already promoted the roster row, so re-read it.
        loadRoster();
        contactId = contactForConversation(record.conversationId);
        if (contactId.isEmpty())
            return;
    }
    if (contactId == m_currentContactId) {
        m_messagesByContact[contactId].append(messageFor(record));
        if (statePermitsPlaintext(m_sessionState))
            m_messages.appendMessage(messageFor(record));
        return;
    }
    const auto chat = m_liveChats.find(contactId);
    if (chat != m_liveChats.end()) {
        ++chat->unread;
        emit chatUnreadCountChanged();
    }
    const auto group = m_liveGroups.find(contactId);
    if (group != m_liveGroups.end()) {
        ++group->unread;
        emit chatUnreadCountChanged();
    }
}

void ChatController::onMessageStateChanged(const MessageId &messageId, DeliveryState state)
{
    const QString stableId = messageId.toHex();
    const MessageDeliveryState modelState = toModelState(state);
    const MessageFailureReason reason = state == DeliveryState::Failed
        ? MessageFailureReason::Network
        : MessageFailureReason::None;
    m_messages.updateDeliveryState(stableId, modelState, reason);
    for (auto &history : m_messagesByContact) {
        for (Message &message : history) {
            if (message.stableId == stableId) {
                message.deliveryState = modelState;
                message.failureReason = reason;
            }
        }
    }
}

void ChatController::onContactAccepted(const AccountId &account)
{
    // Either side of a request completed: the roster now has a new Accepted row
    // with a conversation. Re-read it so the chat appears (and opens if nothing
    // else is open), then introduce ourselves: the new contact has never seen
    // our picture or status.
    loadRoster();
    if (const auto chat = m_liveChats.constFind(account.toHex()); chat != m_liveChats.cend())
        sendProfileTo(*chat);
}

void ChatController::refreshContact(const QString &accountHex)
{
    if (!m_live || !m_liveChats.contains(accountHex))
        return;
    loadRoster();
}

void ChatController::updateCanSend(bool wasSendable)
{
    if (wasSendable != canSend())
        emit canSendChanged();
}

std::optional<Contact> ChatController::currentContact() const
{
    return m_contacts.contactById(m_currentContactId);
}

bool ChatController::statePermitsPlaintext(SessionState state)
{
    switch (state) {
    case SessionState::Locked:
    case SessionState::Quarantined:
    case SessionState::DeviceChanged:
        return false;
    case SessionState::Ready:
    case SessionState::Offline:
    case SessionState::Reconnecting:
    case SessionState::StorageFull:
        return true;
    }
    return false;
}

void ChatController::refreshVisibleMessages()
{
    if (statePermitsPlaintext(m_sessionState))
        m_messages.setMessages(m_messagesByContact.value(m_currentContactId));
    else
        m_messages.setMessages({});
}

} // namespace OpenChat
