#include <QtTest>
#include <QBuffer>
#include <cstring>
#include "call/CallVideoSession.h"
#include "call/CallMediaPacket.h"
#include "call/CallSignal.h"
#include "call/VideoFrameCopy.h"
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAbstractVideoBuffer>
#endif

using namespace OpenChat;

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
// CoreVideo commonly pads NV12 scanlines beyond the visible pixel width.
class PaddedCameraBuffer final : public QAbstractVideoBuffer
{
public:
    QByteArray y = QByteArray(64 * 18, char(0xee));
    QByteArray uv = QByteArray(64 * 9, char(0xee));
    bool truncated = false;
    PaddedCameraBuffer()
    {
        for (int row = 0; row < 18; ++row)
            for (int x = 0; x < 32; ++x)
                y[row * 64 + x] = char(20 + row);
        for (int row = 0; row < 9; ++row)
            for (int x = 0; x < 32; ++x)
                uv[row * 64 + x] = char(110 + row);
    }
    MapData map(QVideoFrame::MapMode) override
    {
        MapData data;
        data.planeCount = 2;
        data.bytesPerLine[0] = data.bytesPerLine[1] = 64;
        data.data[0] = reinterpret_cast<uchar *>(y.data());
        data.data[1] = reinterpret_cast<uchar *>(uv.data());
        data.dataSize[0] = int(y.size());
        data.dataSize[1] = truncated ? 4 : int(uv.size());
        return data;
    }
    QVideoFrameFormat format() const override
    {
        QVideoFrameFormat format(QSize(32, 18), QVideoFrameFormat::Format_NV12);
        format.setColorSpace(QVideoFrameFormat::ColorSpace_BT709);
        format.setColorRange(QVideoFrameFormat::ColorRange_Video);
        return format;
    }
};
#endif

class VideoCallTest final : public QObject
{
    Q_OBJECT
private slots:
    void cameraCopyOwnsItsPixels_data()
    {
        QTest::addColumn<QVideoFrameFormat::PixelFormat>("format");
        QTest::newRow("bgra") << QVideoFrameFormat::Format_BGRA8888;
        QTest::newRow("nv12") << QVideoFrameFormat::Format_NV12;
        QTest::newRow("yuv420") << QVideoFrameFormat::Format_YUV420P;
        QTest::newRow("p010") << QVideoFrameFormat::Format_P010;
    }
    void cameraCopyOwnsItsPixels()
    {
        QFETCH(QVideoFrameFormat::PixelFormat, format);
        QVideoFrame source(QVideoFrameFormat(QSize(32, 18), format));
        QVERIFY(source.map(QVideoFrame::WriteOnly));
        for (int p = 0; p < source.planeCount(); ++p)
            std::memset(source.bits(p), 50 + p, source.mappedBytes(p));
        source.unmap();
        source.setStartTime(12345);
        QVideoFrame owned = copyVideoFrameToMemory(source);
        QVERIFY(owned.isValid());
        QCOMPARE(owned.handleType(), QVideoFrame::NoHandle);
        QCOMPARE(owned.surfaceFormat(), source.surfaceFormat());
        QCOMPARE(owned.startTime(), source.startTime());
        QVERIFY(!source.isMapped());
        QVERIFY(!owned.isMapped());
        // Overwriting and releasing the camera's storage must not change the copy.
        QVERIFY(source.map(QVideoFrame::WriteOnly));
        for (int p = 0; p < source.planeCount(); ++p)
            std::memset(source.bits(p), 0, source.mappedBytes(p));
        source.unmap();
        source = QVideoFrame();
        QVERIFY(owned.map(QVideoFrame::ReadOnly));
        for (int p = 0; p < owned.planeCount(); ++p) {
            const int rows = p == 0 ? 18 : 9;
            for (int i = 0; i < rows * owned.bytesPerLine(p); ++i)
                QCOMPARE(owned.bits(p)[i], uchar(50 + p));
        }
        owned.unmap();
        QVERIFY(!copyVideoFrameToMemory(QVideoFrame()).isValid());
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    void paddedCameraPlanesAreCopiedRowByRow()
    {
        QVideoFrame source(std::make_unique<PaddedCameraBuffer>());
        source.setRotation(QtVideo::Rotation::Clockwise90);
        source.setMirrored(true);
        QVideoFrame owned = copyVideoFrameToMemory(source);
        QVERIFY(owned.isValid());
        QCOMPARE(owned.surfaceFormat(), source.surfaceFormat());
        QCOMPARE(owned.rotation(), source.rotation());
        QCOMPARE(owned.mirrored(), source.mirrored());
        source = QVideoFrame();
        QVERIFY(owned.map(QVideoFrame::ReadOnly));
        QCOMPARE(owned.planeCount(), 2);
        for (int p = 0; p < 2; ++p) {
            const int rows = p == 0 ? 18 : 9;
            for (int row = 0; row < rows; ++row)
                for (int x = 0; x < 32; ++x)
                    QCOMPARE(owned.bits(p)[row * owned.bytesPerLine(p) + x],
                             uchar((p == 0 ? 20 : 110) + row));
        }
        owned.unmap();
        auto shortBuffer = std::make_unique<PaddedCameraBuffer>();
        shortBuffer->truncated = true;
        QVideoFrame invalid(std::move(shortBuffer));
        QVERIFY(!copyVideoFrameToMemory(invalid).isValid());
        QVERIFY(!invalid.isMapped());
    }
#endif

    void framesRetainAspectAndContent_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("wide") << QSize(1920, 1080);
        QTest::newRow("portrait") << QSize(1080, 1920);
        QTest::newRow("standard") << QSize(640, 480);
        QTest::newRow("square") << QSize(480, 480);
    }
    void framesRetainAspectAndContent()
    {
        QFETCH(QSize, size);
        const auto id = CallId::generate();
        const auto secret = generateCallSecret();
        auto sender = CallVideoSession::create(id, CallDirection::Outgoing, secret);
        auto receiver = CallVideoSession::create(id, CallDirection::Incoming, secret);
        QVERIFY(sender && receiver);
        QImage source(size, QImage::Format_RGB32);
        source.fill(QColor(40, 120, 210));
        const auto packet = sender->encode(source);
        QVERIFY(!packet.isEmpty());
        QVERIFY(!CallMediaPacket::decode(packet)); // voice parser stays unchanged
        const auto decoded = receiver->decode(packet);
        QVERIFY(decoded && !decoded->isNull());
        QCOMPARE(decoded->size(), size.width() > 640 || size.height() > 640
            ? size.scaled(640, 640, Qt::KeepAspectRatio) : size);
        const QColor pixel = decoded->pixelColor(decoded->width() / 2, decoded->height() / 2);
        QVERIFY(qAbs(pixel.red() - 40) < 5);
        QVERIFY(qAbs(pixel.green() - 120) < 5);
        QVERIFY(qAbs(pixel.blue() - 210) < 5);
        // Both directions work, with the same independent sequence values.
        QVERIFY(sender->decode(receiver->encode(source)));
    }

