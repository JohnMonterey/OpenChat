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

class KeyVault;
class MlsClient;
class MlsStateStore;
class SqlCipherChatRepository;
class SqlCipherDatabase;
class SqlCipherOutboxRepository;
class SqlCipherSyncRepository;

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

  [[nodiscard]] SqlCipherChatRepository *chats() const noexcept;
  [[nodiscard]] SqlCipherOutboxRepository *outbox() const noexcept;
  [[nodiscard]] SqlCipherSyncRepository *sync() const noexcept;
  [[nodiscard]] MlsClient *mls() const noexcept;

private:
  class DatabaseMlsStateStore;

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
  std::unique_ptr<SqlCipherOutboxRepository> m_outbox;
  std::unique_ptr<SqlCipherSyncRepository> m_sync;
  std::unique_ptr<DatabaseMlsStateStore> m_mlsStateStore;
  std::unique_ptr<MlsClient> m_mls;
  std::unique_ptr<DeviceIdentity> m_identity;
  std::optional<AccountId> m_accountId;
  SecureBuffer m_databaseKey;
  SecureBuffer m_wrappingKey;
  std::unique_ptr<RecoveryCode> m_recoveryCode;
  bool m_unlocked = false;
  bool m_locking = false;
};

} // namespace OpenChat
