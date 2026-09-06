#include <QtTest>

#include <QBuffer>
#include <QPainter>
#include <QRandomGenerator>

#include "call/CallMediaPacket.h"
#include "call/CallScreenSession.h"
#include "call/CallSignal.h"
#include "call/CallVideoSession.h"

#include <cstring>

#ifdef Q_OS_LINUX
#include <QFile>
#include <unistd.h>
#endif

using namespace OpenChat;

namespace {

// A desktop that exercises every branch the tile encoder has: a flat backdrop
// (solid tiles), a panel of crisp two-colour "text" (the lossless path), and a
// noisy photograph (the JPEG path).
[[nodiscard]] QImage syntheticDesktop(QSize size, int seed = 1)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(QColor(28, 34, 42));

    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, size.width(), 48), QColor(52, 62, 74));
    // "Text": hard-edged bars, exactly the content JPEG ruins and PNG does not.
    for (int line = 0; line < 24; ++line) {
        const int y = 90 + line * 22;
        if (y > size.height() - 12)
            break;
        painter.fillRect(QRect(40, y, 60 + (line * 37 + seed * 13) % 420, 9),
                         line % 3 == 0 ? QColor(226, 232, 240) : QColor(150, 200, 240));
    }
    painter.end();

    // A photograph's worth of noise in the lower right quarter.
    QRandomGenerator random{quint32(seed)};
    const int startX = size.width() / 2;
    const int startY = size.height() / 2;
    for (int y = startY; y < size.height(); ++y) {
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = startX; x < size.width(); ++x)
            pixels[x] = random.generate() | 0xff000000u;
    }
    return image;
}

// One end of a share: the shared encoder plus the sealing session, and the
// receiving session at the other end of the same call.
struct SharePair final {
    CallId id = CallId::generate();
    QByteArray secret = generateCallSecret();
    std::shared_ptr<ScreenTileEncoder> encoder;
    std::unique_ptr<CallScreenSession> sender;
    std::unique_ptr<CallScreenSession> receiver;
    // The receiver's own encoder. Unused for picture, but a session always has
    // one because either end may start sharing.
    std::shared_ptr<ScreenTileEncoder> receiverEncoder;

    explicit SharePair(ScreenShareTuning tuning = {})
        : encoder(std::make_shared<ScreenTileEncoder>(tuning))
        , receiverEncoder(std::make_shared<ScreenTileEncoder>(tuning))
    {
        sender = CallScreenSession::create(id, CallDirection::Outgoing, secret, encoder, tuning);
        receiver =
            CallScreenSession::create(id, CallDirection::Incoming, secret, receiverEncoder, tuning);
    }

    [[nodiscard]] QByteArray capture(const QImage &image, qint64 nowMs)
    {
        const QByteArrayView payload =
            encoder->buildUpdate(ScreenFrameView::fromImage(image), nowMs);
        return payload.isEmpty() ? QByteArray() : sender->sealUpdate(payload, nowMs);
    }

    // Captures and delivers in one step, returning what the receiver made of it.
    std::optional<CallScreenSession::Update> push(const QImage &image, qint64 nowMs)
    {
        const QByteArray packet = capture(image, nowMs);
        if (packet.isEmpty())
            return std::nullopt;
        return receiver->decode(packet, nowMs);
    }

    // Runs the report loop once in the receiver-to-sender direction.
    bool report(qint64 nowMs)
    {
        const QByteArray feedback = receiver->encodeFeedback(nowMs);
        if (feedback.isEmpty())
            return false;
        const auto applied = sender->decode(feedback, nowMs);
        return applied && applied->kind == CallScreenSession::Update::Kind::Feedback;
    }
};

// Drives a share until the receiver's canvas is complete, or gives up. A share
// fills in over several frames by design: the first sweep is spread across the
// byte budget rather than sent as one enormous packet.
[[nodiscard]] int settle(SharePair &pair, const QImage &desktop, qint64 &nowMs,
                         int maxFrames = 400)
{
    // Whatever canvas is already there belongs to the previous share; this one
    // is only settled once it has produced a complete canvas of its own.
    const ScreenCanvasPtr previous = pair.receiver->canvas();
    int packets = 0;
    for (int i = 0; i < maxFrames; ++i) {
        // Comfortably wider than the pacing gap of any rung, so the encoder
        // never declines a frame here for arriving too soon.
        nowMs += 110;
        const auto update = pair.push(desktop, nowMs);
        if (update && update->kind == CallScreenSession::Update::Kind::Frame)
            ++packets;
        const ScreenCanvasPtr canvas = pair.receiver->canvas();
        if (canvas && canvas != previous && canvas->isComplete())
            break;
    }
    return packets;
}

#ifdef Q_OS_LINUX
// Resident pages, which is where a leaked desktop-sized buffer would show up.
[[nodiscard]] qint64 residentKb()
{
    QFile status(QStringLiteral("/proc/self/statm"));
    if (!status.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> fields = status.readAll().simplified().split(' ');
    if (fields.size() < 2)
        return -1;
    return fields.at(1).toLongLong() * (qint64(sysconf(_SC_PAGESIZE)) / 1024);
}
#endif

} // namespace

