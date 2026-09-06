#include <QtTest>

#include "AudioTestSupport.h"
#include "CallTestSupport.h"
#include "call/CallEngine.h"
#include "call/CallScreenSession.h"
#include "call/CallSignal.h"
#include "call/CallSounds.h"
#include "call/CallTransport.h"
#include "media/AudioConvert.h"

using namespace OpenChat;

namespace {

// A transport that keeps both ends of the call in one process.
//
// Signals are handed straight to the peer (the durable path is reliable and
// in-order, so modelling it as a direct call is faithful); media can be routed,
// held, or dropped so the media-dependent transitions can be driven exactly.
class LoopbackTransport final : public CallTransport
{
public:
    void sendSignal(const ConversationId &conversation, const DeviceId &recipientDevice,
                    const QByteArray &payload) override
    {
        sentSignals.append(payload);
        if (blockSignals)
            return;
        if (peer != nullptr && peer->onSignal)
            peer->onSignal(conversation, localDevice, payload);
    }

    void sendMedia(const ConversationId &conversation, const DeviceId &recipientDevice,
                   const QByteArray &packet) override
    {
        ++mediaSent;
        if (dropMedia)
            return;
        if (peer != nullptr && peer->onMedia)
            peer->onMedia(conversation, localDevice, packet);
    }

    [[nodiscard]] bool isConnected() const override { return connected; }

    // Decodes the signals this end put on the wire, so a test can assert what
    // was said rather than only what state was reached.
    [[nodiscard]] QList<CallSignalMessage> decodedSignals() const
    {
        QList<CallSignalMessage> messages;
        for (const QByteArray &payload : sentSignals) {
            if (const auto decoded = decodeCallSignal(payload))
                messages.append(*decoded);
        }
        return messages;
    }

    [[nodiscard]] bool sentAny(CallSignalType type) const
    {
        const QList<CallSignalMessage> messages = decodedSignals();
        return std::any_of(messages.cbegin(), messages.cend(),
                           [type](const CallSignalMessage &m) { return m.type == type; });
    }

    LoopbackTransport *peer = nullptr;
    DeviceId localDevice = DeviceId::generate();
    bool connected = true;
    bool blockSignals = false;
    bool dropMedia = false;
    int mediaSent = 0;
    QList<QByteArray> sentSignals;
};

// One end of a call: its transport, its devices, and the engine wiring them
// together. Holding the device state in shared_ptrs lets the test keep driving
// audio after the engine has taken ownership of the device objects.
struct Endpoint final {
    OpenChat::CallTest::ScriptedAudioDevices devices;
    LoopbackTransport transport;
    std::unique_ptr<CallEngine> engine;
    // Scripted time: the speaker is the only clock a call has, so time advances
    // one frame per pull. That presents every arrival to the jitter buffer as
    // exactly on the sender's grid, which is the truth of these tests — the
    // loops here are lockstep, not the burst the wall clock would make of them.
    qint64 nowMs = 0;

    void build(CallEngine::Config config)
    {
        config.mediaClock = [this] { return nowMs; };
        engine = std::make_unique<CallEngine>(config, transport, devices.factory());
    }

    void speak(const AudioFrame &frame) { devices.speak(frame); }
    [[nodiscard]] AudioFrame listen()
    {
        nowMs += CallAudioFormat::frameDurationMs;
        return devices.listen();
    }
};

// Pulls `frames` playback frames and reports the loudest. Everything an endpoint
// makes a noise about — ringing, picking up, hanging up, muting — arrives on
// this one stream, so this is how the tests hear it.
[[nodiscard]] double loudestOver(Endpoint &endpoint, int frames)
{
    double peak = 0.0;
    for (int i = 0; i < frames; ++i)
        peak = std::max(peak, OpenChat::AudioConvert::frameRms(endpoint.listen()));
    return peak;
}

// A desktop with flat regions and hard edges, which is what a screen share is
// actually made of.
[[nodiscard]] QImage desktopImage(QSize size, int seed = 1)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(QColor(24, 30, 38));
    for (int band = 0; band < 6; ++band) {
        const int y = 20 + band * 40 + seed;
        if (y + 12 >= size.height())
            break;
        for (int line = y; line < y + 12; ++line) {
            auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(line));
            for (int x = 16; x < size.width() - 16 && x < 16 + 90 + band * 30 + seed * 7; ++x)
                pixels[x] = qRgb(210, 224, 238);
        }
    }
    return image;
}

[[nodiscard]] CallEngine::CallPeer peerFor(const ConversationId &conversation,
                                           const DeviceId &device, const QString &name)
{
    CallEngine::CallPeer peer;
    peer.conversation = conversation;
    peer.device = device;
    peer.contactId = QStringLiteral("ffff");
    peer.displayName = name;
    peer.avatarKey = QStringLiteral("userpfp_none");
    return peer;
}

} // namespace

class CallEngineTest final : public QObject
{
    Q_OBJECT

private:
    ConversationId m_conversation = ConversationId::generate();
    Endpoint m_alice;
    Endpoint m_bob;

