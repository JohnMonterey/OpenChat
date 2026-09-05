#include "app/ContactRequestService.h"

#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "domain/Contact.h"
#include "network/SyncEngine.h"
#include "protocol/CiphertextEnvelope.h"
#include "security/KeyVault.h"
#include "security/SecureBuffer.h"
#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherDatabase.h"
#include "storage/SqlCipherSyncStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <memory>
#include <optional>

using namespace OpenChat;

namespace {

// Minimal in-memory KeyVault (no OS keychain in this environment). Mirrors the
// fake vault used by tst_profilesession / tst_addcontactservice.
class InMemoryVault final : public KeyVault
{
public:
    KeyVaultAvailability availability() const override { return KeyVaultAvailability::Available; }

    Result<SecureBuffer, KeyVaultError> readProfileKey(const ProfileId &) override
    {
        return read(m_databaseKey);
    }
    Result<SecureBuffer, KeyVaultError> createProfileKey(const ProfileId &) override
    {
        return create(m_databaseKey);
    }
    Result<void, KeyVaultError> deleteProfileKey(const ProfileId &) override
    {
        m_databaseKey.reset();
        return Result<void, KeyVaultError>::success();
    }
    Result<SecureBuffer, KeyVaultError> readDeviceWrappingKey(const ProfileId &) override
    {
        return read(m_wrappingKey);
    }
    Result<SecureBuffer, KeyVaultError> createDeviceWrappingKey(const ProfileId &) override
    {
        return create(m_wrappingKey);
    }
    Result<void, KeyVaultError> deleteDeviceWrappingKey(const ProfileId &) override
    {
        m_wrappingKey.reset();
        return Result<void, KeyVaultError>::success();
    }

private:
    static Result<SecureBuffer, KeyVaultError> read(const std::optional<SecureBuffer> &key)
    {
        if (!key)
            return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::NotFound);
        return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
    }
    static Result<SecureBuffer, KeyVaultError> create(std::optional<SecureBuffer> &key)
    {
        if (key)
            return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::AlreadyExists);
        key = SecureBuffer::random(32);
        return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
    }

    std::optional<SecureBuffer> m_databaseKey;
    std::optional<SecureBuffer> m_wrappingKey;
};

// A connected ciphertext transport that records acks. The receive path calls
// acknowledge() after a durable stash; no sends leave the device on accept.
class FakeTransport final : public SyncTransport
{
public:
    bool isConnected() const override { return true; }
    void sendEnvelope(const CiphertextEnvelopeV1 &envelope) override { sent.append(envelope); }
    void acknowledge(const EnvelopeId &envelopeId, quint64 watermark) override
    {
        acks.append({envelopeId, watermark});
    }

    QVector<CiphertextEnvelopeV1> sent;
    QVector<std::pair<EnvelopeId, quint64>> acks;
};

// Serialized DevicePublicCredential the MLS layer authenticates:
// version(1) || deviceId(16) || signingKey(32). The sender MlsClient is created
// with this as its identity so inspectWelcome returns a credential naming
// `device`.
QByteArray credentialFor(const DeviceId &device)
{
    QByteArray credential;
    credential.append(char{1});
    credential.append(device.bytes());
    credential.append(QByteArray(32, 'k'));
    return credential;
}

} // namespace

class ContactRequestServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void handshakeReceivedRecordsPendingIncoming();
    void acceptContactPromotesJoinsAndClearsStash();
    void declineContactRemovesRowAndStash();
    void blockContactMarksBlockedAndClearsStash();
    void blockedPeerInboundProducesNoPending();
    void reconcileRecreatesMissingAndDropsOrphans();
    void authFailureDropsStashWithoutAccepting();
    void acceptSendsContactAcceptAndOpensConversation();
    void contactAcceptFromPeerPromotesOutgoingRequest();