class ScreenShareTest final : public QObject
{
    Q_OBJECT

private slots:
    void keySchedulesAreAllDistinct()
    {
        const auto id = CallId::generate();
        const auto secret = generateCallSecret();
        const auto audio = CallMediaKeySchedule::derive(secret, id);
        const auto video = CallMediaKeySchedule::deriveVideo(secret, id);
        const auto screen = CallMediaKeySchedule::deriveScreen(secret, id);
        const auto reports = CallMediaKeySchedule::deriveScreenFeedback(secret, id);
        QVERIFY(audio && video && screen && reports);
        // Four domains, eight keys, every one of them different: a call may run
        // a microphone, a camera, a screen and its reports at once, and all four
        // number their frames from zero.
        const QList<QByteArray> keys{audio->fromCaller.key,  audio->fromCallee.key,
                                     video->fromCaller.key,  video->fromCallee.key,
                                     screen->fromCaller.key, screen->fromCallee.key,
                                     reports->fromCaller.key, reports->fromCallee.key};
        for (int i = 0; i < keys.size(); ++i) {
            QVERIFY(!keys.at(i).isEmpty());
            for (int j = i + 1; j < keys.size(); ++j)
                QVERIFY2(keys.at(i) != keys.at(j), "two media domains share a key");
        }
        // A screen packet must not be mistaken for a camera or a voice packet
        // by a client that only knows those.
        QCOMPARE(int(CallScreenSession::wireVersion), 3);
        QVERIFY(CallScreenSession::wireVersion != CallVideoSession::wireVersion);
        QVERIFY(CallScreenSession::wireVersion != CallMediaPacket::currentVersion);
    }

    void reconstructsTheDesktopExactlyWhereItMatters()
    {
        SharePair pair;
        const QImage desktop = syntheticDesktop(QSize(1280, 800));
        qint64 now = 1000;
        QVERIFY(settle(pair, desktop, now) > 0);

        const ScreenCanvasPtr canvas = pair.receiver->canvas();
        QVERIFY(canvas);
        QCOMPARE(canvas->size(), desktop.size()); // inside the 1920 cap, sent 1:1
        QVERIFY(canvas->isComplete());

        // The flat backdrop and the hard-edged text are carried losslessly:
        // those tiles took the palette path, so they come back bit for bit.
        QCOMPARE(canvas->image().pixel(10, 300), desktop.pixel(10, 300));
        QCOMPARE(canvas->image().pixel(45, 92), desktop.pixel(45, 92));
        QCOMPARE(canvas->image().pixel(300, 20), desktop.pixel(300, 20));
        // The noisy quarter went through JPEG, so it is close rather than exact.
        const QColor received = canvas->image().pixelColor(900, 700);
        const QColor original = desktop.pixelColor(900, 700);
        QVERIFY(qAbs(received.red() - original.red()) < 90);
    }

    void onlyTheTilesThatChangedAreSent()
    {
        SharePair pair;
        QImage desktop = syntheticDesktop(QSize(1280, 800));
        qint64 now = 1000;
        QVERIFY(settle(pair, desktop, now) > 0);
        const quint64 tilesAfterFirstSweep = pair.encoder->stats().tilesSent;
        QVERIFY(tilesAfterFirstSweep > 0);

        // An untouched desktop costs a heartbeat and nothing else.
        for (int i = 0; i < 20; ++i) {
            now += 110;
            const QByteArray packet = pair.capture(desktop, now);
            QVERIFY2(packet.size() < 200, "a motionless desktop must not re-encode itself");
        }
        QCOMPARE(pair.encoder->stats().tilesSent, tilesAfterFirstSweep);

        // One small change costs one small update.
        {
            QPainter painter(&desktop);
            painter.fillRect(QRect(200, 300, 40, 30), QColor(240, 90, 60));
        }
        now += 110;
        const QByteArray packet = pair.capture(desktop, now);
        QVERIFY(!packet.isEmpty());
        QCOMPARE(pair.encoder->stats().lastTileCount, 1);
        QVERIFY2(packet.size() < 30000, "one changed tile must not cost a whole frame");
        const auto update = pair.receiver->decode(packet, now);
        QVERIFY(update && update->kind == CallScreenSession::Update::Kind::Frame);
        QVERIFY(!update->dirty.isNull());
        QVERIFY(update->dirty.contains(QRect(200, 300, 40, 30)));
        QCOMPARE(pair.receiver->canvas()->image().pixel(210, 310), desktop.pixel(210, 310));
    }

    void flatRegionsCostAlmostNothing()
    {
        SharePair pair;
        QImage desktop(QSize(1024, 640), QImage::Format_RGB32);
        desktop.fill(QColor(20, 30, 40));
        qint64 now = 1000;
        QVERIFY(settle(pair, desktop, now) > 0);
        QVERIFY(pair.receiver->canvas()->isComplete());
        // 8x5 tiles of one colour: three bytes each plus five of header. An
        // entire blank 1024x640 desktop must fit in well under a kilobyte.
        QVERIFY2(pair.encoder->stats().bytesSent < 1024,
                 qPrintable(QStringLiteral("a blank desktop cost %1 bytes")
                                .arg(pair.encoder->stats().bytesSent)));
        QCOMPARE(pair.receiver->canvas()->image().pixel(500, 300), desktop.pixel(500, 300));
    }

