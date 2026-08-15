#include "protocol/CanonicalCborCodec.h"

#include <QCryptographicHash>
#include <QFile>
#include <QtTest/QTest>

using namespace OpenChat;

namespace {

template <typename Id>
Id idFromHex(const char *hex)
{
    const auto id = Id::fromBytes(QByteArray::fromHex(hex));
    Q_ASSERT(id.has_value());
    return *id;
}

CiphertextEnvelopeV1 fixedEnvelope()
{
    const QByteArray ciphertext("hello");
    return CiphertextEnvelopeV1{
        1,
        idFromHex<EnvelopeId>("000102030405060708090a0b0c0d0e0f"),
        idFromHex<AccountId>("101112131415161718191a1b1c1d1e1f"),
        idFromHex<DeviceId>("202122232425262728292a2b2c2d2e2f"),
        idFromHex<DeviceId>("303132333435363738393a3b3c3d3e3f"),
        idFromHex<ConversationId>("404142434445464748494a4b4c4d4e4f"),
        EnvelopeMessageKind::MlsPrivateMessage,
        1'700'000'000'000,
        1'700'000'060'000,
        idFromHex<EnvelopeId>("505152535455565758595a5b5c5d5e5f"),
        ciphertext,
        QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256),
        QByteArray::fromHex(
            "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
            "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f")};
}

struct Fixture final {
    QByteArray bytes;
    QString error;
};

Fixture goldenFixture()
{
    QFile file(QStringLiteral(OPENCHAT_SOURCE_DIR "/tests/fixtures/envelope-v1.cbor.hex"));
    if (!file.open(QIODevice::ReadOnly))
        return {{}, file.errorString()};
    return {QByteArray::fromHex(file.readAll().trimmed()), {}};
}

} // namespace

class EnvelopeCodecTest final : public QObject
{
    Q_OBJECT

private slots:
    void canonicalEncodingMatchesGoldenFixture();
    void canonicalEnvelopeRoundTrips();
    void rejectsOversizeBeforeParsing();
    void rejectsDuplicateMapKeys();
    void rejectsIndefiniteAndNoncanonicalForms();
    void rejectsWrongIdentifierSize();
    void rejectsInvalidExpiry();
    void rejectsMismatchedCiphertextHash();
    void rejectsTrailingBytes();
    void rejectsUnknownCriticalField();
    void rejectsMissingFieldsAndUnsupportedVersion();
    void rejectsTruncatedAndWrongSignatureSize();
    void acceptsBoundedNoncriticalExtension();
    void preservesNoncriticalExtensionOnRoundTrip();
    void rejectsInvalidLocalExtensionEncoding();
    void rejectsExtensionPastDepthLimit();
    void encodeAndDecodeAgreeOnExtensionDepthLimit();
    void rejectsDuplicateEmptyKeysInExtensionSubmap();
    void encodeForSignatureClearsSignatureField();
    void rejectsOversizeCiphertext();
    void honorsTighterDecodeLimits();
};

void EnvelopeCodecTest::canonicalEncodingMatchesGoldenFixture()
{
    const auto fixture = goldenFixture();
    QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
    QCOMPARE(encodeCanonical(fixedEnvelope()), fixture.bytes);
}

void EnvelopeCodecTest::canonicalEnvelopeRoundTrips()
{
    const auto expected = fixedEnvelope();
    auto decoded = decodeEnvelope(encodeCanonical(expected));
    QVERIFY(decoded.hasValue());
    QCOMPARE(decoded.value(), expected);
}

void EnvelopeCodecTest::rejectsOversizeBeforeParsing()
{
    const QByteArray hostile(maxEnvelopeBytes + 1, '\xff');
    auto decoded = decodeEnvelope(hostile);
    QVERIFY(!decoded.hasValue());
    QCOMPARE(decoded.error(), DecodeError::FrameTooLarge);
}

void EnvelopeCodecTest::rejectsDuplicateMapKeys()
{
    auto decoded = decodeEnvelope(QByteArray::fromHex("a200010001"));
    QVERIFY(!decoded.hasValue());
    QCOMPARE(decoded.error(), DecodeError::DuplicateField);
}

void EnvelopeCodecTest::rejectsIndefiniteAndNoncanonicalForms()
{
    QCOMPARE(decodeEnvelope(QByteArray::fromHex("bf0001ff")).error(),
             DecodeError::NonCanonical);
    QCOMPARE(decodeEnvelope(QByteArray::fromHex("a1001801")).error(),
             DecodeError::NonCanonical);
}

