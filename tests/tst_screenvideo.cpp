#include "call/CallMediaCrypto.h"
#include "call/CallSignal.h"
#include "call/ScreenVideoCodec.h"

#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QElapsedTimer>
#include <QtTest>

#include <cmath>
#include <vector>

using namespace OpenChat;

namespace {

// A desktop-like picture: window chrome, dense text, a photo-ish gradient.
QImage desktop(int width, int height, int scroll = 0)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(236, 240, 244));
    QPainter painter(&image);
    painter.fillRect(0, 0, width, 32, QColor(40, 60, 90));
    QLinearGradient gradient(0, 0, width / 3.0, height);
    gradient.setColorAt(0, QColor(200, 90, 40));
    gradient.setColorAt(1, QColor(40, 90, 200));
    painter.fillRect(width * 2 / 3, 40, width / 3 - 10, height / 2, gradient);
    QFont font;
    font.setPixelSize(13);
    painter.setFont(font);
    QRandomGenerator words(7);
    for (int line = 0; line < 400; ++line) {
        const int y = 60 + line * 18 - scroll;
        if (y < 40 || y > height)
            continue;
        painter.setPen(line % 7 == 0 ? QColor(0, 90, 200) : QColor(25, 25, 25));
        painter.drawText(20, y, QStringLiteral("line %1: the quick brown fox %2 jumps over %3")
                                    .arg(line)
                                    .arg(words.bounded(100000))
                                    .arg(line * 31));
    }
    return image;
}

double psnr(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return 0.0;
    double squared = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const int dr = qRed(pa[x]) - qRed(pb[x]);
            const int dg = qGreen(pa[x]) - qGreen(pb[x]);
            const int db = qBlue(pa[x]) - qBlue(pb[x]);
            squared += dr * dr + dg * dg + db * db;
        }
    }
    const double mean = squared / (3.0 * a.width() * a.height());
    return mean <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mean);
}

// Stands in for the sender's encoder where only the requests a viewer's
// reports make of it matter.
class CountingEncoder final : public ScreenEncoderControl
{
public:
    int resendRequests = 0;

    void requestFullResend() override { ++resendRequests; }
    void setLevel(int) override {}
    [[nodiscard]] int level() const noexcept override { return 2; }
    [[nodiscard]] int levelCount() const noexcept override { return 5; }
    [[nodiscard]] int bytesPerSecondAt(int) const noexcept override { return 375000; }
    void setRemoteView(QSize, bool) override {}
    [[nodiscard]] int targetFps() const noexcept override { return 30; }
    [[nodiscard]] bool remoteViewHidden() const noexcept override { return false; }
    [[nodiscard]] ScreenShareStats stats() const override { return {}; }
    void reset() override {}
};

// Encodes `count` frames of a slowly scrolling desktop, one at a time.
std::vector<EncodedScreenFrame> encodeScroll(ScreenVideoEncoder &encoder, int count, qint64 &now,
                                             int width = 640, int height = 360)
{
    std::vector<EncodedScreenFrame> encoded;
    encoder.setOnEncoded([&](const EncodedScreenFrame &frame) { encoded.push_back(frame); });
    for (int index = 0; index < count; ++index) {
        const QImage source = desktop(width, height, index * 6);
        if (!encoder.submit(ScreenFrameView::fromImage(source), now))
            return {};
        QElapsedTimer wait;
        wait.start();
        while (int(encoded.size()) < index + 1 && wait.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        now += 40;
    }
    encoder.setOnEncoded(nullptr);
    return encoded;
}

} // namespace

class ScreenVideoTest final : public QObject
{
    Q_OBJECT

private slots:
    void colourSurvivesTheRoundTripThroughI420()
    {
        const QImage source = desktop(640, 360);
        I420Picture picture;
        picture.allocate(640, 360);
        convertToI420(ScreenFrameView::fromImage(source), picture);
        QImage back;
        convertI420ToRgb32(picture.y(), 640, picture.u(), 320, picture.v(), 320, 640, 360, back);
        // Chroma is halved, so edges between colours soften (how much depends
        // on the platform's font, ~30 dB for this picture); flat areas and luma
        // must come back almost exactly. Swapped or mis-scaled channels land
        // far below.
        QVERIFY2(psnr(source, back) > 27.0, qPrintable(QString::number(psnr(source, back))));
        // Below the gradient, right of the text: plain window background.
        const QRgb flat = back.pixel(560, 350);
        QVERIFY(qAbs(qRed(flat) - 236) <= 2 && qAbs(qGreen(flat) - 240) <= 2 && qAbs(qBlue(flat) - 244) <= 2);
        const QRgb white = QColor(Qt::white).rgb();
        QImage pure(64, 64, QImage::Format_RGB32);
        pure.fill(white);
        I420Picture whitePicture;
        whitePicture.allocate(64, 64);
        convertToI420(ScreenFrameView::fromImage(pure), whitePicture);
        QImage whiteBack;
        convertI420ToRgb32(whitePicture.y(), 64, whitePicture.u(), 32, whitePicture.v(), 32, 64, 64, whiteBack);
        // Full range: white stays white rather than turning grey.
        QCOMPARE(whiteBack.pixel(10, 10), white);
    }