    void highResolutionSourcesAreCappedNotBlindlyEncoded_data()
    {
        QTest::addColumn<QSize>("source");
        QTest::addColumn<QSize>("expected");
        QTest::newRow("1080p") << QSize(1920, 1080) << QSize(1920, 1080);
        QTest::newRow("1440p") << QSize(2560, 1440) << QSize(1280, 720);
        QTest::newRow("4k") << QSize(3840, 2160) << QSize(1920, 1080);
        QTest::newRow("ultrawide") << QSize(5120, 1440) << QSize(1707, 480);
    }
    void highResolutionSourcesAreCappedNotBlindlyEncoded()
    {
        QFETCH(QSize, source);
        QFETCH(QSize, expected);
        SharePair pair;
        const QImage desktop = syntheticDesktop(source);
        qint64 now = 1000;
        now += 34;
        QVERIFY(!pair.capture(desktop, now).isEmpty());
        // Nothing above the 1920 cap is ever encoded, whatever the display is.
        QCOMPARE(pair.encoder->stats().outputSize, expected);
        QVERIFY(pair.encoder->stats().outputSize.width() <= 1920);
        QVERIFY(pair.encoder->stats().outputSize.height() <= 1920);
    }

    void theSendRateIsOursNotTheDisplays()
    {
        // A 240 Hz display offering a new frame every four milliseconds.
        SharePair pair;
        QImage desktop = syntheticDesktop(QSize(1280, 800));
        qint64 now = 1000;
        settle(pair, desktop, now);

        const int fps = pair.encoder->targetFps();
        QVERIFY(fps > 0 && fps <= 60);
        const quint64 before = pair.encoder->stats().framesSent + pair.encoder->stats().framesIdle;
        int offered = 0;
        for (int step = 0; step < 250; ++step) { // one second at 240 Hz
            now += 4;
            ++offered;
            // Move something every frame so nothing is skipped for being still.
            QPainter painter(&desktop);
            painter.fillRect(QRect(10 + (step % 40), 500, 12, 12), QColor(step % 255, 80, 120));
            painter.end();
            (void)pair.capture(desktop, now);
        }
        const quint64 produced =
            pair.encoder->stats().framesSent + pair.encoder->stats().framesIdle - before;
        QCOMPARE(offered, 250);
        QVERIFY2(produced <= quint64(fps) + 4,
                 qPrintable(QStringLiteral("%1 frames produced from 250 offered at %2 fps")
                                .arg(produced).arg(fps)));
        QVERIFY(produced >= quint64(fps) - 4);
        QVERIFY(pair.encoder->stats().framesSkipped > 200);
    }

    void noPacketEverExceedsTheTransportLimit()
    {
        // The worst case for the budget: a 4K desktop of pure noise, every tile
        // changing every frame.
        ScreenShareTuning tuning;
        SharePair pair(tuning);
        qint64 now = 1000;
        for (int frame = 0; frame < 24; ++frame) {
            const QImage noise = syntheticDesktop(QSize(3840, 2160), frame + 1);
            now += 40;
            const QByteArray packet = pair.capture(noise, now);
            if (packet.isEmpty())
                continue;
            QVERIFY2(packet.size() <= tuning.maxPacketBytes,
                     qPrintable(QStringLiteral("packet was %1 bytes").arg(packet.size())));
            // And it is still a packet the relay will carry.
            QVERIFY(packet.size() < 1024 * 1024);
            QVERIFY(pair.receiver->decode(packet, now).has_value());
        }
        // Nothing queued up behind the budget: what could not be sent stayed
        // dirty and went next time, rather than accumulating as stale frames.
        QVERIFY(pair.encoder->stats().dirtyTiles <= pair.encoder->stats().totalTiles);
    }

    void stoppingClearsTheFarEndAtOnce()
    {
        SharePair pair;
        const QImage desktop = syntheticDesktop(QSize(1024, 640));
        qint64 now = 1000;
        settle(pair, desktop, now);
        QVERIFY(pair.receiver->canvas());
        QVERIFY(pair.receiver->isReceiving());

        const QByteArray stop = pair.sender->encodeStop();
        QVERIFY(!stop.isEmpty());
        const auto update = pair.receiver->decode(stop, now + 10);
        QVERIFY(update);
        QCOMPARE(update->kind, CallScreenSession::Update::Kind::Stopped);
        // And the canvas is gone with it: a stopped share holds no pixels.
        QVERIFY(!pair.receiver->canvas());
        QVERIFY(!pair.receiver->isReceiving());
    }

