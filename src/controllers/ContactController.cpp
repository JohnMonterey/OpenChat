#include "controllers/ContactController.h"

#include "app/AddContactService.h"
#include "app/ContactRequestService.h"
#include "app/OutgoingRequestCleanup.h"
#include "app/ProfileSession.h"
#include "domain/Contact.h"
#include "network/RelayClient.h"
#include "network/SyncEngine.h"
#include "security/SafetyNumber.h"
#include "storage/SqlCipherContactRepository.h"

#include <QByteArray>
#include <QDateTime>

#include <functional>
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

RequestListModel::RequestEntry entryForRequest(const AccountId &account,
                                               const ConversationId &conversation,
                                               const QString &handle = QString())
{
    return RequestListModel::RequestEntry{conversation, account, handle,
                                          RequestListModel::displayNameForHandle(handle),
                                          RequestListModel::subtitleForHandle(handle, account)};
}

// Normalises search text into a directory handle: trimmed, without a leading
// '@'. Returns empty when the text cannot be a handle (empty or inner spaces),
// mirroring OnboardingController's registration rule.
QString normalizeHandle(const QString &query)
{
    QString handle = query.trimmed();
    if (handle.startsWith(QLatin1Char('@')))
        handle.remove(0, 1);
    if (handle.isEmpty() || handle.size() > 64)
        return QString();
    for (const QChar ch : handle) {
        if (ch.isSpace())
            return QString();
    }
    return handle;
}

constexpr int lookupDebounceMs = 250;

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
    m_lookupDebounce.setSingleShot(true);
    m_lookupDebounce.setInterval(lookupDebounceMs);
    connect(&m_lookupDebounce, &QTimer::timeout, this, &ContactController::runLookup);
}

ContactController::~ContactController()
{
    clearLookupConnections();
    for (const QMetaObject::Connection &connection : m_reverseConnections)
        QObject::disconnect(connection);
}

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

// ---------------------------------------------------------------------------
// Search & Find
// ---------------------------------------------------------------------------

ContactController::LookupState ContactController::lookupState() const
{
    return m_lookupState;
}

QString ContactController::lookupHandle() const
{
    return m_lookupHandle;
}

QString ContactController::lookupSubtitle() const
{
    if (!m_lookupMessage.isEmpty())
        return m_lookupMessage;
    switch (m_lookupState) {
    case LookupState::Searching:
        return QStringLiteral("Searching…");
    case LookupState::Found:
        return QStringLiteral("Send a friend request");
    case LookupState::RequestPending:
        return QStringLiteral("Request sent, waiting for a reply");
    case LookupState::RequestSent:
        return QStringLiteral("Request sent");
    case LookupState::IncomingPending:
        return QStringLiteral("Sent you a friend request");
    case LookupState::Self:
        return QStringLiteral("That's you");
    case LookupState::NotFound:
        return QStringLiteral("No user with that name");
    case LookupState::Failed:
        return QStringLiteral("Couldn't reach the server");
    case LookupState::Idle:
    case LookupState::AlreadyContact:
    case LookupState::Blocked:
        return QString();
    }
    return QString();
}

bool ContactController::lookupVisible() const
{
    switch (m_lookupState) {
    case LookupState::Idle:
    case LookupState::AlreadyContact:
    case LookupState::Blocked:
        return false;
    default:
        return !m_lookupHandle.isEmpty();
    }
}

bool ContactController::lookupCanRequest() const
{
    return m_lookupState == LookupState::Found;
}

bool ContactController::lookupCanCancel() const
{
    return m_lookupState == LookupState::RequestPending
        || m_lookupState == LookupState::RequestSent;
}

void ContactController::cancelLookupRequest()
{
    if (!lookupCanCancel())
        return;
    if (m_session != nullptr && m_lookupAccount) {
        if (!cancelOutgoingRequest(*m_lookupAccount)) {
            setStatus(Status::Error, QStringLiteral("Couldn't withdraw that request."));
            return;
        }
    }
    // Mock mode has nothing to remove; the row simply offers the request again.
    setStatus(Status::Success, QStringLiteral("Request withdrawn."));
    setLookup(LookupState::Found);
}

