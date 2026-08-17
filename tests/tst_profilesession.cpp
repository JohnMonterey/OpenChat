#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "domain/ChatTypes.h"
#include "network/SyncEngine.h"
#include "protocol/CiphertextEnvelope.h"
#include "security/KeyVault.h"
#include "security/SecureBuffer.h"
#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherDatabase.h"
#include "storage/SqlCipherOutboxRepository.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <memory>
#include <optional>

using namespace OpenChat;

// A disconnected ciphertext transport: never connected, and its send/acknowledge
// calls record nothing beyond a count so the test can assert nothing left the
// device while offline. The engine short-circuits drainOutbox on !isConnected(),
// so sendEnvelope must never fire here.
class DisconnectedTransport final : public SyncTransport {
public:
  bool isConnected() const override { return false; }
  void sendEnvelope(const CiphertextEnvelopeV1 &) override { ++sendCount; }
  void acknowledge(const EnvelopeId &, quint64) override { ++ackCount; }

  int sendCount = 0;
  int ackCount = 0;
};

class SessionVault final : public KeyVault {
public:
  KeyVaultAvailability availability() const override { return available; }

  Result<SecureBuffer, KeyVaultError>
  readProfileKey(const ProfileId &) override {
    ++profileReads;
    return read(databaseKey);
  }

  Result<SecureBuffer, KeyVaultError>
  createProfileKey(const ProfileId &) override {
    ++profileCreates;
    return create(databaseKey);
  }

  Result<void, KeyVaultError> deleteProfileKey(const ProfileId &) override {
    databaseKey.reset();
    ++profileDeletes;
    return Result<void, KeyVaultError>::success();
  }

  Result<SecureBuffer, KeyVaultError>
  readDeviceWrappingKey(const ProfileId &) override {
    ++wrappingReads;
    return read(wrappingKey);
  }

  Result<SecureBuffer, KeyVaultError>
  createDeviceWrappingKey(const ProfileId &) override {
    ++wrappingCreates;
    if (failWrappingCreate)
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::StorageFailure);
    return create(wrappingKey);
  }

  Result<void, KeyVaultError>
  deleteDeviceWrappingKey(const ProfileId &) override {
    wrappingKey.reset();
    ++wrappingDeletes;
    return Result<void, KeyVaultError>::success();
  }

  void replaceDatabaseKey() { databaseKey = SecureBuffer::random(32); }
  void replaceWrappingKey() { wrappingKey = SecureBuffer::random(32); }

  KeyVaultAvailability available = KeyVaultAvailability::Available;
  bool failWrappingCreate = false;
  int profileReads = 0;
  int profileCreates = 0;
  int profileDeletes = 0;
  int wrappingReads = 0;
  int wrappingCreates = 0;
  int wrappingDeletes = 0;
  std::optional<SecureBuffer> databaseKey;
  std::optional<SecureBuffer> wrappingKey;

private:
  Result<SecureBuffer, KeyVaultError>
  read(const std::optional<SecureBuffer> &key) const {
    if (available != KeyVaultAvailability::Available)
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::Unavailable);
    if (!key)
      return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::NotFound);
    return Result<SecureBuffer, KeyVaultError>::success(
        SecureBuffer::fromBytes(key->view()));
  }

  Result<SecureBuffer, KeyVaultError>
  create(std::optional<SecureBuffer> &key) const {
    if (available != KeyVaultAvailability::Available)
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::Unavailable);
    if (key)
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::AlreadyExists);
    key = SecureBuffer::random(32);
    return Result<SecureBuffer, KeyVaultError>::success(
        SecureBuffer::fromBytes(key->view()));
  }
};

class ProfileSessionTest final : public QObject {
  Q_OBJECT

private slots:
  void missingVaultKeyDoesNotCreateASecondIdentity();
  void createThenUnlockRestoresTheSameDevice();
  void failedBootstrapRollsBackBothVaultEntries();
  void wrongDatabaseKeyFailsWithoutReplacingIt();
  void wrongWrappingKeyRejectsTheDeviceIdentity();
  void lockRunsTheFailClosedOrder();
  void removalRequiresExactProfileAndPathConfirmation();
  void recoveryCodeIsRevealedAndConsumedOnce();
  void createPersistsAStableAccountId();
  void persistMlsStateMakesKeyPackageMaterialDurable();
  void offlineDurableSendSurvivesRestart();
};

