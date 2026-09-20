#pragma once

#include "protocol/CiphertextEnvelope.h"

#include <QByteArray>
#include <QByteArrayView>

#include <optional>

namespace OpenChat::Relay {

// Cryptographic helpers for the relay. The relay only ever handles PUBLIC key
// material and opaque ciphertext: it verifies Ed25519 signatures with published
// device keys, hashes tokens, and generates random challenges/tokens. No
// private key or message plaintext is ever present.

// Verifies an Ed25519 signature. pubKey must be 32 bytes and signature 64
// bytes; any size mismatch returns false. Constant-time properties are provided
// by the underlying OpenSSL implementation.
[[nodiscard]] bool verifyEd25519(QByteArrayView pubKey, QByteArrayView message,
                                 QByteArrayView signature);

// SHA-256 of the input (used to store token hashes and de-duplicate uploads).
[[nodiscard]] QByteArray sha256(QByteArrayView data);

// Cryptographically secure random bytes (OpenSSL RAND_bytes). Returns an empty
// array on failure, which callers must treat as fatal.
[[nodiscard]] QByteArray randomBytes(int count);

// Relay-side cost of hashing a client password key. The expensive, password-
// guessing-resistant stretch already happened on the client (see
// security/PasswordKey.h); this second pass exists so that a leaked accounts
// table cannot be replayed as a login. It is therefore tuned to the OWASP
// minimum rather than the client's 64 MiB, because it runs on the relay's single
// event-loop thread for an unauthenticated request.
struct PasswordHashParams final {
    quint32 memoryKiB = 19 * 1024;
    quint32 iterations = 2;

    // Stable text form persisted next to each hash ("argon2id$m=19456,t=2").
    [[nodiscard]] QByteArray serialize() const;
    [[nodiscard]] static std::optional<PasswordHashParams> parse(QByteArrayView text);
};

inline constexpr int passwordKeyBytes = 32;
inline constexpr int passwordSaltBytes = 16;
inline constexpr int passwordHashBytes = 32;

// Argon2id(passwordKey, salt) -> 32 bytes, single lane. Returns an empty array
// when the inputs are the wrong size or the KDF is unavailable, which callers
// must treat as a failure (never as a matching hash).
[[nodiscard]] QByteArray hashPasswordKey(QByteArrayView passwordKey, QByteArrayView salt,
                                         const PasswordHashParams &params);

// Length-checked constant-time equality, for comparing secret-derived bytes.
[[nodiscard]] bool constantTimeEquals(QByteArrayView left, QByteArrayView right);

// Reconstructs the exact message a device signs for challenge authentication,
// matching DeviceIdentity::signChallenge: the domain-separation label followed
// by the uint32 big-endian length-prefixed context and challenge.
[[nodiscard]] QByteArray challengeSigningMessage(QByteArrayView challenge,
                                                 QByteArrayView context);

// Reconstructs the exact bytes covered by CiphertextEnvelopeV1::senderSignature:
// the canonical envelope encoding with the signature field cleared. The sending
// client signs this same input.
[[nodiscard]] QByteArray envelopeSigningInput(const CiphertextEnvelopeV1 &envelope);

} // namespace OpenChat::Relay
