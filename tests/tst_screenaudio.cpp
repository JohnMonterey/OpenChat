#include "call/CallSession.h"
#include "call/ScreenAudio.h"
#include "call/ScreenAudioCapture.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QtTest>

#include <atomic>
#include <cmath>
#include <mutex>

#if defined(OPENCHAT_TEST_PULSE)
#    include <pulse/error.h>
#    include <pulse/simple.h>
#endif

using namespace OpenChat;

namespace {

constexpr double pi = 3.14159265358979323846;

// One stereo frame of a tone on each side (0 Hz = silence on that side),
// continuing from `phase` samples in.
[[nodiscard]] StereoFrame toneFrame(double leftHz, double rightHz, qint64 phase, double level = 0.4)
{
    StereoFrame frame(ScreenAudioFormat::bytesPerFrame, Qt::Uninitialized);
    auto *samples = reinterpret_cast<qint16 *>(frame.data());
    for (int i = 0; i < ScreenAudioFormat::samplesPerFrame; ++i) {
        const double t = double(phase + i) / ScreenAudioFormat::sampleRate;
        samples[2 * i] = leftHz > 0 ? qint16(32767 * level * std::sin(2 * pi * leftHz * t)) : 0;
        samples[2 * i + 1] = rightHz > 0 ? qint16(32767 * level * std::sin(2 * pi * rightHz * t)) : 0;
    }
    return frame;
}

// Root mean square of one side of a frame, 0..1.
[[nodiscard]] double sideRms(const StereoFrame &frame, int side)
{
    const auto *samples = reinterpret_cast<const qint16 *>(frame.constData());
    double sum = 0.0;
    for (int i = 0; i < ScreenAudioFormat::samplesPerFrame; ++i) {
        const double value = samples[2 * i + side] / 32768.0;
        sum += value * value;
    }
    return std::sqrt(sum / ScreenAudioFormat::samplesPerFrame);
}

// A sender and a receiver of one call's screen sound, on a scripted clock.
struct Pair final {
    CallId callId = CallId::generate();
    QByteArray secret = QByteArray(32, '\x5a');
    qint64 nowMs = 1000;
    ScreenAudioEncoder encoder;
    std::unique_ptr<ScreenAudioSession> sender;
    std::unique_ptr<ScreenAudioSession> receiver;

    Pair()
    {
        sender = ScreenAudioSession::create(callId, CallDirection::Outgoing, secret, [this] { return nowMs; });
        receiver = ScreenAudioSession::create(callId, CallDirection::Incoming, secret, [this] { return nowMs; });
    }

    [[nodiscard]] QByteArray packetFor(const StereoFrame &frame)
    {
        return sender->seal(encoder.encode(frame));
    }
};

} // namespace

class ScreenAudioTest final : public QObject
{
    Q_OBJECT

private slots:
    // Stereo is the point of a separate stream: a tone on the left must come
    // out on the left, not smeared across both sides as the voice path would.
    void stereoSurvivesTheTripWithTheSidesApart()
    {
        Pair pair;
        QVERIFY(pair.encoder.isValid());
        QVERIFY(pair.sender && pair.receiver);
        double left = 0.0;
        double right = 0.0;
        int heard = 0;
        for (int i = 0; i < 60; ++i) {
            const QByteArray packet = pair.packetFor(toneFrame(440.0, 0.0, qint64(i) * 960));
            QVERIFY(!packet.isEmpty());
            QCOMPARE(quint8(packet[0]), ScreenAudioSession::wireVersion);
            // A share's sound is small enough for the UDP path.
            QVERIFY(packet.size() <= 1400);
            QCOMPARE(pair.receiver->receive(packet), ScreenAudioSession::ReceiveResult::Queued);
            pair.nowMs += 20;
            const StereoFrame frame = pair.receiver->nextFrame();
            QVERIFY(isFullStereoFrame(frame));
            if (i >= 20) {
                left += sideRms(frame, 0);
                right += sideRms(frame, 1);
                ++heard;
            }
        }
        QVERIFY(pair.receiver->isActive());
        QVERIFY2(left / heard > 0.2, qPrintable(QString::number(left / heard)));
        // At least 26 dB between the sides.
        QVERIFY2(right < left / 20.0, qPrintable(QStringLiteral("left %1 right %2").arg(left).arg(right)));
    }

