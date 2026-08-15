#include "controllers/ChatController.h"

#include <QDateTime>

namespace OpenChat {

namespace {

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

ContactListModel *ChatController::contacts()
{
    return &m_contacts;
}

MessageListModel *ChatController::messages()
{
    return &m_messages;
}

QString ChatController::currentContactName() const
{
    const auto contact = currentContact();
    return contact ? contact->name : QString();
}

QString ChatController::currentStatusText() const
{
    const auto contact = currentContact();
    return contact ? presenceText(contact->presence) : QString();
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
    return m_sessionState == SessionState::Ready && !m_composerText.trimmed().isEmpty();
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

    m_currentContactId = id;
    m_contacts.selectContact(id);
    refreshVisibleMessages();
    emit currentContactChanged();
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

void ChatController::setComposerText(const QString &text)
{
    if (m_composerText == text)
        return;

    const bool wasSendable = canSend();
    m_composerText = text;
    emit composerTextChanged();
    if (wasSendable != canSend())
        emit canSendChanged();
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
