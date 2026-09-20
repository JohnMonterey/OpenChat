#include "security/PasswordKey.h"

#include "domain/Handle.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <array>

namespace OpenChat {

namespace {

using KeyResult = Result<SecureBuffer, PasswordKeyError>;

// The salt only has to be unique per account and reproducible on every device,
// so it is derived from the canonical handle under a versioned label. (Its job
// is to stop one precomputed table serving every account; the relay adds a
// random per-account salt of its own on top.)
QByteArray saltForHandle(const QString &canonicalHandle)
{
  QByteArray input = QByteArrayLiteral("OpenChat password key v1");
  input.append('\0');
  input.append(canonicalHandle.toUtf8());
  return QCryptographicHash::hash(input, QCryptographicHash::Sha256);
}

} // namespace

Result<SecureBuffer, PasswordKeyError>
derivePasswordKey(const QString &handle, const QString &password)
{
  const std::optional<QString> canonical = normalizeHandle(handle);
  if (!canonical)
    return KeyResult::failure(PasswordKeyError::InvalidHandle);
  if (password.isEmpty() || password.size() > maximumPasswordLength)
    return KeyResult::failure(PasswordKeyError::InvalidPassword);

  // NFKC so that a password typed with composed characters on one keyboard and
  // decomposed ones on another is the same password.
  QString normalized = password.normalized(QString::NormalizationForm_KC);
  QByteArray secret = normalized.toUtf8();
  normalized.fill(QChar(u'\0'));
  QByteArray salt = saltForHandle(*canonical);

  const auto wipe = [&secret] {
    OPENSSL_cleanse(secret.data(), static_cast<std::size_t>(secret.size()));
  };

  EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
  EVP_KDF_CTX *ctx = kdf ? EVP_KDF_CTX_new(kdf) : nullptr;
  if (kdf)
    EVP_KDF_free(kdf);
  if (!ctx) {
    wipe();
    return KeyResult::failure(PasswordKeyError::DerivationFailed);
  }

  // One lane on the calling thread: no dependency on OpenSSL's optional thread
  // pool, and identical output everywhere (lanes are part of the Argon2 input).
  quint32 memory = passwordKeyMemoryKiB;
  quint32 iterations = passwordKeyIterations;
  quint32 lanes = 1;
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, secret.data(),
                                        static_cast<std::size_t>(secret.size())),
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt.data(),
                                        static_cast<std::size_t>(salt.size())),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
      OSSL_PARAM_construct_end(),
  };

  std::array<unsigned char, passwordKeyBytes> key{};
  const bool ok = EVP_KDF_derive(ctx, key.data(), key.size(), params) == 1;
  EVP_KDF_CTX_free(ctx);
  wipe();
  if (!ok) {
    OPENSSL_cleanse(key.data(), key.size());
    return KeyResult::failure(PasswordKeyError::DerivationFailed);
  }

  SecureBuffer buffer = SecureBuffer::fromBytes(
      QByteArrayView(reinterpret_cast<const char *>(key.data()),
                     static_cast<qsizetype>(key.size())));
  OPENSSL_cleanse(key.data(), key.size());
  if (buffer.size() != passwordKeyBytes)
    return KeyResult::failure(PasswordKeyError::DerivationFailed);
  return KeyResult::success(std::move(buffer));
}

} // namespace OpenChat
