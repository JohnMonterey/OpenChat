#include "controllers/ContactController.h"

#include "app/ContactRequestService.h"
#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "domain/Contact.h"
#include "models/RequestListModel.h"
#include "network/SyncEngine.h"
#include "protocol/CiphertextEnvelope.h"
#include "security/KeyVault.h"
#include "security/SafetyNumber.h"
#include "security/SecureBuffer.h"
#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherDatabase.h"
#include "storage/SqlCipherSyncStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <memory>
#include <optional>

using namespace OpenChat;

namespace {

// Minimal in-memory KeyVault (no OS keychain in this environment). Mirrors the
// fake vault used by tst_contactrequestservice / tst_profilesession.
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

// A connected ciphertext transport that records acks; no sends leave on accept.
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
// version(1) || deviceId(16) || signingKey(32).
QByteArray credentialFor(const DeviceId &device)
{
    QByteArray credential;
    credential.append(char{1});
    credential.append(device.bytes());
    credential.append(QByteArray(32, 'k'));
    return credential;
}

// Groups a raw 60-digit safety number into 12 space-separated groups of five,
// matching the controller's on-screen formatting, so the tests can compare against
// an independently computed number.
QString groupedSafetyNumber(const QString &raw)
{
    QString grouped;
    for (qsizetype offset = 0; offset < raw.size(); offset += 5) {
        if (!grouped.isEmpty())
            grouped += QLatin1Char(' ');
        grouped += raw.mid(offset, 5);
    }
    return grouped;
}

} // namespace

class ContactControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // Mock mode (no services): the --capture surface.
    void previewEnablesAndSeedsMockRequests();
    void mockAddResolvesToSuccess();
    void addByInviteRejectsUndecodableCode();
    void inviteRoundTripsBase64Url();
    void mockAcceptDeclineBlockRemoveRow();
    void openResetsStatusAndTogglesDialog();
    void mockSafetyNumberReflectsPresetAndToggles();
    void mockLookupFindsSeededHandleAndSendsRequest();

    // Live services (real session + engine + request service).
    void liveIncomingRequestAppearsThenAcceptClearsIt();
    void liveForgedRequestSurfacesSecurityNotice();
    void liveDeclineAndBlockRemoveRows();
    void liveSeedsPreexistingPendingRoster();
    void liveOpenSafetyNumberComputesAndVerifies();
    void liveOpenSafetyNumberUnavailableWithoutPeerKey();
    void liveAcceptOpensSafetyNumber();

private:
    // A genuine inbound handshake (see tst_contactrequestservice): our KeyPackage is
    // added to a group the sender forms, producing a real Welcome sealed to us.
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

void ContactControllerTest::init()
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

