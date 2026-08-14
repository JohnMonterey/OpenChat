#pragma once

#include <QByteArrayView>

#include <cstddef>
#include <vector>

namespace OpenChat {

class SecureBuffer final {
public:
  SecureBuffer() = default;
  ~SecureBuffer();

  SecureBuffer(const SecureBuffer &) = delete;
  SecureBuffer &operator=(const SecureBuffer &) = delete;

  SecureBuffer(SecureBuffer &&other) noexcept;
  SecureBuffer &operator=(SecureBuffer &&other) noexcept;

  [[nodiscard]] static SecureBuffer fromBytes(QByteArrayView bytes);
  [[nodiscard]] static SecureBuffer random(qsizetype size);

  [[nodiscard]] QByteArrayView view() const noexcept;
  [[nodiscard]] qsizetype size() const noexcept;
  [[nodiscard]] bool isEmpty() const noexcept;

private:
  explicit SecureBuffer(std::vector<unsigned char> bytes);

  void lockMemory() noexcept;
  void wipeAndRelease() noexcept;

  std::vector<unsigned char> m_bytes;
  bool m_locked = false;
};

} // namespace OpenChat