    void aLargerSourceIsScaledIntoTheOutput()
    {
        QImage source(1280, 720, QImage::Format_RGB32);
        source.fill(QColor(10, 200, 30));
        I420Picture picture;
        picture.allocate(640, 360);
        convertToI420(ScreenFrameView::fromImage(source), picture);
        QImage back;
        convertI420ToRgb32(picture.y(), 640, picture.u(), 320, picture.v(), 320, 640, 360, back);
        const QRgb centre = back.pixel(320, 180);
        QVERIFY(qAbs(qRed(centre) - 10) <= 3 && qAbs(qGreen(centre) - 200) <= 3 && qAbs(qBlue(centre) - 30) <= 3);
    }

    // RGBA8888 is laid out byte by byte, not as an ARGB word; read as one it
    // would swap red and blue.
    void byteOrderedCapturesKeepTheirColours()
    {
        for (const QImage::Format format : {QImage::Format_RGBX8888, QImage::Format_RGBA8888}) {
            QImage source(128, 64, format);
            source.fill(QColor(200, 30, 60));
            for (const QSize output : {QSize(128, 64), QSize(64, 32)}) {
                I420Picture picture;
                picture.allocate(output.width(), output.height());
                convertToI420(ScreenFrameView::fromImage(source), picture);
                QImage back;
                convertI420ToRgb32(picture.y(), picture.width, picture.u(), picture.width / 2,
                                   picture.v(), picture.width / 2, picture.width, picture.height, back);
                const QRgb pixel = back.pixel(output.width() / 2, output.height() / 2);
                QVERIFY2(qAbs(qRed(pixel) - 200) <= 3 && qAbs(qGreen(pixel) - 30) <= 3
                             && qAbs(qBlue(pixel) - 60) <= 3,
                         qPrintable(QStringLiteral("format %1 at %2x%3 came back as %4")
                                        .arg(int(format))
                                        .arg(output.width())
                                        .arg(output.height())
                                        .arg(QColor(pixel).name())));
            }
        }
    }

    void aScrollingDesktopSurvivesEncodeAndDecode()
    {
        if (!ScreenVideoEncoder::isAvailable())
            QSKIP("this build has no VP9");
        QObject context;
        ScreenVideoEncoder encoder(&context);
        std::vector<EncodedScreenFrame> encoded;
        encoder.setOnEncoded([&](const EncodedScreenFrame &frame) { encoded.push_back(frame); });
        std::vector<QImage> sources;
        qint64 now = 1000;
        for (int index = 0; index < 20; ++index) {
            sources.push_back(desktop(960, 540, index * 12));
            QVERIFY(encoder.submit(ScreenFrameView::fromImage(sources.back()), now));
            // Each frame is given time to encode before the next, so none is
            // replaced while waiting and every one can be checked.
            QTRY_COMPARE_WITH_TIMEOUT(int(encoded.size()), index + 1, 5000);
            now += 40;
        }
        QVERIFY(encoded.front().keyframe);
        for (size_t index = 1; index < encoded.size(); ++index)
            QVERIFY2(!encoded[index].keyframe, "only the first frame should be a keyframe");
        QCOMPARE(encoded.front().size, QSize(960, 540));

        ScreenVideoDecoder decoder(&context);
        QImage last;
        int pictures = 0;
        decoder.setOnPicture([&](QImage picture) {
            last = std::move(picture);
            ++pictures;
        });
        for (const EncodedScreenFrame &frame : encoded)
            decoder.submit(frame.data, frame.keyframe);
        QTRY_VERIFY_WITH_TIMEOUT(pictures > 0 && !last.isNull(), 5000);
        // The newest picture wins: wait for the last one to arrive.
        QTRY_VERIFY_WITH_TIMEOUT(psnr(last, sources.back()) > 28.0, 5000);

        // A keyframe on request, for a viewer who lost the stream.
        encoder.requestFullResend();
        QVERIFY(encoder.submit(ScreenFrameView::fromImage(desktop(960, 540, 500)), now));
        QTRY_COMPARE_WITH_TIMEOUT(int(encoded.size()), 21, 5000);
        QVERIFY(encoded.back().keyframe);
    }

