#include "app/ProfileSession.h"

#include "crypto/MlsClient.h"
#include "network/MlsSyncSession.h"
#include "network/SyncEngine.h"
#include "repositories/ChatRepository.h"
#include "repositories/OutboxRepository.h"
#include "repositories/SyncRepository.h"
#include "security/KeyVault.h"
#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherDatabase.h"
#include "storage/SqlCipherOutboxRepository.h"
#include "storage/SqlCipherSyncRepository.h"
#include "storage/SqlCipherSyncStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <optional>
#include <utility>

namespace OpenChat {
namespace {

ProfileSessionError mapVaultError(KeyVaultError error) {
  switch (error) {
  case KeyVaultError::Unavailable:
  case KeyVaultError::Locked:
  case KeyVaultError::Cancelled:
    return ProfileSessionError::VaultUnavailable;
  case KeyVaultError::NotFound:
    return ProfileSessionError::MissingKey;
  case KeyVaultError::AlreadyExists:
    return ProfileSessionError::AlreadyExists;
  case KeyVaultError::InvalidKey:
  case KeyVaultError::StorageFailure:
    return ProfileSessionError::VaultFailure;
  }
  return ProfileSessionError::VaultFailure;
}

void removeDatabaseFiles(const QString &databasePath) {
  QFile::remove(databasePath);
  QFile::remove(databasePath + QStringLiteral("-wal"));
  QFile::remove(databasePath + QStringLiteral("-shm"));
}

bool invokeNoThrow(const std::function<void()> &callback) noexcept {
  if (!callback)
    return true;
  try {
    callback();
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace

ProfilePaths ProfilePaths::forProfile(const QString &profilesRoot,
                                      const ProfileId &profileId) {
  const QString root = QDir::cleanPath(QFileInfo(profilesRoot).absoluteFilePath());
  const QString directory = QDir(root).absoluteFilePath(profileId.toHex());
  return ProfilePaths{root, QDir::cleanPath(directory),
                      QDir(directory).absoluteFilePath(QStringLiteral("profile.sqlite3"))};
}

bool ProfilePaths::isValidFor(const ProfileId &profileId) const {
  if (profilesRoot.trimmed().isEmpty())
    return false;
  const QString root = QDir::cleanPath(QFileInfo(profilesRoot).absoluteFilePath());
  const QString expectedDirectory =
      QDir::cleanPath(QDir(root).absoluteFilePath(profileId.toHex()));
  const QString expectedDatabase = QDir::cleanPath(
      QDir(expectedDirectory).absoluteFilePath(QStringLiteral("profile.sqlite3")));
  return !root.isEmpty() && root != QStringLiteral("/") &&
         QDir::cleanPath(QFileInfo(profileDirectory).absoluteFilePath()) ==
             expectedDirectory &&
         QDir::cleanPath(QFileInfo(database).absoluteFilePath()) == expectedDatabase &&
         QFileInfo(expectedDirectory).absolutePath() == root;
}

ProfileSession::ProfileSession(ProfileId profileId, KeyVault &vault,
                               ProfilePaths paths, ProfileSessionHooks hooks)
    : m_profileId(std::move(profileId)), m_vault(vault),
      m_paths(std::move(paths)), m_hooks(std::move(hooks)) {}

ProfileSession::~ProfileSession() { lock(); }

Result<std::unique_ptr<ProfileSession>, ProfileSessionError>
ProfileSession::create(const ProfileId &profileId, KeyVault &vault,
                       const ProfilePaths &paths, ProfileSessionHooks hooks) {
  if (!paths.isValidFor(profileId))
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::InvalidPath);
  if (vault.availability() != KeyVaultAvailability::Available)
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::VaultUnavailable);
  if (QFileInfo::exists(paths.profileDirectory) || QFileInfo::exists(paths.database))
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::AlreadyExists);

