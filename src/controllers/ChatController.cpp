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
    m_messages.setMessages(m_messagesByContact.value(m_currentContactId));
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
    return !m_composerText.trimmed().isEmpty();
}

QString ChatController::searchQuery() const
{
    return m_searchQuery;
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
    m_messages.setMessages(m_messagesByContact.value(id));
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

std::optional<Contact> ChatController::currentContact() const
{
    return m_contacts.contactById(m_currentContactId);
}

} // namespace OpenChat
