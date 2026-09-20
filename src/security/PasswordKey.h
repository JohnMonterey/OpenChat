#pragma once

#include "core/Result.h"
#include "security/SecureBuffer.h"

#include <QString>

namespace OpenChat {

// Turns an account password into the only thing that is ever sent to the relay.
//
// The password itself never leaves this device -- not to the relay, and not to
// the TLS-terminating proxy in front of it. It is stretched locally with
// Argon2id into a 32-byte password key, salted by the account's canonical handle
// so the same password under two handles yields unrelated keys. The relay hashes
// that key again under its own random salt before storing it, so neither a relay
// database leak nor an observer of the request learns the password or anything
// reusable on another service.
//
// These parameters are part of the login protocol: changing any of them changes
// every derived key, so a change must introduce a new version number rather than
// edit version 1 (the relay records the version per account).
inline constexpr int passwordKeyVersion = 1;
inline constexpr int passwordKeyBytes = 32;
inline constexpr quint32 passwordKeyMemoryKiB = 64 * 1024; // 64 MiB
inline constexpr quint32 passwordKeyIterations = 3;

// What the sign-up form enforces. The relay cannot check this (it never sees the
// password), so it is the client's responsibility alone.
inline constexpr qsizetype minimumPasswordLength = 10;
inline constexpr qsizetype maximumPasswordLength = 256;

enum class PasswordKeyError {
  InvalidHandle,   // the handle is not canonicalizable (domain/Handle.h)
  InvalidPassword, // empty, or longer than maximumPasswordLength
  DerivationFailed // Argon2id unavailable or out of memory
};

// Derives the password key for (handle, password). The handle is canonicalized
// first and the password is NFKC-normalized, so the same account and password
// typed on any platform or keyboard derive the same key. Takes roughly a few
// hundred milliseconds and 64 MiB; call it off the UI thread.
[[nodiscard]] Result<SecureBuffer, PasswordKeyError>
derivePasswordKey(const QString &handle, const QString &password);

} // namespace OpenChat
