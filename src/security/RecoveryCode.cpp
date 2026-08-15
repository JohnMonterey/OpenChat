#include "security/RecoveryCode.h"

#include <openssl/crypto.h>
#include <openssl/sha.h>

#include <cctype>

namespace OpenChat {
namespace {

constexpr char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

QByteArray encodeBase32(QByteArrayView input) {
  QByteArray output;
  output.reserve((input.size() * 8 + 4) / 5);
  quint32 accumulator = 0;
  int bits = 0;
  for (const unsigned char byte : input) {
    accumulator = (accumulator << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      output.append(alphabet[(accumulator >> bits) & 0x1f]);
    }
  }
  if (bits > 0)
    output.append(alphabet[(accumulator << (5 - bits)) & 0x1f]);
  return output;
}

QByteArray normalize(QByteArrayView input) {
  QByteArray output;
  output.reserve(input.size());
  for (char character : input) {
    if (character == '-' || character == ' ' || character == '\t' ||
        character == '\r' || character == '\n')
      continue;
    character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    if (character == 'O')
      character = '0';
    else if (character == 'I' || character == 'L')
      character = '1';
    output.append(character);
  }
  return output;
}

QByteArray formatCode(QByteArrayView secret) {
  QByteArray raw = encodeBase32(secret);
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(secret.data()),
         static_cast<std::size_t>(secret.size()), digest);
  raw.append(alphabet[digest[0] >> 3]);
  raw.append(alphabet[((digest[0] & 0x07) << 2) | (digest[1] >> 6)]);
  OPENSSL_cleanse(digest, sizeof(digest));

  QByteArray formatted;
  for (qsizetype index = 0; index < raw.size(); ++index) {
    if (index > 0 && index % 6 == 0)
      formatted.append('-');
    formatted.append(raw.at(index));
  }
  OPENSSL_cleanse(raw.data(), static_cast<std::size_t>(raw.size()));
  return formatted;
}

} // namespace

RecoveryCode::RecoveryCode(SecureBuffer secret, QByteArray encoded)
    : m_secret(std::move(secret)), m_encoded(std::move(encoded)) {}

RecoveryCode::~RecoveryCode() { wipeEncoded(); }

RecoveryCode::RecoveryCode(RecoveryCode &&other) noexcept
    : m_secret(std::move(other.m_secret)), m_encoded(std::move(other.m_encoded)),
      m_revealed(other.m_revealed), m_confirmed(other.m_confirmed) {
  other.m_revealed = false;
  other.m_confirmed = false;
}

RecoveryCode &RecoveryCode::operator=(RecoveryCode &&other) noexcept {
  if (this == &other)
    return *this;
  wipeEncoded();
  m_secret = std::move(other.m_secret);
  m_encoded = std::move(other.m_encoded);
  m_revealed = other.m_revealed;
  m_confirmed = other.m_confirmed;
  other.m_revealed = false;
  other.m_confirmed = false;
  return *this;
}

Result<RecoveryCode, RecoveryCodeError> RecoveryCode::generate() {
  try {
    auto secret = SecureBuffer::random(32);
    auto encoded = formatCode(secret.view());
    return Result<RecoveryCode, RecoveryCodeError>::success(
        RecoveryCode(std::move(secret), std::move(encoded)));
  } catch (...) {
    return Result<RecoveryCode, RecoveryCodeError>::failure(
        RecoveryCodeError::GenerationFailed);
  }
}

Result<QByteArray, RecoveryCodeError> RecoveryCode::reveal() {
  if (m_revealed)
    return Result<QByteArray, RecoveryCodeError>::failure(
        RecoveryCodeError::AlreadyRevealed);
  m_revealed = true;
  return Result<QByteArray, RecoveryCodeError>::success(m_encoded);
}

Result<bool, RecoveryCodeError> RecoveryCode::confirm(QByteArrayView candidate) {
  if (!m_revealed)
    return Result<bool, RecoveryCodeError>::failure(RecoveryCodeError::NotRevealed);
  if (candidate.size() > 128) {
    m_confirmed = false;
    return Result<bool, RecoveryCodeError>::success(false);
  }
  QByteArray expected = normalize(m_encoded);
  QByteArray supplied = normalize(candidate);
  const bool matches = expected.size() == supplied.size() &&
                       CRYPTO_memcmp(expected.constData(), supplied.constData(),
                                     static_cast<std::size_t>(expected.size())) == 0;
  OPENSSL_cleanse(expected.data(), static_cast<std::size_t>(expected.size()));
  OPENSSL_cleanse(supplied.data(), static_cast<std::size_t>(supplied.size()));
  m_confirmed = matches;
  return Result<bool, RecoveryCodeError>::success(matches);
}

Result<SecureBuffer, RecoveryCodeError> RecoveryCode::takeSecret() {
  if (!m_confirmed)
    return Result<SecureBuffer, RecoveryCodeError>::failure(
        RecoveryCodeError::NotConfirmed);
  wipeEncoded();
  m_confirmed = false;
  return Result<SecureBuffer, RecoveryCodeError>::success(std::move(m_secret));
}

bool RecoveryCode::isConfirmed() const noexcept { return m_confirmed; }

void RecoveryCode::wipeEncoded() noexcept {
  if (!m_encoded.isEmpty())
    OPENSSL_cleanse(m_encoded.data(), static_cast<std::size_t>(m_encoded.size()));
  m_encoded.clear();
}

} // namespace OpenChat