    void theDecoderWaitsForAKeyframeAndAsksForOneWhenTheStreamBreaks()
    {
        if (!ScreenVideoEncoder::isAvailable())
            QSKIP("this build has no VP9");
        QObject context;
        ScreenVideoDecoder decoder(&context);
        int pictures = 0;
        int keyframeRequests = 0;
        decoder.setOnPicture([&](QImage) { ++pictures; });
        decoder.setOnKeyframeNeeded([&] { ++keyframeRequests; });
        // A non-keyframe first is ignored outright.
        decoder.submit(QByteArray(200, char(0x55)), false);
        QTest::qWait(100);
        QCOMPARE(pictures, 0);
        // Garbage claiming to be a keyframe breaks the stream: it asks.
        decoder.submit(QByteArray(200, char(0x55)), true);
        QTRY_COMPARE_WITH_TIMEOUT(keyframeRequests, 1, 5000);
        QCOMPARE(pictures, 0);
    }

    // A frame that cannot be decoded breaks the stream, but a keyframe already
    // queued behind it starts it again: it is not thrown away with the frames
    // that depended on the broken one.
    void aKeyframeQueuedBehindABrokenFrameStillShows()
    {
        if (!ScreenVideoEncoder::isAvailable())
            QSKIP("this build has no VP9");
        QObject context;
        ScreenVideoEncoder encoder(&context);
        qint64 now = 1000;
        const std::vector<EncodedScreenFrame> encoded = encodeScroll(encoder, 4, now);
        QCOMPARE(int(encoded.size()), 4);
        QVERIFY(encoded.front().keyframe);

        // What the stream decodes to on a decoder that never saw a bad frame.
        ScreenVideoDecoder clean(&context);
        QImage expected;
        clean.setOnPicture([&](QImage picture) { expected = std::move(picture); });
        for (const EncodedScreenFrame &frame : encoded)
            QVERIFY(clean.submit(frame.data, frame.keyframe));

        ScreenVideoDecoder decoder(&context);
        QImage last;
        decoder.setOnPicture([&](QImage picture) { last = std::move(picture); });
        QVERIFY(decoder.submit(QByteArray(300, char(0x55)), true));
        for (const EncodedScreenFrame &frame : encoded)
            QVERIFY(decoder.submit(frame.data, frame.keyframe));
        // Decoding is exact, so once both have shown the last frame they
        // match to the pixel.
        QTRY_VERIFY_WITH_TIMEOUT(!expected.isNull() && !last.isNull() && last == expected, 5000);
        QVERIFY(psnr(expected, desktop(640, 360, 3 * 6)) > 20.0);
    }