    void packetsThatDoNotBelongAreRefused()
    {
        Pair pair;
        const QByteArray good = pair.packetFor(toneFrame(440.0, 440.0, 0));
        QCOMPARE(pair.receiver->receive(good), ScreenAudioSession::ReceiveResult::Queued);
        QCOMPARE(pair.receiver->receive(good), ScreenAudioSession::ReceiveResult::Replay);

        QByteArray tampered = pair.packetFor(toneFrame(440.0, 440.0, 960));
        tampered[tampered.size() - 3] = char(tampered[tampered.size() - 3] ^ 0x40);
        QCOMPARE(pair.receiver->receive(tampered), ScreenAudioSession::ReceiveResult::Unauthentic);

        QByteArray wrongVersion = pair.packetFor(toneFrame(440.0, 440.0, 1920));
        wrongVersion[0] = char(1);
        QCOMPARE(pair.receiver->receive(wrongVersion), ScreenAudioSession::ReceiveResult::Malformed);

        // Another call's sound, even keyed from the same secret.
        auto stranger = ScreenAudioSession::create(CallId::generate(), CallDirection::Outgoing,
                                                   pair.secret, [&pair] { return pair.nowMs; });
        const QByteArray foreign = stranger->seal(pair.encoder.encode(toneFrame(440.0, 0.0, 0)));
        QCOMPARE(pair.receiver->receive(foreign), ScreenAudioSession::ReceiveResult::WrongCall);

        // The sender's own direction: a reflected packet does not open.
        const QByteArray reflected = pair.packetFor(toneFrame(440.0, 0.0, 2880));
        QCOMPARE(pair.sender->receive(reflected), ScreenAudioSession::ReceiveResult::Unauthentic);

        // Voice keys are a different domain: a voice session cannot open it,
        // and this session cannot open voice.
        CallSession::Config voiceConfig;
        voiceConfig.callId = pair.callId;
        voiceConfig.direction = CallDirection::Incoming;
        auto voice = CallSession::create(voiceConfig, pair.secret);
        QVERIFY(voice);
        QVERIFY(voice->processIncomingPacket(pair.packetFor(toneFrame(440.0, 0.0, 3840)))
                != CallSession::ReceiveResult::Queued);
    }

    // The sender sends a frame every 20 ms while its sound is on, silence
    // included, so a second without one means it is off.
    void soundThatStopsStopsBeingActive()
    {
        Pair pair;
        ScreenAudioMixer mixer;
        auto session = std::shared_ptr<ScreenAudioSession>(std::move(pair.receiver));
        mixer.add(QByteArrayLiteral("peer"), session);
        QVERIFY(mixer.pull().isEmpty()); // nothing received: nothing to mix
        for (int i = 0; i < 30; ++i) {
            QCOMPARE(session->receive(pair.packetFor(toneFrame(440.0, 440.0, qint64(i) * 960))),
                     ScreenAudioSession::ReceiveResult::Queued);
            pair.nowMs += 20;
            (void)mixer.pull();
        }
        QVERIFY(session->isActive());
        QVERIFY(!mixer.pull().isEmpty());
        pair.nowMs += ScreenAudioSession::activeWindowMs + 20;
        QVERIFY(!session->isActive());
        QVERIFY2(mixer.pull().isEmpty(), "a share whose sound stopped is still being mixed");
    }

    void theMixerFollowsTheVolume()
    {
        Pair pair;
        ScreenAudioMixer mixer;
        auto session = std::shared_ptr<ScreenAudioSession>(std::move(pair.receiver));
        mixer.add(QByteArrayLiteral("peer"), session);
        double full = 0.0;
        double half = 0.0;
        for (int i = 0; i < 80; ++i) {
            QCOMPARE(session->receive(pair.packetFor(toneFrame(440.0, 440.0, qint64(i) * 960, 0.5))),
                     ScreenAudioSession::ReceiveResult::Queued);
            pair.nowMs += 20;
            mixer.setVolume(i < 40 ? 1.0 : 0.5);
            const StereoFrame frame = mixer.pull();
            if (i >= 20 && i < 40)
                full += sideRms(frame, 0);
            if (i >= 60)
                half += sideRms(frame, 0);
        }
        QVERIFY2(std::abs(half / full - 0.5) < 0.08, qPrintable(QString::number(half / full)));
        // Muted: nothing to mix, but the buffer is still drained in time.
        mixer.setVolume(0.0);
        QVERIFY(session->receive(pair.packetFor(toneFrame(440.0, 440.0, 81 * 960)))
                == ScreenAudioSession::ReceiveResult::Queued);
        QVERIFY(mixer.pull().isEmpty());
        QVERIFY(session->stats().framesPlayed > 60);
    }