void EnvelopeCodecTest::rejectsWrongIdentifierSize()
{
    const auto fixture = goldenFixture();
    QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
    QByteArray encoded = fixture.bytes;
    encoded[4] = char(0x4f);
    encoded.remove(20, 1);
    QCOMPARE(decodeEnvelope(encoded).error(), DecodeError::InvalidFieldLength);
}

void EnvelopeCodecTest::rejectsInvalidExpiry()
{
    auto envelope = fixedEnvelope();
    envelope.expiresAtMs = envelope.createdAtMs;
    QCOMPARE(decodeEnvelope(encodeCanonical(envelope)).error(), DecodeError::InvalidExpiry);
}

void EnvelopeCodecTest::rejectsMismatchedCiphertextHash()
{
    const auto fixture = goldenFixture();
    QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
    QByteArray encoded = fixture.bytes;
    const qsizetype offset = encoded.indexOf("hello");
    QVERIFY(offset >= 0);
    encoded[offset] = 'j';
    QCOMPARE(decodeEnvelope(encoded).error(), DecodeError::HashMismatch);
}

void EnvelopeCodecTest::rejectsTrailingBytes()
{
    const auto fixture = goldenFixture();
    QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
    QByteArray encoded = fixture.bytes;
    encoded.append('\0');
    QCOMPARE(decodeEnvelope(encoded).error(), DecodeError::TrailingData);
}

void EnvelopeCodecTest::rejectsUnknownCriticalField()
{
    const auto fixture = goldenFixture();
    QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
    QByteArray encoded = fixture.bytes;
    encoded[0] = char(0xae);
    encoded.append(QByteArray::fromHex("0d00"));
    QCOMPARE(decodeEnvelope(encoded).error(), DecodeError::UnknownCriticalField);
}

void EnvelopeCodecTest::rejectsMissingFieldsAndUnsupportedVersion()
{
    QCOMPARE(decodeEnvelope(QByteArray::fromHex("a10001")).error(), DecodeError::MissingField);

    auto envelope = fixedEnvelope();
    envelope.version = 2;
    QCOMPARE(decodeEnvelope(encodeCanonical(envelope)).error(), DecodeError::UnsupportedVersion);
}

void EnvelopeCodecTest::rejectsTruncatedAndWrongSignatureSize()
{
    const auto fixture = goldenFixture();
    QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));

    QByteArray truncated = fixture.bytes;
    truncated.chop(1);
    QCOMPARE(decodeEnvelope(truncated).error(), DecodeError::Truncated);

    QByteArray wrongSignature = fixture.bytes;
    const qsizetype marker = wrongSignature.lastIndexOf(QByteArray::fromHex("0c5840"));
    QVERIFY(marker >= 0);
    wrongSignature[marker + 2] = char(0x3f);
    wrongSignature.chop(1);
    QCOMPARE(decodeEnvelope(wrongSignature).error(), DecodeError::InvalidFieldLength);
}

void EnvelopeCodecTest::acceptsBoundedNoncriticalExtension()
{
    const auto fixture = goldenFixture();
    QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
    QByteArray encoded = fixture.bytes;
    encoded[0] = char(0xae);
    encoded.append(QByteArray::fromHex("18804101"));
    QVERIFY(decodeEnvelope(encoded).hasValue());
}

void EnvelopeCodecTest::preservesNoncriticalExtensionOnRoundTrip()
{
    auto envelope = fixedEnvelope();
    envelope.noncriticalExtensions.insert(128, QByteArray::fromHex("a1616101"));

    const QByteArray encoded = encodeCanonical(envelope);
    QVERIFY(!encoded.isEmpty());
    const auto decoded = decodeEnvelope(encoded);
    QVERIFY(decoded.hasValue());
    QCOMPARE(decoded.value(), envelope);
    QCOMPARE(encodeCanonical(decoded.value()), encoded);
}

void EnvelopeCodecTest::rejectsInvalidLocalExtensionEncoding()
{
    auto envelope = fixedEnvelope();
    envelope.noncriticalExtensions.insert(13, QByteArray::fromHex("00"));
    QVERIFY(encodeCanonical(envelope).isEmpty());

    envelope.noncriticalExtensions.clear();
    envelope.noncriticalExtensions.insert(128, QByteArray::fromHex("1801"));
    QVERIFY(encodeCanonical(envelope).isEmpty());
}

