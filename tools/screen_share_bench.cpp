// Measures what a viewer of a screen share actually sees, for the tile encoder
// and the VP9 encoder side by side, on synthetic 1080p desktops:
//
//   openchat-screen-bench [seconds]
//
// For every frame of a scenario — scrolling a page of text, a video playing in
// a window, typing into a document — the frame is encoded, delivered and
// decoded, and the viewer's picture is compared with what is on the sharer's
// screen at that moment. "Freshness" is that comparison in dB (PSNR): above
// ~35 the viewer is looking at the present, below ~20 at something else. The
// tile encoder runs at its best rung and at the rung a share starts on; VP9 at
// its starting bitrate.

#include "call/CallMediaCrypto.h"
#include "call/CallScreenSession.h"
#include "call/CallSignal.h"
#include "call/ScreenVideoCodec.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QTextStream>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

using namespace OpenChat;

namespace {

constexpr int width = 1920;
constexpr int height = 1080;
constexpr int fps = 30;

QImage page()
{
    static const QImage tall = [] {
        QImage image(width, 9000, QImage::Format_RGB32);
        image.fill(QColor(250, 250, 250));
        QPainter painter(&image);
        QFont font;
        font.setPixelSize(14);
        painter.setFont(font);
        QRandomGenerator words(11);
        static const char *const vocabulary[] = {
            "screen", "share", "frame", "encoder", "OpenChat", "latency", "the", "a",
            "packet", "relay", "canvas", "window", "render", "text", "scroll", "pixel",
        };
        for (int y = 24; y < image.height(); y += 20) {
            int x = 280;
            while (x < width - 320) {
                const char *word = vocabulary[words.bounded(16)];
                painter.setPen(words.bounded(7) == 0 ? QColor(0, 90, 200) : QColor(30, 30, 30));
                painter.drawText(x, y, QString::fromLatin1(word));
                x += 9 * int(qstrlen(word)) + 9;
            }
        }
        return image;
    }();
    return tall;
}

// Window chrome and a sidebar that stay put, around content that does not.
QImage frameFor(const QString &scenario, int index)
{
    QImage frame(width, height, QImage::Format_RGB32);
    QPainter painter(&frame);
    if (scenario == QStringLiteral("scroll")) {
        painter.drawImage(0, 0, page(), 0, (index * 16) % (page().height() - height), width, height);
    } else {
        painter.drawImage(0, 0, page(), 0, 0, width, height);
    }
    painter.fillRect(0, 0, width, 36, QColor(40, 56, 80));
    painter.fillRect(0, 36, 250, height - 36, QColor(230, 234, 240));
    if (scenario == QStringLiteral("video")) {
        // A 1280x720 player: moving gradients and shapes, like footage.
        const QRect player(400, 200, 1280, 720);
        QLinearGradient gradient(player.topLeft(), player.bottomRight());
        gradient.setColorAt(0, QColor::fromHsv((index * 3) % 360, 170, 210));
        gradient.setColorAt(1, QColor::fromHsv((index * 3 + 140) % 360, 150, 80));
        painter.fillRect(player, gradient);
        painter.setPen(Qt::NoPen);
        for (int blob = 0; blob < 14; ++blob) {
            painter.setBrush(QColor::fromHsv((blob * 27) % 360, 200, 230));
            painter.drawEllipse(QPointF(player.center().x() + 520 * std::sin(index * 0.06 + blob),
                                        player.center().y() + 300 * std::cos(index * 0.05 + blob * 1.7)),
                                70, 70);
        }
    } else if (scenario == QStringLiteral("typing")) {
        // A line growing by a character every other frame, and a caret.
        QFont font;
        font.setPixelSize(14);
        painter.setFont(font);
        painter.setPen(QColor(20, 20, 20));
        const QString text = QStringLiteral("The quick brown fox jumps over the lazy dog. ").repeated(8);
        painter.fillRect(280, 500, 1300, 24, QColor(255, 255, 240));
        painter.drawText(284, 517, text.left(index / 2));
        if ((index / 15) % 2 == 0)
            painter.fillRect(284 + 8 * (index / 2), 504, 2, 16, Qt::black);
    }
    return frame;
}

double psnr(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull())
        return 0.0;
    double squared = 0.0;
    qint64 samples = 0;
    for (int y = 0; y < a.height(); y += 2) {
        const auto *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); x += 2) {
            const int dr = qRed(pa[x]) - qRed(pb[x]);
            const int dg = qGreen(pa[x]) - qGreen(pb[x]);
            const int db = qBlue(pa[x]) - qBlue(pb[x]);
            squared += dr * dr + dg * dg + db * db;
            samples += 3;
        }
    }
    const double mean = squared / double(samples);
    return mean <= 0.0 ? 60.0 : std::min(60.0, 10.0 * std::log10(255.0 * 255.0 / mean));
}

struct BenchResult {
    double freshness = 0.0;   // mean PSNR of the viewer's picture against the present
    double worst = 99.0;
    double kilobitsPerSecond = 0.0;
    double encodeMs = 0.0;
};