private:
    // A genuine inbound handshake: our fresh KeyPackage is added to a new group the
    // sender forms, producing a real Welcome sealed to us. Returns the conversation.
    // `claimedSenderDevice` is what the relay envelope claims (defaults to the real
    // sender device so authentication passes; override it to forge a mismatch).
    ConversationId feedInboundHandshake(std::optional<DeviceId> claimedSenderDevice = std::nullopt)
    {
        const ConversationId conversation = ConversationId::generate();
        auto ourKeyPackage = m_session->mls()->generateKeyPackage();
        if (!ourKeyPackage.hasValue()) {
            qWarning("fixture: generateKeyPackage failed (%d)",
                     static_cast<int>(ourKeyPackage.error()));
            return conversation;
        }
        if (auto created = m_sender->createGroup(conversation); !created.hasValue()) {
            qWarning("fixture: createGroup failed (%d)", static_cast<int>(created.error()));
            return conversation;
        }
        auto add = m_sender->addMembers(conversation, {ourKeyPackage.value()});
        if (!add.hasValue()) {
            qWarning("fixture: addMembers failed (%d)", static_cast<int>(add.error()));
            return conversation;
        }
        const QByteArray welcome = add.value().welcome;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const DeviceId claimed = claimedSenderDevice.value_or(m_senderDevice);
        const CiphertextEnvelopeV1 envelope{
            1,
            EnvelopeId::generate(),
            m_senderAccount,
            claimed,
            DeviceId::generate(),
            conversation,
            EnvelopeMessageKind::MlsHandshake,
            now,
            now + 3'600'000,
            EnvelopeId::generate(),
            welcome,
            QCryptographicHash::hash(welcome, QCryptographicHash::Sha256),
            QByteArray(64, '\x03')};
        m_session->syncEngine()->handleEnvelope(envelope, ++m_sequence);
        QCoreApplication::processEvents();
        return conversation;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<InMemoryVault> m_vault;
    std::unique_ptr<ProfileSession> m_session;
    std::unique_ptr<FakeTransport> m_transport;

    std::unique_ptr<SqlCipherDatabase> m_senderDb;
    std::unique_ptr<CapturingMlsStateStore> m_senderCapture;
    std::unique_ptr<MlsClient> m_sender;
    AccountId m_senderAccount = AccountId::generate();
    DeviceId m_senderDevice = DeviceId::generate();
    quint64 m_sequence = 0;
};

void ContactRequestServiceTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_vault = std::make_unique<InMemoryVault>();

    const auto profileId = ProfileId::generate();
    const auto paths = ProfilePaths::forProfile(m_dir->path(), profileId);
    auto created = ProfileSession::create(profileId, *m_vault, paths);
    QVERIFY(created.hasValue());
    m_session = std::move(created).value();

    m_transport = std::make_unique<FakeTransport>();
    QVERIFY(m_session->startNetworking(*m_transport).hasValue());
    QVERIFY(m_session->syncEngine() != nullptr);
    m_session->syncEngine()->start();

    // A second MLS client stands in for the sender. Its identity is a real device
    // credential naming m_senderDevice, so inspectWelcome authenticates against it.
    m_senderAccount = AccountId::generate();
    m_senderDevice = DeviceId::generate();
    auto senderOpened = SqlCipherDatabase::open(m_dir->filePath(QStringLiteral("sender.sqlite3")),
                                                SecureBuffer::random(32));
    QVERIFY(senderOpened.hasValue());
    m_senderDb = std::make_unique<SqlCipherDatabase>(std::move(senderOpened).value());
    m_senderCapture = std::make_unique<CapturingMlsStateStore>(*m_senderDb, ProfileId::generate());
    auto senderResult = MlsClient::create(credentialFor(m_senderDevice), m_senderCapture.get());
    QVERIFY(senderResult.hasValue());
    m_sender = std::move(senderResult).value();
}

void ContactRequestServiceTest::cleanup()
{
    // The engine borrows the transport; tear the session down (stops the engine)
    // before the transport, and keep the vault last since the session borrows it.
    if (m_session)
        m_session->lock();
    m_session.reset();
    m_transport.reset();
    m_sender.reset();
    m_senderCapture.reset();
    m_senderDb.reset();
    m_vault.reset();
    m_dir.reset();
    m_sequence = 0;
}

