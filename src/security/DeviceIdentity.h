#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"
#include "security/SecureBuffer.h"

#include <QByteArray>
#include <QByteArrayView>

namespace OpenChat {

enum class DeviceIdentityError {
  InvalidInput,
  GenerationFailed,
  SigningFailed,
  KeyMismatch
};

struct DevicePublicCredential final {
  DeviceId deviceId;
  QByteArray signingPublicKey;

  [[nodiscard]] QByteArray serialize() const;
};

class DeviceIdentity final {
public:
  DeviceIdentity(DeviceIdentity &&) noexcept = default;
  DeviceIdentity &operator=(DeviceIdentity &&) noexcept = default;
  DeviceIdentity(const DeviceIdentity &) = delete;
  DeviceIdentity &operator=(const DeviceIdentity &) = delete;

  [[nodiscard]] static Result<DeviceIdentity, DeviceIdentityError> generate();
  [[nodiscard]] static Result<DeviceIdentity, DeviceIdentityError>
  restore(const DeviceId &deviceId, SecureBuffer privateKey,
          QByteArrayView expectedPublicKey);

  [[nodiscard]] Result<QByteArray, DeviceIdentityError>
  signChallenge(QByteArrayView challenge, QByteArrayView context) const;
  [[nodiscard]] Result<QByteArray, DeviceIdentityError>
  signEnvelope(QByteArrayView signingInput) const;
  [[nodiscard]] DevicePublicCredential publicCredential() const;
  [[nodiscard]] SecureBuffer privateKeyForStorage() const;

private:
  DeviceIdentity(DeviceId deviceId, SecureBuffer privateKey,
                 QByteArray publicKey);

  DeviceId m_deviceId;
  SecureBuffer m_privateKey;
  QByteArray m_publicKey;
};

} // namespace OpenChat