bool ContactController::cancelOutgoingRequest(const AccountId &peer)
{
    if (m_session == nullptr)
        return false;
    SqlCipherContactRepository *contacts = m_session->contacts();
    if (contacts == nullptr)
        return false;
    const auto found = contacts->find(peer);
    if (!found.hasValue() || !found.value().has_value()
        || found.value()->state != ContactState::PendingOutgoing
        || !found.value()->conversationId.has_value())
        return false;
    // Best effort across the three stores; a partial failure is logged inside,
    // and what did go is gone, so the request cannot come back half-way.
    return discardOutgoingRequest(*m_session, peer, *found.value()->conversationId,
                                  /*removeContactRow=*/true);
}

void ContactController::setLookup(LookupState state, const QString &message)
{
    m_lookupState = state;
    m_lookupMessage = message;
    emit lookupChanged();
}

void ContactController::lookup(const QString &query)
{
    const QString handle = normalizeHandle(query);
    if (handle.isEmpty()) {
        m_lookupDebounce.stop();
        m_lookupHandle.clear();
        m_lookupAccount.reset();
        m_lookupDirty = false;
        setLookup(LookupState::Idle);
        return;
    }
    if (handle == m_lookupHandle && m_lookupState != LookupState::Idle)
        return;
    m_lookupHandle = handle;
    m_lookupAccount.reset();
    setLookup(LookupState::Searching);
    m_lookupDebounce.start();
}

void ContactController::runLookup()
{
    if (m_lookupHandle.isEmpty())
        return;

    if (m_relay == nullptr) {
        // Mock mode: resolve against the seeded directory.
        if (!m_mockDirectory.contains(m_lookupHandle)) {
            setLookup(LookupState::NotFound);
            return;
        }
        auto account = m_mockAccounts.find(m_lookupHandle);
        if (account == m_mockAccounts.end())
            account = m_mockAccounts.insert(m_lookupHandle, AccountId::generate());
        classifyLookup(*account);
        return;
    }

    // One directory call at a time so a reply always belongs to the handle it
    // was issued for; a text change during flight re-runs afterwards.
    if (m_lookupInFlight) {
        m_lookupDirty = true;
        return;
    }
    m_lookupInFlight = true;
    m_lookupDirty = false;
    const QString issuedFor = m_lookupHandle;
    const auto finish = [this, issuedFor](std::function<void()> apply) {
        clearLookupConnections();
        m_lookupInFlight = false;
        if (m_lookupDirty || issuedFor != m_lookupHandle) {
            // Stale: the text moved on. Look the current text up instead.
            m_lookupDirty = false;
            if (!m_lookupHandle.isEmpty())
                runLookup();
            return;
        }
        apply();
    };
    m_lookupConnections << connect(
        m_relay, &RelayClient::handleResolved, this,
        [this, finish](const RelayDirectoryEntry &entry) {
            const AccountId account = entry.accountId;
            finish([this, account] { classifyLookup(account); });
        });
    m_lookupConnections << connect(
        m_relay, &RelayClient::handleResolutionFailed, this,
        [this, finish](RelayDirectoryError error) {
            finish([this, error] {
                setLookup(error == RelayDirectoryError::NotFound ? LookupState::NotFound
                                                                 : LookupState::Failed);
            });
        });
    m_lookupConnections << connect(m_relay, &RelayClient::authExpired, this, [this, finish] {
        finish([this] { setLookup(LookupState::Failed); });
    });
    m_lookupConnections << connect(m_relay, &RelayClient::transportError, this,
                                   [this, finish](RelayTransportError) {
                                       finish([this] { setLookup(LookupState::Failed); });
                                   });
    m_relay->resolveHandle(issuedFor);
}

void ContactController::classifyLookup(const AccountId &account)
{
    m_lookupAccount = account;
    if (m_session != nullptr) {
        if (const auto self = m_session->accountId(); self.hasValue() && self.value() == account) {
            setLookup(LookupState::Self);
            return;
        }
        if (SqlCipherContactRepository *contacts = m_session->contacts()) {
            auto found = contacts->find(account);
            if (found.hasValue() && found.value().has_value()) {
                switch (found.value()->state) {
                case ContactState::PendingOutgoing:
                    setLookup(LookupState::RequestPending);
                    return;
                case ContactState::PendingIncoming:
                    setLookup(LookupState::IncomingPending);
                    return;
                case ContactState::Accepted:
                    setLookup(LookupState::AlreadyContact);
                    return;
                case ContactState::Blocked:
                    setLookup(LookupState::Blocked);
                    return;
                }
            }
        }
    }
    setLookup(LookupState::Found);
}