void ContactRequestServiceTest::handshakeReceivedRecordsPendingIncoming()
{
    ContactRequestService service(*m_session, *m_session->syncEngine());
    std::optional<AccountId> requested;
    std::optional<ConversationId> requestedConversation;
    connect(&service, &ContactRequestService::incomingRequest, this,
            [&](const AccountId &a, const ConversationId &c) {
                requested = a;
                requestedConversation = c;
            });

    const ConversationId conversation = feedInboundHandshake();

    // The engine stashed the Welcome and acknowledged it exactly once.
    QCOMPARE(m_transport->acks.size(), qsizetype(1));

    // The service surfaced the request and recorded a PendingIncoming roster row
    // bound to the sender-chosen conversation.
    QVERIFY(requested.has_value());
    QCOMPARE(requested->bytes(), m_senderAccount.bytes());
    QVERIFY(requestedConversation.has_value());
    QCOMPARE(requestedConversation->bytes(), conversation.bytes());

    auto found = m_session->contacts()->find(m_senderAccount);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QCOMPARE(found.value()->state, ContactState::PendingIncoming);
    QVERIFY(found.value()->conversationId.has_value());
    QCOMPARE(found.value()->conversationId->bytes(), conversation.bytes());

    // The Welcome is durably stashed until the user decides.
    auto stash = m_session->syncStore()->loadPendingHandshake(conversation);
    QVERIFY(stash.hasValue());
    QVERIFY(stash.value().has_value());
}

void ContactRequestServiceTest::acceptContactPromotesJoinsAndClearsStash()
{
    ContactRequestService service(*m_session, *m_session->syncEngine());
    std::optional<AccountId> accepted;
    bool securityNotice = false;
    connect(&service, &ContactRequestService::contactAccepted, this,
            [&](const AccountId &a) { accepted = a; });
    connect(&service, &ContactRequestService::securityNotice, this,
            [&](const ConversationId &, const AccountId &) { securityNotice = true; });

    const ConversationId conversation = feedInboundHandshake();
    service.acceptContact(conversation);
    QCoreApplication::processEvents();

    // The accept authenticated, joined and committed: no security notice, and the
    // peer surfaced as Accepted.
    QVERIFY(!securityNotice);
    QVERIFY(accepted.has_value());
    QCOMPARE(accepted->bytes(), m_senderAccount.bytes());

    auto found = m_session->contacts()->find(m_senderAccount);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QCOMPARE(found.value()->state, ContactState::Accepted);

    // The stash was deleted atomically with the accept.
    auto stash = m_session->syncStore()->loadPendingHandshake(conversation);
    QVERIFY(stash.hasValue());
    QVERIFY(!stash.value().has_value());

    // The join really persisted: the sender encrypts an application message at the
    // joined epoch and our live client decrypts it, proving the epochs match.
    auto ciphertext = m_sender->encrypt(conversation, QByteArrayView("hi from sender"));
    QVERIFY(ciphertext.hasValue());
    auto processed = m_session->mls()->process(conversation, ciphertext.value().bytes);
    QVERIFY(processed.hasValue());
    QCOMPARE(processed.value().kind, MlsProcessKind::Application);
    QCOMPARE(processed.value().applicationData, QByteArray("hi from sender"));
}

void ContactRequestServiceTest::declineContactRemovesRowAndStash()
{
    ContactRequestService service(*m_session, *m_session->syncEngine());
    const ConversationId conversation = feedInboundHandshake();

    service.declineContact(conversation);

    // The roster row is gone and the stash is cleared.
    auto found = m_session->contacts()->find(m_senderAccount);
    QVERIFY(found.hasValue());
    QVERIFY(!found.value().has_value());

    auto stash = m_session->syncStore()->loadPendingHandshake(conversation);
    QVERIFY(stash.hasValue());
    QVERIFY(!stash.value().has_value());
}

void ContactRequestServiceTest::blockContactMarksBlockedAndClearsStash()
{
    ContactRequestService service(*m_session, *m_session->syncEngine());
    const ConversationId conversation = feedInboundHandshake();

    service.blockContact(conversation);

    // The peer is Blocked and the stash is cleared.
    auto found = m_session->contacts()->find(m_senderAccount);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QCOMPARE(found.value()->state, ContactState::Blocked);

    auto stash = m_session->syncStore()->loadPendingHandshake(conversation);
    QVERIFY(stash.hasValue());
    QVERIFY(!stash.value().has_value());
}

