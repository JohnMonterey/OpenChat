#include "app/ContactRequestService.h"

#include "app/ProfileSession.h"
#include "domain/Contact.h"
#include "network/SyncEngine.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherSyncStore.h"

#include <QDateTime>

#include <optional>

namespace OpenChat {

namespace {

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

ContactRequestService::ContactRequestService(ProfileSession &session, SyncEngine &engine,
                                             QObject *parent)
    : QObject(parent), m_session(session), m_engine(engine)
{
    // Wire every engine outcome up front and keep it connected for the session's
    // lifetime (teardown() drops them). handshakeReceived surfaces a new request;
    // handshakeAccepted/handshakeAuthFailed report the outcome of an accept.
    m_connections << connect(&m_engine, &SyncEngine::handshakeReceived, this,
                             &ContactRequestService::onHandshakeReceived);
    m_connections << connect(&m_engine, &SyncEngine::handshakeAccepted, this,
                             &ContactRequestService::onHandshakeAccepted);
    m_connections << connect(&m_engine, &SyncEngine::handshakeAuthFailed, this,
                             &ContactRequestService::onHandshakeAuthFailed);
}

ContactRequestService::~ContactRequestService() { teardown(); }

void ContactRequestService::onHandshakeReceived(const AccountId &sender,
                                                const DeviceId & /*senderDevice*/,
                                                const ConversationId &conversation,
                                                qint64 receivedAtMs)
{
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (contacts == nullptr)
        return;
    // recordIncomingRequest is the authoritative roster policy: it creates a
    // PendingIncoming row for a new peer, never resurrects Blocked, and never
    // regresses Accepted/PendingOutgoing. The engine already dropped blocked
    // senders before emitting, so this normally lands a fresh PendingIncoming row.
    const ContactRecord record{sender,
                               QString(),
                               QString(),
                               ContactState::PendingIncoming,
                               conversation,
                               receivedAtMs,
                               receivedAtMs};
    if (!contacts->recordIncomingRequest(record).hasValue())
        return;
    emit incomingRequest(sender, conversation);
}

void ContactRequestService::acceptContact(const ConversationId &conversation)
{
    SqlCipherSyncStore *store = m_session.syncStore();
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (store == nullptr || contacts == nullptr) {
        emit requestActionFailed(conversation);
        return;
    }
    auto stash = store->loadPendingHandshake(conversation);
    if (!stash.hasValue() || !stash.value().has_value()) {
        emit requestActionFailed(conversation);
        return;
    }
    const PendingHandshakeRecord record = std::move(stash).value().value();

    // Precheck the peer is still PendingIncoming. Single-threaded, so this read is
    // race-free with the accept that follows; keeping the check here (rather than
    // relying on commitHandshakeAccept's guarded flip) keeps a non-pending peer off
    // the store's rollback path entirely.
    auto found = contacts->find(record.senderAccountId);
    if (!found.hasValue() || !found.value().has_value()
        || found.value()->state != ContactState::PendingIncoming) {
        emit requestActionFailed(conversation);
        return;
    }

    // The engine authenticates the Welcome against the stashed sender device before
    // any join, then joins + commits atomically. Its handshakeAccepted /
    // handshakeAuthFailed signal drives onHandshakeAccepted / onHandshakeAuthFailed.
    m_engine.acceptHandshake(conversation, record.senderAccountId, record.senderDeviceId,
                             record.welcome);
}

void ContactRequestService::declineContact(const ConversationId &conversation)
{
    SqlCipherSyncStore *store = m_session.syncStore();
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (store == nullptr || contacts == nullptr) {
        emit requestActionFailed(conversation);
        return;
    }
    auto stash = store->loadPendingHandshake(conversation);
    if (!stash.hasValue() || !stash.value().has_value()) {
        emit requestActionFailed(conversation);
        return;
    }
    const AccountId sender = stash.value()->senderAccountId;
    // Forget the peer and drop the stash. No MLS state exists (we never joined).
    (void)contacts->remove(sender);
    (void)store->deletePendingHandshake(conversation);
}

void ContactRequestService::blockContact(const ConversationId &conversation)
{
    SqlCipherSyncStore *store = m_session.syncStore();
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (store == nullptr || contacts == nullptr) {
        emit requestActionFailed(conversation);
        return;
    }
    auto stash = store->loadPendingHandshake(conversation);
    if (!stash.hasValue() || !stash.value().has_value()) {
        emit requestActionFailed(conversation);
        return;
    }
    const AccountId sender = stash.value()->senderAccountId;
    // Block FIRST: if a crash lands between here and the stash delete, the peer is
    // already Blocked and reconcileOnStartup drops the now-orphan stash.
    (void)contacts->block(sender, nowMs());
    (void)store->deletePendingHandshake(conversation);
}

void ContactRequestService::onHandshakeAccepted(const ConversationId & /*conversation*/,
                                                const AccountId &sender)
{
    // commitHandshakeAccept already flipped the roster to Accepted and deleted the
    // stash atomically; nothing to clean up here. Surface the outcome for the UI.
    emit contactAccepted(sender);
}

void ContactRequestService::onHandshakeAuthFailed(const ConversationId &conversation,
                                                  const AccountId &sender)
{
    // Authentication failed and NO MLS state was mutated (inspectWelcome is
    // side-effect-free and any join ran only after auth passed). Treat it exactly
    // like a decline -- drop the stash and forget the peer, never mark Accepted --
    // and raise a security notice so the UI can warn the user.
    if (SqlCipherSyncStore *store = m_session.syncStore())
        (void)store->deletePendingHandshake(conversation);
    if (SqlCipherContactRepository *contacts = m_session.contacts())
        (void)contacts->remove(sender);
    emit securityNotice(conversation, sender);
}

void ContactRequestService::reconcileOnStartup()
{
    SqlCipherSyncStore *store = m_session.syncStore();
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (store == nullptr || contacts == nullptr)
        return;
    auto rows = store->pendingHandshakes();
    if (!rows.hasValue())
        return;
    for (const PendingHandshakeRecord &row : rows.value()) {
        auto found = contacts->find(row.senderAccountId);
        if (!found.hasValue())
            continue;
        const bool orphan = found.value().has_value()
                            && (found.value()->state == ContactState::Blocked
                                || found.value()->state == ContactState::Accepted);
        if (orphan) {
            // A stash left behind after the peer was blocked or already accepted:
            // there is no decision left to make, so drop it.
            (void)store->deletePendingHandshake(row.conversationId);
            continue;
        }
        // Self-heal a crash between commitHandshakeReceive and the in-memory
        // recordIncomingRequest: (re)assert the PendingIncoming roster row. Idempotent.
        const ContactRecord record{row.senderAccountId,
                                   QString(),
                                   QString(),
                                   ContactState::PendingIncoming,
                                   row.conversationId,
                                   row.receivedAtMs,
                                   row.receivedAtMs};
        (void)contacts->recordIncomingRequest(record);
    }
}

void ContactRequestService::teardown()
{
    for (const QMetaObject::Connection &connection : m_connections)
        QObject::disconnect(connection);
    m_connections.clear();
}

} // namespace OpenChat
