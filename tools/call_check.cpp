// Runs ONE side of a real voice call against a real relay.
//
// The automated tests prove the pipeline and the protocol, but they run both
// ends inside a single process against an in-process relay. That leaves the
// questions you actually care about before shipping unanswered: does the
// DEPLOYED relay accept and forward call traffic, and does audio survive a real
// internet path between two separate machines?
//
// This answers them. Run the answering side on one computer and the calling side
// on another, both pointed at the same relay:
//
//   openchat-call-check --relay https://host/v1 --answer
//   openchat-call-check --relay https://host/v1 --call <handle-printed-by-the-answerer>
//
// Add --screen to carry a synthetic desktop alongside the audio. The calling
// side shares it; the answering side reconstructs it and reports whether the
// picture that arrived is the picture that was sent, pixel for pixel in the
// regions a screen share is supposed to carry losslessly. That is the check
// that a share works over a REAL network path rather than only in-process.
//
// The caller streams a deterministic reference signal at real-time pace; the
// answerer regenerates the same signal locally and reports how much of it
// arrived intact. No files are exchanged, so the two sides need share nothing
// but the relay.
//
// Profiles are temporary and in-memory-keyed: this creates throwaway accounts on
// the relay, it does not touch or need a real one.

#include "app/AccountBootstrap.h"
#include "app/AddContactService.h"
#include "app/ContactRequestService.h"
#include "app/ProfileSession.h"
#include "call/CallEngine.h"
#include "call/CallScreenSession.h"
#include "call/QtAudioIo.h"
#include "call/SyncCallTransport.h"
#include "media/AudioConvert.h"
#include "media/WavFile.h"
#include "network/RelayClient.h"
#include "network/RelayTransport.h"
#include "security/KeyVault.h"
#include "storage/SqlCipherContactRepository.h"

#include <QImage>
#include <QPainter>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QUuid>

#include <cmath>
#include <memory>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace OpenChat;

namespace {

QTextStream out(stdout);

void say(const QString &line)
{
    out << QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz")) << "  " << line
        << Qt::endl;
}

// Keys live only for this process: the tool creates a throwaway profile and
// never wants an OS keychain entry for it (and a headless machine has no
// keychain to offer anyway).
class InMemoryVault final : public KeyVault
{
public:
    KeyVaultAvailability availability() const override { return KeyVaultAvailability::Available; }

    Result<SecureBuffer, KeyVaultError> readProfileKey(const ProfileId &) override
    {
        return read(m_databaseKey);
    }
    Result<SecureBuffer, KeyVaultError> createProfileKey(const ProfileId &) override
    {
        return create(m_databaseKey);
    }
    Result<void, KeyVaultError> deleteProfileKey(const ProfileId &) override
    {
        m_databaseKey.reset();
        return Result<void, KeyVaultError>::success();
    }
    Result<SecureBuffer, KeyVaultError> readDeviceWrappingKey(const ProfileId &) override
    {
        return read(m_wrappingKey);
    }
    Result<SecureBuffer, KeyVaultError> createDeviceWrappingKey(const ProfileId &) override
    {
        return create(m_wrappingKey);
    }
    Result<void, KeyVaultError> deleteDeviceWrappingKey(const ProfileId &) override
    {
        m_wrappingKey.reset();
        return Result<void, KeyVaultError>::success();
    }

private:
    static Result<SecureBuffer, KeyVaultError> read(const std::optional<SecureBuffer> &key)
    {
        if (!key)
            return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::NotFound);
        return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
    }
    static Result<SecureBuffer, KeyVaultError> create(std::optional<SecureBuffer> &key)
    {
        if (key)
            return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::AlreadyExists);
        key = SecureBuffer::random(32);
        return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
    }

    std::optional<SecureBuffer> m_databaseKey;
    std::optional<SecureBuffer> m_wrappingKey;
};