void ContactRequestServiceTest::blockedPeerInboundProducesNoPending()
{
    // Pre-block the sender before any request arrives.
    QVERIFY(m_session->contacts()
                ->block(m_senderAccount, QDateTime::currentMSecsSinceEpoch())
                .hasValue());

    ContactRequestService service(*m_session, *m_session->syncEngine());
    bool sawRequest = false;
    connect(&service, &ContactRequestService::incomingRequest, this,
            [&](const AccountId &, const ConversationId &) { sawRequest = true; });

    const ConversationId conversation = feedInboundHandshake();

    // The engine dropped the blocked sender (still acked so the relay stops
    // redelivering) and never surfaced a request.
    QVERIFY(!sawRequest);
    QCOMPARE(m_transport->acks.size(), qsizetype(1));

    // No stash was written and the peer is left Blocked, never regressed.
    auto stash = m_session->syncStore()->loadPendingHandshake(conversation);
    QVERIFY(stash.hasValue());
    QVERIFY(!stash.value().has_value());

    auto found = m_session->contacts()->find(m_senderAccount);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QCOMPARE(found.value()->state, ContactState::Blocked);
}

void ContactRequestServiceTest::reconcileRecreatesMissingAndDropsOrphans()
{
    SqlCipherSyncStore *store = m_session->syncStore();
    SqlCipherContactRepository *contacts = m_session->contacts();

    // (1) A durable stash whose in-memory PendingIncoming row never landed (a crash
    // between commitHandshakeReceive and the service's recordIncomingRequest).
    const ConversationId missing = ConversationId::generate();
    const AccountId missingPeer = AccountId::generate();
    QVERIFY(store
                ->commitHandshakeReceive(EnvelopeId::generate(), missingPeer, DeviceId::generate(),
                                         missing, QByteArray("welcome-missing"),
                                         QDateTime::currentMSecsSinceEpoch(), 1)
                .hasValue());

    // (2) An orphan stash for a now-Blocked peer: stash first (peer unknown), then
    // block, so the stash outlives the decision.
    const ConversationId blockedOrphan = ConversationId::generate();
    const AccountId blockedPeer = AccountId::generate();
    QVERIFY(store
                ->commitHandshakeReceive(EnvelopeId::generate(), blockedPeer, DeviceId::generate(),
                                         blockedOrphan, QByteArray("welcome-blocked"),
                                         QDateTime::currentMSecsSinceEpoch(), 2)
                .hasValue());
    QVERIFY(contacts->block(blockedPeer, QDateTime::currentMSecsSinceEpoch()).hasValue());

    // (3) An orphan stash for an already-Accepted peer.
    const ConversationId acceptedOrphan = ConversationId::generate();
    const AccountId acceptedPeer = AccountId::generate();
    QVERIFY(store
                ->commitHandshakeReceive(EnvelopeId::generate(), acceptedPeer, DeviceId::generate(),
                                         acceptedOrphan, QByteArray("welcome-accepted"),
                                         QDateTime::currentMSecsSinceEpoch(), 3)
                .hasValue());
    // Move the peer to Accepted (PendingIncoming -> Accepted) via a recorded request.
    const ContactRecord pending{acceptedPeer,     QString(),
                                QString(),        ContactState::PendingIncoming,
                                acceptedOrphan,   QDateTime::currentMSecsSinceEpoch(),
                                QDateTime::currentMSecsSinceEpoch()};
    QVERIFY(contacts->recordIncomingRequest(pending).hasValue());
    QVERIFY(contacts
                ->markAccepted(acceptedPeer, acceptedOrphan, QDateTime::currentMSecsSinceEpoch())
                .hasValue());

    ContactRequestService service(*m_session, *m_session->syncEngine());
    service.reconcileOnStartup();

    // (1) The missing PendingIncoming row was recreated from the durable stash.
    auto recreated = contacts->find(missingPeer);
    QVERIFY(recreated.hasValue());
    QVERIFY(recreated.value().has_value());
    QCOMPARE(recreated.value()->state, ContactState::PendingIncoming);
    // Its stash is retained -- the request is still pending a decision.
    auto missingStash = store->loadPendingHandshake(missing);
    QVERIFY(missingStash.hasValue());
    QVERIFY(missingStash.value().has_value());

    // (2) The blocked peer's orphan stash was dropped; the peer stays Blocked.
    auto blockedStash = store->loadPendingHandshake(blockedOrphan);
    QVERIFY(blockedStash.hasValue());
    QVERIFY(!blockedStash.value().has_value());
    auto blockedFound = contacts->find(blockedPeer);
    QVERIFY(blockedFound.hasValue());
    QVERIFY(blockedFound.value().has_value());
    QCOMPARE(blockedFound.value()->state, ContactState::Blocked);

    // (3) The accepted peer's orphan stash was dropped; the peer stays Accepted.
    auto acceptedStash = store->loadPendingHandshake(acceptedOrphan);
    QVERIFY(acceptedStash.hasValue());
    QVERIFY(!acceptedStash.value().has_value());
    auto acceptedFound = contacts->find(acceptedPeer);
    QVERIFY(acceptedFound.hasValue());
    QVERIFY(acceptedFound.value().has_value());
    QCOMPARE(acceptedFound.value()->state, ContactState::Accepted);
}

