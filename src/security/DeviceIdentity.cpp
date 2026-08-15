#include "security/DeviceIdentity.h"

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <memory>

namespace OpenChat {
namespace {

constexpr qsizetype privateKeySize = 32;
constexpr qsizetype publicKeySize = 32;
constexpr qsizetype signatureSize = 64;
constexpr qsizetype maximumChallengeSize = 4096;
constexpr qsizetype maximumContextSize = 1024;

using PkeyPointer = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using MdContextPointer = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

PkeyPointer pkeyFromSeed(QByteArrayView seed) {
  if (seed.size() != privateKeySize)
    return PkeyPointer(nullptr, EVP_PKEY_free);
  return PkeyPointer(EVP_PKEY_new_raw_private_key(
                         EVP_PKEY_ED25519, nullptr,
                         reinterpret_cast<const unsigned char *>(seed.data()),
                         static_cast<std::size_t>(seed.size())),
                     EVP_PKEY_free);
}

Result<QByteArray, DeviceIdentityError> derivePublicKey(QByteArrayView seed) {
  auto key = pkeyFromSeed(seed);
  if (!key)
    return Result<QByteArray, DeviceIdentityError>::failure(
        DeviceIdentityError::GenerationFailed);

  QByteArray publicKey(publicKeySize, Qt::Uninitialized);
  std::size_t length = static_cast<std::size_t>(publicKey.size());
  if (EVP_PKEY_get_raw_public_key(
          key.get(), reinterpret_cast<unsigned char *>(publicKey.data()),
          &length) != 1 ||
      length != static_cast<std::size_t>(publicKeySize))
    return Result<QByteArray, DeviceIdentityError>::failure(
        DeviceIdentityError::GenerationFailed);
  return Result<QByteArray, DeviceIdentityError>::success(std::move(publicKey));
}

void appendLength(QByteArray &output, qsizetype size) {
  const quint32 value = static_cast<quint32>(size);
  output.append(static_cast<char>((value >> 24) & 0xff));
  output.append(static_cast<char>((value >> 16) & 0xff));
  output.append(static_cast<char>((value >> 8) & 0xff));
  output.append(static_cast<char>(value & 0xff));
}

} // namespace

QByteArray DevicePublicCredential::serialize() const {
  QByteArray output;
  output.reserve(1 + DeviceId::byteCount + publicKeySize);
  output.append(char{1});
  output.append(deviceId.bytes());
  output.append(signingPublicKey);
  return output;
}

DeviceIdentity::DeviceIdentity(DeviceId deviceId, SecureBuffer privateKey,
                               QByteArray publicKey)
    : m_deviceId(std::move(deviceId)), m_privateKey(std::move(privateKey)),
      m_publicKey(std::move(publicKey)) {}

Result<DeviceIdentity, DeviceIdentityError> DeviceIdentity::generate() {
  QByteArray seed(privateKeySize, Qt::Uninitialized);
  if (RAND_bytes(reinterpret_cast<unsigned char *>(seed.data()),
                 static_cast<int>(seed.size())) != 1)
    return Result<DeviceIdentity, DeviceIdentityError>::failure(
        DeviceIdentityError::GenerationFailed);

  auto privateKey = SecureBuffer::fromBytes(seed);
  OPENSSL_cleanse(seed.data(), static_cast<std::size_t>(seed.size()));
  auto publicKey = derivePublicKey(privateKey.view());
  if (!publicKey.hasValue())
    return Result<DeviceIdentity, DeviceIdentityError>::failure(publicKey.error());
  return Result<DeviceIdentity, DeviceIdentityError>::success(DeviceIdentity(
      DeviceId::generate(), std::move(privateKey), std::move(publicKey).value()));
}

Result<DeviceIdentity, DeviceIdentityError>
DeviceIdentity::restore(const DeviceId &deviceId, SecureBuffer privateKey,
                        QByteArrayView expectedPublicKey) {
  if (privateKey.size() != privateKeySize ||
      expectedPublicKey.size() != publicKeySize)
    return Result<DeviceIdentity, DeviceIdentityError>::failure(
        DeviceIdentityError::InvalidInput);
  auto derived = derivePublicKey(privateKey.view());
  if (!derived.hasValue())
    return Result<DeviceIdentity, DeviceIdentityError>::failure(derived.error());
  if (CRYPTO_memcmp(derived.value().constData(), expectedPublicKey.data(),
                    static_cast<std::size_t>(publicKeySize)) != 0)
    return Result<DeviceIdentity, DeviceIdentityError>::failure(
        DeviceIdentityError::KeyMismatch);
  return Result<DeviceIdentity, DeviceIdentityError>::success(DeviceIdentity(
      deviceId, std::move(privateKey), std::move(derived).value()));
}

Result<QByteArray, DeviceIdentityError>
DeviceIdentity::signChallenge(QByteArrayView challenge,
                              QByteArrayView context) const {
  if (challenge.isEmpty() || challenge.size() > maximumChallengeSize ||
      context.isEmpty() || context.size() > maximumContextSize)
    return Result<QByteArray, DeviceIdentityError>::failure(
        DeviceIdentityError::InvalidInput);

  QByteArray message("OpenChat device challenge v1", 28);
  appendLength(message, context.size());
  message.append(context.data(), context.size());
  appendLength(message, challenge.size());
  message.append(challenge.data(), challenge.size());

  auto key = pkeyFromSeed(m_privateKey.view());
  MdContextPointer md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!key || !md ||
      EVP_DigestSignInit(md.get(), nullptr, nullptr, nullptr, key.get()) != 1)
    return Result<QByteArray, DeviceIdentityError>::failure(
        DeviceIdentityError::SigningFailed);

  QByteArray signature(signatureSize, Qt::Uninitialized);
  std::size_t length = static_cast<std::size_t>(signature.size());
  if (EVP_DigestSign(
          md.get(), reinterpret_cast<unsigned char *>(signature.data()), &length,
          reinterpret_cast<const unsigned char *>(message.constData()),
          static_cast<std::size_t>(message.size())) != 1 ||
      length != static_cast<std::size_t>(signatureSize))
    return Result<QByteArray, DeviceIdentityError>::failure(
        DeviceIdentityError::SigningFailed);
  return Result<QByteArray, DeviceIdentityError>::success(std::move(signature));
}

DevicePublicCredential DeviceIdentity::publicCredential() const {
  return DevicePublicCredential{m_deviceId, m_publicKey};
}

SecureBuffer DeviceIdentity::privateKeyForStorage() const {
  return SecureBuffer::fromBytes(m_privateKey.view());
}

} // namespace OpenChat
