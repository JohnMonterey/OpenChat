#pragma once

#include "core/Result.h"
#include "protocol/CiphertextEnvelope.h"

#include <QByteArray>
#include <QByteArrayView>

namespace OpenChat {

inline constexpr qsizetype maxEnvelopeBytes = 1024 * 1024;
inline constexpr qsizetype maxCiphertextBytes = 960 * 1024;
inline constexpr int maxCborDepth = 8;
// The longest expiry - creation span an envelope may declare. The relay keeps an
// envelope for an offline recipient until it expires, so this bounds how long a
// message can wait for its recipient to come back.
inline constexpr qint64 maxEnvelopeLifetimeMs = 30LL * 24 * 60 * 60 * 1000;

struct DecodeLimits final {
    qsizetype envelopeBytes = maxEnvelopeBytes;
    qsizetype ciphertextBytes = maxCiphertextBytes;
    int cborDepth = maxCborDepth;
};

enum class DecodeError {
    FrameTooLarge,
    Truncated,
    Malformed,
    NonCanonical,
    DuplicateField,
    MissingField,
    UnknownCriticalField,
    UnsupportedVersion,
    InvalidFieldType,
    InvalidFieldLength,
    InvalidFieldValue,
    InvalidExpiry,
    CiphertextTooLarge,
    HashMismatch,
    DepthLimitExceeded,
    TrailingData,
};

[[nodiscard]] QByteArray encodeCanonical(const CiphertextEnvelopeV1 &envelope);

// The exact bytes covered by CiphertextEnvelopeV1::senderSignature: the canonical
// encoding with the signature field cleared. Signer and verifier MUST both derive
// the signed input from this one definition so the two can never drift apart.
[[nodiscard]] QByteArray encodeForSignature(const CiphertextEnvelopeV1 &envelope);

[[nodiscard]] Result<CiphertextEnvelopeV1, DecodeError>
decodeEnvelope(QByteArrayView encoded, DecodeLimits limits = {});

} // namespace OpenChat
