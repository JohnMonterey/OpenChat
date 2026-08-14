#include "security/SecureBuffer.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <limits>
#include <stdexcept>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <sys/mman.h>
#endif

namespace OpenChat {

SecureBuffer::SecureBuffer(std::vector<unsigned char> bytes)
    : m_bytes(std::move(bytes)) {
  lockMemory();
}

SecureBuffer::~SecureBuffer() { wipeAndRelease(); }

SecureBuffer::SecureBuffer(SecureBuffer &&other) noexcept
    : m_bytes(std::move(other.m_bytes)), m_locked(other.m_locked) {
  other.m_locked = false;
  other.m_bytes.clear();
}

SecureBuffer &SecureBuffer::operator=(SecureBuffer &&other) noexcept {
  if (this == &other)
    return *this;

  wipeAndRelease();
  m_bytes = std::move(other.m_bytes);
  m_locked = other.m_locked;
  other.m_locked = false;
  other.m_bytes.clear();
  return *this;
}

SecureBuffer SecureBuffer::fromBytes(QByteArrayView bytes) {
  if (bytes.isEmpty())
    return {};
  const auto *first = reinterpret_cast<const unsigned char *>(bytes.data());
  return SecureBuffer(std::vector<unsigned char>(first, first + bytes.size()));
}

SecureBuffer SecureBuffer::random(qsizetype size) {
  if (size <= 0 || size > std::numeric_limits<int>::max())
    throw std::invalid_argument("SecureBuffer size must be positive");

  std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
  if (RAND_priv_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    OPENSSL_cleanse(bytes.data(), bytes.size());
    throw std::runtime_error("The operating system random source failed");
  }
  return SecureBuffer(std::move(bytes));
}

QByteArrayView SecureBuffer::view() const noexcept {
  return QByteArrayView(reinterpret_cast<const char *>(m_bytes.data()),
                        static_cast<qsizetype>(m_bytes.size()));
}

qsizetype SecureBuffer::size() const noexcept {
  return static_cast<qsizetype>(m_bytes.size());
}

bool SecureBuffer::isEmpty() const noexcept { return m_bytes.empty(); }

void SecureBuffer::lockMemory() noexcept {
  if (m_bytes.empty())
    return;

#if defined(Q_OS_WIN)
  m_locked = VirtualLock(m_bytes.data(), m_bytes.size()) != 0;
#elif defined(Q_OS_UNIX)
  m_locked = mlock(m_bytes.data(), m_bytes.size()) == 0;
#endif
}

void SecureBuffer::wipeAndRelease() noexcept {
  if (m_bytes.empty())
    return;

  OPENSSL_cleanse(m_bytes.data(), m_bytes.size());
#if defined(Q_OS_WIN)
  if (m_locked)
    VirtualUnlock(m_bytes.data(), m_bytes.size());
#elif defined(Q_OS_UNIX)
  if (m_locked)
    munlock(m_bytes.data(), m_bytes.size());
#endif
  m_locked = false;
  m_bytes.clear();
  m_bytes.shrink_to_fit();
}

} // namespace OpenChat