void ContactController::requestLookup()
{
    if (m_lookupState != LookupState::Found || m_lookupHandle.isEmpty())
        return;
    setLookup(LookupState::Searching, QStringLiteral("Sending request…"));
    addByHandle(m_lookupHandle);
}

void ContactController::clearLookupConnections()
{
    for (const QMetaObject::Connection &connection : m_lookupConnections)
        QObject::disconnect(connection);
    m_lookupConnections.clear();
}

void ContactController::setMockDirectory(const QStringList &handles)
{
    m_mockDirectory = handles;
}

void ContactController::resolveHandleFor(const AccountId &account)
{
    if (m_relay != nullptr)
        m_relay->resolveAccount(account);
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
                if (!m_pendingAddHandle.isEmpty() && m_pendingAddHandle == m_lookupHandle)
                    setLookup(LookupState::RequestSent);
            });
    connect(m_pendingAdd.get(), &AddContactService::failed, this,
            [this](AddContactService::Error error) {
                setStatus(Status::Error, messageForAddError(error));
                if (!m_pendingAddHandle.isEmpty() && m_pendingAddHandle == m_lookupHandle)
                    setLookup(LookupState::Found, messageForAddError(error));
            });
    return m_pendingAdd.get();
}

void ContactController::addByHandle(const QString &handle)
{
    m_pendingAddHandle = normalizeHandle(handle);
    if (AddContactService *service = beginAdd()) {
        setStatus(Status::Working, QStringLiteral("Sending request…"));
        service->startByHandle(m_pendingAddHandle.isEmpty() ? handle : m_pendingAddHandle);
        return;
    }

    // Mock mode: no network, resolve to a simulated success.
    setStatus(Status::Working, QStringLiteral("Sending request…"));
    setStatus(Status::Success, QStringLiteral("Request sent."));
    if (!m_pendingAddHandle.isEmpty() && m_pendingAddHandle == m_lookupHandle)
        setLookup(LookupState::RequestSent);
}

void ContactController::addByInvite(const QString &inviteText)
{
    m_pendingAddHandle.clear();
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

    if (m_relay != nullptr) {
        // Reverse lookups resolve an inbound request's sender to a handle so the
        // request (and later the chat) shows a name instead of an id fingerprint.
        m_reverseConnections << connect(
            m_relay, &RelayClient::accountResolved, this,
            [this](const AccountId &account, const QString &handle) {
                if (m_session != nullptr) {
                    if (SqlCipherContactRepository *contacts = m_session->contacts())
                        (void)contacts->setHandle(account, handle);
                }
                m_requests.updateHandle(account, handle);
                emit contactHandleResolved(account.toHex(), handle);
            });
    }

    seedFromRoster();

    if (m_requestsSvc == nullptr)
        return;

    connect(m_requestsSvc, &ContactRequestService::incomingRequest, this,
            [this](const AccountId &sender, const ConversationId &conversation) {
                m_requests.appendRequest(entryForRequest(sender, conversation));
                resolveHandleFor(sender);
            });
    connect(m_requestsSvc, &ContactRequestService::contactAccepted, this,
            [this](const AccountId &sender) {
                const bool wasInbound = m_requests.removeByAccount(sender);
                setStatus(Status::Success, QStringLiteral("Contact added."));
                if (m_lookupAccount && *m_lookupAccount == sender)
                    setLookup(LookupState::AlreadyContact);
                // Show the safety number at the natural verify moment: an inbound
                // request was just accepted and the peer key is now bound.
                if (wasInbound)
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
    QVector<AccountId> unresolved;
    for (const ContactRecord &record : all.value()) {
        if (record.state != ContactState::PendingIncoming || !record.conversationId.has_value())
            continue;
        entries.append(entryForRequest(record.accountId, *record.conversationId, record.handle));
        if (record.handle.isEmpty())
            unresolved.append(record.accountId);
    }
    m_requests.setRequests(std::move(entries));
    for (const AccountId &account : unresolved)
        resolveHandleFor(account);
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