    // Connects the two endpoints and gives each a distinct device identity, so
    // the "is this signal from my peer" checks are actually exercised.
    void connectEndpoints(CallEngine::Config config = {})
    {
        connectEndpoints(config, config);
    }

    void connectEndpoints(CallEngine::Config aliceConfig, CallEngine::Config bobConfig)
    {
        m_conversation = ConversationId::generate();
        m_alice.transport.peer = &m_bob.transport;
        m_bob.transport.peer = &m_alice.transport;
        m_alice.build(aliceConfig);
        m_bob.build(bobConfig);
    }

    [[nodiscard]] CallEngine::CallPeer aliceCallsBob() const
    {
        return peerFor(m_conversation, m_bob.transport.localDevice, QStringLiteral("Bob"));
    }

    // Pushes one desktop frame from `from`, advancing its media clock past the
    // encoder's pacing gate so the frame is actually looked at.
    void shareScreen(Endpoint &from, const QImage &desktop)
    {
        from.nowMs += 200;
        from.engine->sendScreenFrame(ScreenFrameView::fromImage(desktop));
    }

    // Arms a share and pushes its first frame, the way the app does.
    [[nodiscard]] bool beginShare(Endpoint &from, const QImage &desktop)
    {
        if (!from.engine->startScreenShare())
            return false;
        shareScreen(from, desktop);
        return true;
    }

    // Drives a share until the far end has a complete picture of it, or gives
    // up. A share fills in over several frames by design.
    [[nodiscard]] ScreenCanvasPtr settleShare(Endpoint &from, Endpoint &to,
                                              const QImage &desktop, QSignalSpy &spy,
                                              int maxFrames = 400)
    {
        Q_UNUSED(to);
        if (!from.engine->isScreenSharing() && !from.engine->startScreenShare())
            return {};
        for (int i = 0; i < maxFrames; ++i) {
            shareScreen(from, desktop);
            if (spy.isEmpty())
                continue;
            const auto canvas = qvariant_cast<ScreenCanvasPtr>(spy.last().first());
            if (canvas && canvas->isComplete())
                return canvas;
        }
        return {};
    }

    // Drives one media frame in each direction, which is what promotes a
    // connected call to Active.
    void exchangeMedia()
    {
        const AudioFrame frame =
            AudioConvert::toFrames(OpenChat::AudioTest::tone(20, 440.0, 0.5)).first();
        m_alice.speak(frame);
        m_bob.speak(frame);
    }

private slots:
    void init()
    {
        m_alice = Endpoint{};
        m_bob = Endpoint{};
    }

    void aCallGoesFromOfferToAudioFlowing()
    {
        connectEndpoints();
        QSignalSpy incoming(m_bob.engine.get(), &CallEngine::incomingCall);

        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        // Bob's device answered the offer, so Alice is ringing rather than
        // merely dialling into the void.
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);
        QCOMPARE(m_bob.engine->state(), CallState::Ringing);
        QCOMPARE(m_bob.engine->direction(), CallDirection::Incoming);
        QCOMPARE(incoming.count(), 1);
        QCOMPARE(m_bob.engine->peer().device, m_alice.transport.localDevice);

        m_bob.engine->acceptCall();
        QCOMPARE(m_alice.engine->state(), CallState::Connecting);
        QCOMPARE(m_bob.engine->state(), CallState::Connecting);
        QVERIFY(m_alice.devices.capture->started);
        QVERIFY(m_alice.devices.playback->started);
        QVERIFY(m_bob.devices.capture->started);