    // The framer hands out one frame every 20 ms whatever the source does:
    // bursts, gaps, or nothing at all.
    void theFramerKeepsASteadyCadence()
    {
        ScreenAudioFramer framer;
        QList<StereoFrame> out;
        const auto collect = [&out](const StereoFrame &frame) { out.append(frame); };
        qint64 now = 10'000;
        framer.pump(now, collect);
        QCOMPARE(out.size(), 1); // silence, before any source
        QCOMPARE(out.first(), silentStereoFrame());

        // Irregular pieces: 30 ms, then nothing, then 50 ms, and so on.
        std::vector<qint16> piece;
        qint64 phase = 0;
        const auto push = [&](int ms) {
            const int frames = ms * 48;
            piece.assign(size_t(frames) * 2, 0);
            for (int i = 0; i < frames; ++i) {
                const double t = double(phase + i) / 48000.0;
                piece[size_t(2 * i)] = piece[size_t(2 * i + 1)] = qint16(12000 * std::sin(2 * pi * 440.0 * t));
            }
            phase += frames;
            framer.push(1, piece.data(), frames);
        };
        const int pattern[] = {30, 0, 50, 10, 0, 30, 40, 0, 20, 20};
        for (int round = 0; round < 10; ++round) {
            for (int ms : pattern) {
                push(ms);
                now += 20;
                framer.pump(now, collect);
            }
        }
        QCOMPARE(out.size(), 1 + 100);
        for (const StereoFrame &frame : out)
            QVERIFY(isFullStereoFrame(frame));
        // After the first few frames (the cushion), the tone is there.
        int loud = 0;
        for (qsizetype i = 5; i < out.size(); ++i)
            loud += sideRms(out.at(i), 0) > 0.1 ? 1 : 0;
        QVERIFY2(loud > 85, qPrintable(QString::number(loud)));

        // A source far ahead of the clock is trimmed, not queued.
        push(1000);
        QVERIFY(framer.trimmedFrames() > 0);

        // Two sources are summed.
        ScreenAudioFramer both;
        QList<StereoFrame> mixed;
        std::vector<qint16> left(48 * 100 * 2, 0);
        std::vector<qint16> right(48 * 100 * 2, 0);
        for (size_t i = 0; i < left.size(); i += 2) {
            left[i] = 8000;
            right[i + 1] = 8000;
        }
        both.push(1, left.data(), 48 * 100);
        both.push(2, right.data(), 48 * 100);
        qint64 clock = 0;
        for (int i = 0; i < 3; ++i) {
            both.pump(clock, [&mixed](const StereoFrame &frame) { mixed.append(frame); });
            clock += 20;
        }
        const auto *samples = reinterpret_cast<const qint16 *>(mixed.last().constData());
        QCOMPARE(samples[0], qint16(8000));
        QCOMPARE(samples[1], qint16(8000));
    }