  auto databaseKey = vault.createProfileKey(profileId);
  if (!databaseKey.hasValue())
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        mapVaultError(databaseKey.error()));
  auto wrappingKey = vault.createDeviceWrappingKey(profileId);
  if (!wrappingKey.hasValue()) {
    (void)vault.deleteProfileKey(profileId);
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        mapVaultError(wrappingKey.error()));
  }

  const auto rollBack = [&] {
    removeDatabaseFiles(paths.database);
    QDir().rmdir(paths.profileDirectory);
    (void)vault.deleteDeviceWrappingKey(profileId);
    (void)vault.deleteProfileKey(profileId);
  };
  if (!QDir().mkpath(paths.profileDirectory)) {
    rollBack();
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::DatabaseFailure);
  }
  auto opened = SqlCipherDatabase::open(paths.database, databaseKey.value());
  if (!opened.hasValue()) {
    rollBack();
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::DatabaseFailure);
  }
  auto database = std::make_unique<SqlCipherDatabase>(std::move(opened).value());
  auto generated = DeviceIdentity::generate();
  if (!generated.hasValue()) {
    database->close();
    rollBack();
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::IdentityFailure);
  }
  auto identity = std::make_unique<DeviceIdentity>(std::move(generated).value());
  const auto credential = identity->publicCredential();
  const auto privateKey = identity->privateKeyForStorage();
  auto stored = database->storeDeviceIdentity(
      profileId, credential.deviceId, credential.signingPublicKey, privateKey,
      wrappingKey.value(), QDateTime::currentMSecsSinceEpoch());
  if (!stored.hasValue()) {
    database->close();
    rollBack();
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::DatabaseFailure);
  }
  const auto accountId = AccountId::generate();
  auto storedAccount = database->storeAccountId(profileId, accountId);
  if (!storedAccount.hasValue()) {
    database->close();
    rollBack();
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::DatabaseFailure);
  }
  auto recoveryCode = RecoveryCode::generate();
  if (!recoveryCode.hasValue()) {
    database->close();
    rollBack();
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::IdentityFailure);
  }

  auto session = std::unique_ptr<ProfileSession>(
      new ProfileSession(profileId, vault, paths, std::move(hooks)));
  auto activated = session->activate(
      std::move(database), std::move(databaseKey).value(),
      std::move(wrappingKey).value(), std::move(identity), accountId);
  if (!activated.hasValue()) {
    session->lock();
    rollBack();
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        activated.error());
  }
  session->m_recoveryCode =
      std::make_unique<RecoveryCode>(std::move(recoveryCode).value());
  return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::success(
      std::move(session));
}

Result<std::unique_ptr<ProfileSession>, ProfileSessionError>
ProfileSession::unlock(const ProfileId &profileId, KeyVault &vault,
                       const ProfilePaths &paths, ProfileSessionHooks hooks) {
  if (!paths.isValidFor(profileId))
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::InvalidPath);
  if (vault.availability() != KeyVaultAvailability::Available)
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::VaultUnavailable);
  if (!QFileInfo(paths.database).isFile())
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::DatabaseMissing);

  auto databaseKey = vault.readProfileKey(profileId);
  if (!databaseKey.hasValue())
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        mapVaultError(databaseKey.error()));
  auto wrappingKey = vault.readDeviceWrappingKey(profileId);
  if (!wrappingKey.hasValue())
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        mapVaultError(wrappingKey.error()));
  auto opened = SqlCipherDatabase::open(paths.database, databaseKey.value());
  if (!opened.hasValue())
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::DatabaseFailure);
  auto database = std::make_unique<SqlCipherDatabase>(std::move(opened).value());
  auto stored = database->loadDeviceIdentity(profileId, wrappingKey.value());
  if (!stored.hasValue())
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::IdentityFailure);
  auto restored = DeviceIdentity::restore(
      stored.value().deviceId, std::move(stored.value().privateKey),
      stored.value().publicKey);
  if (!restored.hasValue())
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::IdentityFailure);
  auto storedAccount = database->loadAccountId(profileId);
  if (!storedAccount.hasValue())
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::DatabaseFailure);
  auto storedDisplayName = database->loadProfileDisplayName(profileId);
  if (!storedDisplayName.hasValue())
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        ProfileSessionError::DatabaseFailure);

  auto session = std::unique_ptr<ProfileSession>(
      new ProfileSession(profileId, vault, paths, std::move(hooks)));
  auto identity = std::make_unique<DeviceIdentity>(std::move(restored).value());
  auto activated = session->activate(
      std::move(database), std::move(databaseKey).value(),
      std::move(wrappingKey).value(), std::move(identity),
      std::move(storedAccount).value());
  if (!activated.hasValue())
    return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::failure(
        activated.error());
  session->m_displayName = std::move(storedDisplayName).value();
  return Result<std::unique_ptr<ProfileSession>, ProfileSessionError>::success(
      std::move(session));
}