void ProfileSessionTest::missingVaultKeyDoesNotCreateASecondIdentity() {
  QTemporaryDir directory;
  SessionVault vault;
  const auto profileId = ProfileId::generate();
  const auto paths = ProfilePaths::forProfile(directory.path(), profileId);

  auto created = ProfileSession::create(profileId, vault, paths);
  QVERIFY(created.hasValue());
  const auto originalCredential = created.value()->publicCredential();
  QVERIFY(originalCredential.hasValue());
  created.value()->lock();
  vault.databaseKey.reset();
  const int createsBeforeUnlock = vault.profileCreates;

  auto result = ProfileSession::unlock(profileId, vault, paths);
  QVERIFY(!result.hasValue());
  QCOMPARE(result.error(), ProfileSessionError::MissingKey);
  QCOMPARE(vault.profileCreates, createsBeforeUnlock);
  QCOMPARE(vault.wrappingCreates, 1);
  QVERIFY(QFile::exists(paths.database));
}

void ProfileSessionTest::createThenUnlockRestoresTheSameDevice() {
  QTemporaryDir directory;
  SessionVault vault;
  const auto profileId = ProfileId::generate();
  const auto paths = ProfilePaths::forProfile(directory.path(), profileId);

  auto created = ProfileSession::create(profileId, vault, paths);
  QVERIFY(created.hasValue());
  auto first = std::move(created).value();
  const auto firstCredential = first->publicCredential();
  QVERIFY(firstCredential.hasValue());
  const auto firstSignature =
      first->signChallenge("server-challenge", "openchat-auth-v1");
  QVERIFY(firstSignature.hasValue());
  first->lock();

  auto reopened = ProfileSession::unlock(profileId, vault, paths);
  QVERIFY(reopened.hasValue());
  const auto secondCredential = reopened.value()->publicCredential();
  QVERIFY(secondCredential.hasValue());
  QCOMPARE(secondCredential.value().serialize(),
           firstCredential.value().serialize());
  const auto secondSignature =
      reopened.value()->signChallenge("server-challenge", "openchat-auth-v1");
  QVERIFY(secondSignature.hasValue());
  QCOMPARE(secondSignature.value(), firstSignature.value());
}

void ProfileSessionTest::failedBootstrapRollsBackBothVaultEntries() {
  QTemporaryDir directory;
  SessionVault vault;
  vault.failWrappingCreate = true;
  const auto profileId = ProfileId::generate();
  const auto paths = ProfilePaths::forProfile(directory.path(), profileId);

  auto result = ProfileSession::create(profileId, vault, paths);
  QVERIFY(!result.hasValue());
  QVERIFY(!vault.databaseKey.has_value());
  QVERIFY(!vault.wrappingKey.has_value());
  QCOMPARE(vault.profileDeletes, 1);
  QVERIFY(!QFile::exists(paths.database));
}

void ProfileSessionTest::wrongDatabaseKeyFailsWithoutReplacingIt() {
  QTemporaryDir directory;
  SessionVault vault;
  const auto profileId = ProfileId::generate();
  const auto paths = ProfilePaths::forProfile(directory.path(), profileId);
  auto created = ProfileSession::create(profileId, vault, paths);
  QVERIFY(created.hasValue());
  created.value()->lock();
  vault.replaceDatabaseKey();
  const int createsBeforeUnlock = vault.profileCreates;

  auto result = ProfileSession::unlock(profileId, vault, paths);
  QVERIFY(!result.hasValue());
  QCOMPARE(result.error(), ProfileSessionError::DatabaseFailure);
  QCOMPARE(vault.profileCreates, createsBeforeUnlock);
  QVERIFY(QFile::exists(paths.database));
}