    void noisyFramesStayWithinThePacketBudget()
    {
        const auto id = CallId::generate();
        const auto secret = generateCallSecret();
        auto sender = CallVideoSession::create(id, CallDirection::Outgoing, secret);
        auto receiver = CallVideoSession::create(id, CallDirection::Incoming, secret);
        QImage noise(640, 640, QImage::Format_RGB32);
        QRandomGenerator random(42);
        for (int y = 0; y < noise.height(); ++y) {
            auto *pixels = reinterpret_cast<QRgb *>(noise.scanLine(y));
            for (int x = 0; x < noise.width(); ++x)
                pixels[x] = random.generate() | 0xff000000;
        }
        const auto packet = sender->encode(noise);
        QVERIFY(!packet.isEmpty());
        QVERIFY(packet.size() <= CallVideoSession::maxPayloadBytes
                + CallMediaPacket::headerBytes + CallMediaSealer::tagBytes);
        const auto received = receiver->decode(packet);
        QVERIFY(received && !received->isNull());
        QCOMPARE(received->width(), received->height());
    }

    void togglesLossReplayAndTampering()
    {
        const auto id = CallId::generate();
        const auto secret = generateCallSecret();
        auto sender = CallVideoSession::create(id, CallDirection::Outgoing, secret);
        auto receiver = CallVideoSession::create(id, CallDirection::Incoming, secret);
        QImage source(640, 360, QImage::Format_RGB32);
        source.fill(Qt::red);
        const auto first = sender->encode(source);
        QByteArray forged = first;
        forged[forged.size() - 1] ^= 1;
        QVERIFY(!receiver->decode(forged));
        QVERIFY(receiver->decode(first));
        QVERIFY(!receiver->decode(first));
        const auto late = sender->encode(source);
        const auto off = receiver->decode(sender->encode(QImage()));
        QVERIFY(off && off->isNull());
        QVERIFY(!receiver->decode(late)); // cannot resurrect video after camera off
        for (int i = 0; i < 100; ++i)
            (void)sender->encode(source); // dropped frames
        const auto resumed = receiver->decode(sender->encode(source));
        QVERIFY(resumed && !resumed->isNull());
        auto wrongCall = CallVideoSession::create(CallId::generate(), CallDirection::Incoming, secret);
        QVERIFY(!wrongCall->decode(sender->encode(source)));
        auto wrongKey = CallVideoSession::create(id, CallDirection::Incoming, generateCallSecret());
        QVERIFY(!wrongKey->decode(sender->encode(source)));
        QVERIFY(!receiver->decode(QByteArray(200000, '\0')));
        QVERIFY(!receiver->decode(QByteArray("\2", 1)));
        const auto audioKeys = CallMediaKeySchedule::derive(secret, id);
        const auto videoKeys = CallMediaKeySchedule::deriveVideo(secret, id);
        QVERIFY(audioKeys->fromCaller.key != videoKeys->fromCaller.key);
        QVERIFY(videoKeys->fromCaller.key != videoKeys->fromCallee.key);
    }

    void rejectsAuthenticatedOversizedImagesAndInvalidPayloads()
    {
        const auto id = CallId::generate();
        const auto secret = generateCallSecret();
        auto receiver = CallVideoSession::create(id, CallDirection::Incoming, secret);
        CallMediaSealer sealer(CallMediaKeySchedule::deriveVideo(secret, id)->fromCaller);
        CallMediaPacket packet;
        packet.callId = id;
        packet.version = CallVideoSession::wireVersion;
        packet.flags = 1;
        QImage oversized(641, 480, QImage::Format_RGB32);
        oversized.fill(Qt::green);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(oversized.save(&buffer, "JPEG"));
        packet.sealed = sealer.seal(packet.sequence, jpeg, packet.header());
        QVERIFY(!receiver->decode(packet.encode()));
        ++packet.sequence;
        packet.sealed = sealer.seal(packet.sequence, QByteArrayView("not an image"), packet.header());
        QVERIFY(!receiver->decode(packet.encode()));
        ++packet.sequence;
        packet.flags = 0;
        packet.sealed = sealer.seal(packet.sequence, QByteArrayView("invalid off"), packet.header());
        QVERIFY(!receiver->decode(packet.encode()));
    }
};

QTEST_GUILESS_MAIN(VideoCallTest)
#include "tst_videocall.moc"