    void restartingAfterAStopWorks()
    {
        SharePair pair;
        const QImage first = syntheticDesktop(QSize(1024, 640), 1);
        const QImage second = syntheticDesktop(QSize(1280, 800), 2);
        qint64 now = 1000;

        for (int cycle = 0; cycle < 5; ++cycle) {
            const QImage &desktop = cycle % 2 == 0 ? first : second;
            settle(pair, desktop, now);
            QVERIFY2(pair.receiver->canvas(), "a restarted share produced no canvas");
            QCOMPARE(pair.receiver->canvas()->size(), desktop.size());
            QVERIFY(pair.receiver->canvas()->isComplete());

            const QByteArray stop = pair.sender->encodeStop();
            QVERIFY(!stop.isEmpty());
            QVERIFY(pair.receiver->decode(stop, ++now));
            QVERIFY(!pair.receiver->canvas());
            pair.encoder->reset();
        }
    }

    void aLostPacketIsNoticedAndRepaired()
    {
        SharePair pair;
        QImage desktop = syntheticDesktop(QSize(1024, 640));
        qint64 now = 1000;
        settle(pair, desktop, now);
        QVERIFY(pair.receiver->canvas()->isComplete());

        // Change the desktop and drop the update on the floor, as a dropped
        // link would. The receiver's picture is now stale in that square.
        {
            QPainter painter(&desktop);
            painter.fillRect(QRect(300, 200, 200, 150), QColor(250, 40, 40));
        }
        now += 110;
        const QByteArray lost = pair.capture(desktop, now);
        QVERIFY(!lost.isEmpty());
        QVERIFY(pair.receiver->canvas()->image().pixel(350, 250) != desktop.pixel(350, 250));

        // The next packet's sequence has a hole in it, which is exactly how the
        // receiver knows: it asks for the picture again in its next report.
        {
            QPainter painter(&desktop);
            painter.fillRect(QRect(20, 20, 30, 20), QColor(10, 240, 10));
        }
        now += 110;
        const QByteArray next = pair.capture(desktop, now);
        QVERIFY(!next.isEmpty());
        QVERIFY(pair.receiver->decode(next, now));

        now += 600;
        QVERIFY2(pair.report(now), "the receiver never reported the gap");
        // The sender resent everything, so the square that was lost is repaired.
        for (int i = 0; i < 200; ++i) {
            now += 110;
            (void)pair.push(desktop, now);
            if (pair.receiver->canvas()->image().pixel(350, 250) == desktop.pixel(350, 250))
                break;
        }
        QCOMPARE(pair.receiver->canvas()->image().pixel(350, 250), desktop.pixel(350, 250));
    }

    void qualityFallsOnLossAndClimbsBackOnCleanReports()
    {
        SharePair pair;
        QImage desktop = syntheticDesktop(QSize(1280, 800));
        qint64 now = 1000;
        settle(pair, desktop, now);
        const int startingLevel = pair.sender->desiredLevel();

        // A link that drops three packets in four. The receiver still sees the
        // ones that arrive, so it can still report — and what it reports is a
        // sequence that jumped by four for every one frame it applied.
        int previous = startingLevel;
        int delivered = 0;
        for (int round = 0; round < 6; ++round) {
            for (int i = 0; i < 20; ++i) {
                now += 110;
                QPainter painter(&desktop);
                painter.fillRect(QRect(600 + (i % 10) * 6, 100, 6, 6), QColor(i * 9, 30, 40));
                painter.end();
                const QByteArray packet = pair.capture(desktop, now);
                if (packet.isEmpty())
                    continue;
                if (++delivered % 4 == 0)
                    QVERIFY(pair.receiver->decode(packet, now).has_value());
            }
            now += 600;
            QVERIFY(pair.report(now));
            QVERIFY2(pair.sender->desiredLevel() >= previous, "quality climbed while losing");
            previous = pair.sender->desiredLevel();
        }
        QVERIFY2(pair.sender->desiredLevel() > startingLevel,
                 "sustained loss never cost a single rung");
        QVERIFY(pair.sender->stats().lossRatio > 0.5);
        const int degraded = pair.sender->desiredLevel();

        // The ladder falls in the order the brief asks for: the byte ceiling
        // first, then the frame rate, and only then the resolution.
        const auto &levels = screenShareLevels();
        for (size_t i = 1; i < levels.size(); ++i) {
            QVERIFY(levels.at(i).bytesPerSecond < levels.at(i - 1).bytesPerSecond);
            QVERIFY(levels.at(i).targetFps <= levels.at(i - 1).targetFps);
            QVERIFY(levels.at(i).divisor >= levels.at(i - 1).divisor);
        }
        QVERIFY(levels.front().divisor == 1);

        // Now deliver everything and push hard enough that the link is being
        // asked for the whole ceiling; clean reports earn the rungs back.
        for (int round = 0; round < 40 && pair.sender->desiredLevel() > 0; ++round) {
            for (int i = 0; i < 20; ++i) {
                now += 34;
                const QImage moving = syntheticDesktop(QSize(1280, 800), round * 20 + i + 3);
                const QByteArray packet = pair.capture(moving, now);
                if (!packet.isEmpty())
                    QVERIFY(pair.receiver->decode(packet, now).has_value());
            }
            now += 600;
            (void)pair.report(now);
        }
        QVERIFY2(pair.sender->desiredLevel() < degraded,
                 "a clean, saturated link never earned a rung back");
    }