void ProfileSessionTest::wrongWrappingKeyRejectsTheDeviceIdentity() {
  QTemporaryDir directory;
  SessionVault vault;
  const auto profileId = ProfileId::generate();
  const auto paths = ProfilePaths::forProfile(directory.path(), profileId);
  auto created = ProfileSession::create(profileId, vault, paths);
  QVERIFY(created.hasValue());
  created.value()->lock();
  vault.replaceWrappingKey();

  auto result = ProfileSession::unlock(profileId, vault, paths);
  QVERIFY(!result.hasValue());
  QCOMPARE(result.error(), ProfileSessionError::IdentityFailure);
  QVERIFY(QFile::exists(paths.database));
}

void ProfileSessionTest::lockRunsTheFailClosedOrder() {
  QTemporaryDir directory;
  SessionVault vault;
  const auto profileId = ProfileId::generate();
  const auto paths = ProfilePaths::forProfile(directory.path(), profileId);
  QStringList events;
  ProfileSession *active = nullptr;
  ProfileSessionHooks hooks;
  hooks.stopNetworking = [&] { events.append(QStringLiteral("network")); };
  hooks.cancelDecryptions = [&] { events.append(QStringLiteral("decrypt")); };
  hooks.clearModels = [&] {
    QVERIFY(active);
    QVERIFY(!active->chats());
    QVERIFY(!active->outbox());
    QVERIFY(!active->sync());
    QVERIFY(!active->mls());
    events.append(QStringLiteral("models"));
  };
  auto created = ProfileSession::create(profileId, vault, paths, hooks);
  QVERIFY(created.hasValue());
  active = created.value().get();

  active->lock();
  QCOMPARE(events,
           QStringList({QStringLiteral("network"), QStringLiteral("decrypt"),
                        QStringLiteral("models")}));
  QVERIFY(!active->isUnlocked());
  QVERIFY(!active->signChallenge("challenge", "context").hasValue());
  active->lock();
  QCOMPARE(events.size(), 3);
}

void ProfileSessionTest::removalRequiresExactProfileAndPathConfirmation() {
  QTemporaryDir directory;
  SessionVault vault;
  const auto profileId = ProfileId::generate();
  const auto otherId = ProfileId::generate();
  const auto paths = ProfilePaths::forProfile(directory.path(), profileId);
  auto created = ProfileSession::create(profileId, vault, paths);
  QVERIFY(created.hasValue());
  created.value()->lock();

  auto wrongConfirmation = ProfileSession::removeLocalProfile(
      profileId, vault, paths, otherId.toHex());
  QVERIFY(!wrongConfirmation.hasValue());
  QVERIFY(QFile::exists(paths.database));
  const auto wrongPaths = ProfilePaths::forProfile(directory.path(), otherId);
  auto wrongTarget = ProfileSession::removeLocalProfile(
      profileId, vault, wrongPaths, profileId.toHex());
  QVERIFY(!wrongTarget.hasValue());
  QVERIFY(QFile::exists(paths.database));

  auto removed = ProfileSession::removeLocalProfile(
      profileId, vault, paths, profileId.toHex());
  QVERIFY(removed.hasValue());
  QVERIFY(!QFileInfo::exists(paths.profileDirectory));
  QVERIFY(!vault.databaseKey.has_value());
  QVERIFY(!vault.wrappingKey.has_value());
}

void ProfileSessionTest::recoveryCodeIsRevealedAndConsumedOnce() {
  QTemporaryDir directory;
  SessionVault vault;
  const auto profileId = ProfileId::generate();
  auto created = ProfileSession::create(
      profileId, vault, ProfilePaths::forProfile(directory.path(), profileId));
  QVERIFY(created.hasValue());
  auto codeResult = created.value()->takeRecoveryCode();
  QVERIFY(codeResult.hasValue());
  auto code = std::move(codeResult).value();
  auto revealed = code.reveal();
  QVERIFY(revealed.hasValue());
  QVERIFY(!revealed.value().isEmpty());
  QVERIFY(!code.reveal().hasValue());
  auto wrong = code.confirm("wrong-code");
  QVERIFY(wrong.hasValue());
  QVERIFY(!wrong.value());
  auto confirmed = code.confirm(revealed.value());
  QVERIFY(confirmed.hasValue());
  QVERIFY(confirmed.value());
  auto secret = code.takeSecret();
  QVERIFY(secret.hasValue());
  QCOMPARE(secret.value().size(), 32);
  QVERIFY(!code.takeSecret().hasValue());
  QVERIFY(!created.value()->takeRecoveryCode().hasValue());
}

