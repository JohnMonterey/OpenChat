#pragma once

#include "core/Result.h"
#include "protocol/CiphertextEnvelope.h"

#include <QByteArray>
#include <QByteArrayView>

namespace OpenChat {

inline constexpr qsizetype maxEnvelopeBytes = 1024 * 1024;
inline constexpr qsizetype maxCiphertextBytes = 960 * 1024;
inline constexpr int maxCborDepth = 8;

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
[[nodiscard]] Result<CiphertextEnvelopeV1, DecodeError>
decodeEnvelope(QByteArrayView encoded, DecodeLimits limits = {});

} // namespace OpenChat
