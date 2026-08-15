#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"
#include "security/SecureBuffer.h"

namespace OpenChat {

enum class KeyVaultAvailability { Available, Unavailable };

enum class KeyVaultError {
  Unavailable,
  Locked,
  Cancelled,
  NotFound,
  AlreadyExists,
  InvalidKey,
  StorageFailure
};

class KeyVault {
public:
  virtual ~KeyVault() = default;

  [[nodiscard]] virtual KeyVaultAvailability availability() const = 0;
  [[nodiscard]] virtual Result<SecureBuffer, KeyVaultError>
  readProfileKey(const ProfileId &profileId) = 0;
  [[nodiscard]] virtual Result<SecureBuffer, KeyVaultError>
  createProfileKey(const ProfileId &profileId) = 0;
  [[nodiscard]] virtual Result<void, KeyVaultError>
  deleteProfileKey(const ProfileId &profileId) = 0;
  [[nodiscard]] virtual Result<SecureBuffer, KeyVaultError>
  readDeviceWrappingKey(const ProfileId &profileId) = 0;
  [[nodiscard]] virtual Result<SecureBuffer, KeyVaultError>
  createDeviceWrappingKey(const ProfileId &profileId) = 0;
  [[nodiscard]] virtual Result<void, KeyVaultError>
  deleteDeviceWrappingKey(const ProfileId &profileId) = 0;
};

} // namespace OpenChat