void ContactRequestServiceTest::authFailureDropsStashWithoutAccepting()
{
    ContactRequestService service(*m_session, *m_session->syncEngine());
    std::optional<AccountId> accepted;
    std::optional<AccountId> flagged;
    connect(&service, &ContactRequestService::contactAccepted, this,
            [&](const AccountId &a) { accepted = a; });
    connect(&service, &ContactRequestService::securityNotice, this,
            [&](const ConversationId &, const AccountId &a) { flagged = a; });

    // The relay claims a DIFFERENT sender device than the Welcome's credential
    // names: the accept must reject it before any join.
    const DeviceId forgedDevice = DeviceId::generate();
    QVERIFY(forgedDevice != m_senderDevice);
    const ConversationId conversation = feedInboundHandshake(forgedDevice);

    // The request still surfaced (recorded PendingIncoming) so the user could act.
    auto pending = m_session->contacts()->find(m_senderAccount);
    QVERIFY(pending.hasValue());
    QVERIFY(pending.value().has_value());
    QCOMPARE(pending.value()->state, ContactState::PendingIncoming);

    service.acceptContact(conversation);
    QCoreApplication::processEvents();

    // Authentication failed: never accepted, a security notice was raised, and the
    // peer was forgotten (treated as a decline).
    QVERIFY(!accepted.has_value());
    QVERIFY(flagged.has_value());
    QCOMPARE(flagged->bytes(), m_senderAccount.bytes());

    auto found = m_session->contacts()->find(m_senderAccount);
    QVERIFY(found.hasValue());
    QVERIFY(!found.value().has_value());

    // The stash is gone and no group was joined (the conversation never became a
    // known MLS group), so a sender application message cannot be processed.
    auto stash = m_session->syncStore()->loadPendingHandshake(conversation);
    QVERIFY(stash.hasValue());
    QVERIFY(!stash.value().has_value());
}

void ContactRequestServiceTest::acceptSendsContactAcceptAndOpensConversation()
{
    ContactRequestService service(*m_session, *m_session->syncEngine());
    std::optional<AccountId> accepted;
    connect(&service, &ContactRequestService::contactAccepted, this,
            [&](const AccountId &a) { accepted = a; });

    const ConversationId conversation = feedInboundHandshake();
    // The inbound request bound the sender device for later addressing.
    {
        auto pending = m_session->contacts()->find(m_senderAccount);
        QVERIFY(pending.hasValue() && pending.value().has_value());
        QVERIFY(pending.value()->peerDeviceId.has_value());
        QCOMPARE(pending.value()->peerDeviceId->bytes(), m_senderDevice.bytes());
    }

    service.acceptContact(conversation);
    QCoreApplication::processEvents();
    QVERIFY(accepted.has_value());

    // The accept shipped a ContactAccept control message to the requester's
    // device through the just-joined group (and nothing else left the device).
    QCOMPARE(m_transport->sent.size(), qsizetype(1));
    const CiphertextEnvelopeV1 &sent = m_transport->sent.first();
    QCOMPARE(sent.messageKind, EnvelopeMessageKind::ContactAccept);
    QCOMPARE(sent.recipientDeviceId.bytes(), m_senderDevice.bytes());
    QCOMPARE(sent.conversationId.bytes(), conversation.bytes());
    // The requester can decrypt it at the joined epoch: proof of membership.
    auto processed = m_sender->process(conversation, sent.ciphertext);
    QVERIFY(processed.hasValue());
    QCOMPARE(processed.value().kind, MlsProcessKind::Application);
    QCOMPARE(processed.value().applicationData, QByteArray("ACCEPT"));

    // The durable conversation row exists so messages can be committed.
    auto conversations = m_session->chats()->conversations();
    QVERIFY(conversations.hasValue());
    bool present = false;
    for (const ConversationRecord &record : conversations.value())
        present = present || record.id == conversation;
    QVERIFY(present);
}