    // Device audio in another format — 44.1 kHz float mono here — comes out
    // as 48 kHz stereo at the same pitch.
    void theConverterResamplesAndSpreads()
    {
        StereoConverter converter(44'100, 1, StereoConverter::Sample::Float32);
        std::vector<qint16> out;
        std::vector<float> block(441);
        qint64 phase = 0;
        for (int b = 0; b < 100; ++b) { // one second, in 10 ms blocks
            for (int i = 0; i < 441; ++i)
                block[size_t(i)] = float(0.5 * std::sin(2 * pi * 1000.0 * double(phase + i) / 44100.0));
            phase += 441;
            converter.convert(block.data(), qsizetype(block.size()), out);
        }
        const qsizetype frames = qsizetype(out.size() / 2);
        QVERIFY2(std::abs(frames - 48000) <= 2, qPrintable(QString::number(frames)));
        int crossings = 0;
        for (qsizetype i = 1; i < frames; ++i) {
            QCOMPARE(out[size_t(2 * i)], out[size_t(2 * i + 1)]);
            if ((out[size_t(2 * i)] >= 0) != (out[size_t(2 * i - 2)] >= 0))
                ++crossings;
        }
        QVERIFY2(std::abs(crossings - 2000) <= 4, qPrintable(QString::number(crossings)));
    }

    // Against a real sound server: what OpenChat itself plays is never
    // captured, and what another program plays is. Needs a sink to play into
    // silently — `pactl load-module module-null-sink sink_name=<name>` — named
    // in OPENCHAT_TEST_PULSE_SINK; skipped otherwise.
    void ownPlaybackIsNeverCapturedButOthersIs()
    {
#if !defined(OPENCHAT_TEST_PULSE)
        QSKIP("built without the PulseAudio client library");
#else
        const QByteArray sink = qgetenv("OPENCHAT_TEST_PULSE_SINK");
        if (sink.isEmpty())
            QSKIP("set OPENCHAT_TEST_PULSE_SINK to a null sink to run this");
        auto capture = ScreenAudioCapturePlatform::create({});
        QVERIFY(capture);
        std::atomic<int> loudest{0};
        std::atomic<int> frames{0};
        capture->onFrame = [&](const StereoFrame &frame) {
            ++frames;
            const auto *samples = reinterpret_cast<const qint16 *>(frame.constData());
            for (qsizetype i = 0; i < frame.size() / 2; ++i) {
                int previous = loudest.load();
                const int value = std::abs(int(samples[i]));
                while (value > previous && !loudest.compare_exchange_weak(previous, value)) {
                }
            }
        };
        QString failure;
        QVERIFY2(capture->start(failure), qPrintable(failure));

        // Our own process plays a loud tone into the sink.
        const pa_sample_spec spec{PA_SAMPLE_S16LE, 48000, 2};
        int error = 0;
        pa_simple *own = pa_simple_new(nullptr, "tst_screenaudio", PA_STREAM_PLAYBACK, sink.constData(),
                                       "own tone", &spec, nullptr, nullptr, &error);
        QVERIFY2(own != nullptr, pa_strerror(error));
        for (int i = 0; i < 75; ++i) { // 1.5 s
            const StereoFrame tone = toneFrame(440.0, 440.0, qint64(i) * 960, 0.5);
            QCOMPARE(pa_simple_write(own, tone.constData(), size_t(tone.size()), &error), 0);
        }
        pa_simple_drain(own, &error);
        QVERIFY2(loudest.load() == 0, qPrintable(QStringLiteral("captured our own tone at %1").arg(loudest.load())));

        // Another program's tone is captured.
        const QString wav = QDir(QDir::tempPath()).filePath(QStringLiteral("openchat-tst-screenaudio.wav"));
        {
            QFile file(wav);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const quint32 dataBytes = 48000 * 4 * 2;
            QByteArray header;
            const auto le32 = [&header](quint32 v) { header.append(reinterpret_cast<const char *>(&v), 4); };
            const auto le16 = [&header](quint16 v) { header.append(reinterpret_cast<const char *>(&v), 2); };
            header.append("RIFF");
            le32(36 + dataBytes);
            header.append("WAVEfmt ");
            le32(16);
            le16(1);
            le16(2);
            le32(48000);
            le32(48000 * 4);
            le16(4);
            le16(16);
            header.append("data");
            le32(dataBytes);
            file.write(header);
            for (int i = 0; i < 100; ++i)
                file.write(toneFrame(660.0, 660.0, qint64(i) * 960, 0.5));
        }
        QProcess other;
        other.start(QStringLiteral("paplay"), {QStringLiteral("-d"), QString::fromLatin1(sink), wav});
        QVERIFY(other.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(loudest.load() > 8000, 5000);
        other.waitForFinished(5000);
        capture->stop();
        pa_simple_free(own);
        QFile::remove(wav);
        QVERIFY(frames.load() > 50);
#endif
    }
};

QTEST_GUILESS_MAIN(ScreenAudioTest)

#include "tst_screenaudio.moc"
