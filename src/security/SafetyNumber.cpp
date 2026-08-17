#include "security/SafetyNumber.h"

#include <QCryptographicHash>
#include <QLatin1Char>

namespace OpenChat {
namespace {

constexpr qsizetype kSigningKeySize = 32;
constexpr qsizetype kAccountIdSize = 16;
constexpr int kIterations = 5200;
constexpr qsizetype kFingerprintSize = 30;
constexpr quint64 kGroupModulus = 100000;

// version(2) = {0x00, 0x01}, prepended to the first hash input.
QByteArray versionPrefix()
{
    QByteArray prefix;
    prefix.append(char(0x00));
    prefix.append(char(0x01));
    return prefix;
}

// Per-party 30-byte fingerprint: hash = version || key || id, then iterated
// hash = SHA512(hash || key), keeping the first 30 bytes of the final digest.
QByteArray partyFingerprint(QByteArrayView key, QByteArrayView id)
{
    QByteArray hash = versionPrefix();
    hash.append(key.data(), key.size());
    hash.append(id.data(), id.size());
    for (int round = 0; round < kIterations; ++round) {
        QCryptographicHash sha(QCryptographicHash::Sha512);
        sha.addData(hash);
        sha.addData(key);
        hash = sha.result();
    }
    return hash.left(kFingerprintSize);
}

// Encodes a 30-byte fingerprint into 30 decimal digits: for each of the six
// 5-byte groups, read a big-endian unsigned 40-bit integer, reduce modulo
// 100000, and zero-pad to exactly five digits.
QString encodeDigits(QByteArrayView fingerprint30)
{
    QString digits;
    digits.reserve(30);
    for (qsizetype offset = 0; offset < kFingerprintSize; offset += 5) {
        quint64 value = 0;
        for (qsizetype byte = 0; byte < 5; ++byte)
            value = (value << 8) | static_cast<quint8>(fingerprint30[offset + byte]);
        const quint64 group = value % kGroupModulus;
        digits += QStringLiteral("%1").arg(static_cast<qulonglong>(group), 5, 10,
                                           QLatin1Char('0'));
    }
    return digits;
}

} // namespace

Result<QString, SafetyNumberError> computeSafetyNumber(
    QByteArrayView signingKeyLocal, QByteArrayView accountIdLocal,
    QByteArrayView signingKeyPeer, QByteArrayView accountIdPeer)
{
    if (signingKeyLocal.size() != kSigningKeySize || signingKeyPeer.size() != kSigningKeySize)
        return Result<QString, SafetyNumberError>::failure(SafetyNumberError::InvalidKeySize);
    if (accountIdLocal.size() != kAccountIdSize || accountIdPeer.size() != kAccountIdSize)
        return Result<QString, SafetyNumberError>::failure(SafetyNumberError::InvalidIdSize);

    // The canonical tuple key||id per party fixes the block order so both sides
    // emit the identical concatenation regardless of who is "local".
    QByteArray localTuple;
    localTuple.append(signingKeyLocal.data(), signingKeyLocal.size());
    localTuple.append(accountIdLocal.data(), accountIdLocal.size());
    QByteArray peerTuple;
    peerTuple.append(signingKeyPeer.data(), signingKeyPeer.size());
    peerTuple.append(accountIdPeer.data(), accountIdPeer.size());

    const QString localDigits = encodeDigits(partyFingerprint(signingKeyLocal, accountIdLocal));
    const QString peerDigits = encodeDigits(partyFingerprint(signingKeyPeer, accountIdPeer));

    // Smaller tuple first; equal tuples (self-contact) make the order moot.
    const QString number =
        localTuple <= peerTuple ? localDigits + peerDigits : peerDigits + localDigits;
    return Result<QString, SafetyNumberError>::success(number);
}

QString safetyNumberDigitsForTest(QByteArrayView fingerprint30)
{
    if (fingerprint30.size() != kFingerprintSize)
        return {};
    return encodeDigits(fingerprint30);
}

} // namespace OpenChat