// The signal both sides know: a phase-continuous sweep with harmonics, rising a
// little every frame.
//
// Two properties matter. Every frame has a different fundamental, so the
// receiving side can identify exactly WHICH frames arrived and in what order —
// no two frames in a run are alike, so a global match cannot alias onto the
// wrong one. And the sweep is continuous and unwindowed, so it looks like real
// audio to a speech codec; a tone that jumped and re-windowed every 20 ms would
// be a worst case for Opus and would measure the codec rather than the network.
[[nodiscard]] QList<AudioFrame> referenceFrames(int frameCount)
{
    QList<AudioFrame> frames;
    frames.reserve(frameCount);
    double phase = 0.0;
    for (int index = 0; index < frameCount; ++index) {
        const double frequency = 220.0 + 2.0 * index; // unique per frame, well under Nyquist
        QVector<qint16> samples;
        samples.reserve(CallAudioFormat::samplesPerFrame);
        for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i) {
            const double value = std::sin(phase) + 0.35 * std::sin(2.0 * phase)
                + 0.15 * std::sin(3.0 * phase);
            samples.append(static_cast<qint16>(
                std::clamp(0.32 * value, -1.0, 1.0) * 32767.0));
            phase += 2.0 * M_PI * frequency / CallAudioFormat::sampleRate;
        }
        // Keep the accumulated phase bounded so precision does not drift over a
        // long run; a whole number of turns leaves the waveform unchanged.
        phase = std::fmod(phase, 2.0 * M_PI);
        frames.append(AudioConvert::frameOf(samples));
    }
    return frames;
}

// How closely two frames match, as a correlation in [0, 1]. Used instead of
// equality because a lossy codec never reproduces bytes exactly, but a frame
// that arrived is still overwhelmingly more similar to its own reference than
// to any other frame in the sequence.
[[nodiscard]] double similarity(const AudioFrame &first, const AudioFrame &second)
{
    if (!isFullAudioFrame(first) || !isFullAudioFrame(second))
        return 0.0;
    const QVector<qint16> a = AudioConvert::samplesOf(first);
    const QVector<qint16> b = AudioConvert::samplesOf(second);
    double dot = 0.0;
    double energyA = 0.0;
    double energyB = 0.0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a.at(i)) * b.at(i);
        energyA += static_cast<double>(a.at(i)) * a.at(i);
        energyB += static_cast<double>(b.at(i)) * b.at(i);
    }
    if (energyA <= 0.0 || energyB <= 0.0)
        return 0.0;
    return std::abs(dot) / std::sqrt(energyA * energyB);
}

// Stands in for a microphone: emits a frame every 20 ms, for as long as the call
// lasts, exactly as real capture hardware does.
//
// Running continuously matters for more than realism. A call only becomes Active
// once media arrives, so an end that stays silent until it sees the other end's
// audio deadlocks against a peer doing the same. Real microphones never do this
// because they are always producing frames; this has to behave the same way.
//
// Pacing matters too: a tight loop would post a whole call's audio to the
// network in milliseconds, which tells you nothing about how a real one behaves.
class PacedSource final : public QObject
{
public:
    PacedSource(QList<AudioFrame> frames, std::function<void(const AudioFrame &)> sink,
                std::function<void()> onFinished, QObject *parent = nullptr)
        : QObject(parent)
        , m_frames(std::move(frames))
        , m_sink(std::move(sink))
        , m_onFinished(std::move(onFinished))
    {
        m_timer.setInterval(CallAudioFormat::frameDurationMs);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            const bool haveFrame = m_index < m_frames.size();
            if (m_sink)
                m_sink(haveFrame ? m_frames.at(m_index) : silentAudioFrame());
            if (!haveFrame) {
                // Out of material, but keep the stream running: a call whose
                // audio stops entirely reads as a dead link, not as a pause.
                if (!m_finished) {
                    m_finished = true;
                    if (m_onFinished)
                        m_onFinished();
                }
                return;
            }
            ++m_index;
        });
    }

    void start()
    {
        if (!m_timer.isActive())
            m_timer.start();
    }
    void stop() { m_timer.stop(); }
    [[nodiscard]] int sent() const { return m_index; }