    // The session half of the video path: a frame bigger than one fragment is
    // split and put back together, its picture arrives asynchronously, and a
    // lost fragment makes the viewer ask for a keyframe at once rather than at
    // the next periodic report.
    void aFragmentedStreamIsReassembledAndALossAsksForAKeyframeAtOnce()
    {
        if (!ScreenVideoEncoder::isAvailable())
            QSKIP("this build has no VP9");
        QObject context;
        const CallId id = CallId::generate();
        const QByteArray secret = generateCallSecret();
        auto encoder = std::make_shared<ScreenVideoEncoder>(&context);
        auto receiverEncoder = std::make_shared<ScreenVideoEncoder>(&context);
        auto sender = CallScreenSession::create(id, CallDirection::Outgoing, secret, encoder);
        auto receiver = CallScreenSession::create(id, CallDirection::Incoming, secret, receiverEncoder);
        QVERIFY(sender && receiver);
        std::vector<CallScreenSession::Update> pictures;
        receiver->setAsyncUpdateHandler(&context, [&](const CallScreenSession::Update &update) {
            pictures.push_back(update);
        });
        std::vector<EncodedScreenFrame> encoded;
        encoder->setOnEncoded([&](const EncodedScreenFrame &frame) { encoded.push_back(frame); });

        // A detailed desktop: its keyframe is far more than one fragment.
        QImage detailed = desktop(1280, 720);
        QRandomGenerator noise(5);
        for (int y = 300; y < 720; ++y) {
            auto *pixels = reinterpret_cast<QRgb *>(detailed.scanLine(y));
            for (int x = 0; x < 640; ++x)
                pixels[x] = noise.generate() | 0xff000000u;
        }
        qint64 now = 1000;
        QVERIFY(encoder->submit(ScreenFrameView::fromImage(detailed), now));
        QTRY_COMPARE_WITH_TIMEOUT(int(encoded.size()), 1, 5000);
        QVERIFY(encoded.front().keyframe);
        const std::vector<QByteArray> keyframe = sender->sealVideoFrame(encoded.front(), now);
        QVERIFY2(keyframe.size() > 1, "the keyframe should span several fragments");
        for (const QByteArray &fragment : keyframe) {
            QCOMPARE(quint8(fragment[0]), CallScreenSession::videoWireVersion);
            QVERIFY(fragment.size() <= CallScreenSession::maxVideoFragmentBytes + 128);
            const auto update = receiver->decode(fragment, now);
            QVERIFY(update && update->kind == CallScreenSession::Update::Kind::Pending);
        }
        QTRY_COMPARE_WITH_TIMEOUT(int(pictures.size()), 1, 5000);
        QVERIFY(pictures.front().canvasReplaced);
        QCOMPARE(pictures.front().canvas->size(), QSize(1280, 720));

        // Tampered, oversized or inconsistent fragments are refused outright.
        QByteArray forged = keyframe.front();
        forged[forged.size() - 1] = char(forged[forged.size() - 1] ^ 0x5a);
        QVERIFY(!receiver->decode(forged, now));

        // The next frame loses a fragment on the way. A report goes out at
        // once, asking for a keyframe, even though one was just sent.
        (void)receiver->encodeFeedback(now);
        now += 40;
        detailed.fill(Qt::darkCyan);
        for (int y = 0; y < 720; ++y) {
            auto *pixels = reinterpret_cast<QRgb *>(detailed.scanLine(y));
            for (int x = 0; x < 1280; x += 3)
                pixels[x] = noise.generate() | 0xff000000u;
        }
        QVERIFY(encoder->submit(ScreenFrameView::fromImage(detailed), now));
        QTRY_COMPARE_WITH_TIMEOUT(int(encoded.size()), 2, 5000);
        const std::vector<QByteArray> next = sender->sealVideoFrame(encoded.back(), now);
        QVERIFY(next.size() > 1);
        for (size_t index = 1; index < next.size(); ++index)
            (void)receiver->decode(next[index], now + 10);
        const QByteArray report = receiver->encodeFeedback(now + 150);
        QVERIFY2(!report.isEmpty(), "the keyframe request waited for the periodic report");
        QVERIFY(sender->decode(report, now + 160).has_value());
        QVERIFY(encoder->submit(ScreenFrameView::fromImage(desktop(1280, 720, 40)), now + 200));
        QTRY_COMPARE_WITH_TIMEOUT(int(encoded.size()), 3, 5000);
        QVERIFY2(encoded.back().keyframe, "the sender did not answer with a keyframe");
    }

    // A keyframe request is asked once, but if frames keep arriving that
    // cannot be decoded without the keyframe — the request or its answer was
    // dropped — it is asked again, rather than leaving the viewer on a frozen
    // picture for as long as the share lasts.
    void aViewerStillWaitingForAKeyframeAsksAgain()
    {
        if (!ScreenVideoEncoder::isAvailable())
            QSKIP("this build has no VP9");
        QObject context;
        ScreenVideoEncoder encoder(&context);
        qint64 now = 1000;
        const std::vector<EncodedScreenFrame> encoded = encodeScroll(encoder, 12, now);
        QCOMPARE(int(encoded.size()), 12);

        const CallId id = CallId::generate();
        const QByteArray secret = generateCallSecret();
        auto requests = std::make_shared<CountingEncoder>();
        auto sender = CallScreenSession::create(id, CallDirection::Outgoing, secret, requests);
        auto receiver = CallScreenSession::create(id, CallDirection::Incoming, secret,
                                                  std::make_shared<CountingEncoder>());
        QVERIFY(sender && receiver);
        receiver->setAsyncUpdateHandler(&context, [](const CallScreenSession::Update &) {});
        const qint64 start = 10000;
        const auto deliver = [&](const EncodedScreenFrame &frame, qint64 at) {
            for (const QByteArray &fragment : sender->sealVideoFrame(frame, at))
                (void)receiver->decode(fragment, at);
        };
        const auto report = [&](qint64 at) {
            const QByteArray packet = receiver->encodeFeedback(at);
            if (!packet.isEmpty())
                (void)sender->decode(packet, at);
            return !packet.isEmpty();
        };
        // The keyframe never arrives; only the frames after it do.
        (void)sender->sealVideoFrame(encoded.front(), start);
        deliver(encoded[1], start);
        QVERIFY(report(start));
        QCOMPARE(requests->resendRequests, 1);
        // Frames that are still undecodable half a second later: the first
        // request may simply still be being answered.
        deliver(encoded[2], start + 500);
        deliver(encoded[3], start + 600);
        QVERIFY(report(start + 600));
        QCOMPARE(requests->resendRequests, 1);
        // Still nothing decodable well after that: ask again, at once.
        deliver(encoded[4], start + 1600);
        QVERIFY(report(start + 1700));
        QCOMPARE(requests->resendRequests, 2);
    }