Result<void, ProfileSessionError> ProfileSession::activate(
    std::unique_ptr<SqlCipherDatabase> database, SecureBuffer databaseKey,
    SecureBuffer wrappingKey, std::unique_ptr<DeviceIdentity> identity,
    AccountId accountId) {
  m_database = std::move(database);
  m_databaseKey = std::move(databaseKey);
  m_wrappingKey = std::move(wrappingKey);
  m_identity = std::move(identity);
  m_accountId = std::move(accountId);
  m_chats = std::make_unique<SqlCipherChatRepository>(*m_database);
  m_contacts = std::make_unique<SqlCipherContactRepository>(*m_database);
  m_outbox = std::make_unique<SqlCipherOutboxRepository>(*m_database);
  m_sync = std::make_unique<SqlCipherSyncRepository>(*m_database);
  // MLS state is captured in memory, not written through. load() still returns
  // the last durably-committed blob so a fresh MlsClient reconstructs from a
  // consistent state, but store() only captures the new blob; the SyncEngine
  // then persists it via SqlCipherSyncStore inside the SAME transaction as the
  // message / outbox / watermark it belongs to (finding F3). CAVEAT: any MLS
  // mutation that does NOT flow through the SyncEngine's send/receive path — for
  // example future group setup done directly on mls() — MUST hand its captured
  // state to the SyncStore, or that ratchet advance is dropped on lock().
  m_mlsStateStore =
      std::make_unique<CapturingMlsStateStore>(*m_database, m_profileId);
  const auto mlsIdentity = m_identity->publicCredential().serialize();
  auto mls = MlsClient::create(mlsIdentity, m_mlsStateStore.get());
  if (!mls.hasValue()) {
    lock();
    return Result<void, ProfileSessionError>::failure(
        ProfileSessionError::MlsFailure);
  }
  m_mls = std::move(mls).value();
  // Durable sync pieces: the SyncStore owns the atomic commit* combinations, and
  // the MlsSyncSession adapts this MlsClient plus its capturing store to the
  // narrow surface the SyncEngine consumes. Both are borrowed by the engine that
  // startNetworking() builds; both must outlive it (enforced by lock()'s order).
  m_syncStore = std::make_unique<SqlCipherSyncStore>(*m_database, m_profileId);
  m_mlsSyncSession = std::make_unique<MlsSyncSession>(*m_mls, *m_mlsStateStore);
  m_unlocked = true;
  return Result<void, ProfileSessionError>::success();
}

void ProfileSession::lock() noexcept {
  if (m_locking || (!m_unlocked && !m_database))
    return;
  m_locking = true;
  const bool wasUnlocked = std::exchange(m_unlocked, false);
  if (wasUnlocked) {
    invokeNoThrow(m_hooks.stopNetworking);
    invokeNoThrow(m_hooks.cancelDecryptions);
  }
  // Teardown order is load-bearing. The SyncEngine borrows the SyncStore, the
  // MlsSyncSession, the injected transport and (through its signer) the device
  // identity, so it MUST be stopped and destroyed before any of them. The
  // MlsSyncSession borrows the MlsClient + capturing store and the SyncStore
  // borrows the database, so both are released next, ahead of what they borrow.
  if (m_syncEngine)
    m_syncEngine->stop();
  m_syncEngine.reset();
  m_mlsSyncSession.reset();
  m_syncStore.reset();
  m_chats.reset();
  m_contacts.reset();
  m_outbox.reset();
  m_sync.reset();
  m_mls.reset();
  m_mlsStateStore.reset();
  m_identity.reset();
  m_accountId.reset();
  m_displayName.clear();
  m_databaseKey = {};
  m_wrappingKey = {};
  m_recoveryCode.reset();
  if (wasUnlocked)
    invokeNoThrow(m_hooks.clearModels);
  if (m_database)
    m_database->close();
  m_database.reset();
  m_locking = false;
}