        // Media, not the answer, is what proves the path works in each direction.
        exchangeMedia();
        QCOMPARE(m_alice.engine->state(), CallState::Active);
        QCOMPARE(m_bob.engine->state(), CallState::Active);
    }

    void camerasStreamIndependentlyWithoutInterruptingVoice()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        exchangeMedia();
        QSignalSpy aliceVideo(m_alice.engine.get(), &CallEngine::remoteVideoFrame);
        QSignalSpy bobVideo(m_bob.engine.get(), &CallEngine::remoteVideoFrame);
        QImage landscape(640, 360, QImage::Format_RGB32);
        landscape.fill(Qt::blue);
        QImage portrait(360, 640, QImage::Format_RGB32);
        portrait.fill(Qt::green);
        const auto audioBefore = m_bob.engine->session()->stats().packetsReceived;
        m_alice.engine->sendVideoFrame(landscape);
        QCOMPARE(bobVideo.count(), 1);
        QCOMPARE(qvariant_cast<QImage>(bobVideo.last().first()).size(), landscape.size());
        QCOMPARE(aliceVideo.count(), 0); // receiving never enables our own camera
        QCOMPARE(m_bob.engine->session()->stats().packetsReceived, audioBefore);
        m_bob.engine->sendVideoFrame(portrait);
        QCOMPARE(qvariant_cast<QImage>(aliceVideo.last().first()).size(), portrait.size());
        m_alice.engine->sendVideoFrame(QImage());
        QVERIFY(qvariant_cast<QImage>(bobVideo.last().first()).isNull());
        m_alice.engine->sendVideoFrame(landscape);
        QVERIFY(!qvariant_cast<QImage>(bobVideo.last().first()).isNull());
        exchangeMedia();
        QCOMPARE(m_bob.engine->session()->stats().packetsReceived, audioBefore + 1);
        QCOMPARE(m_alice.engine->state(), CallState::Active);
        QCOMPARE(m_bob.engine->state(), CallState::Active);
        QTRY_VERIFY_WITH_TIMEOUT(qvariant_cast<QImage>(bobVideo.last().first()).isNull(), 3500);
        m_alice.engine->sendVideoFrame(landscape);
        m_alice.engine->hangUp();
        QVERIFY(qvariant_cast<QImage>(bobVideo.last().first()).isNull());
        const int frames = bobVideo.count();
        m_alice.engine->sendVideoFrame(landscape);
        QCOMPARE(bobVideo.count(), frames);
    }

    void aSharedScreenReachesThePeerAndDisappearsWhenItStops()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        exchangeMedia();
        QSignalSpy bobScreen(m_bob.engine.get(), &CallEngine::remoteScreenFrame);
        QSignalSpy aliceScreen(m_alice.engine.get(), &CallEngine::remoteScreenFrame);
        QVERIFY(!m_alice.engine->isScreenSharing());

        const QImage desktop = desktopImage(QSize(1280, 800));
        const ScreenCanvasPtr canvas = settleShare(m_alice, m_bob, desktop, bobScreen);
        QVERIFY2(canvas, "the peer never received a complete picture of the share");
        QCOMPARE(canvas->size(), desktop.size());
        QVERIFY(m_alice.engine->isScreenSharing());
        // Receiving a share never turns our own on.
        QCOMPARE(aliceScreen.count(), 0);
        // The flat backdrop crosses the engine untouched.
        QCOMPARE(canvas->image().pixel(600, 600), desktop.pixel(600, 600));
        QCOMPARE(canvas->image().pixel(40, 26), desktop.pixel(40, 26));

        // Stopping clears the far end at once, rather than on a timeout.
        m_alice.engine->stopScreenShare();
        QVERIFY(!m_alice.engine->isScreenSharing());
        QVERIFY(!bobScreen.isEmpty());
        QVERIFY2(!qvariant_cast<ScreenCanvasPtr>(bobScreen.last().first()),
                 "the share's view was left on screen after it stopped");
        // A frame from a capture callback still in flight when the user pressed
        // stop must not quietly start the whole thing up again.
        const int settled = bobScreen.count();
        const int sentBefore = m_alice.transport.mediaSent;
        shareScreen(m_alice, desktop);
        QCOMPARE(bobScreen.count(), settled);
        QCOMPARE(m_alice.transport.mediaSent, sentBefore);
        QVERIFY(!m_alice.engine->isScreenSharing());
    }

    void aScreenAndACameraRunAtTheSameTimeWithoutDisturbingVoice()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        exchangeMedia();
        QSignalSpy bobScreen(m_bob.engine.get(), &CallEngine::remoteScreenFrame);
        QSignalSpy bobCamera(m_bob.engine.get(), &CallEngine::remoteVideoFrame);

        QImage camera(640, 360, QImage::Format_RGB32);
        camera.fill(Qt::blue);
        const QImage desktop = desktopImage(QSize(1024, 640));
        const auto audioBefore = m_bob.engine->session()->stats().packetsReceived;

        // Three sources at once: microphone, camera, screen.
        QVERIFY(m_alice.engine->startScreenShare());
        for (int i = 0; i < 60; ++i) {
            m_alice.engine->sendVideoFrame(camera);
            shareScreen(m_alice, desktop);
        }
        QVERIFY(!bobCamera.isEmpty());
        QCOMPARE(qvariant_cast<QImage>(bobCamera.last().first()).size(), camera.size());
        QVERIFY(!bobScreen.isEmpty());
        const auto canvas = qvariant_cast<ScreenCanvasPtr>(bobScreen.last().first());
        QVERIFY(canvas);
        QCOMPARE(canvas->size(), desktop.size());
        // Neither video source disturbed the voice path.
        QCOMPARE(m_bob.engine->session()->stats().packetsReceived, audioBefore);
        exchangeMedia();
        QCOMPARE(m_bob.engine->session()->stats().packetsReceived, audioBefore + 1);
        QCOMPARE(m_alice.engine->state(), CallState::Active);
        QCOMPARE(m_bob.engine->state(), CallState::Active);

        // Stopping the screen leaves the camera alone, and the reverse.
        m_alice.engine->stopScreenShare();
        QVERIFY(!qvariant_cast<ScreenCanvasPtr>(bobScreen.last().first()));
        m_alice.engine->sendVideoFrame(camera);
        QVERIFY(!qvariant_cast<QImage>(bobCamera.last().first()).isNull());
    }

    void sharesCanBeStartedAndStoppedRepeatedly()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        exchangeMedia();
        QSignalSpy bobScreen(m_bob.engine.get(), &CallEngine::remoteScreenFrame);

        for (int cycle = 0; cycle < 6; ++cycle) {
            const QImage desktop =
                desktopImage(cycle % 2 == 0 ? QSize(1024, 640) : QSize(800, 600), cycle + 1);
            const ScreenCanvasPtr canvas = settleShare(m_alice, m_bob, desktop, bobScreen);
            QVERIFY2(canvas, qPrintable(QStringLiteral("cycle %1 never delivered").arg(cycle)));
            QCOMPARE(canvas->size(), desktop.size());
            QCOMPARE(canvas->image().pixel(400, 500 % desktop.height()),
                     desktop.pixel(400, 500 % desktop.height()));
            m_alice.engine->stopScreenShare();
            QVERIFY(!qvariant_cast<ScreenCanvasPtr>(bobScreen.last().first()));
            bobScreen.clear();
        }
        QCOMPARE(m_alice.engine->state(), CallState::Active);
        QCOMPARE(m_bob.engine->state(), CallState::Active);
    }

    void aShareIsNotSentWhileTheLinkIsDownAndLosesNothingWhenItReturns()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        exchangeMedia();
        QSignalSpy bobScreen(m_bob.engine.get(), &CallEngine::remoteScreenFrame);

        QImage desktop = desktopImage(QSize(1024, 640));
        QVERIFY(settleShare(m_alice, m_bob, desktop, bobScreen));

        // The link drops. Nothing is put on the wire, and — the point of the
        // check — nothing the encoder was told about is quietly discarded.
        QVERIFY(m_alice.engine->isScreenSharing());
        m_alice.transport.connected = false;
        const int sentBefore = m_alice.transport.mediaSent;
        desktop = desktopImage(QSize(1024, 640), 9);
        for (int i = 0; i < 20; ++i)
            shareScreen(m_alice, desktop);
        QCOMPARE(m_alice.transport.mediaSent, sentBefore);

        // It comes back, and the change made while it was down still arrives.
        m_alice.transport.connected = true;
        const ScreenCanvasPtr repaired = settleShare(m_alice, m_bob, desktop, bobScreen);
        QVERIFY2(repaired, "the share never recovered after the link returned");
        QCOMPARE(repaired->image().pixel(40, 30), desktop.pixel(40, 30));
        QCOMPARE(m_alice.engine->state(), CallState::Active);
    }

    void endingACallReleasesEverythingTheShareHeld()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        exchangeMedia();
        QSignalSpy bobScreen(m_bob.engine.get(), &CallEngine::remoteScreenFrame);
        const QImage desktop = desktopImage(QSize(1280, 800));
        const ScreenCanvasPtr canvas = settleShare(m_alice, m_bob, desktop, bobScreen);
        QVERIFY(canvas);
        std::weak_ptr<ScreenCanvas> observer = canvas;

        // Bob hangs up mid-share. Alice's sending half and Bob's canvas both go.
        m_bob.engine->hangUp();
        QCOMPARE(m_alice.engine->state(), CallState::Ended);
        QVERIFY(!m_alice.engine->isScreenSharing());
        QVERIFY2(!qvariant_cast<ScreenCanvasPtr>(bobScreen.last().first()),
                 "the view was left up after the call ended");
        // The only thing still holding those pixels is this test's own handle.
        QVERIFY(!observer.expired());

        // And a share pushed at a dead call goes nowhere at all.
        const int sentBefore = m_alice.transport.mediaSent;
        QVERIFY(!m_alice.engine->startScreenShare());
        shareScreen(m_alice, desktop);
        QCOMPARE(m_alice.transport.mediaSent, sentBefore);
        QCOMPARE(m_alice.engine->screenShareStats().outputSize, QSize());
    }

    void aShareOfferedBeforeACallExistsIsSimplyIgnored()
    {
        connectEndpoints();
        const QImage desktop = desktopImage(QSize(640, 480));
        // Idle: there is nothing to arm, so there is nothing to send.
        QVERIFY(!m_alice.engine->startScreenShare());
        shareScreen(m_alice, desktop);
        QVERIFY(!m_alice.engine->isScreenSharing());
        QCOMPARE(m_alice.transport.mediaSent, 0);
        m_alice.engine->stopScreenShare(); // safe when nothing is running
        QVERIFY(!m_alice.engine->isScreenSharing());

        // Still ringing: a call that has not been answered has no media keys,
        // so there is nothing to seal a share with.
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QVERIFY(!m_alice.engine->startScreenShare());
        shareScreen(m_alice, desktop);
        QVERIFY(!m_alice.engine->isScreenSharing());
        QCOMPARE(m_alice.engine->screenShareStats().outputSize, QSize());
    }

    void audioCrossesTheEngineIntact()
    {
        // Pin the lossless codec: this test is about the engine wiring carrying
        // audio unchanged, and a lossy codec would make "unchanged" untestable.
        // The codec the engine picks by default is covered separately.
        CallEngine::Config config;
        config.preferredCodec = AudioCodecKind::Pcm;
        connectEndpoints(config);
        // Silence the call's own tones: the pick-up chime mixes into the same
        // stream by design, and this test is about the media path carrying audio
        // unchanged. The sounds have their own tests.
        m_alice.engine->setSoundsEnabled(false);
        m_bob.engine->setSoundsEnabled(false);
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();

        const QList<AudioFrame> speech =
            AudioConvert::toFrames(OpenChat::AudioTest::syntheticSpeech(200));
        QList<AudioFrame> heard;
        for (const AudioFrame &frame : speech) {
            m_alice.speak(frame);
            // Bob's microphone runs too, as it does in a real call even when he
            // is not talking; that steady stream is what promotes Alice's end.
            m_bob.speak(silentAudioFrame());
            heard.append(m_bob.listen());
            (void)m_alice.listen();
        }
        // A call is Active at each end once THAT end is receiving audio, so both
        // ends only reach it when both are sending.
        QCOMPARE(m_alice.engine->state(), CallState::Active);
        QCOMPARE(m_bob.engine->state(), CallState::Active);
        // The engine's default jitter cushion primes on four frames, so playout
        // starts three slots in and everything after that is Alice's audio
        // exactly, frame for frame. Pinning the lead-in here documents the
        // startup latency a call actually has.
        constexpr int leadIn = 3;
        for (int i = 0; i < leadIn; ++i)
            QCOMPARE(heard.at(i), silentAudioFrame());
        QCOMPARE(heard.mid(leadIn), speech.first(speech.size() - leadIn));
        QVERIFY(m_bob.engine->isRemoteSpeaking());
        QVERIFY(!m_bob.engine->isLocalSpeaking());
    }

    void decliningEndsBothSidesWithTheSameReason()
    {
        connectEndpoints();
        QSignalSpy aliceEnded(m_alice.engine.get(), &CallEngine::callEnded);

        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->declineCall();

        QCOMPARE(m_bob.engine->state(), CallState::Ended);
        QCOMPARE(m_bob.engine->endReason(), CallEndReason::Declined);
        QCOMPARE(m_alice.engine->state(), CallState::Ended);
        QCOMPARE(m_alice.engine->endReason(), CallEndReason::Declined);
        QCOMPARE(aliceEnded.count(), 1);
        // Declining must not open the microphone.
        QVERIFY(!m_bob.devices.capture->started);
    }

    void hangingUpTearsDownTheDevicesOnBothSides()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        exchangeMedia();
        QCOMPARE(m_alice.engine->state(), CallState::Active);

        m_alice.engine->hangUp();
        QCOMPARE(m_alice.engine->endReason(), CallEndReason::LocalHangup);
        QCOMPARE(m_bob.engine->endReason(), CallEndReason::RemoteHangup);
        // The microphone stops the instant the call does — that is the part that
        // must not linger — along with the session holding the call keys.
        QVERIFY(!m_alice.devices.capture->started);
        QVERIFY(!m_bob.devices.capture->started);
        QVERIFY(m_alice.engine->session() == nullptr);
        QVERIFY(m_bob.engine->session() == nullptr);

        // The speaker is held a moment longer so the hang-up sound is heard
        // rather than cut off by its own device closing, then released.
        QVERIFY(m_alice.devices.playback->started);
        QTRY_VERIFY_WITH_TIMEOUT(!m_alice.devices.playback->started, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!m_bob.devices.playback->started, 5000);
    }

    void anEndedCallIsHeldUntilItIsDismissed()
    {
        // The reason a call finished is worth showing, so Ended does not fall
        // straight back to Idle on its own.
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_alice.engine->hangUp();
        QCOMPARE(m_alice.engine->state(), CallState::Ended);

        m_alice.engine->dismissEndedCall();
        QCOMPARE(m_alice.engine->state(), CallState::Idle);
        QCOMPARE(m_alice.engine->endReason(), CallEndReason::None);
        // And a new call can be placed straight afterwards.
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
    }

    void aSecondCallerIsToldTheLineIsBusy()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        exchangeMedia();

        // A third party offers Bob a call while he is already talking to Alice.
        Endpoint carol;
        carol.transport.peer = &m_bob.transport;
        carol.build(CallEngine::Config{});
        m_bob.transport.peer = &carol.transport; // Bob's reply goes to Carol
        QVERIFY(carol.engine->placeCall(
            peerFor(ConversationId::generate(), m_bob.transport.localDevice,
                    QStringLiteral("Bob"))));

        QCOMPARE(carol.engine->state(), CallState::Ended);
        QCOMPARE(carol.engine->endReason(), CallEndReason::Busy);
        // Bob's call with Alice is untouched.
        QCOMPARE(m_bob.engine->state(), CallState::Active);
        QCOMPARE(m_bob.engine->peer().device, m_alice.transport.localDevice);
    }

    void simultaneousCallsResolveToTheSameSurvivorOnBothSides()
    {
        // Glare: both ends dial at the same instant. Whatever the rule is, both
        // machines must reach the same answer from information they both have.
        connectEndpoints();
        // Hold the signals so both offers are in flight at once.
        m_alice.transport.blockSignals = true;
        m_bob.transport.blockSignals = true;
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QVERIFY(m_bob.engine->placeCall(
            peerFor(m_conversation, m_alice.transport.localDevice, QStringLiteral("Alice"))));
        m_alice.transport.blockSignals = false;
        m_bob.transport.blockSignals = false;

        const QByteArray aliceOffer = m_alice.transport.sentSignals.constFirst();
        const QByteArray bobOffer = m_bob.transport.sentSignals.constFirst();
        // Now deliver each offer to the other end.
        m_bob.transport.onSignal(m_conversation, m_alice.transport.localDevice, aliceOffer);
        m_alice.transport.onSignal(m_conversation, m_bob.transport.localDevice, bobOffer);

        const CallId aliceId = decodeCallSignal(aliceOffer)->callId;
        const CallId bobId = decodeCallSignal(bobOffer)->callId;
        // Exactly one call survives, and it is the one with the lower id.
        const bool aliceWins = aliceId.bytes() < bobId.bytes();
        if (aliceWins) {
            QCOMPARE(m_alice.engine->direction(), CallDirection::Outgoing);
            QCOMPARE(m_bob.engine->direction(), CallDirection::Incoming);
        } else {
            QCOMPARE(m_alice.engine->direction(), CallDirection::Incoming);
            QCOMPARE(m_bob.engine->direction(), CallDirection::Outgoing);
        }
        // Neither end is left idle or in two calls at once.
        QVERIFY(callOccupiesDevice(m_alice.engine->state()));
        QVERIFY(callOccupiesDevice(m_bob.engine->state()));
    }

    void aRedeliveredOfferDoesNotRestartTheRinging()
    {
        // The signalling path retries, so the same offer arriving twice is
        // routine and must be idempotent.
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QSignalSpy incoming(m_bob.engine.get(), &CallEngine::incomingCall);
        const CallId originalCall = m_bob.engine->peer().device == m_alice.transport.localDevice
            ? CallId::generate()
            : CallId::generate();
        Q_UNUSED(originalCall)

        const QByteArray offer = m_alice.transport.sentSignals.constFirst();
        m_bob.transport.onSignal(m_conversation, m_alice.transport.localDevice, offer);

        QCOMPARE(incoming.count(), 0); // no second alert
        QCOMPARE(m_bob.engine->state(), CallState::Ringing);
    }

    void signalsForOtherCallsAndOtherDevicesAreIgnored()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);

        // A hangup for a call id that is not the live one: a stale message from
        // a call that already ended must not tear down its successor.
        m_alice.transport.onSignal(
            m_conversation, m_bob.transport.localDevice,
            encodeCallSignal(CallSignalMessage::hangup(CallId::generate(),
                                                        CallEndReason::RemoteHangup)));
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);

        // A well-formed answer from a device that is not the peer.
        const CallId liveCall = decodeCallSignal(m_alice.transport.sentSignals.constFirst())->callId;
        m_alice.transport.onSignal(
            m_conversation, DeviceId::generate(),
            encodeCallSignal(CallSignalMessage::answer(liveCall, true, AudioCodecKind::Pcm)));
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);

        // Outright garbage.
        m_alice.transport.onSignal(m_conversation, m_bob.transport.localDevice,
                                    QByteArray("not a signal"));
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);

        // The genuine answer still works.
        m_alice.transport.onSignal(
            m_conversation, m_bob.transport.localDevice,
            encodeCallSignal(CallSignalMessage::answer(liveCall, true, AudioCodecKind::Pcm)));
        QCOMPARE(m_alice.engine->state(), CallState::Connecting);
    }

    void mediaFromTheWrongPlaceNeverReachesTheCall()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        QCOMPARE(m_bob.engine->state(), CallState::Connecting);

        // A media packet attributed to a device that is not the peer.
        m_bob.transport.onMedia(m_conversation, DeviceId::generate(), QByteArray(64, '\x01'));
        // And one for a conversation that is not this call's.
        m_bob.transport.onMedia(ConversationId::generate(), m_alice.transport.localDevice,
                                 QByteArray(64, '\x01'));
        // Neither promoted the call, because neither was accepted.
        QCOMPARE(m_bob.engine->state(), CallState::Connecting);

        exchangeMedia();
        QCOMPARE(m_bob.engine->state(), CallState::Active);
    }

    void anUnansweredCallTimesOut()
    {
        // Only the caller's patience runs out here: give the callee a long ring
        // timeout so the outcome is the caller giving up, not a race between two
        // identical timers.
        CallEngine::Config caller;
        caller.ringTimeoutMs = 40;
        CallEngine::Config callee;
        callee.ringTimeoutMs = 60'000;
        connectEndpoints(caller, callee);
        QSignalSpy ended(m_alice.engine.get(), &CallEngine::callEnded);

        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QVERIFY(ended.wait(2000));
        QCOMPARE(m_alice.engine->endReason(), CallEndReason::NoAnswer);
        // The callee records it as a missed call rather than as a refusal.
        QCOMPARE(m_bob.engine->state(), CallState::Ended);
        QCOMPARE(m_bob.engine->endReason(), CallEndReason::RemoteHangup);
    }

    void aCallWhoseMediaNeverArrivesGivesUp()
    {
        CallEngine::Config config;
        config.ringTimeoutMs = 60'000;
        config.connectTimeoutMs = 40;
        connectEndpoints(config);
        // Answer, but let no media through in either direction.
        m_alice.transport.dropMedia = true;
        m_bob.transport.dropMedia = true;

        QSignalSpy ended(m_alice.engine.get(), &CallEngine::callEnded);
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        QCOMPARE(m_alice.engine->state(), CallState::Connecting);

        QVERIFY(ended.wait(2000));
        QCOMPARE(m_alice.engine->endReason(), CallEndReason::TransportFailed);
    }

    void aCallThatCannotOpenItsMicrophoneFailsCleanly()
    {
        connectEndpoints();
        m_bob.devices.capture->failToStart = true;
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();

        QCOMPARE(m_bob.engine->state(), CallState::Ended);
        QCOMPARE(m_bob.engine->endReason(), CallEndReason::SetupFailed);
        // And the peer is told, rather than left connecting forever.
        QCOMPARE(m_alice.engine->state(), CallState::Ended);
        QVERIFY(m_bob.transport.sentAny(CallSignalType::Hangup));
    }

    void mutingIsCarriedIntoTheLiveSession()
    {
        connectEndpoints();
        QSignalSpy muted(m_alice.engine.get(), &CallEngine::mutedChanged);
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        // What reaches Bob's ear is the subject here, so Bob's own pick-up chime
        // is silenced; the blips themselves are covered by the sound tests.
        m_bob.engine->setSoundsEnabled(false);

        m_alice.engine->setMuted(true);
        QCOMPARE(muted.count(), 1);
        QVERIFY(m_alice.engine->isMuted());
        QVERIFY(m_alice.engine->session()->isMuted());

        // Bob hears silence even though Alice's microphone is producing a tone.
        const AudioFrame loud =
            AudioConvert::toFrames(OpenChat::AudioTest::tone(20, 440.0, 0.9)).first();
        m_alice.speak(loud);
        QCOMPARE(m_bob.listen(), silentAudioFrame());

        // Setting the same value again is not a change.
        m_alice.engine->setMuted(true);
        QCOMPARE(muted.count(), 1);
    }

    void mediaIsNotSentWhileTheLinkIsDown()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();

        m_alice.transport.connected = false;
        const AudioFrame frame =
            AudioConvert::toFrames(OpenChat::AudioTest::tone(20, 440.0, 0.5)).first();
        m_alice.speak(frame);
        QCOMPARE(m_alice.transport.mediaSent, 0);

        m_alice.transport.connected = true;
        m_alice.speak(frame);
        QCOMPARE(m_alice.transport.mediaSent, 1);
    }

    void placingACallWhileOneIsRunningIsRefused()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QVERIFY(!m_alice.engine->placeCall(aliceCallsBob()));
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);
    }

    void acceptingOrDecliningWithNothingRingingDoesNothing()
    {
        connectEndpoints();
        m_alice.engine->acceptCall();
        m_alice.engine->declineCall();
        m_alice.engine->hangUp();
        m_alice.engine->dismissEndedCall();
        QCOMPARE(m_alice.engine->state(), CallState::Idle);
        QVERIFY(m_alice.transport.sentSignals.isEmpty());

        // Nor can the caller accept its own outgoing call.
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        const int signalsSoFar = m_alice.transport.sentSignals.size();
        m_alice.engine->acceptCall();
        QCOMPARE(m_alice.transport.sentSignals.size(), signalsSoFar);
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);
    }

    void acallerAndCalleeAgreeOnACodecBothCanRun()
    {
        // An offer naming a codec this build lacks must be answered on one it
        // has, and the caller must adopt what came back.
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        exchangeMedia();

        QVERIFY(m_alice.engine->session() != nullptr);
        QVERIFY(m_bob.engine->session() != nullptr);
        QCOMPARE(static_cast<int>(m_alice.engine->session()->codec()),
                 static_cast<int>(m_bob.engine->session()->codec()));
        QVERIFY(isAudioCodecAvailable(m_alice.engine->session()->codec()));
        // The default is whatever this build can do best, which is what an
        // ordinary call gets without anyone configuring anything.
        QCOMPARE(static_cast<int>(m_alice.engine->session()->codec()),
                 static_cast<int>(preferredAudioCodec()));
    }

    void theRingingPeersNameCanBeFilledInLate()
    {
        // An inbound call from a contact whose handle has not resolved yet must
        // still be answerable, and must rename itself once it does resolve.
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QVERIFY(m_bob.engine->peer().displayName.isEmpty());

        QSignalSpy changed(m_bob.engine.get(), &CallEngine::stateChanged);
        m_bob.engine->updatePeerIdentity(QStringLiteral("Alice"), QStringLiteral("jessica"));
        QCOMPARE(m_bob.engine->peer().displayName, QStringLiteral("Alice"));
        QCOMPARE(m_bob.engine->peer().avatarKey, QStringLiteral("jessica"));
        QCOMPARE(changed.count(), 1);
    }

    void theRingbackPlaysWhileWaitingForAnAnswer()
    {
        // The caller must hear that something is happening. The ringback starts
        // with the call — before there is any media session at all — which is
        // exactly the stretch a naive implementation leaves silent.
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);
        QVERIFY2(m_alice.devices.playback->started,
                 "the speaker is not open while the call is ringing out");
        QVERIFY(m_alice.engine->session() == nullptr); // no media yet, by design
        // The microphone stays shut until the call is actually answered.
        QVERIFY(!m_alice.devices.capture->started);

        // One second of playback: the ring is audible in it.
        QVERIFY2(loudestOver(m_alice, 50) > 0.02, "no ringback was heard while dialling");

        // Answering stops the ring and plays the pick-up sound instead.
        m_bob.engine->acceptCall();
        QVERIFY(m_alice.engine->session() != nullptr);
        QVERIFY(m_alice.devices.capture->started);
        // Well past the length of the pick-up chime, the caller's stream is
        // quiet again rather than still ringing.
        (void)loudestOver(m_alice, 40);
        QVERIFY2(loudestOver(m_alice, 60) < 0.02, "the ringback kept playing after the answer");
    }

    void theIncomingRingPlaysUntilTheCallIsAnswered()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QCOMPARE(m_bob.engine->state(), CallState::Ringing);
        QVERIFY(m_bob.devices.playback->started);
        // Ringing must not open the microphone: an unanswered call has no
        // business listening to the room.
        QVERIFY(!m_bob.devices.capture->started);
        QVERIFY2(loudestOver(m_bob, 60) > 0.02, "the incoming call made no sound");

        m_bob.engine->acceptCall();
        (void)loudestOver(m_bob, 40); // let the pick-up chime run out
        QVERIFY2(loudestOver(m_bob, 80) < 0.02, "the incoming ring kept playing after answering");
    }

    void decliningStopsTheRingImmediately()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QVERIFY(loudestOver(m_bob, 20) > 0.02);

        m_bob.engine->declineCall();
        // Only the short hang-up sound is left; after it, silence.
        (void)loudestOver(m_bob, 40);
        QVERIFY2(loudestOver(m_bob, 80) < 0.02, "the ring outlived the decline");
    }

    void mutingAndUnmutingAreEachHeardOnce()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        (void)loudestOver(m_alice, 40); // drain the pick-up chime
        QVERIFY(loudestOver(m_alice, 20) < 0.02);

        m_alice.engine->setMuted(true);
        QVERIFY2(loudestOver(m_alice, 12) > 0.02, "muting made no sound");
        QVERIFY2(loudestOver(m_alice, 30) < 0.02, "the mute blip did not stop");

        m_alice.engine->setMuted(false);
        QVERIFY2(loudestOver(m_alice, 12) > 0.02, "unmuting made no sound");

        // Setting the same value again is not a change, so it makes no sound.
        (void)loudestOver(m_alice, 30);
        m_alice.engine->setMuted(false);
        QVERIFY2(loudestOver(m_alice, 12) < 0.02, "a redundant unmute made a sound");
    }

    void hangingUpIsHeardBeforeTheSpeakerCloses()
    {
        // The whole point of holding the speaker open past the end of the call:
        // a hang-up sound cut off by its own device closing is worse than none.
        connectEndpoints();
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        m_bob.engine->acceptCall();
        (void)loudestOver(m_alice, 40);

        m_alice.engine->hangUp();
        QCOMPARE(m_alice.engine->state(), CallState::Ended);
        QVERIFY(m_alice.devices.playback->started);
        QVERIFY2(loudestOver(m_alice, 20) > 0.02, "hanging up made no sound");
        QTRY_VERIFY_WITH_TIMEOUT(!m_alice.devices.playback->started, 5000);
    }

    void silencedSoundsLeaveTheCallItselfWorking()
    {
        // Lossless codec so "untouched by any mixing" can be checked exactly.
        CallEngine::Config config;
        config.preferredCodec = AudioCodecKind::Pcm;
        connectEndpoints(config);
        m_alice.engine->setSoundsEnabled(false);
        QVERIFY(!m_alice.engine->soundsEnabled());
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        // No ring, but the call proceeds exactly as it otherwise would.
        QVERIFY2(loudestOver(m_alice, 60) < 0.02, "a silenced engine still rang");
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);
        QCOMPARE(m_bob.engine->state(), CallState::Ringing);

        m_bob.engine->acceptCall();
        m_bob.engine->setSoundsEnabled(false);
        QCOMPARE(m_alice.engine->state(), CallState::Connecting);

        // And audio still crosses, untouched by any mixing.
        const AudioFrame frame =
            AudioConvert::toFrames(OpenChat::AudioTest::tone(20, 440.0, 0.5)).first();
        for (int i = 0; i < 4; ++i) {
            m_alice.speak(frame);
            (void)m_bob.listen();
        }
        QCOMPARE(m_bob.listen(), frame);

        // Turning them back on mid-ring resumes the right one for the direction.
        m_alice.engine->hangUp();
        m_alice.engine->dismissEndedCall();
        m_alice.engine->setSoundsEnabled(true);
        QVERIFY(m_alice.engine->placeCall(aliceCallsBob()));
        QVERIFY2(loudestOver(m_alice, 50) > 0.02,
                 "re-enabling the sounds did not restore the ring");
    }
};

QTEST_MAIN(CallEngineTest)
#include "tst_callengine.moc"