BenchResult runTiles(const QString &scenario, int frames, int level)
{
    const CallId id = CallId::generate();
    const QByteArray secret = generateCallSecret();
    auto encoder = std::make_shared<ScreenTileEncoder>();
    encoder->setLevel(level);
    auto sender = CallScreenSession::create(id, CallDirection::Outgoing, secret, encoder);
    auto receiver = CallScreenSession::create(id, CallDirection::Incoming, secret,
                                              std::make_shared<ScreenTileEncoder>());
    BenchResult result;
    qint64 bytes = 0;
    double encodeMs = 0.0;
    int measured = 0;
    for (int index = 0; index < frames; ++index) {
        const QImage source = frameFor(scenario, index);
        const qint64 now = 1000 + index * (1000 / fps);
        QElapsedTimer clock;
        clock.start();
        const QByteArrayView payload = encoder->buildUpdate(ScreenFrameView::fromImage(source), now);
        encodeMs += clock.nsecsElapsed() / 1e6;
        if (!payload.isEmpty()) {
            const QByteArray packet = sender->sealUpdate(payload, now);
            bytes += packet.size();
            (void)receiver->decode(packet, now);
        }
        // The first second is the share starting up; judge the steady state.
        if (index >= fps && receiver->canvas()) {
            const double score = psnr(receiver->canvas()->image(), source);
            result.freshness += score;
            result.worst = std::min(result.worst, score);
            ++measured;
        }
    }
    result.freshness /= std::max(1, measured);
    result.kilobitsPerSecond = double(bytes) * 8.0 / (double(frames) / fps) / 1000.0;
    result.encodeMs = encodeMs / frames;
    return result;
}

BenchResult runVideo(const QString &scenario, int frames)
{
    QObject context;
    ScreenVideoEncoder encoder(&context);
    ScreenVideoDecoder decoder(&context);
    BenchResult result;
    qint64 bytes = 0;
    int encodedCount = 0;
    QImage picture;
    int pictures = 0;
    encoder.setOnEncoded([&](const EncodedScreenFrame &frame) {
        bytes += frame.data.size();
        ++encodedCount;
        decoder.submit(frame.data, frame.keyframe);
    });
    decoder.setOnPicture([&](QImage next) {
        picture = std::move(next);
        ++pictures;
    });
    double encodeMs = 0.0;
    int measured = 0;
    for (int index = 0; index < frames; ++index) {
        const QImage source = frameFor(scenario, index);
        const qint64 now = 1000 + index * (1000 / fps);
        const int before = encodedCount;
        const int picturesBefore = pictures;
        if (encoder.submit(ScreenFrameView::fromImage(source), now)) {
            // Wait for this frame's picture, so it is compared with its own
            // source (a real viewer sees it a frame or two later).
            QElapsedTimer wait;
            wait.start();
            while (encodedCount == before && wait.elapsed() < 2000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            encodeMs += encoder.stats().lastEncodeUs / 1000.0;
            while (pictures == picturesBefore && wait.elapsed() < 2000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        if (index >= fps && !picture.isNull()) {
            const double score = psnr(picture, source);
            result.freshness += score;
            result.worst = std::min(result.worst, score);
            ++measured;
        }
    }
    result.freshness /= std::max(1, measured);
    result.kilobitsPerSecond = double(bytes) * 8.0 / (double(frames) / fps) / 1000.0;
    result.encodeMs = encodeMs / std::max(1, encodedCount);
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv);
    const int seconds = argc > 1 ? std::max(2, atoi(argv[1])) : 6;
    const int frames = seconds * fps;
    QTextStream out(stdout);
    out << "Screen share, 1920x1080 at " << fps << " fps, " << seconds << " s per scenario\n"
        << "freshness = viewer's picture vs. the sharer's screen at that moment (PSNR, dB; "
           "higher is better, >35 is current)\n\n";
    out << QStringLiteral("%1 %2 %3 %4 %5\n")
               .arg(QStringLiteral("scenario / encoder"), -34)
               .arg(QStringLiteral("fresh dB"), 9)
               .arg(QStringLiteral("worst dB"), 9)
               .arg(QStringLiteral("kbit/s"), 8)
               .arg(QStringLiteral("encode ms"), 10);
    for (const QString scenario : {QStringLiteral("scroll"), QStringLiteral("video"), QStringLiteral("typing")}) {
        const auto row = [&](const QString &label, const BenchResult &r) {
            out << QStringLiteral("%1 %2 %3 %4 %5\n")
                       .arg(scenario + QStringLiteral(" / ") + label, -34)
                       .arg(r.freshness, 9, 'f', 1)
                       .arg(r.worst, 9, 'f', 1)
                       .arg(r.kilobitsPerSecond, 8, 'f', 0)
                       .arg(r.encodeMs, 10, 'f', 1);
            out.flush();
        };
        row(QStringLiteral("tiles, starting rung"), runTiles(scenario, frames, 2));
        row(QStringLiteral("tiles, best rung"), runTiles(scenario, frames, 0));
        if (ScreenVideoEncoder::isAvailable())
            row(QStringLiteral("VP9, starting rate"), runVideo(scenario, frames));
        else
            out << scenario << " / VP9: not in this build\n";
    }
    return 0;
}