void ContactRequestServiceTest::contactAcceptFromPeerPromotesOutgoingRequest()
{
    // Requester side: WE formed the group around the peer's KeyPackage and hold a
    // PendingOutgoing row (what AddContactService leaves behind). The peer joins
    // the Welcome and answers with a ContactAccept under the group ratchet.
    ContactRequestService service(*m_session, *m_session->syncEngine());
    std::optional<AccountId> accepted;
    connect(&service, &ContactRequestService::contactAccepted, this,
            [&](const AccountId &a) { accepted = a; });

    const ConversationId conversation = ConversationId::generate();
    auto peerKeyPackage = m_sender->generateKeyPackage();
    QVERIFY(peerKeyPackage.hasValue());
    QVERIFY(m_session->mls()->createGroup(conversation).hasValue());
    auto add = m_session->mls()->addMembers(conversation, {peerKeyPackage.value()});
    QVERIFY(add.hasValue());
    QVERIFY(m_session->persistMlsState().hasValue());
    QVERIFY(m_sender->joinGroup(conversation, add.value().welcome).hasValue());

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ContactRecord outgoing{m_senderAccount, QStringLiteral("peer"), QString(),
                           ContactState::PendingOutgoing, conversation, now, now};
    outgoing.peerDeviceId = m_senderDevice;
    QVERIFY(m_session->contacts()->recordOutgoingRequest(outgoing).hasValue());
    QVERIFY(m_session->chats()
                ->upsertConversation(ConversationRecord{conversation, conversation.bytes(),
                                                        QString(), ConversationKind::Direct, now})
                .hasValue());

    auto ciphertext = m_sender->encrypt(conversation, QByteArrayView("ACCEPT"));
    QVERIFY(ciphertext.hasValue());
    const CiphertextEnvelopeV1 envelope{
        1,
        EnvelopeId::generate(),
        m_senderAccount,
        m_senderDevice,
        DeviceId::generate(),
        conversation,
        EnvelopeMessageKind::ContactAccept,
        now,
        now + 3'600'000,
        EnvelopeId::generate(),
        ciphertext.value().bytes,
        QCryptographicHash::hash(ciphertext.value().bytes, QCryptographicHash::Sha256),
        QByteArray(64, '\x03')};
    m_session->syncEngine()->handleEnvelope(envelope, ++m_sequence);
    QCoreApplication::processEvents();

    // Promoted to Accepted, surfaced, acknowledged, and never shown as a message.
    QVERIFY(accepted.has_value());
    QCOMPARE(accepted->bytes(), m_senderAccount.bytes());
    auto found = m_session->contacts()->find(m_senderAccount);
    QVERIFY(found.hasValue() && found.value().has_value());
    QCOMPARE(found.value()->state, ContactState::Accepted);
    QCOMPARE(m_transport->acks.size(), qsizetype(1));
    auto history = m_session->chats()->messages(conversation, 10, std::nullopt);
    QVERIFY(history.hasValue());
    QVERIFY(history.value().isEmpty());

    // A redelivered accept is a no-op: still Accepted, no second signal.
    accepted.reset();
    m_session->syncEngine()->handleEnvelope(envelope, ++m_sequence);
    QCoreApplication::processEvents();
    QVERIFY(!accepted.has_value());
}

QTEST_GUILESS_MAIN(ContactRequestServiceTest)
#include "tst_contactrequestservice.moc"
