#include "controllers/ContactController.h"

#include "app/AddContactService.h"
#include "app/ContactRequestService.h"
#include "app/ProfileSession.h"
#include "domain/Contact.h"
#include "network/RelayClient.h"
#include "network/SyncEngine.h"
#include "security/SafetyNumber.h"
#include "storage/SqlCipherContactRepository.h"

#include <QByteArray>
#include <QDateTime>

#include <utility>

namespace OpenChat {

namespace {

// Maps a send-side failure to user-facing copy. Never surfaces a handle, token or
// account id -- only the typed reason.
QString messageForAddError(AddContactService::Error error)
{
    switch (error) {
    case AddContactService::Error::NotFound:
        return QStringLiteral("No account with that handle.");
    case AddContactService::Error::SelfContact:
        return QStringLiteral("That's you.");
    case AddContactService::Error::NoDevice:
    case AddContactService::Error::NoKeyPackage:
        return QStringLiteral("That contact isn't reachable right now.");
    case AddContactService::Error::Blocked:
        return QStringLiteral("You've blocked this contact.");
    case AddContactService::Error::Mls:
    case AddContactService::Error::Storage:
        return QStringLiteral("Something went wrong. Try again.");
    case AddContactService::Error::Transport:
        return QStringLiteral("Couldn't reach the server.");
    }
    return QStringLiteral("Something went wrong. Try again.");
}

// A short, id-free subtitle for a request row: a truncated account fingerprint.
QString subtitleForAccount(const AccountId &account)
{
    return QStringLiteral("ID ") + account.toHex().left(10);
}

RequestListModel::RequestEntry entryForRequest(const AccountId &account,
                                               const ConversationId &conversation,
                                               const QString &handle = QString())
{
    return RequestListModel::RequestEntry{conversation, account, handle,
                                          QStringLiteral("New contact request"),
                                          subtitleForAccount(account)};
}

// Formats the raw 60-decimal-digit safety number into 12 space-separated groups of
// five, the on-screen presentation both sides compare out-of-band.
QString groupSafetyNumber(const QString &raw)
{
    QString grouped;
    grouped.reserve(raw.size() + raw.size() / 5);
    for (qsizetype offset = 0; offset < raw.size(); offset += 5) {
        if (!grouped.isEmpty())
            grouped += QLatin1Char(' ');
        grouped += QStringView(raw).mid(offset, 5);
    }
    return grouped;
}

} // namespace

ContactController::ContactController(QObject *parent)
    : QObject(parent)
{
}

ContactController::~ContactController() = default;

RequestListModel *ContactController::requests()
{
    return &m_requests;
}

bool ContactController::enabled() const
{
    return m_enabled;
}

bool ContactController::dialogOpen() const
{
    return m_dialogOpen;
}

ContactController::Status ContactController::status() const
{
    return m_status;
}

QString ContactController::statusMessage() const
{
    return m_statusMessage;
}

QString ContactController::myInvite() const
{
    return m_myInvite;
}

bool ContactController::inviteReady() const
{
    return !m_myInvite.isEmpty();
}

bool ContactController::safetyNumberOpen() const
{
    return m_safetyNumberOpen;
}

QString ContactController::safetyNumber() const
{
    return m_safetyNumber;
}

bool ContactController::safetyNumberVerified() const
{
    return m_safetyNumberVerified;
}

QString ContactController::safetyNumberContact() const
{
    return m_safetyNumberContact;
}

void ContactController::setStatus(Status status, const QString &message)
{
    m_status = status;
    m_statusMessage = message;
    emit statusChanged();
}

void ContactController::openDialog()
{
    // Opening always resets the surface to a clean, neutral state.
    setStatus(Status::Idle, QString());
    if (!m_dialogOpen) {
        m_dialogOpen = true;
        emit dialogOpenChanged();
    }
}

void ContactController::closeDialog()
{
    if (!m_dialogOpen)
        return;
    m_dialogOpen = false;
    emit dialogOpenChanged();
}

AddContactService *ContactController::beginAdd()
{
    if (m_session == nullptr || m_relay == nullptr || m_engine == nullptr)
        return nullptr;

    // A fresh single-shot sender per attempt (the service terminates after one run).
    m_pendingAdd = std::make_unique<AddContactService>(*m_session, *m_relay, *m_engine);
    connect(m_pendingAdd.get(), &AddContactService::succeeded, this,
            [this](const ConversationId &, const AccountId &) {
                setStatus(Status::Success, QStringLiteral("Request sent."));
            });
    connect(m_pendingAdd.get(), &AddContactService::failed, this,
            [this](AddContactService::Error error) {
                setStatus(Status::Error, messageForAddError(error));
            });
    return m_pendingAdd.get();
}

void ContactController::addByHandle(const QString &handle)
{
    if (AddContactService *service = beginAdd()) {
        setStatus(Status::Working, QStringLiteral("Sending request…"));
        service->startByHandle(handle);
        return;
    }

    // Mock mode: no network, resolve to a simulated success.
    setStatus(Status::Working, QStringLiteral("Sending request…"));
    setStatus(Status::Success, QStringLiteral("Request sent."));
}

void ContactController::addByInvite(const QString &inviteText)
{
    const QByteArray bytes = QByteArray::fromBase64(
        inviteText.trimmed().toUtf8(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (bytes.isEmpty()) {
        setStatus(Status::Error, QStringLiteral("That invite code isn't valid."));
        return;
    }

    if (AddContactService *service = beginAdd()) {
        setStatus(Status::Working, QStringLiteral("Sending request…"));
        service->startByInvite(bytes);
        return;
    }

    // Mock mode: the code decoded, so resolve to a simulated success.
    setStatus(Status::Working, QStringLiteral("Sending request…"));
    setStatus(Status::Success, QStringLiteral("Request sent."));
}

void ContactController::createMyInvite()
{
    if (m_relay == nullptr) {
        // Mock mode: hand back the preset invite.
        m_myInvite = m_mockInvite;
        emit myInviteChanged();
        setStatus(Status::Success, QStringLiteral("Invite ready to share."));
        return;
    }

    clearInviteConnections();
    setStatus(Status::Working, QStringLiteral("Creating invite…"));
    m_inviteConnections << connect(
        m_relay, &RelayClient::inviteCreated, this,
        [this](const QByteArray &token, qint64 /*expiresAtMs*/) {
            clearInviteConnections();
            m_myInvite = QString::fromLatin1(token.toBase64(
                QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
            emit myInviteChanged();
            setStatus(Status::Success, QStringLiteral("Invite ready to share."));
        });
    m_inviteConnections << connect(m_relay, &RelayClient::inviteCreationFailed, this, [this] {
        clearInviteConnections();
        setStatus(Status::Error, QStringLiteral("Couldn't create an invite. Try again."));
    });
    m_inviteConnections << connect(m_relay, &RelayClient::authExpired, this, [this] {
        clearInviteConnections();
        setStatus(Status::Error, QStringLiteral("Couldn't create an invite. Try again."));
    });
    m_relay->createInvite();
}

void ContactController::accept(const QString &requestId)
{
    const std::optional<ConversationId> conversation =
        ConversationId::fromBytes(QByteArray::fromHex(requestId.toLatin1()));
    if (!conversation) {
        setStatus(Status::Error, QStringLiteral("Couldn't complete that action."));
        return;
    }

    if (m_requestsSvc != nullptr) {
        // Do NOT remove the row yet: contactAccepted / securityNotice resolve it.
        m_requestsSvc->acceptContact(*conversation);
        return;
    }

    // Mock mode: accept locally.
    m_requests.removeByConversation(*conversation);
    setStatus(Status::Success, QStringLiteral("Contact added."));
}

void ContactController::decline(const QString &requestId)
{
    const std::optional<ConversationId> conversation =
        ConversationId::fromBytes(QByteArray::fromHex(requestId.toLatin1()));
    if (!conversation) {
        setStatus(Status::Error, QStringLiteral("Couldn't complete that action."));
        return;
    }

    if (m_requestsSvc != nullptr)
        m_requestsSvc->declineContact(*conversation); // emits no success signal
    m_requests.removeByConversation(*conversation);
    setStatus(Status::Success, QStringLiteral("Request declined."));
}

void ContactController::block(const QString &requestId)
{
    const std::optional<ConversationId> conversation =
        ConversationId::fromBytes(QByteArray::fromHex(requestId.toLatin1()));
    if (!conversation) {
        setStatus(Status::Error, QStringLiteral("Couldn't complete that action."));
        return;
    }

    if (m_requestsSvc != nullptr)
        m_requestsSvc->blockContact(*conversation); // emits no success signal
    m_requests.removeByConversation(*conversation);
    setStatus(Status::Success, QStringLiteral("Contact blocked."));
}

void ContactController::openSafetyNumber(const QString &contactId)
{
    if (m_session == nullptr) {
        // Mock mode: the preset from setMockSafetyNumber() is already in place; just
        // reveal the dialog.
        openSafetyNumberPreview();
        return;
    }

    const std::optional<AccountId> account =
        AccountId::fromBytes(QByteArray::fromHex(contactId.toLatin1()));

    // Reset the surface, then fill from the live contact. A missing key or an
    // unresolvable id leaves an empty number so the dialog shows "unavailable".
    m_safetyNumber.clear();
    m_safetyNumberVerified = false;
    m_safetyNumberContact = QStringLiteral("ID ") + contactId.left(10);
    m_safetyNumberAccount = account;

    if (account) {
        SqlCipherContactRepository *contacts = m_session->contacts();
        std::optional<ContactRecord> record;
        if (contacts != nullptr) {
            if (auto found = contacts->find(*account); found.hasValue())
                record = std::move(found).value();
        }

        if (record) {
            if (!record->handle.isEmpty())
                m_safetyNumberContact = record->handle;
            m_safetyNumberVerified = record->verified;

            if (record->peerSigningKey && record->peerSigningKey->size() == 32) {
                const auto credential = m_session->publicCredential();
                const auto ourAccount = m_session->accountId();
                if (credential.hasValue() && ourAccount.hasValue()) {
                    auto number = computeSafetyNumber(
                        credential.value().signingPublicKey, ourAccount.value().bytes(),
                        *record->peerSigningKey, account->bytes());
                    if (number.hasValue())
                        m_safetyNumber = groupSafetyNumber(number.value());
                }
            }
        }
    }

    m_safetyNumberOpen = true;
    emit safetyNumberChanged();
}

void ContactController::closeSafetyNumber()
{
    if (!m_safetyNumberOpen)
        return;
    m_safetyNumberOpen = false;
    emit safetyNumberChanged();
}

void ContactController::markVerified()
{
    if (m_session != nullptr) {
        // Live: persist the assertion for the contact the dialog is showing.
        if (m_safetyNumberAccount) {
            if (SqlCipherContactRepository *contacts = m_session->contacts())
                (void)contacts->setVerified(*m_safetyNumberAccount, true,
                                            QDateTime::currentMSecsSinceEpoch());
        }
        m_safetyNumberVerified = true;
        emit safetyNumberChanged();
        return;
    }

    // Mock mode: flip the flag with no persistence.
    m_safetyNumberVerified = !m_safetyNumberVerified;
    emit safetyNumberChanged();
}

void ContactController::setLiveServices(ContactRequestService *requests, RelayClient *relay,
                                        ProfileSession *session, SyncEngine *engine)
{
    m_requestsSvc = requests;
    m_relay = relay;
    m_session = session;
    m_engine = engine;

    if (!m_enabled) {
        m_enabled = true;
        emit enabledChanged();
    }

    seedFromRoster();

    if (m_requestsSvc == nullptr)
        return;

    connect(m_requestsSvc, &ContactRequestService::incomingRequest, this,
            [this](const AccountId &sender, const ConversationId &conversation) {
                m_requests.appendRequest(entryForRequest(sender, conversation));
            });
    connect(m_requestsSvc, &ContactRequestService::contactAccepted, this,
            [this](const AccountId &sender) {
                m_requests.removeByAccount(sender);
                setStatus(Status::Success, QStringLiteral("Contact added."));
                // Show the safety number at the natural verify moment: the request
                // was just accepted and the peer key is now bound.
                openSafetyNumber(sender.toHex());
            });
    connect(m_requestsSvc, &ContactRequestService::securityNotice, this,
            [this](const ConversationId &conversation, const AccountId &) {
                m_requests.removeByConversation(conversation);
                setStatus(Status::Error, QStringLiteral("Couldn't verify that request."));
            });
    connect(m_requestsSvc, &ContactRequestService::requestActionFailed, this,
            [this](const ConversationId &) {
                setStatus(Status::Error, QStringLiteral("Couldn't complete that action."));
            });
}

void ContactController::seedFromRoster()
{
    if (m_session == nullptr)
        return;
    SqlCipherContactRepository *contacts = m_session->contacts();
    if (contacts == nullptr)
        return;
    auto all = contacts->contacts();
    if (!all.hasValue())
        return;

    QVector<RequestListModel::RequestEntry> entries;
    for (const ContactRecord &record : all.value()) {
        if (record.state != ContactState::PendingIncoming || !record.conversationId.has_value())
            continue;
        entries.append(entryForRequest(record.accountId, *record.conversationId, record.handle));
    }
    m_requests.setRequests(std::move(entries));
}

void ContactController::enableForPreview()
{
    if (m_enabled)
        return;
    m_enabled = true;
    emit enabledChanged();
}

void ContactController::addMockRequest(const QString &displayName, const QString &subtitle)
{
    RequestListModel::RequestEntry entry{ConversationId::generate(), AccountId::generate(),
                                         QString(), displayName, subtitle};
    m_requests.appendRequest(std::move(entry));
}

void ContactController::setMockInvite(const QString &inviteText)
{
    m_mockInvite = inviteText;
    m_myInvite = inviteText;
    emit myInviteChanged();
}

void ContactController::setMockSafetyNumber(const QString &groupedNumber, bool verified,
                                            const QString &contactLabel)
{
    m_safetyNumber = groupedNumber;
    m_safetyNumberVerified = verified;
    m_safetyNumberContact = contactLabel;
    emit safetyNumberChanged();
}

void ContactController::openSafetyNumberPreview()
{
    if (m_safetyNumberOpen)
        return;
    m_safetyNumberOpen = true;
    emit safetyNumberChanged();
}

void ContactController::clearInviteConnections()
{
    for (const QMetaObject::Connection &connection : m_inviteConnections)
        QObject::disconnect(connection);
    m_inviteConnections.clear();
}

} // namespace OpenChat
