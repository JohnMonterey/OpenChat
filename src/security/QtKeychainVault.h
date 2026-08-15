#pragma once

#include "security/KeyVault.h"

namespace OpenChat {

class QtKeychainVault final : public KeyVault {
public:
  [[nodiscard]] KeyVaultAvailability availability() const override;
  [[nodiscard]] Result<SecureBuffer, KeyVaultError>
  readProfileKey(const ProfileId &profileId) override;
  [[nodiscard]] Result<SecureBuffer, KeyVaultError>
  createProfileKey(const ProfileId &profileId) override;
  [[nodiscard]] Result<void, KeyVaultError>
  deleteProfileKey(const ProfileId &profileId) override;
  [[nodiscard]] Result<SecureBuffer, KeyVaultError>
  readDeviceWrappingKey(const ProfileId &profileId) override;
  [[nodiscard]] Result<SecureBuffer, KeyVaultError>
  createDeviceWrappingKey(const ProfileId &profileId) override;
  [[nodiscard]] Result<void, KeyVaultError>
  deleteDeviceWrappingKey(const ProfileId &profileId) override;

};

} // namespace OpenChat
