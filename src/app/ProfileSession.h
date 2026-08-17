#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"
#include "security/DeviceIdentity.h"
#include "security/RecoveryCode.h"

#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace OpenChat {

class CapturingMlsStateStore;
class KeyVault;
class MlsClient;
class MlsSyncSession;
class SqlCipherChatRepository;
class SqlCipherContactRepository;
class SqlCipherDatabase;
class SqlCipherOutboxRepository;
class SqlCipherSyncRepository;
class SqlCipherSyncStore;
class SyncEngine;
class SyncTransport;

struct ProfilePaths final {
  QString profilesRoot;
  QString profileDirectory;
  QString database;

  [[nodiscard]] static ProfilePaths forProfile(const QString &profilesRoot,
                                               const ProfileId &profileId);
  [[nodiscard]] bool isValidFor(const ProfileId &profileId) const;
};

struct ProfileSessionHooks final {
  std::function<void()> stopNetworking;
  std::function<void()> cancelDecryptions;
  std::function<void()> clearModels;
};

enum class ProfileSessionError {
  InvalidPath,
  AlreadyExists,
  VaultUnavailable,
  VaultFailure,
  MissingKey,
  DatabaseMissing,
  DatabaseFailure,
  IdentityFailure,
  MlsFailure,
  NotUnlocked,
  ConfirmationMismatch,
  RemovalFailed,
  RecoveryCodeUnavailable
};

class ProfileSession final {
public:
  ~ProfileSession();
  ProfileSession(const ProfileSession &) = delete;
  ProfileSession &operator=(const ProfileSession &) = delete;

  [[nodiscard]] static Result<std::unique_ptr<ProfileSession>, ProfileSessionError>
  create(const ProfileId &profileId, KeyVault &vault,
         const ProfilePaths &paths, ProfileSessionHooks hooks = {});
  [[nodiscard]] static Result<std::unique_ptr<ProfileSession>, ProfileSessionError>
  unlock(const ProfileId &profileId, KeyVault &vault,
         const ProfilePaths &paths, ProfileSessionHooks hooks = {});
  [[nodiscard]] static Result<void, ProfileSessionError>
  removeLocalProfile(const ProfileId &profileId, KeyVault &vault,
                     const ProfilePaths &paths, const QString &confirmation);

  void lock() noexcept;
  [[nodiscard]] bool isUnlocked() const noexcept;
  [[nodiscard]] Result<DevicePublicCredential, ProfileSessionError>
  publicCredential() const;
  [[nodiscard]] Result<QByteArray, ProfileSessionError>
  signChallenge(QByteArrayView challenge, QByteArrayView context) const;
  [[nodiscard]] Result<AccountId, ProfileSessionError> accountId() const;
  [[nodiscard]] Result<RecoveryCode, ProfileSessionError> takeRecoveryCode();

  // Durably commits any MLS ratchet state captured by an out-of-band mutation
  // (KeyPackage generation, group setup) that did NOT flow through the
  // SyncEngine's send/receive path. Surrenders the capturing store's pending
  // blob and writes it through the SyncStore in one transaction; a no-op that
  // succeeds when nothing is pending. Without this call such a mutation's
  // private material is dropped on lock(), so a published KeyPackage could not
  // be used after a restart (see the Phase-4b caveat in activate()).
  [[nodiscard]] Result<void, ProfileSessionError> persistMlsState();

  [[nodiscard]] SqlCipherChatRepository *chats() const noexcept;
  [[nodiscard]] SqlCipherContactRepository *contacts() const noexcept;
  [[nodiscard]] SqlCipherOutboxRepository *outbox() const noexcept;
  [[nodiscard]] SqlCipherSyncRepository *sync() const noexcept;
  [[nodiscard]] MlsClient *mls() const noexcept;

  // Constructs and starts the durable SyncEngine over an injected ciphertext
  // transport, wiring it to the owned SyncStore + MlsSyncSession, a signer over
  // the device identity and the system UTC clock. Requires the session to be
  // unlocked. Idempotent: a second call is a no-op while an engine already runs
  // (it never double-constructs).
  //
  // The caller owns `transport` and MUST keep it alive until lock() or session
  // destruction: the engine borrows it (and the other pieces) by reference. The
  // live network transport is injected rather than built here because the app
  // bootstrap that constructs the TLS relay transport is a separate layer; the
  // session only wires the durable engine to whatever SyncTransport it is given
  // and never itself constructs a RelayClient/RelayTransport.
  [[nodiscard]] Result<void, ProfileSessionError>
  startNetworking(SyncTransport &transport);
  // Null until startNetworking() succeeds; reset to null on lock().
  [[nodiscard]] SyncEngine *syncEngine() const noexcept;

private:
  ProfileSession(ProfileId profileId, KeyVault &vault, ProfilePaths paths,
                 ProfileSessionHooks hooks);
  [[nodiscard]] Result<void, ProfileSessionError>
  activate(std::unique_ptr<SqlCipherDatabase> database,
           SecureBuffer databaseKey, SecureBuffer wrappingKey,
           std::unique_ptr<DeviceIdentity> identity, AccountId accountId);

  ProfileId m_profileId;
  KeyVault &m_vault;
  ProfilePaths m_paths;
  ProfileSessionHooks m_hooks;
  std::unique_ptr<SqlCipherDatabase> m_database;
  std::unique_ptr<SqlCipherChatRepository> m_chats;
  std::unique_ptr<SqlCipherContactRepository> m_contacts;
  std::unique_ptr<SqlCipherOutboxRepository> m_outbox;
  std::unique_ptr<SqlCipherSyncRepository> m_sync;
  std::unique_ptr<CapturingMlsStateStore> m_mlsStateStore;
  std::unique_ptr<MlsClient> m_mls;
  std::unique_ptr<DeviceIdentity> m_identity;
  // Durable sync pieces. Declared AFTER what they borrow so member-destruction
  // order is a safe backstop to lock()'s explicit teardown: m_syncStore borrows
  // the database, m_mlsSyncSession borrows the MlsClient + capturing store, and
  // m_syncEngine (last) borrows all of them plus the identity via its signer, so
  // it is destroyed first.
  std::unique_ptr<SqlCipherSyncStore> m_syncStore;
  std::unique_ptr<MlsSyncSession> m_mlsSyncSession;
  std::unique_ptr<SyncEngine> m_syncEngine;
  std::optional<AccountId> m_accountId;
  SecureBuffer m_databaseKey;
  SecureBuffer m_wrappingKey;
  std::unique_ptr<RecoveryCode> m_recoveryCode;
  bool m_unlocked = false;
  bool m_locking = false;
};

} // namespace OpenChat