void ContactControllerTest::cleanup()
{
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

// ---- Mock mode ----------------------------------------------------------------

void ContactControllerTest::mockLookupFindsSeededHandleAndSendsRequest()
{
    ContactController controller;
    controller.enableForPreview();
    controller.setMockDirectory({QStringLiteral("alice")});
    QSignalSpy lookupSpy(&controller, &ContactController::lookupChanged);

    // Nothing to look up: idle and hidden.
    controller.lookup(QStringLiteral("   "));
    QCOMPARE(controller.lookupState(), ContactController::LookupState::Idle);
    QVERIFY(!controller.lookupVisible());

    // A handle (with a leading '@' and padding) is normalised, shows the row as
    // Searching immediately, then resolves after the debounce.
    controller.lookup(QStringLiteral(" @alice "));
    QCOMPARE(controller.lookupHandle(), QStringLiteral("alice"));
    QCOMPARE(controller.lookupState(), ContactController::LookupState::Searching);
    QVERIFY(controller.lookupVisible());
    QVERIFY(!controller.lookupCanRequest());
    QTRY_COMPARE(controller.lookupState(), ContactController::LookupState::Found);
    QVERIFY(controller.lookupCanRequest());
    QVERIFY(!controller.lookupSubtitle().isEmpty());

    // Sending from the row flips it to RequestSent (mock add resolves at once).
    controller.requestLookup();
    QCOMPARE(controller.lookupState(), ContactController::LookupState::RequestSent);
    QVERIFY(controller.lookupVisible());
    QVERIFY(!controller.lookupCanRequest());
    QCOMPARE(controller.status(), ContactController::Status::Success);

    // An unknown handle resolves to NotFound (still visible, no affordance); text
    // with inner whitespace can never be a handle and hides the row.
    controller.lookup(QStringLiteral("nobody"));
    QTRY_COMPARE(controller.lookupState(), ContactController::LookupState::NotFound);
    QVERIFY(controller.lookupVisible());
    QVERIFY(!controller.lookupCanRequest());
    controller.lookup(QStringLiteral("two words"));
    QCOMPARE(controller.lookupState(), ContactController::LookupState::Idle);
    QVERIFY(!controller.lookupVisible());
    QVERIFY(lookupSpy.count() >= 5);
}

void ContactControllerTest::previewEnablesAndSeedsMockRequests()
{
    ContactController controller;
    QVERIFY(!controller.enabled());

    controller.enableForPreview();
    QVERIFY(controller.enabled());

    controller.addMockRequest(QStringLiteral("New contact request"), QStringLiteral("ID abc123"));
    controller.addMockRequest(QStringLiteral("New contact request"), QStringLiteral("ID def456"));

    RequestListModel *model = controller.requests();
    QCOMPARE(model->count(), 2);
    const QModelIndex first = model->index(0);
    QCOMPARE(model->data(first, RequestListModel::NameRole).toString(),
             QStringLiteral("New contact request"));
    QCOMPARE(model->data(first, RequestListModel::SubtitleRole).toString(),
             QStringLiteral("ID abc123"));
    // requestId is a non-empty ConversationId hex that round-trips.
    const QString requestId = model->data(first, RequestListModel::IdRole).toString();
    QVERIFY(!requestId.isEmpty());
    QVERIFY(ConversationId::fromBytes(QByteArray::fromHex(requestId.toLatin1())).has_value());
}

void ContactControllerTest::mockAddResolvesToSuccess()
{
    ContactController controller;
    QSignalSpy statusSpy(&controller, &ContactController::statusChanged);

    controller.addByHandle(QStringLiteral("alice"));
    // Working then Success are both emitted; the terminal state is Success.
    QVERIFY(statusSpy.count() >= 1);
    QCOMPARE(controller.status(), ContactController::Status::Success);
    QVERIFY(!controller.statusMessage().isEmpty());
}

void ContactControllerTest::addByInviteRejectsUndecodableCode()
{
    ContactController controller;
    controller.addByInvite(QString());
    QCOMPARE(controller.status(), ContactController::Status::Error);
}

void ContactControllerTest::inviteRoundTripsBase64Url()
{
    // A token as the relay would mint it survives base64url encode -> decode, so
    // addByInvite treats it as a valid (non-empty) code and, in mock mode, succeeds.
    const QByteArray token = QByteArray(24, '\x7f') + QByteArray("\x00\xff\x10", 3);
    const QString encoded = QString::fromLatin1(
        token.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    const QByteArray decoded = QByteArray::fromBase64(
        encoded.toUtf8(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    QCOMPARE(decoded, token);

    ContactController controller;
    controller.addByInvite(encoded);
    QCOMPARE(controller.status(), ContactController::Status::Success);

    // setMockInvite exposes a copyable invite string and flips inviteReady.
    controller.setMockInvite(QStringLiteral("OPENCHAT-INV-9F3K"));
    QVERIFY(controller.inviteReady());
    QCOMPARE(controller.myInvite(), QStringLiteral("OPENCHAT-INV-9F3K"));
}

void ContactControllerTest::mockAcceptDeclineBlockRemoveRow()
{
    ContactController controller;
    controller.enableForPreview();
    controller.addMockRequest(QStringLiteral("New contact request"), QStringLiteral("ID one"));
    controller.addMockRequest(QStringLiteral("New contact request"), QStringLiteral("ID two"));
    controller.addMockRequest(QStringLiteral("New contact request"), QStringLiteral("ID three"));
    RequestListModel *model = controller.requests();
    QCOMPARE(model->count(), 3);

    const QString acceptId = model->data(model->index(0), RequestListModel::IdRole).toString();
    controller.accept(acceptId);
    QCOMPARE(model->count(), 2);
    QCOMPARE(controller.status(), ContactController::Status::Success);

    const QString declineId = model->data(model->index(0), RequestListModel::IdRole).toString();
    controller.decline(declineId);
    QCOMPARE(model->count(), 1);

    const QString blockId = model->data(model->index(0), RequestListModel::IdRole).toString();
    controller.block(blockId);
    QCOMPARE(model->count(), 0);
}

void ContactControllerTest::openResetsStatusAndTogglesDialog()
{
    ContactController controller;
    controller.addByInvite(QString()); // drive status to Error first
    QCOMPARE(controller.status(), ContactController::Status::Error);

    QVERIFY(!controller.dialogOpen());
    controller.openDialog();
    QVERIFY(controller.dialogOpen());
    QCOMPARE(controller.status(), ContactController::Status::Idle);
    QVERIFY(controller.statusMessage().isEmpty());

    controller.closeDialog();
    QVERIFY(!controller.dialogOpen());
}

void ContactControllerTest::mockSafetyNumberReflectsPresetAndToggles()
{
    ContactController controller;
    QSignalSpy spy(&controller, &ContactController::safetyNumberChanged);
    QVERIFY(!controller.safetyNumberOpen());

    const QString preset =
        QStringLiteral("11111 22222 33333 44444 55555 66666 77777 88888 99999 00000 "
                       "12121 34343");
    controller.setMockSafetyNumber(preset, /*verified*/ false, QStringLiteral("@ada"));
    QVERIFY(spy.count() >= 1);
    QCOMPARE(controller.safetyNumber(), preset);
    QCOMPARE(controller.safetyNumberContact(), QStringLiteral("@ada"));
    QVERIFY(!controller.safetyNumberVerified());

    controller.openSafetyNumberPreview();
    QVERIFY(controller.safetyNumberOpen());

    // Mock markVerified() flips the flag (no session behind it, nothing persisted).
    controller.markVerified();
    QVERIFY(controller.safetyNumberVerified());

    controller.closeSafetyNumber();
    QVERIFY(!controller.safetyNumberOpen());
    // The number and label survive a close; only the open flag cleared.
    QCOMPARE(controller.safetyNumber(), preset);
}

// ---- Live services ------------------------------------------------------------

void ContactControllerTest::liveIncomingRequestAppearsThenAcceptClearsIt()
{
    ContactRequestService requestsSvc(*m_session, *m_session->syncEngine());
    ContactController controller;
    controller.setLiveServices(&requestsSvc, nullptr, m_session.get(), m_session->syncEngine());
    QVERIFY(controller.enabled());

    const ConversationId conversation = feedInboundHandshake();

    RequestListModel *model = controller.requests();
    QCOMPARE(model->count(), 1);
    QCOMPARE(model->data(model->index(0), RequestListModel::AccountIdRole).toString(),
             m_senderAccount.toHex());

    controller.accept(conversation.toHex());
    QCoreApplication::processEvents();

    // The engine authenticated + joined; contactAccepted removed the row.
    QCOMPARE(model->count(), 0);
    QCOMPARE(controller.status(), ContactController::Status::Success);
    auto found = m_session->contacts()->find(m_senderAccount);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QCOMPARE(found.value()->state, ContactState::Accepted);
}

void ContactControllerTest::liveForgedRequestSurfacesSecurityNotice()
{
    ContactRequestService requestsSvc(*m_session, *m_session->syncEngine());
    ContactController controller;
    controller.setLiveServices(&requestsSvc, nullptr, m_session.get(), m_session->syncEngine());

    // The relay claims a different sender device than the Welcome's credential names.
    const DeviceId forged = DeviceId::generate();
    QVERIFY(forged != m_senderDevice);
    const ConversationId conversation = feedInboundHandshake(forged);

    RequestListModel *model = controller.requests();
    QCOMPARE(model->count(), 1);

    controller.accept(conversation.toHex());
    QCoreApplication::processEvents();

    // Authentication failed: the row was removed and an error surfaced; the contact
    // was never Accepted.
    QCOMPARE(model->count(), 0);
    QCOMPARE(controller.status(), ContactController::Status::Error);
    auto found = m_session->contacts()->find(m_senderAccount);
    QVERIFY(found.hasValue());
    QVERIFY(!found.value().has_value());
}

void ContactControllerTest::liveDeclineAndBlockRemoveRows()
{
    ContactRequestService requestsSvc(*m_session, *m_session->syncEngine());
    ContactController controller;
    controller.setLiveServices(&requestsSvc, nullptr, m_session.get(), m_session->syncEngine());
    RequestListModel *model = controller.requests();

    const ConversationId declined = feedInboundHandshake();
    QCOMPARE(model->count(), 1);
    controller.decline(declined.toHex());
    QCOMPARE(model->count(), 0);
    auto declinedFound = m_session->contacts()->find(m_senderAccount);
    QVERIFY(declinedFound.hasValue());
    QVERIFY(!declinedFound.value().has_value());

    // A second inbound from a different sender, then block it.
    m_senderAccount = AccountId::generate();
    m_senderDevice = DeviceId::generate();
    m_senderCapture = std::make_unique<CapturingMlsStateStore>(*m_senderDb, ProfileId::generate());
    auto senderResult = MlsClient::create(credentialFor(m_senderDevice), m_senderCapture.get());
    QVERIFY(senderResult.hasValue());
    m_sender = std::move(senderResult).value();

    const ConversationId blocked = feedInboundHandshake();
    QCOMPARE(model->count(), 1);
    controller.block(blocked.toHex());
    QCOMPARE(model->count(), 0);
    auto blockedFound = m_session->contacts()->find(m_senderAccount);
    QVERIFY(blockedFound.hasValue());
    QVERIFY(blockedFound.value().has_value());
    QCOMPARE(blockedFound.value()->state, ContactState::Blocked);
}

void ContactControllerTest::liveSeedsPreexistingPendingRoster()
{
    // A PendingIncoming row recorded in a prior session (before the controller is
    // wired) must be seeded into the model when the live services are installed.
    const ConversationId conversation = ConversationId::generate();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const ContactRecord pending{m_senderAccount, QStringLiteral("earlier"),
                                QString(),        ContactState::PendingIncoming,
                                conversation,     now,
                                now};
    QVERIFY(m_session->contacts()->recordIncomingRequest(pending).hasValue());

    ContactRequestService requestsSvc(*m_session, *m_session->syncEngine());
    ContactController controller;
    controller.setLiveServices(&requestsSvc, nullptr, m_session.get(), m_session->syncEngine());

    RequestListModel *model = controller.requests();
    QCOMPARE(model->count(), 1);
    QCOMPARE(model->data(model->index(0), RequestListModel::IdRole).toString(),
             conversation.toHex());
    QCOMPARE(model->data(model->index(0), RequestListModel::AccountIdRole).toString(),
             m_senderAccount.toHex());
}

void ContactControllerTest::liveOpenSafetyNumberComputesAndVerifies()
{
    ContactController controller;
    controller.setLiveServices(nullptr, nullptr, m_session.get(), m_session->syncEngine());

    // Seed an Accepted contact with a known peer signing key directly through the
    // repository (the accept path binds the key the same way in production).
    SqlCipherContactRepository *contacts = m_session->contacts();
    const AccountId peer = AccountId::generate();
    const ConversationId conversation = ConversationId::generate();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QByteArray peerKey(32, '\x5a');
    const ContactRecord pending{peer,         QString(), QString(), ContactState::PendingIncoming,
                                conversation, now,       now};
    QVERIFY(contacts->recordIncomingRequest(pending).hasValue());
    QVERIFY(contacts->markAccepted(peer, conversation, now).hasValue());
    QVERIFY(contacts->setPeerSigningKey(peer, peerKey).hasValue());

    controller.openSafetyNumber(peer.toHex());
    QVERIFY(controller.safetyNumberOpen());

    // Independently compute the expected number from our identity and the seeded
    // peer key, then compare against the controller's formatted surface.
    const auto credential = m_session->publicCredential();
    const auto ourAccount = m_session->accountId();
    QVERIFY(credential.hasValue());
    QVERIFY(ourAccount.hasValue());
    auto expected = computeSafetyNumber(credential.value().signingPublicKey,
                                        ourAccount.value().bytes(), peerKey, peer.bytes());
    QVERIFY(expected.hasValue());
    QVERIFY(!controller.safetyNumber().isEmpty());
    QCOMPARE(controller.safetyNumber(), groupedSafetyNumber(expected.value()));
    // Twelve space-separated groups of five (60 digits + 11 spaces).
    QCOMPARE(controller.safetyNumber().size(), qsizetype(71));
    QCOMPARE(controller.safetyNumberContact(),
             QStringLiteral("ID ") + peer.toHex().left(10));
    QVERIFY(!controller.safetyNumberVerified());

    // markVerified() persists through setVerified and reflects on the surface.
    controller.markVerified();
    QVERIFY(controller.safetyNumberVerified());
    auto found = contacts->find(peer);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QVERIFY(found.value()->verified);
}

void ContactControllerTest::liveOpenSafetyNumberUnavailableWithoutPeerKey()
{
    ContactController controller;
    controller.setLiveServices(nullptr, nullptr, m_session.get(), m_session->syncEngine());

    // An Accepted contact whose peer signing key is not yet known: the dialog opens
    // with an empty number so it can show the "unavailable" message.
    SqlCipherContactRepository *contacts = m_session->contacts();
    const AccountId peer = AccountId::generate();
    const ConversationId conversation = ConversationId::generate();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const ContactRecord pending{peer,         QString(), QString(), ContactState::PendingIncoming,
                                conversation, now,       now};
    QVERIFY(contacts->recordIncomingRequest(pending).hasValue());
    QVERIFY(contacts->markAccepted(peer, conversation, now).hasValue());

    controller.openSafetyNumber(peer.toHex());
    QVERIFY(controller.safetyNumberOpen());
    QVERIFY(controller.safetyNumber().isEmpty());
    QCOMPARE(controller.safetyNumberContact(),
             QStringLiteral("ID ") + peer.toHex().left(10));
}

void ContactControllerTest::liveAcceptOpensSafetyNumber()
{
    ContactRequestService requestsSvc(*m_session, *m_session->syncEngine());
    ContactController controller;
    controller.setLiveServices(&requestsSvc, nullptr, m_session.get(), m_session->syncEngine());

    const ConversationId conversation = feedInboundHandshake();
    QCOMPARE(controller.requests()->count(), 1);

    controller.accept(conversation.toHex());
    QCoreApplication::processEvents();

    // contactAccepted fired: the row cleared, the contact is Accepted with its peer
    // key bound, and the safety number opened at the natural verify moment.
    QCOMPARE(controller.requests()->count(), 0);
    QCOMPARE(controller.status(), ContactController::Status::Success);
    QVERIFY(controller.safetyNumberOpen());

    auto found = m_session->contacts()->find(m_senderAccount);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QCOMPARE(found.value()->state, ContactState::Accepted);
    QVERIFY(found.value()->peerSigningKey.has_value());

    // The opened number equals the independently computed value for the just-
    // accepted peer.
    const auto credential = m_session->publicCredential();
    const auto ourAccount = m_session->accountId();
    QVERIFY(credential.hasValue());
    QVERIFY(ourAccount.hasValue());
    auto expected = computeSafetyNumber(credential.value().signingPublicKey,
                                        ourAccount.value().bytes(),
                                        *found.value()->peerSigningKey, m_senderAccount.bytes());
    QVERIFY(expected.hasValue());
    QVERIFY(!controller.safetyNumber().isEmpty());
    QCOMPARE(controller.safetyNumber(), groupedSafetyNumber(expected.value()));
}

QTEST_GUILESS_MAIN(ContactControllerTest)
#include "tst_contactcontroller.moc"
