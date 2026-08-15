#pragma once

#include "protocol/CiphertextEnvelope.h"

#include <QByteArray>
#include <QByteArrayView>

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
