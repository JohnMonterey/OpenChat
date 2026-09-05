#include "controllers/ChatController.h"
#include "network/RelayClient.h"

#include "app/ContactRequestService.h"
#include "app/ProfileSession.h"
#include "domain/Contact.h"
#include "network/SyncEngine.h"
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
    return contact->statusText.isEmpty() ? presenceText(contact->presence) : contact->statusText;
}

QString ChatController::currentAvatarKey() const
{
    const auto contact = currentContact();
    return contact ? contact->avatarKey : QStringLiteral("neutral");
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
    const auto chat = m_liveChats.constFind(m_currentContactId);
    return chat != m_liveChats.cend() && chat->peerDevice.has_value() && m_engine != nullptr;
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
    }
    refreshVisibleMessages();
    emit currentContactChanged();
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
        const auto chat = m_liveChats.constFind(m_currentContactId);
        if (chat == m_liveChats.cend() || !chat->peerDevice || m_engine == nullptr)
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
                                     ContactRequestService *requests)
{
    if (session == nullptr || engine == nullptr)
        return;
    m_session = session;
    m_engine = engine;
    m_requests = requests;
    m_live = true;

    // Drop the reference mock entirely; the profile's roster replaces it.
    m_messagesByContact.clear();
    m_liveChats.clear();
    m_contactByConversation.clear();
    m_currentContactId.clear();
    m_contacts.setContacts({});
    m_messages.setMessages({});

    connect(m_engine, &SyncEngine::messageQueued, this, &ChatController::onMessageQueued);
    connect(m_engine, &SyncEngine::messageReceived, this, &ChatController::onMessageReceived);
    connect(m_engine, &SyncEngine::messageStateChanged, this,
            &ChatController::onMessageStateChanged);
    if (m_requests != nullptr) {
        connect(m_requests, &ContactRequestService::contactAccepted, this,
                &ChatController::onContactAccepted);
    }

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
        m_contacts.setPresence(chat.account.toHex(), m_onlineDevices.contains(device.bytes())
            ? Presence::Available : Presence::Offline);
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
        const LiveChat chat{record.accountId, *record.conversationId, record.peerDeviceId,
                            record.handle, unread};
        chats.insert(id, chat);
        byConversation.insert(record.conversationId->bytes(), id);
        rows.append(contactRowFor(chat));
    }
    m_liveChats = std::move(chats);
    m_contactByConversation = std::move(byConversation);
    m_contacts.setContacts(std::move(rows));

    const bool wasSendable = canSend();
    if (!m_liveChats.contains(m_currentContactId))
        m_currentContactId.clear();
    if (m_currentContactId.isEmpty() && m_contacts.contactAt(0)) {
        // Open the first chat so the surface is never blank while one exists.
        const QString first = m_contacts.contactAt(0)->id;
        m_currentContactId = first;
        m_contacts.selectContact(first);
        if (const auto chat = m_liveChats.constFind(first); chat != m_liveChats.cend())
            m_messagesByContact.insert(first, loadHistory(chat->conversation));
    } else if (!m_currentContactId.isEmpty()) {
        m_contacts.selectContact(m_currentContactId);
    }
    refreshVisibleMessages();
    emit currentContactChanged();
    updateCanSend(wasSendable);
    if (m_presenceRelay)
        refreshPresence();
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

QString ChatController::localAvatarKey() const
{
    // The local user has no stored avatar yet, so the call screen shows the same
    // neutral artwork the rest of the interface uses for an unset picture.
    return QStringLiteral("userpfp_none");
}

Contact ChatController::contactRowFor(const LiveChat &chat) const
{
    Contact row;
    row.id = chat.account.toHex();
    row.name = chat.handle.isEmpty() ? shortIdLabel(chat.account) : chat.handle;
    row.presence = chat.peerDevice && m_onlineDevices.contains(chat.peerDevice->bytes())
        ? Presence::Available : Presence::Offline;
    row.favorite = false;
    row.avatarKey = QStringLiteral("userpfp_none");
    return row;
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
        history.append(toMessage(*it));
    return history;
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
    m_messagesByContact[contactId].append(toMessage(record));
    if (contactId == m_currentContactId && statePermitsPlaintext(m_sessionState))
        m_messages.appendMessage(toMessage(record));
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
        m_messagesByContact[contactId].append(toMessage(record));
        if (statePermitsPlaintext(m_sessionState))
            m_messages.appendMessage(toMessage(record));
        return;
    }
    const auto chat = m_liveChats.find(contactId);
    if (chat != m_liveChats.end()) {
        ++chat->unread;
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
    (void)account;
    // Either side of a request completed: the roster now has a new Accepted row
    // with a conversation. Re-read it so the chat appears (and opens if nothing
    // else is open).
    loadRoster();
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
