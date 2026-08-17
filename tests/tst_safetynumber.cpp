#include "security/SafetyNumber.h"

#include <QtTest/QTest>

#include <array>

using namespace OpenChat;

namespace {

QByteArray key(char fill)
{
    return QByteArray(32, fill);
}

QByteArray accountId(char fill)
{
    return QByteArray(16, fill);
}

} // namespace

class SafetyNumberTest final : public QObject
{
    Q_OBJECT

private slots:
    void isDeterministic();
    void isSymmetric();
    void isSensitiveToEachInput();
    void hasSixtyDecimalDigits();
    void encodesFortyBitGroupsModuloHundredThousand();
    void rejectsWrongKeySize();
    void rejectsWrongIdSize();
};

void SafetyNumberTest::isDeterministic()
{
    auto first = computeSafetyNumber(key('\x01'), accountId('\x02'), key('\x03'), accountId('\x04'));
    auto second = computeSafetyNumber(key('\x01'), accountId('\x02'), key('\x03'), accountId('\x04'));
    QVERIFY(first);
    QVERIFY(second);
    QCOMPARE(first.value(), second.value());
}

void SafetyNumberTest::isSymmetric()
{
    const QByteArray keyA = key('\x11');
    const QByteArray idA = accountId('\x22');
    const QByteArray keyB = key('\x33');
    const QByteArray idB = accountId('\x44');

    auto forward = computeSafetyNumber(keyA, idA, keyB, idB);
    auto swapped = computeSafetyNumber(keyB, idB, keyA, idA);
    QVERIFY(forward);
    QVERIFY(swapped);
    // The core property: both parties derive the same number regardless of
    // which side is treated as "local".
    QCOMPARE(forward.value(), swapped.value());
}

void SafetyNumberTest::isSensitiveToEachInput()
{
    const QByteArray keyA = key('\x11');
    const QByteArray idA = accountId('\x22');
    const QByteArray keyB = key('\x33');
    const QByteArray idB = accountId('\x44');

    auto base = computeSafetyNumber(keyA, idA, keyB, idB);
    QVERIFY(base);

    QByteArray localKeyFlipped = keyA;
    localKeyFlipped[0] = static_cast<char>(localKeyFlipped[0] ^ 0x01);
    auto d1 = computeSafetyNumber(localKeyFlipped, idA, keyB, idB);
    QVERIFY(d1);
    QVERIFY(d1.value() != base.value());

    QByteArray localIdFlipped = idA;
    localIdFlipped[5] = static_cast<char>(localIdFlipped[5] ^ 0x01);
    auto d2 = computeSafetyNumber(keyA, localIdFlipped, keyB, idB);
    QVERIFY(d2);
    QVERIFY(d2.value() != base.value());

    QByteArray peerKeyFlipped = keyB;
    peerKeyFlipped[31] = static_cast<char>(peerKeyFlipped[31] ^ 0x01);
    auto d3 = computeSafetyNumber(keyA, idA, peerKeyFlipped, idB);
    QVERIFY(d3);
    QVERIFY(d3.value() != base.value());

    QByteArray peerIdFlipped = idB;
    peerIdFlipped[15] = static_cast<char>(peerIdFlipped[15] ^ 0x01);
    auto d4 = computeSafetyNumber(keyA, idA, keyB, peerIdFlipped);
    QVERIFY(d4);
    QVERIFY(d4.value() != base.value());
}

void SafetyNumberTest::hasSixtyDecimalDigits()
{
    auto number = computeSafetyNumber(key('\x01'), accountId('\x02'), key('\x03'), accountId('\x04'));
    QVERIFY(number);
    QCOMPARE(number.value().size(), qsizetype(60));
    for (const QChar character : number.value())
        QVERIFY(character.isDigit());
}

void SafetyNumberTest::encodesFortyBitGroupsModuloHundredThousand()
{
    const auto firstGroupFor = [](std::array<char, 5> bytes) {
        QByteArray fingerprint(30, '\0');
        for (int i = 0; i < 5; ++i)
            fingerprint[i] = bytes[i];
        return safetyNumberDigitsForTest(fingerprint).left(5);
    };

    // 0x0102030405 = 4328719365; 4328719365 % 100000 = 19365.
    QCOMPARE(firstGroupFor({'\x01', '\x02', '\x03', '\x04', '\x05'}), QStringLiteral("19365"));
    // 0xFFFFFFFFFF = 1099511627775; 1099511627775 % 100000 = 27775.
    QCOMPARE(firstGroupFor({'\xFF', '\xFF', '\xFF', '\xFF', '\xFF'}), QStringLiteral("27775"));
    // All-zero group encodes to "00000".
    QCOMPARE(firstGroupFor({'\x00', '\x00', '\x00', '\x00', '\x00'}), QStringLiteral("00000"));

    // A zero fingerprint encodes to 30 zero digits across all six groups.
    QCOMPARE(safetyNumberDigitsForTest(QByteArray(30, '\0')), QString(30, QLatin1Char('0')));
    // Wrong-sized input is rejected by the seam.
    QVERIFY(safetyNumberDigitsForTest(QByteArray(29, '\0')).isEmpty());
}

void SafetyNumberTest::rejectsWrongKeySize()
{
    auto shortLocalKey =
        computeSafetyNumber(QByteArray(31, '\x01'), accountId('\x02'), key('\x03'), accountId('\x04'));
    QVERIFY(!shortLocalKey);
    QCOMPARE(shortLocalKey.error(), SafetyNumberError::InvalidKeySize);

    auto longPeerKey =
        computeSafetyNumber(key('\x01'), accountId('\x02'), QByteArray(33, '\x03'), accountId('\x04'));
    QVERIFY(!longPeerKey);
    QCOMPARE(longPeerKey.error(), SafetyNumberError::InvalidKeySize);
}

void SafetyNumberTest::rejectsWrongIdSize()
{
    auto shortLocalId =
        computeSafetyNumber(key('\x01'), QByteArray(15, '\x02'), key('\x03'), accountId('\x04'));
    QVERIFY(!shortLocalId);
    QCOMPARE(shortLocalId.error(), SafetyNumberError::InvalidIdSize);

    auto longPeerId =
        computeSafetyNumber(key('\x01'), accountId('\x02'), key('\x03'), QByteArray(17, '\x04'));
    QVERIFY(!longPeerId);
    QCOMPARE(longPeerId.error(), SafetyNumberError::InvalidIdSize);
}

QTEST_GUILESS_MAIN(SafetyNumberTest)
#include "tst_safetynumber.moc"