    void aFrameStillInFlightIsNotMistakenForALostOne()
    {
        // The case a real network path turned up. A near-motionless desktop
        // sends only a heartbeat a second, so at any moment one of the handful
        // of frames in a reporting window is still on the wire. Counting that
        // against what was sent reads as catastrophic loss and walks a
        // perfectly good link all the way down the ladder.
        SharePair pair;
        QImage desktop = syntheticDesktop(QSize(1280, 800));
        qint64 now = 1000;
        settle(pair, desktop, now);
        const int settledLevel = pair.sender->desiredLevel();
        const QSize settledSize = pair.encoder->stats().outputSize;

        QByteArray inFlight;
        for (int round = 0; round < 30; ++round) {
            for (int i = 0; i < 6; ++i) {
                now += 200;
                QPainter painter(&desktop);
                painter.fillRect(QRect(100 + (i * 5), 700, 5, 5), QColor(30, i * 30, 90));
                painter.end();
                // Everything is delivered, but always one packet behind: there
                // is permanently one frame in flight, exactly as on a real link.
                const QByteArray packet = pair.capture(desktop, now);
                if (packet.isEmpty())
                    continue;
                if (!inFlight.isEmpty())
                    QVERIFY(pair.receiver->decode(inFlight, now).has_value());
                inFlight = packet;
            }
            now += 600;
            QVERIFY(pair.report(now));
        }
        QVERIFY2(pair.sender->stats().lossRatio < 0.05,
                 qPrintable(QStringLiteral("a lossless link measured %1 loss")
                                .arg(pair.sender->stats().lossRatio)));
        QVERIFY2(pair.sender->desiredLevel() <= settledLevel,
                 "a healthy link was walked down the quality ladder");
        QCOMPARE(pair.encoder->stats().outputSize, settledSize);
    }

    void aViewNobodyIsShowingIsNotEncodedFor()
    {
        SharePair pair;
        const QImage desktop = syntheticDesktop(QSize(1920, 1080));
        qint64 now = 1000;
        settle(pair, desktop, now);
        const int busyFps = pair.encoder->targetFps();
        QVERIFY(busyFps >= 10);

        // The viewer closes the panel. Its next report says the view has no
        // area, and the sender drops to a trickle rather than encoding a
        // desktop nobody is looking at.
        pair.receiver->setViewSize(QSize());
        now += 600;
        QVERIFY(pair.report(now));
        QVERIFY(pair.sender->remoteViewHidden());
        pair.encoder->setRemoteView(QSize(), false);
        QVERIFY(pair.encoder->targetFps() < busyFps);
        QCOMPARE(pair.encoder->targetFps(), ScreenShareTuning{}.idleFps);

        // A small panel caps the resolution, without going below a floor that
        // would make text unreadable if the panel is later enlarged.
        pair.encoder->reset();
        pair.encoder->setRemoteView(QSize(320, 180), true);
        now += 34;
        QVERIFY(!pair.capture(desktop, now).isEmpty());
        QVERIFY2(pair.encoder->stats().outputSize.width() < 1920,
                 "a share in a 320 px panel was still encoded at full width");
        QVERIFY(pair.encoder->stats().outputSize.width() >= 640);
    }

    void forgedReplayedAndMisaddressedPacketsAreRefused()
    {
        SharePair pair;
        const QImage desktop = syntheticDesktop(QSize(640, 480));
        qint64 now = 1000;
        const QByteArray first = pair.capture(desktop, now);
        QVERIFY(!first.isEmpty());

        QByteArray forged = first;
        forged[forged.size() - 1] = char(forged.at(forged.size() - 1) ^ 1);
        QVERIFY(!pair.receiver->decode(forged, now));
        // Flipping an authenticated header byte is caught too: the header is the
        // AEAD's associated data, not just a hint.
        QByteArray reflagged = first;
        reflagged[1] = char(0);
        QVERIFY(!pair.receiver->decode(reflagged, now));

        QVERIFY(pair.receiver->decode(first, now).has_value());
        QVERIFY2(!pair.receiver->decode(first, now), "a replayed packet was applied twice");

        // Another call's session, and the right call with the wrong secret.
        auto strayEncoder = std::make_shared<ScreenTileEncoder>();
        auto wrongCall = CallScreenSession::create(CallId::generate(), CallDirection::Incoming,
                                                   pair.secret, strayEncoder);
        QVERIFY(wrongCall && !wrongCall->decode(first, now));
        auto wrongKey = CallScreenSession::create(pair.id, CallDirection::Incoming,
                                                  generateCallSecret(), strayEncoder);
        QVERIFY(wrongKey && !wrongKey->decode(first, now));

        // Shape errors, before anything is allocated for them.
        QVERIFY(!pair.receiver->decode(QByteArray(), now));
        QVERIFY(!pair.receiver->decode(QByteArray("\3", 1), now));
        QVERIFY(!pair.receiver->decode(QByteArray(4096, '\0'), now));
        // A camera packet must not be read as a screen packet, or the reverse.
        auto camera = CallVideoSession::create(pair.id, CallDirection::Outgoing, pair.secret);
        QVERIFY(camera);
        QImage small(320, 240, QImage::Format_RGB32);
        small.fill(Qt::blue);
        const QByteArray cameraPacket = camera->encode(small);
        QVERIFY(!cameraPacket.isEmpty());
        QVERIFY(!pair.receiver->decode(cameraPacket, now));
    }