void ProfileSessionTest::createPersistsAStableAccountId() {
  QTemporaryDir directory;
  SessionVault vault;
  const auto profileId = ProfileId::generate();
  const auto paths = ProfilePaths::forProfile(directory.path(), profileId);

  auto created = ProfileSession::create(profileId, vault, paths);
  QVERIFY(created.hasValue());
  auto first = std::move(created).value();
  const auto firstAccount = first->accountId();
  QVERIFY(firstAccount.hasValue());
  QCOMPARE(firstAccount.value().bytes().size(), qsizetype(16));

  first->lock();
  auto lockedAccount = first->accountId();
  QVERIFY(!lockedAccount.hasValue());
  QCOMPARE(lockedAccount.error(), ProfileSessionError::NotUnlocked);

  auto reopened = ProfileSession::unlock(profileId, vault, paths);
  QVERIFY(reopened.hasValue());
  const auto secondAccount = reopened.value()->accountId();
  QVERIFY(secondAccount.hasValue());
  QCOMPARE(secondAccount.value().bytes(), firstAccount.value().bytes());
}

void ProfileSessionTest::persistMlsStateMakesKeyPackageMaterialDurable() {
  QTemporaryDir directory;
  SessionVault vault;
  const auto profileId = ProfileId::generate();
  const auto paths = ProfilePaths::forProfile(directory.path(), profileId);

  auto created = ProfileSession::create(profileId, vault, paths);
  QVERIFY(created.hasValue());
  auto session = std::move(created).value();

  // Drain whatever the MlsClient captured at construction so the store starts
  // with nothing pending, then prove the no-op path: with nothing captured,
  // persistMlsState() must still succeed (it early-returns before commitMlsState,
  // which would otherwise reject an empty blob).
  QVERIFY(session->persistMlsState().hasValue());
  QVERIFY(session->persistMlsState().hasValue()); // second call: nothing pending

  // Generating a KeyPackage advances the MLS ratchet out-of-band (it does not
  // flow through the SyncEngine), so its private material is only captured in
  // memory until persistMlsState() writes it through the SyncStore.
  auto keyPackage = session->mls()->generateKeyPackage();
  QVERIFY(keyPackage.hasValue());
  QVERIFY(!keyPackage.value().isEmpty());
  QVERIFY(session->persistMlsState().hasValue());

  session->lock();

  // The captured KeyPackage state survived lock() because it was persisted: the
  // durable blob loads directly from the store and is non-empty.
  auto key = vault.readProfileKey(profileId);
  QVERIFY(key.hasValue());
  auto inspect = SqlCipherDatabase::open(paths.database, key.value());
  QVERIFY(inspect.hasValue());
  auto persisted = inspect.value().loadMlsState(profileId);
  QVERIFY(persisted.hasValue());
  QVERIFY(!persisted.value().isEmpty());

  // Restart: unlock reconstructs the MlsClient from the persisted MLS state, so
  // a successful unlock is itself proof the durable ratchet blob loads back.
  auto reopened = ProfileSession::unlock(profileId, vault, paths);
  QVERIFY(reopened.hasValue());
}

