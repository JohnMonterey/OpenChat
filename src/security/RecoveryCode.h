#pragma once

#include "core/Result.h"
#include "security/SecureBuffer.h"

#include <QByteArray>
#include <QByteArrayView>

namespace OpenChat {

enum class RecoveryCodeError {
  GenerationFailed,
  AlreadyRevealed,
  NotRevealed,
  NotConfirmed
};

class RecoveryCode final {
public:
  ~RecoveryCode();
  RecoveryCode(RecoveryCode &&other) noexcept;
  RecoveryCode &operator=(RecoveryCode &&other) noexcept;
  RecoveryCode(const RecoveryCode &) = delete;
  RecoveryCode &operator=(const RecoveryCode &) = delete;

  [[nodiscard]] static Result<RecoveryCode, RecoveryCodeError> generate();
  [[nodiscard]] Result<QByteArray, RecoveryCodeError> reveal();
  [[nodiscard]] Result<bool, RecoveryCodeError> confirm(QByteArrayView candidate);
  [[nodiscard]] Result<SecureBuffer, RecoveryCodeError> takeSecret();
  [[nodiscard]] bool isConfirmed() const noexcept;

private:
  RecoveryCode(SecureBuffer secret, QByteArray encoded);

  SecureBuffer m_secret;
  QByteArray m_encoded;
  bool m_revealed = false;
  bool m_confirmed = false;

  void wipeEncoded() noexcept;
};

} // namespace OpenChat