bool ProfileSession::isUnlocked() const noexcept { return m_unlocked; }

Result<DevicePublicCredential, ProfileSessionError>
ProfileSession::publicCredential() const {
  if (!m_unlocked || !m_identity)
    return Result<DevicePublicCredential, ProfileSessionError>::failure(
        ProfileSessionError::NotUnlocked);
  return Result<DevicePublicCredential, ProfileSessionError>::success(
      m_identity->publicCredential());
}

Result<QByteArray, ProfileSessionError>
ProfileSession::signChallenge(QByteArrayView challenge,
                              QByteArrayView context) const {
  if (!m_unlocked || !m_identity)
    return Result<QByteArray, ProfileSessionError>::failure(
        ProfileSessionError::NotUnlocked);
  auto signature = m_identity->signChallenge(challenge, context);
  if (!signature.hasValue())
    return Result<QByteArray, ProfileSessionError>::failure(
        ProfileSessionError::IdentityFailure);
  return Result<QByteArray, ProfileSessionError>::success(
      std::move(signature).value());
}

Result<AccountId, ProfileSessionError> ProfileSession::accountId() const {
  if (!m_unlocked || !m_accountId)
    return Result<AccountId, ProfileSessionError>::failure(
        ProfileSessionError::NotUnlocked);
  return Result<AccountId, ProfileSessionError>::success(*m_accountId);
}

QString ProfileSession::displayName() const {
  return m_unlocked ? m_displayName : QString();
}

Result<void, ProfileSessionError>
ProfileSession::setDisplayName(const QString &displayName) {
  if (!m_unlocked || !m_database)
    return Result<void, ProfileSessionError>::failure(ProfileSessionError::NotUnlocked);
  const QString normalized = displayName.trimmed();
  if (normalized.isEmpty())
    return Result<void, ProfileSessionError>::failure(ProfileSessionError::DatabaseFailure);
  if (!m_database->storeProfileDisplayName(m_profileId, normalized).hasValue())
    return Result<void, ProfileSessionError>::failure(ProfileSessionError::DatabaseFailure);
  m_displayName = normalized;
  return Result<void, ProfileSessionError>::success();
}

Result<RecoveryCode, ProfileSessionError> ProfileSession::takeRecoveryCode() {
  if (!m_recoveryCode)
    return Result<RecoveryCode, ProfileSessionError>::failure(
        ProfileSessionError::RecoveryCodeUnavailable);
  RecoveryCode code = std::move(*m_recoveryCode);
  m_recoveryCode.reset();
  return Result<RecoveryCode, ProfileSessionError>::success(std::move(code));
}

Result<void, ProfileSessionError> ProfileSession::persistMlsState() {
  if (!m_unlocked || !m_mlsStateStore || !m_syncStore)
    return Result<void, ProfileSessionError>::failure(
        ProfileSessionError::NotUnlocked);
  // Nothing captured since the last durable commit: succeed without a write.
  if (!m_mlsStateStore->hasPendingState())
    return Result<void, ProfileSessionError>::success();
  const QByteArray pending = m_mlsStateStore->takePendingState();
  if (pending.isEmpty())
    return Result<void, ProfileSessionError>::success();
  auto committed = m_syncStore->commitMlsState(pending);
  if (!committed.hasValue())
    return Result<void, ProfileSessionError>::failure(
        ProfileSessionError::DatabaseFailure);
  return Result<void, ProfileSessionError>::success();
}