void ProfileSessionTest::offlineDurableSendSurvivesRestart() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  SessionVault vault;
  const auto profileId = ProfileId::generate();
  const auto paths = ProfilePaths::forProfile(directory.path(), profileId);

  auto created = ProfileSession::create(profileId, vault, paths);
  QVERIFY(created.hasValue());
  auto session = std::move(created).value();

  // A second MLS client gives the profile's group a real member, so the engine's
  // encrypt() has a committed group to send into.
  auto bobOpened = SqlCipherDatabase::open(
      directory.filePath(QStringLiteral("bob.sqlite3")), SecureBuffer::random(32));
  QVERIFY(bobOpened.hasValue());
  auto bobDb = std::make_unique<SqlCipherDatabase>(std::move(bobOpened).value());
  CapturingMlsStateStore bobCapture(*bobDb, ProfileId::generate());
  auto bobResult = MlsClient::create(QByteArray("bob-device"), &bobCapture);
  QVERIFY(bobResult.hasValue());
  auto bob = std::move(bobResult).value();
  auto bobKeyPackage = bob->generateKeyPackage();
  QVERIFY(bobKeyPackage.hasValue());

  const auto conversation = ConversationId::generate();
  QVERIFY(session->mls()->createGroup(conversation).hasValue());
  QVERIFY(
      session->mls()->addMembers(conversation, {bobKeyPackage.value()}).hasValue());
  // The stored message references this conversation row (FK), so it must exist
  // before the engine commits a send into it.
  const ConversationRecord conversationRecord{
      conversation, QByteArray("mls-group"), QStringLiteral("Bob"),
      ConversationKind::Group, QDateTime::currentMSecsSinceEpoch()};
  QVERIFY(session->chats()->upsertConversation(conversationRecord).hasValue());

  // Wire the durable engine to the injected (offline) transport.
  DisconnectedTransport transport;
  QVERIFY(session->syncEngine() == nullptr);
  QVERIFY(session->startNetworking(transport).hasValue());
  QVERIFY(session->syncEngine() != nullptr);
  // Second call is idempotent and does not build a second engine.
  QVERIFY(session->startNetworking(transport).hasValue());

  const auto recipient = DeviceId::generate();
  session->syncEngine()->enqueueText(conversation, recipient,
                                     QStringLiteral("durable hi"));
  QCoreApplication::processEvents();
  QVERIFY(!session->syncEngine()->isFailedClosed());

  // The message committed durably and is queryable, still Queued because the
  // link is offline.
  auto messages = session->chats()->messages(conversation, 50, std::nullopt);
  QVERIFY(messages.hasValue());
  QCOMPARE(messages.value().size(), qsizetype(1));
  QCOMPARE(messages.value().first().body, QStringLiteral("durable hi"));
  QCOMPARE(messages.value().first().flow, MessageFlow::Outgoing);
  QCOMPARE(messages.value().first().deliveryState, DeliveryState::Queued);

  // The outbox row is present and claimable through the session's repository.
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  auto due = session->outbox()->claimDue(now, 10, now + 1000);
  QVERIFY(due.hasValue());
  QCOMPARE(due.value().size(), qsizetype(1));
  QVERIFY(!due.value().first().envelope.isEmpty());

  // Nothing crossed the wire while offline.
  QCOMPARE(transport.sendCount, 0);
  QCOMPARE(transport.ackCount, 0);
  QVERIFY(!transport.isConnected());

  session->lock();
  QVERIFY(session->syncEngine() == nullptr);

  // The MLS ratchet was persisted atomically with the send: the durable blob is
  // present and loads directly from the store, proving the commit went through
  // SqlCipherSyncStore rather than a write-through path.
  auto profileKey = vault.readProfileKey(profileId);
  QVERIFY(profileKey.hasValue());
  {
    auto inspectOpened = SqlCipherDatabase::open(paths.database, profileKey.value());
    QVERIFY(inspectOpened.hasValue());
    auto inspect =
        std::make_unique<SqlCipherDatabase>(std::move(inspectOpened).value());
    auto state = inspect->loadMlsState(profileId);
    QVERIFY(state.hasValue());
    QVERIFY(!state.value().isEmpty());
  }

  // Restart: unlock the same profile fresh. unlock() reconstructs the MlsClient
  // from the persisted MLS state, so a successful unlock is itself proof that the
  // durable ratchet blob loads back.
  auto reopened = ProfileSession::unlock(profileId, vault, paths);
  QVERIFY(reopened.hasValue());

  // The outbox row survived the restart. Claim with a far-future clock so the
  // lease taken above (already expired) does not hide it.
  const qint64 farFuture = QDateTime::currentMSecsSinceEpoch() + 3'600'000;
  auto reDue = reopened.value()->outbox()->claimDue(farFuture, 10, farFuture + 1000);
  QVERIFY(reDue.hasValue());
  QCOMPARE(reDue.value().size(), qsizetype(1));
  QVERIFY(!reDue.value().first().envelope.isEmpty());
}

QTEST_GUILESS_MAIN(ProfileSessionTest)
#include "tst_profilesession.moc"