void EnvelopeCodecTest::rejectsExtensionPastDepthLimit()
{
    const auto fixture = goldenFixture();
    QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
    QByteArray encoded = fixture.bytes;
    encoded[0] = char(0xae);
    encoded.append(QByteArray::fromHex("188081818181818181818100"));
    QCOMPARE(decodeEnvelope(encoded).error(), DecodeError::DepthLimitExceeded);
}

void EnvelopeCodecTest::encodeAndDecodeAgreeOnExtensionDepthLimit()
{
    // Six nested arrays around an integer put the innermost value exactly at the
    // decoder's depth budget: it round-trips through both sides.
    auto accepted = fixedEnvelope();
    accepted.noncriticalExtensions.insert(128, QByteArray::fromHex("81818181818100"));
    const QByteArray encoded = encodeCanonical(accepted);
    QVERIFY(!encoded.isEmpty());
    const auto decoded = decodeEnvelope(encoded);
    QVERIFY(decoded.hasValue());
    QCOMPARE(decoded.value(), accepted);

    // Seven nested arrays exceed that budget. The encoder must now reject it too
    // (previously it emitted a frame every decoder rejects), and a hand-built
    // frame with the same extension is rejected on decode — the two sides agree.
    auto rejected = fixedEnvelope();
    rejected.noncriticalExtensions.insert(128, QByteArray::fromHex("8181818181818100"));
    QVERIFY(encodeCanonical(rejected).isEmpty());

    const auto fixture = goldenFixture();
    QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
    QByteArray hostile = fixture.bytes;
    hostile[0] = char(0xae); // 14 map entries: the 13 required plus one extension
    hostile.append(QByteArray::fromHex("18808181818181818100"));
    QCOMPARE(decodeEnvelope(hostile).error(), DecodeError::DepthLimitExceeded);
}

void EnvelopeCodecTest::rejectsDuplicateEmptyKeysInExtensionSubmap()
{
    // A noncritical extension whose value is a map with two empty-bstr keys
    // (a2 40 00 40 01) must be rejected: the encoded key slice is never empty, so
    // the ordering/duplication check still fires on the second empty key.
    const auto fixture = goldenFixture();
    QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
    QByteArray hostile = fixture.bytes;
    hostile[0] = char(0xae); // 14 map entries: 13 required plus one extension
    // Extension key 128 (18 80) → value map a2 { 40:00, 40:01 } with duplicate
    // empty-bstr keys.
    hostile.append(QByteArray::fromHex("1880a240004001"));
    QCOMPARE(decodeEnvelope(hostile).error(), DecodeError::DuplicateField);
}

void EnvelopeCodecTest::encodeForSignatureClearsSignatureField()
{
    const auto signed_ = fixedEnvelope();
    auto unsigned_ = signed_;
    unsigned_.senderSignature.clear();

    // The signed input is the canonical encoding with the signature field empty,
    // regardless of whatever signature the envelope currently carries.
    QCOMPARE(encodeForSignature(signed_), encodeCanonical(unsigned_));
    QVERIFY(encodeForSignature(signed_) != encodeCanonical(signed_));

    // An envelope signed over that input still decodes and round-trips.
    auto resigned = unsigned_;
    resigned.senderSignature = signed_.senderSignature;
    const auto decoded = decodeEnvelope(encodeCanonical(resigned));
    QVERIFY(decoded.hasValue());
    QCOMPARE(decoded.value(), resigned);
}

void EnvelopeCodecTest::rejectsOversizeCiphertext()
{
    auto envelope = fixedEnvelope();
    envelope.ciphertext = QByteArray(maxCiphertextBytes + 1, 'x');
    envelope.ciphertextSha256 =
        QCryptographicHash::hash(envelope.ciphertext, QCryptographicHash::Sha256);
    QCOMPARE(decodeEnvelope(encodeCanonical(envelope)).error(), DecodeError::CiphertextTooLarge);
}

void EnvelopeCodecTest::honorsTighterDecodeLimits()
{
    const QByteArray encoded = encodeCanonical(fixedEnvelope());
    QVERIFY(!encoded.isEmpty());

    DecodeLimits frameLimits;
    frameLimits.envelopeBytes = encoded.size() - 1;
    QCOMPARE(decodeEnvelope(encoded, frameLimits).error(), DecodeError::FrameTooLarge);

    DecodeLimits ciphertextLimits;
    ciphertextLimits.ciphertextBytes = 4;
    QCOMPARE(decodeEnvelope(encoded, ciphertextLimits).error(),
             DecodeError::CiphertextTooLarge);
}

QTEST_GUILESS_MAIN(EnvelopeCodecTest)
#include "tst_envelopecodec.moc"
