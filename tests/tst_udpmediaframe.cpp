#include "protocol/UdpMediaFrame.h"

#include <QtTest>

using namespace OpenChat;

class UdpMediaFrameTest final : public QObject
{
    Q_OBJECT

private slots:
    void roundTripAllTypes();
    void boundsAndRejections();
    void factoryHelpersAndIdentifiers();
    void maxDatagramCap();
};

void UdpMediaFrameTest::roundTripAllTypes()
{
    const auto sender = DeviceId::generate();
    const auto recipient = DeviceId::generate();
    const QByteArray token(32, 'T');
    const QByteArray mediaPayload = QByteArray::fromHex("0102030405060708090a");
    const QByteArray pingPayload = QByteArray::number(1234567890LL);

    // Hello
    {
        const auto frame = UdpMediaFrame::makeHello(sender, token);
        const auto encoded = frame.encode();
        QVERIFY(!encoded.isEmpty());
        const auto decoded = UdpMediaFrame::decode(encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->version, UdpMediaFrame::currentProtocolVersion);
        QCOMPARE(decoded->type, UdpMediaFrame::Type::Hello);
        QCOMPARE(decoded->senderDeviceId, sender.bytes());
        QVERIFY(decoded->recipientDeviceId.isEmpty());
        QCOMPARE(decoded->payload, token);
        QCOMPARE(decoded->senderId(), sender);
        QCOMPARE(decoded->recipientId(), std::nullopt);
    }

    // HelloOk
    {
        const auto frame = UdpMediaFrame::makeHelloOk(recipient);
        const auto encoded = frame.encode();
        QVERIFY(!encoded.isEmpty());
        const auto decoded = UdpMediaFrame::decode(encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->type, UdpMediaFrame::Type::HelloOk);
        QVERIFY(decoded->senderDeviceId.isEmpty());
        QCOMPARE(decoded->recipientDeviceId, recipient.bytes());
        QVERIFY(decoded->payload.isEmpty());
        QCOMPARE(decoded->recipientId(), recipient);
    }

    // HelloErr
    {
        const auto frame = UdpMediaFrame::makeHelloErr(recipient, "expired");
        const auto encoded = frame.encode();
        QVERIFY(!encoded.isEmpty());
        const auto decoded = UdpMediaFrame::decode(encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->type, UdpMediaFrame::Type::HelloErr);
        QCOMPARE(decoded->recipientDeviceId, recipient.bytes());
        QCOMPARE(decoded->payload, QByteArray("expired"));
    }

    // Media
    {
        const auto frame = UdpMediaFrame::makeMedia(sender, recipient, mediaPayload);
        const auto encoded = frame.encode();
        QVERIFY(!encoded.isEmpty());
        const auto decoded = UdpMediaFrame::decode(encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->type, UdpMediaFrame::Type::Media);
        QCOMPARE(decoded->senderDeviceId, sender.bytes());
        QCOMPARE(decoded->recipientDeviceId, recipient.bytes());
        QCOMPARE(decoded->payload, mediaPayload);
        QCOMPARE(decoded->senderId(), sender);
        QCOMPARE(decoded->recipientId(), recipient);
    }

    // Ping
    {
        const auto frame = UdpMediaFrame::makePing(sender, recipient, pingPayload);
        const auto encoded = frame.encode();
        QVERIFY(!encoded.isEmpty());
        const auto decoded = UdpMediaFrame::decode(encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->type, UdpMediaFrame::Type::Ping);
        QCOMPARE(decoded->senderDeviceId, sender.bytes());
        QCOMPARE(decoded->recipientDeviceId, recipient.bytes());
        QCOMPARE(decoded->payload, pingPayload);
    }

    // Pong
    {
        const auto frame = UdpMediaFrame::makePong(sender, recipient, pingPayload);
        const auto encoded = frame.encode();
        QVERIFY(!encoded.isEmpty());
        const auto decoded = UdpMediaFrame::decode(encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->type, UdpMediaFrame::Type::Pong);
        QCOMPARE(decoded->senderDeviceId, sender.bytes());
        QCOMPARE(decoded->recipientDeviceId, recipient.bytes());
        QCOMPARE(decoded->payload, pingPayload);
    }

    // Bye
    {
        const auto frame = UdpMediaFrame::makeBye(sender, recipient);
        const auto encoded = frame.encode();
        QVERIFY(!encoded.isEmpty());
        const auto decoded = UdpMediaFrame::decode(encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->type, UdpMediaFrame::Type::Bye);
        QCOMPARE(decoded->senderDeviceId, sender.bytes());
        QCOMPARE(decoded->recipientDeviceId, recipient.bytes());
        QVERIFY(decoded->payload.isEmpty());
    }
}

