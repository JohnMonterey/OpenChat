#include "app/ProfileSession.h"
#include "security/KeyVault.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <optional>

using namespace OpenChat;

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

QTEST_GUILESS_MAIN(ProfileSessionTest)
#include "tst_profilesession.moc"