    // A fragment lost in the middle of a frame breaks the stream once. The
    // rest of that frame is dropped quietly, and the next frame does not
    // count as a second break that asks for a second keyframe.
    void aLossMidFrameAsksForOneKeyframe()
    {
        if (!ScreenVideoEncoder::isAvailable())
            QSKIP("this build has no VP9");
        QObject context;
        ScreenVideoEncoder encoder(&context);
        qint64 now = 1000;
        // Large, detailed frames, so each spans several fragments.
        encoder.setLevel(0);
        std::vector<EncodedScreenFrame> encoded;
        encoder.setOnEncoded([&](const EncodedScreenFrame &frame) { encoded.push_back(frame); });
        QRandomGenerator noise(9);
        for (int index = 0; index < 3; ++index) {
            QImage frame(1280, 720, QImage::Format_RGB32);
            for (int y = 0; y < 720; ++y) {
                auto *pixels = reinterpret_cast<QRgb *>(frame.scanLine(y));
                for (int x = 0; x < 1280; ++x)
                    pixels[x] = noise.generate() | 0xff000000u;
            }
            QVERIFY(encoder.submit(ScreenFrameView::fromImage(frame), now));
            QTRY_COMPARE_WITH_TIMEOUT(int(encoded.size()), index + 1, 5000);
            now += 40;
        }

        const CallId id = CallId::generate();
        const QByteArray secret = generateCallSecret();
        auto requests = std::make_shared<CountingEncoder>();
        auto sender = CallScreenSession::create(id, CallDirection::Outgoing, secret, requests);
        auto receiver = CallScreenSession::create(id, CallDirection::Incoming, secret,
                                                  std::make_shared<CountingEncoder>());
        QVERIFY(sender && receiver);
        receiver->setAsyncUpdateHandler(&context, [](const CallScreenSession::Update &) {});
        const qint64 start = 10000;
        for (const QByteArray &fragment : sender->sealVideoFrame(encoded[0], start))
            (void)receiver->decode(fragment, start);
        QVERIFY(!receiver->encodeFeedback(start).isEmpty());

        // The second frame loses its second fragment.
        const std::vector<QByteArray> second = sender->sealVideoFrame(encoded[1], start + 40);
        QVERIFY2(second.size() > 2, "the frame should span at least three fragments");
        for (size_t index = 0; index < second.size(); ++index) {
            if (index != 1)
                (void)receiver->decode(second[index], start + 40);
        }
        const QByteArray urgent = receiver->encodeFeedback(start + 150);
        QVERIFY2(!urgent.isEmpty(), "the loss should be reported at once");
        (void)sender->decode(urgent, start + 150);
        QCOMPARE(requests->resendRequests, 1);

        // The third frame arrives whole. Nothing new is lost, so nothing new
        // is urgent: the next report waits for its turn.
        for (const QByteArray &fragment : sender->sealVideoFrame(encoded[2], start + 80))
            (void)receiver->decode(fragment, start + 260);
        QVERIFY2(receiver->encodeFeedback(start + 300).isEmpty(),
                 "the frame after the loss was treated as a second break");
    }

    void aStillScreenStopsCostingFramesAfterItSharpens()
    {
        if (!ScreenVideoEncoder::isAvailable())
            QSKIP("this build has no VP9");
        QObject context;
        ScreenVideoEncoder encoder(&context);
        int frames = 0;
        encoder.setOnEncoded([&](const EncodedScreenFrame &) { ++frames; });
        const QImage still = desktop(640, 360);
        qint64 now = 1000;
        int taken = 0;
        // Ten seconds of the same picture at the pacing rate.
        for (int index = 0; index < 300; ++index) {
            if (encoder.submit(ScreenFrameView::fromImage(still), now))
                ++taken;
            now += 34;
        }
        // Three seconds of sharpening at thirty frames, then one a second.
        QVERIFY2(taken >= 85 && taken <= 100, qPrintable(QString::number(taken)));
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv);
    ScreenVideoTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_screenvideo.moc"