void UdpMediaFrameTest::boundsAndRejections()
{
    // Empty buffer
    QVERIFY(!UdpMediaFrame::decode(QByteArray{}).has_value());

    // Less than 4 bytes minimum header
    QVERIFY(!UdpMediaFrame::decode(QByteArray("\x01\x01\x00", 3)).has_value());

    // Wrong proto version (!= 1)
    QVERIFY(!UdpMediaFrame::decode(QByteArray("\x02\x01\x00\x00", 4)).has_value());

    // Unknown type (< 1 or > 7)
    QVERIFY(!UdpMediaFrame::decode(QByteArray("\x01\x00\x00\x00", 4)).has_value());
    QVERIFY(!UdpMediaFrame::decode(QByteArray("\x01\x08\x00\x00", 4)).has_value());

    // Truncated sender len
    // Header indicates senderLen = 10, but total length is only 5
    QVERIFY(!UdpMediaFrame::decode(QByteArray("\x01\x04\x0a\x00\x00", 5)).has_value());

    // Truncated recipient len
    // Header has senderLen = 2, recipientLen = 10, but truncated
    QVERIFY(!UdpMediaFrame::decode(QByteArray("\x01\x04\x02\xaa\xbb\x0a\xcc", 7)).has_value());

    // Hello with invalid token length (!= 32 bytes)
    const auto sender = DeviceId::generate();
    QByteArray shortHello("\x01\x01\x10", 3);
    shortHello.append(sender.bytes());
    shortHello.append('\x00'); // recipientLen = 0
    shortHello.append(QByteArray(16, 'x')); // only 16 bytes payload, expected 32
    QVERIFY(!UdpMediaFrame::decode(shortHello).has_value());

    QByteArray longHello("\x01\x01\x10", 3);
    longHello.append(sender.bytes());
    longHello.append('\x00');
    longHello.append(QByteArray(33, 'x')); // 33 bytes payload, expected 32
    QVERIFY(!UdpMediaFrame::decode(longHello).has_value());
}

void UdpMediaFrameTest::factoryHelpersAndIdentifiers()
{
    const auto sender = DeviceId::generate();
    const auto recipient = DeviceId::generate();

    const auto frame = UdpMediaFrame::makeMedia(sender, recipient, "payload");
    QCOMPARE(frame.senderId(), sender);
    QCOMPARE(frame.recipientId(), recipient);

    // Invalid length device ID bytes
    UdpMediaFrame invalidIdFrame = frame;
    invalidIdFrame.senderDeviceId = QByteArray(15, 'z'); // invalid DeviceId length
    QCOMPARE(invalidIdFrame.senderId(), std::nullopt);
}

void UdpMediaFrameTest::maxDatagramCap()
{
    const auto sender = DeviceId::generate();
    const auto recipient = DeviceId::generate();

    // 1 (proto) + 1 (type) + 1 (senderLen) + 16 (sender) + 1 (recipientLen) + 16 (recipient) = 36 bytes header
    // Max datagram = 1400 bytes, so max payload = 1400 - 36 = 1364 bytes.
    const QByteArray exactMaxPayload(1364, 'X');
    const auto validFrame = UdpMediaFrame::makeMedia(sender, recipient, exactMaxPayload);
    const auto encodedValid = validFrame.encode();
    QCOMPARE(encodedValid.size(), 1400);
    QVERIFY(UdpMediaFrame::decode(encodedValid).has_value());

    // Exceeding 1400 bytes
    const QByteArray oversizePayload(1365, 'X');
    const auto oversizeFrame = UdpMediaFrame::makeMedia(sender, recipient, oversizePayload);
    const auto encodedOversize = oversizeFrame.encode();
    QVERIFY(encodedOversize.isEmpty()); // encode rejects > 1400

    // Manually craft 1401 bytes datagram and verify decode rejects
    QByteArray rawOversize = encodedValid;
    rawOversize.append('Y');
    QCOMPARE(rawOversize.size(), 1401);
    QVERIFY(!UdpMediaFrame::decode(rawOversize).has_value());
}

QTEST_MAIN(UdpMediaFrameTest)
#include "tst_udpmediaframe.moc"
