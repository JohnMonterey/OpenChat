#include "app/AddContactService.h"

#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "domain/Contact.h"
#include "network/SyncEngine.h"
#include "repositories/RepositoryError.h"
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
    m_relay.claimKeyPackage(m_entry->devices[m_deviceIndex].deviceId);
}

void AddContactService::onKeyPackageClaimed(const QByteArray &keyPackage)
{
    if (m_state != State::Claiming)
        return;

    // This handler MUST run to completion synchronously: there is no event-loop
    // spin between addMembers() and sendHandshake(), so the pending MLS state the
    // engine surrenders is exactly this group's createGroup + addMembers snapshot
    // and cannot be raced by another mutation of the shared MlsClient.
    MlsClient *mls = m_session.mls();
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (mls == nullptr || contacts == nullptr) {
        fail(Error::Storage);
        return;
    }

    const ConversationId conversation = ConversationId::generate();
    if (!mls->createGroup(conversation).hasValue()) {
        fail(Error::Mls);
        return;
    }
    auto added = mls->addMembers(conversation, {keyPackage});
    if (!added.hasValue()) {
        fail(Error::Mls);
        return;
    }
    // Discard .commit: addMembers already merged the pending commit locally, and a
    // brand-new 2-party group has no other member to receive it. Only the Welcome
    // ships.
    const QByteArray welcome = added.value().welcome;

    // Record the outgoing request BEFORE sending the Welcome. recordOutgoingRequest
    // is the authoritative Blocked gate (a Conflict result means the peer is locally
    // blocked), and ordering it ahead of the send keeps crash-safety
    // one-directional: commitControlSend atomically commits the group's MLS state
    // with the Welcome outbox, so "durable group <=> durable Welcome" always holds.
    // The only crash window (after this record, before/around the send) leaves a
    // benign PendingOutgoing row with no durable group, which is retryable and
    // strictly better than a durable group/Welcome with no roster row.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const ContactRecord record{m_entry->accountId,
                               m_handle,
                               QString(),
                               ContactState::PendingOutgoing,
                               conversation,
                               now,
                               now};
    auto recorded = contacts->recordOutgoingRequest(record);
    if (!recorded.hasValue()) {
        fail(recorded.error().code == RepositoryErrorCode::Conflict ? Error::Blocked
                                                                    : Error::Storage);
        return;
    }

    m_engine.sendHandshake(conversation, m_entry->devices[m_deviceIndex].deviceId, welcome);
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