    void authenticatedButMalformedUpdatesAreRefused()
    {
        SharePair pair;
        CallMediaSealer sealer(
            CallMediaKeySchedule::deriveScreen(pair.secret, pair.id)->fromCaller);
        CallMediaPacket packet;
        packet.version = CallScreenSession::wireVersion;
        packet.callId = pair.id;
        packet.flags = CallScreenSession::flagContent;

        const auto sealed = [&](const QByteArray &payload) {
            ++packet.sequence;
            packet.sealed = sealer.seal(packet.sequence, payload, packet.header());
            return packet.encode();
        };
        const auto header = [](int width, int height, int shift, int generation, int tiles) {
            QByteArray out;
            out.append(char(width >> 8));
            out.append(char(width & 0xff));
            out.append(char(height >> 8));
            out.append(char(height & 0xff));
            out.append(char(shift));
            out.append(char(generation));
            out.append(char(tiles >> 8));
            out.append(char(tiles & 0xff));
            return out;
        };

        QVERIFY(!pair.receiver->decode(sealed(QByteArray()), 1)); // truncated header
        QVERIFY(!pair.receiver->decode(sealed(header(0, 480, 7, 1, 0)), 1));      // zero width
        QVERIFY(!pair.receiver->decode(sealed(header(60000, 480, 7, 1, 0)), 1));  // absurd canvas
        QVERIFY(!pair.receiver->decode(sealed(header(640, 480, 2, 1, 0)), 1));    // tiles too small
        QVERIFY(!pair.receiver->decode(sealed(header(640, 480, 14, 1, 0)), 1));   // tiles too large
        QVERIFY(!pair.receiver->decode(sealed(header(640, 480, 7, 1, 9999)), 1)); // impossible count
        QVERIFY(!pair.receiver->canvas());

        // A tile whose declared body runs off the end of the payload stops the
        // walk rather than reading past it.
        QByteArray runaway = header(640, 480, 7, 1, 1);
        runaway.append(char(0)).append(char(0));                        // index 0
        runaway.append(char(quint8(ScreenTileEncoder::TileEncoding::Jpeg)));
        runaway.append(char(0xff)).append(char(0xff));                  // 65535 bytes that follow
        runaway.append("nope");
        const auto walked = pair.receiver->decode(sealed(runaway), 1);
        QVERIFY(!walked || walked->canvas->image().pixel(0, 0) == qRgb(0, 0, 0));

        // A well-formed frame still works afterwards: nothing above poisoned it.
        const QImage desktop = syntheticDesktop(QSize(640, 480));
        qint64 now = 100;
        int applied = 0;
        for (int i = 0; i < 200 && applied < 1; ++i) {
            now += 34;
            const QByteArray good = pair.capture(desktop, now);
            if (good.isEmpty())
                continue;
            if (pair.receiver->decode(good, now))
                ++applied;
        }
        QCOMPARE(applied, 1);
    }

    void reportsCannotCollideWithThePeersOwnShare()
    {
        // Both ends share at once, which is the case that makes the fourth key
        // schedule necessary. A holds one session; B's picture and B's reports
        // about A's picture both arrive on it, and both number their packets
        // from zero. Sharing a key between them would be an outright (key,
        // nonce) collision, and confusing one for the other would corrupt the
        // picture.
        SharePair pair; // sender = A (outgoing), receiver = B (incoming)
        const QImage desktopA = syntheticDesktop(QSize(640, 480), 1);
        const QImage desktopB = syntheticDesktop(QSize(800, 600), 2);
        qint64 now = 1000;
        settle(pair, desktopA, now);
        QVERIFY(pair.receiver->isReceiving());

        int picturesFromB = 0;
        int reportsFromB = 0;
        for (int i = 0; i < 60; ++i) {
            now += 110;
            // B's own share, travelling A-ward on the same media path.
            const QByteArrayView payload = pair.receiverEncoder->buildUpdate(
                ScreenFrameView::fromImage(desktopB), now);
            if (!payload.isEmpty()) {
                const QByteArray packet = pair.receiver->sealUpdate(payload, now);
                QVERIFY(!packet.isEmpty());
                const auto update = pair.sender->decode(packet, now);
                QVERIFY2(update, "A refused B's picture");
                QCOMPARE(update->kind, CallScreenSession::Update::Kind::Frame);
                ++picturesFromB;
            }
            // B's report about A's share, travelling the same way.
            const QByteArray report = pair.receiver->encodeFeedback(now);
            if (!report.isEmpty()) {
                const auto applied = pair.sender->decode(report, now);
                QVERIFY2(applied, "A refused B's report");
                QCOMPARE(applied->kind, CallScreenSession::Update::Kind::Feedback);
                ++reportsFromB;
            }
            // A keeps sharing throughout, so both directions are live.
            const QByteArray onward = pair.capture(desktopA, now);
            if (!onward.isEmpty())
                QVERIFY(pair.receiver->decode(onward, now).has_value());
        }
        QVERIFY(picturesFromB > 5);
        QVERIFY(reportsFromB > 5);
        // A reconstructed B's desktop while B was reconstructing A's, and
        // neither stream lost a packet to the other.
        QVERIFY(pair.sender->canvas());
        QCOMPARE(pair.sender->canvas()->size(), desktopB.size());
        QCOMPARE(pair.receiver->canvas()->size(), desktopA.size());
        QCOMPARE(pair.sender->stats().framesRejected, quint64(0));
        QCOMPARE(pair.receiver->stats().framesRejected, quint64(0));
    }