SqlCipherChatRepository *ProfileSession::chats() const noexcept {
  return m_chats.get();
}

SqlCipherContactRepository *ProfileSession::contacts() const noexcept {
  return m_contacts.get();
}

SqlCipherOutboxRepository *ProfileSession::outbox() const noexcept {
  return m_outbox.get();
}

SqlCipherSyncRepository *ProfileSession::sync() const noexcept {
  return m_sync.get();
}

SqlCipherSyncStore *ProfileSession::syncStore() const noexcept {
  return m_syncStore.get();
}

MlsClient *ProfileSession::mls() const noexcept { return m_mls.get(); }

Result<void, ProfileSessionError>
ProfileSession::startNetworking(SyncTransport &transport) {
  if (!m_unlocked || !m_identity || !m_accountId || !m_syncStore ||
      !m_mlsSyncSession)
    return Result<void, ProfileSessionError>::failure(
        ProfileSessionError::NotUnlocked);
  // Idempotent: a second call must not build a second engine over the same
  // borrowed pieces. The transport passed to the first call stays in effect.
  if (m_syncEngine)
    return Result<void, ProfileSessionError>::success();

  // Aggregate-initialize the account/device fields; maxSendAttempts, drainBatch
  // and leaseMs keep their engine defaults.
  const SyncEngine::Config config{*m_accountId,
                                  m_identity->publicCredential().deviceId};

  // The signer keeps the Ed25519 private key inside DeviceIdentity/OpenSSL and
  // returns the raw 64-byte signature, or an empty QByteArray on failure so the
  // engine fails closed (it rejects a signature whose size is not 64) rather
  // than emitting an unsigned envelope. Capturing `this` is safe: the engine —
  // and therefore this lambda — is destroyed in lock() before m_identity, and
  // the signer is only invoked synchronously while the session is unlocked.
  SyncEngine::Signer signer = [this](QByteArrayView signingInput) -> QByteArray {
    if (!m_identity)
      return {};
    auto signature = m_identity->signEnvelope(signingInput);
    if (!signature.hasValue())
      return {};
    return std::move(signature).value();
  };
  SyncEngine::Clock clock = [] { return QDateTime::currentMSecsSinceEpoch(); };

  m_syncEngine = std::make_unique<SyncEngine>(
      config, *m_syncStore, *m_mlsSyncSession, transport, std::move(signer),
      std::move(clock));
  m_syncEngine->start();
  return Result<void, ProfileSessionError>::success();
}

SyncEngine *ProfileSession::syncEngine() const noexcept {
  return m_syncEngine.get();
}

Result<void, ProfileSessionError> ProfileSession::removeLocalProfile(
    const ProfileId &profileId, KeyVault &vault, const ProfilePaths &paths,
    const QString &confirmation) {
  if (!paths.isValidFor(profileId) || confirmation != profileId.toHex())
    return Result<void, ProfileSessionError>::failure(
        confirmation != profileId.toHex()
            ? ProfileSessionError::ConfirmationMismatch
            : ProfileSessionError::InvalidPath);
  if (vault.availability() != KeyVaultAvailability::Available)
    return Result<void, ProfileSessionError>::failure(
        ProfileSessionError::VaultUnavailable);
  const QFileInfo directoryInfo(paths.profileDirectory);
  if (directoryInfo.isSymLink())
    return Result<void, ProfileSessionError>::failure(
        ProfileSessionError::InvalidPath);

  bool removed = true;
  if (directoryInfo.exists())
    removed = QDir(paths.profileDirectory).removeRecursively();
  auto wrappingDeleted = vault.deleteDeviceWrappingKey(profileId);
  auto databaseDeleted = vault.deleteProfileKey(profileId);
  if (!removed || !wrappingDeleted.hasValue() || !databaseDeleted.hasValue())
    return Result<void, ProfileSessionError>::failure(
        ProfileSessionError::RemovalFailed);
  return Result<void, ProfileSessionError>::success();
}

} // namespace OpenChat