private:
    QList<AudioFrame> m_frames;
    std::function<void(const AudioFrame &)> m_sink;
    std::function<void()> m_onFinished;
    QTimer m_timer;
    int m_index = 0;
    bool m_finished = false;
};

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("openchat-call-check"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Runs one side of a real voice call against a real relay."));
    parser.addHelpOption();
    const QCommandLineOption relayOption(
        QStringLiteral("relay"), QStringLiteral("Relay base URL, e.g. https://host/v1."),
        QStringLiteral("url"), QStringLiteral("https://chat.rigidstudios.de/v1"));
    const QCommandLineOption answerOption(
        QStringLiteral("answer"),
        QStringLiteral("Register, print this side's handle, and answer one incoming call."));
    const QCommandLineOption callOption(
        QStringLiteral("call"), QStringLiteral("Register, then call <handle>."),
        QStringLiteral("handle"));
    const QCommandLineOption secondsOption(
        QStringLiteral("seconds"), QStringLiteral("How long to stream audio for."),
        QStringLiteral("n"), QStringLiteral("5"));
    const QCommandLineOption pcmOption(
        QStringLiteral("pcm"),
        QStringLiteral("Force the lossless codec, so frames can be compared exactly."));
    const QCommandLineOption screenOption(
        QStringLiteral("screen"),
        QStringLiteral("Also share a synthetic desktop, and verify it arrived intact."));
    const QCommandLineOption waitOption(
        QStringLiteral("wait"), QStringLiteral("Seconds the answerer waits for a call."),
        QStringLiteral("n"), QStringLiteral("180"));
    const QCommandLineOption audioOption(
        QStringLiteral("audio-devices"),
        QStringLiteral("Open this machine's real microphone and speaker and check frames flow."));
    const QCommandLineOption prefixOption(
        QStringLiteral("handle-prefix"), QStringLiteral("Prefix for the throwaway handle."),
        QStringLiteral("text"), QStringLiteral("callcheck"));
    parser.addOptions({relayOption, answerOption, callOption, secondsOption, pcmOption,
                       screenOption, waitOption,
                       audioOption, prefixOption});
    parser.process(application);

    // --- Device probe: no relay, no call, just the audio hardware. ---
    //
    // Everything else in this tool (and every automated test) substitutes
    // scripted devices, which is what makes them deterministic — but it also
    // means the real capture and playback path is otherwise never exercised.
    // This is the check that the machine can actually hear and speak.
    if (parser.isSet(audioOption)) {
        say(QStringLiteral("default devices usable for the call format: %1")
                .arg(hasUsableCallAudioDevices() ? QStringLiteral("yes") : QStringLiteral("no")));
        const CallAudioIoFactory factory = makeQtCallAudioIoFactory();
        std::unique_ptr<AudioCaptureSource> capture = factory.makeCapture();
        std::unique_ptr<AudioPlaybackSink> playback = factory.makePlayback();
        if (!capture || !playback) {
            say(QStringLiteral("could not construct the audio devices"));
            return 1;
        }

        int captured = 0;
        int pulled = 0;
        double loudest = 0.0;
        bool wrongLength = false;
        capture->onFrame = [&](const AudioFrame &frame) {
            ++captured;
            if (!isFullAudioFrame(frame))
                wrongLength = true;
            else
                loudest = std::max(loudest, AudioConvert::frameRms(frame));
        };
        playback->pullFrame = [&]() -> AudioFrame {
            ++pulled;
            return silentAudioFrame(); // silence, so the probe is not audible
        };

        const bool playbackUp = playback->start();
        const bool captureUp = capture->start();
        say(QStringLiteral("speaker opened: %1")
                .arg(playbackUp ? QStringLiteral("yes") : QStringLiteral("no")));
        say(QStringLiteral("microphone opened: %1")
                .arg(captureUp ? QStringLiteral("yes") : QStringLiteral("no")));

        QTimer::singleShot(2500, &application, [] { QCoreApplication::quit(); });
        application.exec();
        capture->stop();
        playback->stop();

        say(QStringLiteral("captured   %1 frames in 2.5 s (expect ~125)").arg(captured));
        say(QStringLiteral("played     %1 frames pulled by the speaker").arg(pulled));
        say(QStringLiteral("loudest captured frame RMS %1").arg(loudest, 0, 'f', 4));
        const bool good = playbackUp && captureUp && !wrongLength && captured > 80 && pulled > 80;
        say(good ? QStringLiteral("RESULT     PASS — real devices open and stream at the right rate")
                 : QStringLiteral("RESULT     FAIL — the real audio path did not run"));
        return good ? 0 : 1;
    }

    const bool answering = parser.isSet(answerOption);
    const bool calling = parser.isSet(callOption);
    if (answering == calling) {
        out << "choose exactly one of --answer or --call <handle>\n";
        return 2;
    }

    const QString base = parser.value(relayOption);
    const int streamSeconds = std::max(1, parser.value(secondsOption).toInt());
    const int frameCount = streamSeconds * 1000 / CallAudioFormat::frameDurationMs;
    const QList<AudioFrame> reference = referenceFrames(frameCount);

    // --- Profile and account, created fresh on the relay. ---
    QTemporaryDir directory;
    InMemoryVault vault;
    const ProfileId profileId = ProfileId::generate();
    auto created = ProfileSession::create(profileId, vault,
                                          ProfilePaths::forProfile(directory.path(), profileId));
    if (!created.hasValue()) {
        out << "could not create a local profile\n";
        return 1;
    }
    std::unique_ptr<ProfileSession> session = std::move(created).value();

    const auto account = session->accountId();
    const auto credential = session->publicCredential();
    if (!account.hasValue() || !credential.hasValue()) {
        out << "profile has no identity\n";
        return 1;
    }

    RelayClient relay(credential.value().deviceId, account.value(),
                      RelayEndpoints::fromBaseUrl(base), RelayCredentials{});
    RelayTransport transport(relay);

    const QString handle = parser.value(prefixOption) + QStringLiteral("-")
        + QUuid::createUuid().toString(QUuid::Id128).left(10).toLower();

    say(QStringLiteral("relay      %1").arg(base));
    say(QStringLiteral("handle     %1").arg(handle));
    say(QStringLiteral("device     %1").arg(credential.value().deviceId.toHex()));

    int exitCode = 1;
    AccountBootstrap bootstrap(*session, relay, transport);
    bool live = false;

    // --- Everything below is wired up front and driven by the event loop. ---
    std::unique_ptr<ContactRequestService> requests;
    std::unique_ptr<AddContactService> add;
    std::unique_ptr<SyncCallTransport> callTransport;
    std::unique_ptr<CallEngine> call;
    std::unique_ptr<PacedSource> source;
    CallAudioIoFactory devices;

    // Scripted devices: the caller injects the reference signal and the answerer
    // records what it hears. Real hardware is deliberately not in the loop here —
    // this is measuring the network path, not the sound card.
    std::function<void(const AudioFrame &)> captureSink;
    QList<AudioFrame> heard;
    struct ScriptedCapture final : AudioCaptureSource {
        std::function<void(const AudioFrame &)> *slot = nullptr;
        bool start() override { *slot = onFrame; return true; }
        void stop() override { *slot = nullptr; }
    };
    struct ScriptedPlayback final : AudioPlaybackSink {
        QList<AudioFrame> *sink = nullptr;
        QTimer timer;
        bool start() override
        {
            timer.setInterval(CallAudioFormat::frameDurationMs);
            timer.setTimerType(Qt::PreciseTimer);
            QObject::connect(&timer, &QTimer::timeout, &timer, [this] {
                if (pullFrame)
                    sink->append(pullFrame());
            });
            timer.start();
            return true;
        }
        void stop() override { timer.stop(); }
    };
    devices.makeCapture = [&captureSink] {
        auto made = std::make_unique<ScriptedCapture>();
        made->slot = &captureSink;
        return made;
    };
    devices.makePlayback = [&heard] {
        auto made = std::make_unique<ScriptedPlayback>();
        made->sink = &heard;
        return made;
    };

    // The reference desktop. Built the same way on both sides from nothing but
    // its size, so the answerer can compare pixel for pixel without the caller
    // having to send it twice.
    const auto referenceDesktop = []() {
        QImage image(1280, 800, QImage::Format_RGB32);
        image.fill(QColor(24, 30, 38));
        QPainter painter(&image);
        painter.fillRect(QRect(0, 0, 1280, 44), QColor(52, 62, 74));
        for (int line = 0; line < 20; ++line) {
            painter.fillRect(QRect(36, 74 + line * 30, 90 + (line * 53) % 420, 11),
                             line % 2 == 0 ? QColor(226, 232, 240) : QColor(150, 200, 240));
        }
        painter.end();
        // A smooth gradient: many colours, so this block takes the JPEG path
        // while everything above it takes the lossless one. Deterministic, so
        // both sides can generate the identical reference.
        for (int y = 420; y < 740; ++y) {
            auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 700; x < 1180; ++x)
                pixels[x] = qRgb((x * 7) % 256, (y * 5) % 256, ((x + y) * 3) % 256);
        }
        return image;
    }();

    const bool sharingScreen = parser.isSet(screenOption);
    QImage sharedDesktop = referenceDesktop;
    QTimer screenTimer;
    ScreenCanvasPtr receivedDesktop;
    qint64 screenBytes = 0;
    int screenUpdates = 0;

    // Reports what arrived, by matching each played frame against the reference
    // sequence it should have come from.
    const bool lossless = parser.isSet(pcmOption);
    const auto reportScreen = [&]() -> bool {
        if (!sharingScreen)
            return true;
        if (calling) {
            say(QStringLiteral("screen     shared %1 updates, %2 KiB")
                    .arg(screenUpdates)
                    .arg(screenBytes / 1024));
            return true;
        }
        if (!receivedDesktop) {
            say(QStringLiteral("screen     NOTHING ARRIVED"));
            return false;
        }
        say(QStringLiteral("screen     received %1x%2 in %3 updates, %4 KiB")
                .arg(receivedDesktop->size().width())
                .arg(receivedDesktop->size().height())
                .arg(screenUpdates)
                .arg(screenBytes / 1024));
        if (receivedDesktop->size() != referenceDesktop.size()) {
            say(QStringLiteral("screen     WRONG SIZE (expected %1x%2)")
                    .arg(referenceDesktop.width())
                    .arg(referenceDesktop.height()));
            return false;
        }
        if (!receivedDesktop->isComplete()) {
            say(QStringLiteral("screen     INCOMPLETE — parts of the desktop never arrived"));
            return false;
        }
        // The backdrop, the header bar and the text are flat or few-coloured,
        // so they take the lossless path and must come back byte for byte. The
        // photographic block is JPEG and only has to be close.
        const QList<QPoint> exact{{10, 300}, {40, 78}, {600, 20}, {1240, 300}, {300, 400}};
        int wrong = 0;
        for (const QPoint &at : exact) {
            if (receivedDesktop->image().pixel(at) != referenceDesktop.pixel(at))
                ++wrong;
        }
        if (wrong > 0) {
            say(QStringLiteral("screen     %1 of %2 lossless sample points differ")
                    .arg(wrong)
                    .arg(exact.size()));
            return false;
        }
        const QColor got = receivedDesktop->image().pixelColor(900, 600);
        const QColor want = referenceDesktop.pixelColor(900, 600);
        const int drift = qAbs(got.red() - want.red()) + qAbs(got.green() - want.green())
            + qAbs(got.blue() - want.blue());
        say(QStringLiteral("screen     lossless regions exact; photographic drift %1/765")
                .arg(drift));
        return drift < 120;
    };

    const auto report = [&reference, &heard, calling, &source, lossless, &reportScreen]() -> int {
        const bool screenOk = reportScreen();
        if (calling) {
            // This side sent the audio; the other side is the one that can say
            // whether it arrived. Reporting a match here would be measuring the
            // silence the answerer sends back.
            say(QStringLiteral("sent       %1 of %2 reference frames")
                    .arg(source ? source->sent() : 0)
                    .arg(reference.size()));
            say(QStringLiteral("RESULT     see the answering side for the verdict"));
            return screenOk ? 0 : 1;
        }
        // A compressing codec delays its output by a fraction of a frame, so a
        // decoded frame straddles two reference frames and the best per-frame
        // match legitimately alternates between them. Only a jump further back
        // than that smear is real reordering.
        constexpr int codecSmearFrames = 1;
        int matched = 0;
        int misordered = 0;
        int expected = 0;
        double worstMatch = 1.0;
        double totalMatch = 0.0;
        for (const AudioFrame &frame : heard) {
            if (!isFullAudioFrame(frame) || AudioConvert::frameRms(frame) < 0.01)
                continue; // pre-roll, a concealed gap, or silence
            // Find which reference frame this is, searching forward from where
            // the last match left off so ordering is checked as well as content.
            int best = -1;
            double bestScore = 0.0;
            for (int candidate = 0; candidate < reference.size(); ++candidate) {
                const double score = similarity(frame, reference.at(candidate));
                if (score > bestScore) {
                    bestScore = score;
                    best = candidate;
                }
            }
            // A lossy codec never reproduces a frame exactly, so the bar is
            // "unmistakably this frame and not any other", not equality.
            if (best < 0 || bestScore < 0.4)
                continue;
            worstMatch = std::min(worstMatch, bestScore);
            totalMatch += bestScore;
            ++matched;
            if (best < expected - codecSmearFrames)
                ++misordered;
            expected = std::max(expected, best + 1);
        }
        say(QStringLiteral("heard      %1 playback frames").arg(heard.size()));
        say(QStringLiteral("matched    %1 of %2 reference frames (%3%)")
                .arg(matched)
                .arg(reference.size())
                .arg(100.0 * matched / std::max<qsizetype>(1, reference.size()), 0, 'f', 1));
        const double meanMatch = matched > 0 ? totalMatch / matched : 0.0;
        say(QStringLiteral("frame correlation  mean %1  worst %2")
                .arg(meanMatch, 0, 'f', 3)
                .arg(worstMatch, 0, 'f', 3));

        const bool arrived = matched >= static_cast<int>(reference.size()) * 8 / 10;
        bool good = arrived;
        if (lossless) {
            // Every frame is reproduced bit for bit, so each one identifies
            // itself unambiguously and ordering can be held to exactly.
            say(QStringLiteral("misordered %1").arg(misordered));
            good = good && misordered == 0 && meanMatch > 0.99;
        } else {
            // A lossy codec smears each frame into its neighbours, and this
            // reference sweeps slowly enough that adjacent frames genuinely
            // resemble each other. Frame-level ORDERING therefore cannot be
            // established through the codec — run with --pcm to test that. What
            // this mode establishes is that everything arrived and came back out
            // recognisable.
            say(QStringLiteral("ordering   not asserted through a lossy codec "
                               "(use --pcm for exact frame ordering)"));
            good = good && meanMatch > 0.75;
        }
        say(good ? QStringLiteral("RESULT     PASS — audio crossed the relay intact")
                 : QStringLiteral("RESULT     FAIL — too much of the audio did not arrive"));
        if (!screenOk)
            say(QStringLiteral("RESULT     FAIL — the shared screen did not arrive intact"));
        return good && screenOk ? 0 : 1;
    };

    const auto finish = [&application, &exitCode](int code) {
        exitCode = code;
        QTimer::singleShot(0, &application, [] { QCoreApplication::quit(); });
    };

    QObject::connect(&bootstrap, &AccountBootstrap::failed, &application,
                     [&](AccountBootstrap::Error) {
                         say(QStringLiteral("account bootstrap FAILED — the relay refused "
                                            "registration or authentication"));
                         finish(1);
                     });

    QObject::connect(&bootstrap, &AccountBootstrap::succeeded, &application, [&] {
        say(QStringLiteral("registered and authenticated"));
        if (!session->startNetworking(transport).hasValue()) {
            say(QStringLiteral("could not start the sync engine"));
            finish(1);
            return;
        }
        SyncEngine *engine = session->syncEngine();
        requests = std::make_unique<ContactRequestService>(*session, *engine);
        callTransport = std::make_unique<SyncCallTransport>(*engine);
        CallEngine::Config config;
        if (parser.isSet(pcmOption))
            config.preferredCodec = AudioCodecKind::Pcm;
        config.ringTimeoutMs = 120'000;
        call = std::make_unique<CallEngine>(config, *callTransport, devices);
        call->setSoundsEnabled(false); // the tones would mix into the measurement

        // The answering side reconstructs whatever the caller shares.
        QObject::connect(call.get(), &CallEngine::remoteScreenFrame, &application,
                         [&](const ScreenCanvasPtr &canvas) {
                             // Ending the call correctly clears the view, so the
                             // last picture that actually arrived is what gets
                             // reported rather than the null that follows it.
                             if (!canvas)
                                 return;
                             receivedDesktop = canvas;
                             ++screenUpdates;
                         });

        // The calling side's capture: a synthetic desktop rather than a real
        // one, so the two sides can agree on what should have arrived. The
        // pacing is the encoder's, exactly as a real capture's would be.
        screenTimer.setInterval(33);
        QObject::connect(&screenTimer, &QTimer::timeout, &application, [&] {
            if (!call || !call->isScreenSharing())
                return;
            // A moving cursor, so the share is not one motionless frame.
            const int step = (screenUpdates * 7) % 900;
            QPainter painter(&sharedDesktop);
            painter.fillRect(QRect(0, 760, 1280, 40), QColor(24, 30, 38));
            painter.fillRect(QRect(120 + step, 764, 26, 26), QColor(250, 200, 60));
            painter.end();
            ++screenUpdates;
            call->sendScreenFrame(ScreenFrameView::fromImage(sharedDesktop));
            screenBytes = qint64(call->screenShareStats().bytesSent);
            const int wanted = call->screenShareTargetFps();
            const int interval = std::max(8, 1000 / std::max(1, wanted));
            if (screenTimer.interval() != interval)
                screenTimer.setInterval(interval);
        });

        QObject::connect(call.get(), &CallEngine::stateChanged, &application, [&] {
            say(QStringLiteral("call state %1").arg(callStateName(call->state())));
            if (sharingScreen && calling
                && (call->state() == CallState::Connecting
                    || call->state() == CallState::Active)) {
                if (!call->isScreenSharing() && call->startScreenShare()) {
                    say(QStringLiteral("sharing the reference desktop"));
                    screenTimer.start();
                }
            }
            if (call->state() == CallState::Ended) {
                screenTimer.stop();
                call->stopScreenShare();
            }
            // Capture starts at Connecting, the moment the call is answered and
            // the devices are up — not at Active, which is itself a consequence
            // of the peer's media arriving.
            if ((call->state() == CallState::Connecting || call->state() == CallState::Active)
                && source)
                source->start();
            if (call->state() == CallState::Ended) {
                if (source)
                    source->stop();
                say(QStringLiteral("call ended: %1")
                        .arg(callEndReasonName(call->endReason())));
                // Give the last frames a moment to drain before measuring.
                QTimer::singleShot(600, &application, [&] { finish(report()); });
            }
        });

        if (answering) {
            QObject::connect(requests.get(), &ContactRequestService::incomingRequest, &application,
                             [&](const AccountId &, const ConversationId &conversation) {
                                 say(QStringLiteral("contact request received; accepting"));
                                 requests->acceptContact(conversation);
                             });
            // The answering side's "microphone" is silence, but it must still
            // run: without it the caller never sees media and never promotes the
            // call to Active.
            source = std::make_unique<PacedSource>(
                QList<AudioFrame>{}, [&captureSink](const AudioFrame &frame) {
                    if (captureSink)
                        captureSink(frame);
                },
                nullptr);
            QObject::connect(call.get(), &CallEngine::incomingCall, &application, [&] {
                say(QStringLiteral("incoming call; answering"));
                call->acceptCall();
            });
            say(QStringLiteral("WAITING — run the other side with:"));
            say(QStringLiteral("    openchat-call-check --relay %1 --call %2").arg(base, handle));
            return;
        }

        // Calling side: add the peer, wait for them to accept, then dial.
        add = std::make_unique<AddContactService>(*session, relay, *engine);
        QObject::connect(add.get(), &AddContactService::failed, &application,
                         [&](AddContactService::Error) {
                             say(QStringLiteral("could not add the peer — is the handle right "
                                                "and is that side running?"));
                             finish(1);
                         });
        QObject::connect(
            requests.get(), &ContactRequestService::contactAccepted, &application,
            [&](const AccountId &peer) {
                say(QStringLiteral("peer accepted the contact request"));
                auto found = session->contacts()->find(peer);
                if (!found.hasValue() || !found.value() || !found.value()->conversationId
                    || !found.value()->peerDeviceId) {
                    say(QStringLiteral("the accepted contact has no reachable device"));
                    finish(1);
                    return;
                }
                CallEngine::CallPeer target;
                target.conversation = *found.value()->conversationId;
                target.device = *found.value()->peerDeviceId;
                target.displayName = parser.value(callOption);
                source = std::make_unique<PacedSource>(
                    reference, [&captureSink](const AudioFrame &frame) {
                        if (captureSink)
                            captureSink(frame);
                    },
                    [&] {
                        // Let what is still in flight arrive before tearing the
                        // call down, or the measurement blames the network for
                        // frames this side cut off.
                        say(QStringLiteral("finished streaming; draining"));
                        QTimer::singleShot(1000, &application, [&] { call->hangUp(); });
                    });
                say(QStringLiteral("placing the call"));
                if (!call->placeCall(target)) {
                    say(QStringLiteral("could not place the call"));
                    finish(1);
                }
            });
        say(QStringLiteral("adding %1").arg(parser.value(callOption)));
        add->startByHandle(parser.value(callOption));
    });

    QObject::connect(&relay, &RelayClient::connected, &application, [&] {
        if (!live) {
            live = true;
            say(QStringLiteral("live stream connected"));
        }
    });
    QObject::connect(&relay, &RelayClient::transportError, &application,
                     [&](RelayTransportError error) {
                         say(QStringLiteral("transport error %1").arg(static_cast<int>(error)));
                     });

    bootstrap.start(handle, AccountBootstrap::defaultKeyPackageCount);

    const int waitSeconds = std::max(10, parser.value(waitOption).toInt());
    QTimer::singleShot(waitSeconds * 1000, &application, [&] {
        say(QStringLiteral("timed out after %1s").arg(waitSeconds));
        if (!heard.isEmpty())
            exitCode = report();
        QCoreApplication::quit();
    });

    application.exec();
    if (call)
        call.reset();
    relay.disconnect();
    session->lock();
    return exitCode;
}