    void oneEncodeServesEveryMemberOfAMesh()
    {
        // A group call: one desktop, three peers. The picture is built once and
        // only the seal is repeated, which is the whole point of the split.
        const CallId id = CallId::generate();
        auto encoder = std::make_shared<ScreenTileEncoder>();
        std::vector<std::unique_ptr<CallScreenSession>> senders;
        std::vector<std::unique_ptr<CallScreenSession>> receivers;
        for (int peer = 0; peer < 3; ++peer) {
            const QByteArray pairSecret = generateCallSecret();
            senders.push_back(
                CallScreenSession::create(id, CallDirection::Outgoing, pairSecret, encoder));
            receivers.push_back(CallScreenSession::create(
                id, CallDirection::Incoming, pairSecret, std::make_shared<ScreenTileEncoder>()));
            QVERIFY(senders.back() && receivers.back());
        }

        const QImage desktop = syntheticDesktop(QSize(1024, 640));
        qint64 now = 1000;
        for (int frame = 0; frame < 300; ++frame) {
            now += 34;
            const QByteArrayView payload =
                encoder->buildUpdate(ScreenFrameView::fromImage(desktop), now);
            if (payload.isEmpty())
                continue;
            for (size_t peer = 0; peer < senders.size(); ++peer) {
                const QByteArray packet = senders[peer]->sealUpdate(payload, now);
                QVERIFY(!packet.isEmpty());
                // Every peer's copy is sealed under its own pair key, so no two
                // are the same bytes even though the picture is identical.
                if (peer > 0) {
                    const QByteArray other = senders[0]->sealUpdate(payload, now);
                    QVERIFY(packet != other);
                }
                QVERIFY(receivers[peer]->decode(packet, now).has_value());
            }
            if (receivers[0]->canvas() && receivers[0]->canvas()->isComplete())
                break;
        }
        for (const auto &receiver : receivers) {
            QVERIFY(receiver->canvas());
            QCOMPARE(receiver->canvas()->size(), desktop.size());
            QVERIFY(receiver->canvas()->isComplete());
            QCOMPARE(receiver->canvas()->image().pixel(10, 300), desktop.pixel(10, 300));
        }
        // Frames were counted once, not once per peer.
        QVERIFY(encoder->stats().framesSent > 0);
    }

    void repeatedStartAndStopReturnsToBaseline()
    {
        // The leak test the brief asks for: start, stop, start, stop, many
        // times, and check that nothing grows without bound.
        const QImage desktop = syntheticDesktop(QSize(1920, 1080));
        qint64 now = 1000;

        const auto cycle = [&](int frames) {
            SharePair pair;
            for (int i = 0; i < frames; ++i) {
                now += 34;
                (void)pair.push(desktop, now);
            }
            const QByteArray stop = pair.sender->encodeStop();
            if (!stop.isEmpty())
                (void)pair.receiver->decode(stop, ++now);
            pair.encoder->reset();
            // Both halves must be back to holding nothing at all.
            QCOMPARE(pair.encoder->stats().totalTiles, 0);
            QVERIFY(pair.encoder->stats().outputSize.isEmpty());
            QVERIFY(!pair.receiver->canvas());
        };

        // Warm up: the first cycles fault in Qt's image plugins and its own
        // caches, which are not the leak being looked for.
        for (int i = 0; i < 6; ++i)
            cycle(12);

#ifdef Q_OS_LINUX
        const qint64 baseline = residentKb();
        QVERIFY(baseline > 0);
        for (int i = 0; i < 60; ++i)
            cycle(12);
        const qint64 after = residentKb();
        // One 1080p canvas is 8 MB and one encoder's tile state is a few
        // hundred kilobytes; sixty leaked cycles would be hundreds of megabytes.
        QVERIFY2(after - baseline < 24 * 1024,
                 qPrintable(QStringLiteral("resident memory grew %1 KiB over 60 share cycles")
                                .arg(after - baseline)));
#else
        for (int i = 0; i < 20; ++i)
            cycle(12);
#endif
    }

