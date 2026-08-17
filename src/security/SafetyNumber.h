#pragma once

#include "core/Result.h"

#include <QByteArrayView>
#include <QString>

namespace OpenChat {

enum class SafetyNumberError {
    InvalidKeySize,
    InvalidIdSize,
};

// Derives the raw 60-decimal-digit contact safety number from the two parties'
// authenticated Ed25519 signing keys (exactly 32 bytes each) and account ids
// (exactly 16 bytes each). Deterministic and symmetric: swapping (local, peer)
// yields an identical number, so both sides display the same code to compare
// out-of-band and rule out a man-in-the-middle. The caller formats the digits
// (e.g. into spaced groups); this returns them raw with no spaces.
[[nodiscard]] Result<QString, SafetyNumberError> computeSafetyNumber(
    QByteArrayView signingKeyLocal, QByteArrayView accountIdLocal,
    QByteArrayView signingKeyPeer, QByteArrayView accountIdPeer);

// Testing seam: encodes a 30-byte fingerprint into its 30 decimal digits (six
// big-endian 40-bit groups, each reduced modulo 100000 and zero-padded to five
// digits). Exposed only to pin the digit encoding in unit tests; returns an
// empty string for any input that is not exactly 30 bytes.
[[nodiscard]] QString safetyNumberDigitsForTest(QByteArrayView fingerprint30);

} // namespace OpenChat
