#include "app/AddContactService.h"
#include "diagnostics/Logging.h"

#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "domain/Contact.h"
#include "network/SyncEngine.h"
#include "repositories/RepositoryError.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"

#include <QDateTime>

#include <utility>

namespace OpenChat {

AddContactService::AddContactService(ProfileSession &session, RelayClient &relay,
                                     SyncEngine &engine, QObject *parent)
    : QObject(parent), m_session(session), m_relay(relay), m_engine(engine)
{
}

AddContactService::~AddContactService() { teardown(); }

void AddContactService::startByHandle(const QString &handle)
{
    if (m_state != State::Idle)
        return;
    m_handle = handle;
    connectRelay();
    m_state = State::Resolving;
    m_relay.resolveHandle(handle);
}

void AddContactService::startByInvite(const QByteArray &inviteToken)
{
    if (m_state != State::Idle)
        return;
    // The invite path carries no handle; the roster row is stamped with an empty
    // handle and can be refreshed from the directory later.
    m_handle.clear();
    connectRelay();
    m_state = State::Resolving;
    m_relay.redeemInvite(inviteToken);
}

void AddContactService::connectRelay()
{
    // Wire every relay outcome before issuing the first call so a synchronous
    // failure (e.g. an insecure endpoint) is caught by onTransportError. The
    // handle and invite paths share the claim/fail/transport signals; only the
    // resolve success/failure pair differs and both are routed to one handler.
    m_connections << connect(&m_relay, &RelayClient::handleResolved, this,
                             &AddContactService::onResolved);
    m_connections << connect(&m_relay, &RelayClient::handleResolutionFailed, this,
                             &AddContactService::onResolutionFailed);
    m_connections << connect(&m_relay, &RelayClient::inviteRedeemed, this,
                             &AddContactService::onResolved);
    m_connections << connect(&m_relay, &RelayClient::inviteRedemptionFailed, this,
                             &AddContactService::onResolutionFailed);
    m_connections << connect(&m_relay, &RelayClient::keyPackageClaimed, this,
                             &AddContactService::onKeyPackageClaimed);
    m_connections << connect(&m_relay, &RelayClient::keyPackageClaimFailed, this,
                             &AddContactService::onKeyPackageClaimFailed);
    m_connections << connect(&m_relay, &RelayClient::authExpired, this,
                             &AddContactService::onAuthExpired);
    m_connections << connect(&m_relay, &RelayClient::transportError, this,
                             &AddContactService::onTransportError);
}

void AddContactService::onResolved(const RelayDirectoryEntry &entry)
{
    if (m_state != State::Resolving)
        return;
    const auto self = m_session.accountId();
    if (!self.hasValue()) {
        fail(Error::Storage);
        return;
    }
    if (entry.accountId == self.value()) {
        fail(Error::SelfContact);
        return;
    }
    if (entry.devices.isEmpty()) {
        fail(Error::NoDevice);
        return;
    }
    m_entry = entry;
    m_deviceIndex = 0;
    m_state = State::Claiming;
    claimCurrentDevice();
}

void AddContactService::onResolutionFailed(RelayDirectoryError error)
{
    if (m_state != State::Resolving)
        return;
    fail(error == RelayDirectoryError::NotFound ? Error::NotFound : Error::Transport);
}

void AddContactService::claimCurrentDevice()
{
    // Complete package-independent validation/storage before consuming a one-time
    // package. Failed attempts may leave a retryable PendingOutgoing row.
    auto *mls = m_session.mls();
    auto *contacts = m_session.contacts();
    auto *chats = m_session.chats();
    if (!mls || !contacts || !chats) {
        fail(Error::Storage);
        return;
    }
    const auto existing = contacts->find(m_entry->accountId);
    if (!existing.hasValue()) {
        fail(Error::Storage);
        return;
    }
    if (existing.value() && existing.value()->state == ContactState::Blocked) {
        fail(Error::Blocked);
        return;
    }
    const ConversationId conversation = ConversationId::generate();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ContactRecord record{m_entry->accountId, m_handle, QString(),
                         ContactState::PendingOutgoing, conversation, now, now};
    record.peerDeviceId = m_entry->devices[m_deviceIndex].deviceId;
    const auto recorded = contacts->recordOutgoingRequest(record);
    if (!recorded.hasValue()) {
        fail(recorded.error().code == RepositoryErrorCode::Conflict ? Error::Blocked : Error::Storage);
        return;
    }
    if (!chats->upsertConversation(ConversationRecord{conversation, conversation.bytes(),
            QString(), ConversationKind::Direct, now}).hasValue()) {
        fail(Error::Storage);
        return;
    }
    if (!mls->createGroup(conversation).hasValue()) {
        fail(Error::Mls);
        return;
    }
    // Do not leave captured MLS state pending across the asynchronous claim.
    if (!m_session.persistMlsState().hasValue()) {
        fail(Error::Storage);
        return;
    }
    m_conversation = conversation;
    qCDebug(contactsLog) << "Local contact preparation complete; claiming key package";
    m_relay.claimKeyPackage(*record.peerDeviceId);
}

void AddContactService::onKeyPackageClaimed(const QByteArray &keyPackage)
{
    if (m_state != State::Claiming)
        return;
    auto *mls = m_session.mls();
    auto *contacts = m_session.contacts();
    if (!mls || !contacts || !m_conversation) {
        fail(Error::Storage);
        return;
    }
    // The user may have blocked the peer while the claim was in flight.
    const auto current = contacts->find(m_entry->accountId);
    if (!current.hasValue()) {
        fail(Error::Storage);
        return;
    }
    if (current.value() && current.value()->state == ContactState::Blocked) {
        fail(Error::Blocked);
        return;
    }
    const ConversationId conversation = *m_conversation;
    // No event-loop spin between addMembers and sendHandshake: the MLS state
    // and Welcome are committed together by SyncEngine.
    auto added = mls->addMembers(conversation, {keyPackage});
    if (!added.hasValue()) {
        fail(Error::Mls);
        return;
    }
    const auto credential = mls->inspectKeyPackage(keyPackage);
    if (credential.hasValue() && credential.value().size() >= 49
        && static_cast<unsigned char>(credential.value().at(0)) == 1) {
        // Optional pin backfill: a failed write does not abandon the Welcome.
        if (!contacts->setPeerSigningKey(m_entry->accountId, credential.value().sliced(17, 32)).hasValue())
            qCWarning(contactsLog) << "Could not save peer signing key";
    }
    m_engine.sendHandshake(conversation, m_entry->devices[m_deviceIndex].deviceId,
                           added.value().welcome);
    succeed(conversation, m_entry->accountId);
}

void AddContactService::onKeyPackageClaimFailed(RelayClaimError error)
{
    if (m_state != State::Claiming)
        return;
    if (error == RelayClaimError::Transport) {
        fail(Error::Transport);
        return;
    }
    // Unavailable means this device has no KeyPackage left to hand out; try the
    // next advertised device before giving up. Malformed (or an exhausted device
    // list) is terminal.
    if (error == RelayClaimError::Unavailable
        && m_deviceIndex + 1 < m_entry->devices.size()) {
        ++m_deviceIndex;
        claimCurrentDevice();
        return;
    }
    fail(Error::NoKeyPackage);
}

void AddContactService::onAuthExpired()
{
    if (m_state == State::Resolving || m_state == State::Claiming)
        fail(Error::Transport);
}

void AddContactService::onTransportError(RelayTransportError error)
{
    (void)error;
    if (m_state == State::Resolving || m_state == State::Claiming)
        fail(Error::Transport);
}

void AddContactService::succeed(const ConversationId &conversation, const AccountId &peer)
{
    if (m_state == State::Terminated)
        return;
    m_state = State::Terminated;
    teardown();
    emit succeeded(conversation, peer);
}

void AddContactService::fail(Error error)
{
    qCWarning(contactsLog) << "Add contact failed; stage" << static_cast<int>(m_state)
                           << "error" << static_cast<int>(error);
    if (m_state == State::Terminated)
        return;
    m_state = State::Terminated;
    teardown();
    emit failed(error);
}

void AddContactService::teardown()
{
    for (const QMetaObject::Connection &connection : m_connections)
        QObject::disconnect(connection);
    m_connections.clear();
}

} // namespace OpenChat