    void aStoppedShareHoldsNoBuffers()
    {
        SharePair pair;
        const QImage desktop = syntheticDesktop(QSize(1920, 1080));
        qint64 now = 1000;
        settle(pair, desktop, now);
        QVERIFY(pair.encoder->stats().totalTiles > 0);
        QVERIFY(pair.receiver->canvas());

        // The canvas is shared, so a view holding it keeps its pixels alive —
        // and letting go is all it takes for them to be freed.
        std::weak_ptr<ScreenCanvas> observer = pair.receiver->canvas();
        ScreenCanvasPtr held = pair.receiver->canvas();
        pair.receiver->resetReceiver();
        QVERIFY2(!observer.expired(), "a view still holding the canvas lost its pixels");
        held.reset();
        QVERIFY2(observer.expired(), "the canvas outlived both the session and its viewer");

        pair.encoder->reset();
        QCOMPARE(pair.encoder->stats().totalTiles, 0);
        QCOMPARE(pair.encoder->stats().dirtyTiles, 0);
        QVERIFY(!pair.receiver->canvas());
    }

    void aResizedSourceRebuildsTheCanvasWithoutStranding()
    {
        // A window being dragged bigger, a monitor changing mode, a display
        // rotating: the geometry moves and the far end follows it.
        SharePair pair;
        qint64 now = 1000;
        const QImage landscape = syntheticDesktop(QSize(1280, 720), 1);
        settle(pair, landscape, now);
        QCOMPARE(pair.receiver->canvas()->size(), QSize(1280, 720));
        const ScreenCanvasPtr first = pair.receiver->canvas();

        const QImage portrait = syntheticDesktop(QSize(720, 1280), 2);
        settle(pair, portrait, now);
        QCOMPARE(pair.receiver->canvas()->size(), QSize(720, 1280));
        QVERIFY2(pair.receiver->canvas() != first,
                 "a new geometry must be a new canvas, not a resized one");
        // The old surface is still valid for anyone who was painting it, which
        // is exactly why it was replaced rather than reallocated.
        QCOMPARE(first->size(), QSize(1280, 720));
        QVERIFY(pair.receiver->canvas()->isComplete());
    }

    void everySupportedCaptureFormatIsReadDirectly_data()
    {
        QTest::addColumn<QImage::Format>("format");
        QTest::newRow("rgb32") << QImage::Format_RGB32;
        QTest::newRow("argb32") << QImage::Format_ARGB32;
        QTest::newRow("argb32pm") << QImage::Format_ARGB32_Premultiplied;
        QTest::newRow("rgbx8888") << QImage::Format_RGBX8888;
        QTest::newRow("rgba8888") << QImage::Format_RGBA8888;
    }
    void everySupportedCaptureFormatIsReadDirectly()
    {
        QFETCH(QImage::Format, format);
        const QImage reference = syntheticDesktop(QSize(512, 384));
        QImage desktop = reference.convertToFormat(format);
        // A capture is entitled to leave its alpha channel as noise. That must
        // not turn a shared screen into a transparent one.
        if (desktop.hasAlphaChannel()) {
            for (int y = 0; y < desktop.height(); ++y) {
                auto *pixels = reinterpret_cast<quint32 *>(desktop.scanLine(y));
                for (int x = 0; x < desktop.width(); ++x)
                    pixels[x] &= 0x00ffffffu;
            }
        }
        const ScreenFrameView view = ScreenFrameView::fromImage(desktop);
        QVERIFY2(view.isValid(), "a plain 32-bit capture format was refused");

        SharePair pair;
        qint64 now = 1000;
        for (int i = 0; i < 300; ++i) {
            now += 34;
            const QByteArrayView payload = pair.encoder->buildUpdate(view, now);
            if (payload.isEmpty())
                continue;
            const QByteArray packet = pair.sender->sealUpdate(payload, now);
            QVERIFY(pair.receiver->decode(packet, now).has_value());
            if (pair.receiver->canvas()->isComplete())
                break;
        }
        QVERIFY(pair.receiver->canvas());
        QVERIFY(pair.receiver->canvas()->isComplete());
        // The flat backdrop survives every format's channel order intact.
        const QColor got = pair.receiver->canvas()->image().pixelColor(10, 300);
        const QColor want = reference.pixelColor(10, 300);
        QCOMPARE(got.rgb() | 0xff000000u, want.rgb() | 0xff000000u);
    }

    void planarAndUnsupportedFramesAreRefusedNotMisread()
    {
        ScreenTileEncoder encoder;
        QVERIFY(encoder.buildUpdate(ScreenFrameView{}, 1).isEmpty());

        QImage indexed(64, 64, QImage::Format_Indexed8);
        indexed.setColorCount(2);
        indexed.fill(0);
        QVERIFY(!ScreenFrameView::fromImage(indexed).isValid());
        QVERIFY(!ScreenFrameView::fromImage(QImage()).isValid());

        // A view whose stride cannot hold its own width is nonsense, whoever
        // built it.
        ScreenFrameView bogus;
        QImage source(64, 64, QImage::Format_RGB32);
        bogus.bits = source.constBits();
        bogus.width = 64;
        bogus.height = 64;
        bogus.bytesPerLine = 16;
        bogus.format = QImage::Format_RGB32;
        QVERIFY(!bogus.isValid());
        QVERIFY(encoder.buildUpdate(bogus, 1).isEmpty());
    }
};

QTEST_GUILESS_MAIN(ScreenShareTest)
#include "tst_screenshare.moc"
