#pragma once

#include "domain/Identifiers.h"

#include <QList>
#include <QObject>

namespace OpenChat {

class ProfileSession;
class SyncEngine;
struct MessageRecord;

// Surfaces inbound contact requests and applies the local user's decision on
// them (Accept / Decline / Block). The durable receive branch of the SyncEngine
// only STASHES an inbound handshake Welcome and emits handshakeReceived; this
// long-lived service turns that into a roster PendingIncoming row, and drives the
// authenticate-before-join accept, the decline, and the block through the engine
// and the store.
//
// It also closes the loop for the REQUESTER: an accept sends a ContactAccept
// control message back through the freshly joined group, and receiving one (or
// any authenticated application message) in a PendingOutgoing conversation
// promotes that contact to Accepted. Both sides materialise the durable
// `conversations` row the message store's foreign key requires, so a chat can
// be opened the moment a contact becomes Accepted.
//
// Unlike AddContactService (a single-shot state machine), this is a durable
// QObject: it wires every engine signal in the constructor and stays connected
// for the session's lifetime, disconnecting only on teardown(). Modelled on
// AccountBootstrap's connect/teardown discipline.
//
// Lifetimes: the session and engine are borrowed by reference and MUST outlive
// this object. The session must be unlocked (contacts()/syncStore() live) and
// networking started (the borrowed engine is the session's live engine).
class ContactRequestService final : public QObject
{
    Q_OBJECT

public:
    ContactRequestService(ProfileSession &session, SyncEngine &engine, QObject *parent = nullptr);
    ~ContactRequestService() override;

    ContactRequestService(const ContactRequestService &) = delete;
    ContactRequestService &operator=(const ContactRequestService &) = delete;

    // Accept the stashed request for `conversation`: loads the stash, prechecks the
    // peer is still PendingIncoming (single-threaded, race-free), then asks the
    // engine to authenticate the Welcome and, only if it passes, join + commit.
    // An absent stash or a non-PendingIncoming peer emits requestActionFailed.
    void acceptContact(const ConversationId &conversation);
    // Decline the stashed request: forget the peer and drop the stash.
    void declineContact(const ConversationId &conversation);
    // Block the stashed request's sender: block FIRST (so a crash before the stash
    // delete still leaves the peer blocked, and reconcile drops the orphan), then
    // drop the stash.
    void blockContact(const ConversationId &conversation);

    // Reconcile the durable stash against the roster on startup (call right after
    // startNetworking). For each stashed row: drop the orphan when the peer is
    // Blocked or Accepted; otherwise re-assert its PendingIncoming roster row so a
    // crash between the durable stash and the in-memory record self-heals.
    void reconcileOnStartup();

    // Disconnect from the engine. Called by the destructor; safe to call before
    // the borrowed engine/session are torn down.
    void teardown();

signals:
    // A new inbound request is pending the user's decision (for the Phase 10 UI).
    void incomingRequest(const OpenChat::AccountId &sender,
                         const OpenChat::ConversationId &conversation);
    // A contact became Accepted: either the local user accepted an inbound
    // request (`sender` is the requester) or the peer accepted ours (`sender` is
    // the peer we asked). Either way a conversation row exists and the chat can
    // be opened.
    void contactAccepted(const OpenChat::AccountId &sender);
    // A request action could not be applied (e.g. no stash, or the peer is no
    // longer PendingIncoming).
    void requestActionFailed(const OpenChat::ConversationId &conversation);
    // An accept failed authentication: the Welcome did not name the claimed sender
    // device. Treated as a decline; surfaced so the UI can warn the user.
    void securityNotice(const OpenChat::ConversationId &conversation,
                        const OpenChat::AccountId &sender);

private:
    void onHandshakeReceived(const AccountId &sender, const DeviceId &senderDevice,
                             const ConversationId &conversation, qint64 receivedAtMs);
    void onHandshakeAccepted(const ConversationId &conversation, const AccountId &sender);
    void onHandshakeAuthFailed(const ConversationId &conversation, const AccountId &sender);
    void onContactAcceptReceived(const ConversationId &conversation, const DeviceId &senderDevice);
    void onMessageReceived(const MessageRecord &message);
    // Promotes the PendingOutgoing contact bound to `conversation` (if any) to
    // Accepted, binding `peerDevice` when the row has none, and emits
    // contactAccepted. A no-op for any other state.
    void promoteOutgoing(const ConversationId &conversation, const DeviceId &peerDevice);
    // Idempotently creates the durable conversation row messages are keyed by.
    [[nodiscard]] bool ensureConversationRow(const ConversationId &conversation);

    ProfileSession &m_session;
    SyncEngine &m_engine;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace OpenChat
