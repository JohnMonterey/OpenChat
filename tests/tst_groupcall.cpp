#include <QtTest>

#include "AudioTestSupport.h"
#include "CallTestSupport.h"
#include "call/CallEngine.h"
#include "call/CallScreenSession.h"
#include "call/CallSignal.h"
#include "call/CallTransport.h"
#include "media/AudioConvert.h"

#include <map>
#include <memory>

using namespace OpenChat;

namespace {

// A group's worth of endpoints in one process. Signals and media are routed by
// recipient device, so any member can reach any other, exactly as the relay
// would carry them; media can be held back per endpoint to drive the
// media-dependent transitions.
class MeshTransport;

struct Mesh final {
    std::map<QByteArray, MeshTransport *> byDevice;
};

class MeshTransport final : public CallTransport
{
public:
    void sendSignal(const ConversationId &conversation, const DeviceId &recipientDevice,
                    const QByteArray &payload) override;
    void sendMedia(const ConversationId &conversation, const DeviceId &recipientDevice,
                   const QByteArray &packet) override;
    [[nodiscard]] bool isConnected() const override { return connected; }

    [[nodiscard]] int signalsOfType(CallSignalType type) const
    {
        int count = 0;
        for (const auto &[recipient, payload] : sentSignals)
            if (const auto decoded = decodeCallSignal(payload); decoded && decoded->type == type)
                ++count;
        return count;
    }

    Mesh *mesh = nullptr;
    DeviceId localDevice = DeviceId::generate();
    bool connected = true;
    bool dropMedia = false;
    QList<std::pair<DeviceId, QByteArray>> sentSignals;
    QList<DeviceId> mediaRecipients;
};

void MeshTransport::sendSignal(const ConversationId &conversation, const DeviceId &recipientDevice,
                               const QByteArray &payload)
{
    sentSignals.append({recipientDevice, payload});
    if (mesh == nullptr)
        return;
    const auto it = mesh->byDevice.find(recipientDevice.bytes());
    if (it != mesh->byDevice.end() && it->second->onSignal)
        it->second->onSignal(conversation, localDevice, payload);
}

void MeshTransport::sendMedia(const ConversationId &conversation, const DeviceId &recipientDevice,
                              const QByteArray &packet)
{
    mediaRecipients.append(recipientDevice);
    if (mesh == nullptr || dropMedia)
        return;
    const auto it = mesh->byDevice.find(recipientDevice.bytes());
    if (it != mesh->byDevice.end() && it->second->onMedia)
        it->second->onMedia(conversation, localDevice, packet);
}

// A desktop of flat regions and hard edges, which is what a share is made of.
[[nodiscard]] QImage desktopImage(QSize size, int seed = 1)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(QColor(24, 30, 38));
    for (int band = 0; band < 5; ++band) {
        const int y = 18 + band * 36 + seed;
        if (y + 10 >= size.height())
            break;
        for (int line = y; line < y + 10; ++line) {
            auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(line));
            for (int x = 12; x < size.width() - 12 && x < 12 + 70 + band * 25 + seed * 5; ++x)
                pixels[x] = qRgb(214, 226, 240);
        }
    }
    return image;
}

struct Endpoint final {
    QString name;
    OpenChat::CallTest::ScriptedAudioDevices devices;
    MeshTransport transport;
    std::unique_ptr<CallEngine> engine;
    qint64 nowMs = 0;

    void build(Mesh &mesh, CallEngine::Config config)
    {
        transport.mesh = &mesh;
        mesh.byDevice[transport.localDevice.bytes()] = &transport;
        config.mediaClock = [this] { return nowMs; };
        config.localDevice = transport.localDevice;
        engine = std::make_unique<CallEngine>(config, transport, devices.factory());
        engine->setSoundsEnabled(false);
    }

    void speak(const AudioFrame &frame) { devices.speak(frame); }
    [[nodiscard]] AudioFrame listen()
    {
        nowMs += CallAudioFormat::frameDurationMs;
        return devices.listen();
    }

    [[nodiscard]] CallEngine::CallPeer asPeer(const ConversationId &conversation) const
    {
        CallEngine::CallPeer peer;
        peer.conversation = conversation;
        peer.device = transport.localDevice;
        peer.contactId = name.toLower();
        peer.displayName = name;
        peer.avatarKey = QStringLiteral("userpfp_none");
        return peer;
    }

    [[nodiscard]] std::optional<CallParticipantState> stateOf(const DeviceId &device) const
    {
        for (const CallEngine::Participant &participant : engine->participants())
            if (participant.peer.device == device)
                return participant.state;
        return std::nullopt;
    }
};

// A frame of a pure tone, distinct per talker so a mixed frame can be taken
// apart again by comparing against what each one said.
[[nodiscard]] AudioFrame toneFrame(double hz)
{
    return AudioConvert::toFrames(OpenChat::AudioTest::tone(20, hz, 0.25)).first();
}

[[nodiscard]] AudioFrame sum(const AudioFrame &a, const AudioFrame &b)
{
    const QVector<qint16> left = AudioConvert::samplesOf(a);
    const QVector<qint16> right = AudioConvert::samplesOf(b);
    QVector<qint16> out(left.size());
    for (int i = 0; i < left.size(); ++i)
        out[i] = static_cast<qint16>(std::clamp(int(left[i]) + int(right[i]), -32768, 32767));
    return AudioConvert::frameOf(out);
}

} // namespace

class GroupCallTest final : public QObject
{
    Q_OBJECT

private:
    Mesh m_mesh;
    ConversationId m_group = ConversationId::generate();
    Endpoint m_alice;
    Endpoint m_bob;
    Endpoint m_carol;
    Endpoint m_dave;

    [[nodiscard]] CallEngine::GroupCallRoute routeFrom(const Endpoint &self) const
    {
        CallEngine::GroupCallRoute route;
        route.conversation = m_group;
        route.title = QStringLiteral("Weekend plans");
        for (const Endpoint *member : {&m_alice, &m_bob, &m_carol, &m_dave})
            if (member != &self && member->engine)
                route.members.append(member->asPeer(m_group));
        return route;
    }

    // Every endpoint answers "which group is this?" from the same roster,
    // the way the app's chat roster does.
    void connectEndpoints(CallEngine::Config config = {}, bool withDave = false)
    {
        m_mesh = Mesh{};
        m_group = ConversationId::generate();
        config.preferredCodec = AudioCodecKind::Pcm;
        m_alice.name = QStringLiteral("Alice");
        m_bob.name = QStringLiteral("Bob");
        m_carol.name = QStringLiteral("Carol");
        m_dave.name = QStringLiteral("Dave");
        m_alice.build(m_mesh, config);
        m_bob.build(m_mesh, config);
        m_carol.build(m_mesh, config);
        if (withDave)
            m_dave.build(m_mesh, config);
        for (Endpoint *endpoint : {&m_alice, &m_bob, &m_carol, &m_dave}) {
            if (!endpoint->engine)
                continue;
            endpoint->engine->groupRouteResolver =
                [this, endpoint](const ConversationId &conversation)
                -> std::optional<CallEngine::GroupCallRoute> {
                if (conversation != m_group)
                    return std::nullopt;
                return routeFrom(*endpoint);
            };
        }
    }

    // One frame from each joined member, so every pair path carries media in
    // both directions and every end reaches Active.
    void exchangeMedia()
    {
        const AudioFrame frame = toneFrame(440.0);
        for (Endpoint *endpoint : {&m_alice, &m_bob, &m_carol, &m_dave})
            if (endpoint->engine && callOccupiesDevice(endpoint->engine->state()))
                endpoint->speak(frame);
    }

private slots:
    void init()
    {
        m_alice = Endpoint{};
        m_bob = Endpoint{};
        m_carol = Endpoint{};
        m_dave = Endpoint{};
    }

    void aGroupCallRingsEveryoneAndTheyJoinOneByOne()
    {
        connectEndpoints();
        QSignalSpy bobRinging(m_bob.engine.get(), &CallEngine::incomingCall);
        QSignalSpy carolRinging(m_carol.engine.get(), &CallEngine::incomingCall);
        QSignalSpy aliceParticipants(m_alice.engine.get(), &CallEngine::participantsChanged);

        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        // Both devices alert at once, each sees the caller as in the call and
        // the other callee as still ringing, and the caller sees both ringing.
        QCOMPARE(bobRinging.count(), 1);
        QCOMPARE(carolRinging.count(), 1);
        QVERIFY(m_alice.engine->isGroupCall());
        QVERIFY(m_bob.engine->isGroupCall());
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);
        QCOMPARE(m_bob.engine->state(), CallState::Ringing);
        QCOMPARE(m_bob.engine->direction(), CallDirection::Incoming);
        QCOMPARE(m_bob.engine->peer().device, m_alice.transport.localDevice);
        QCOMPARE(m_bob.engine->groupTitle(), QStringLiteral("Weekend plans"));
        QCOMPARE(m_alice.engine->participants().size(), 2);
        QCOMPARE(m_alice.stateOf(m_bob.transport.localDevice), CallParticipantState::Ringing);
        QCOMPARE(m_alice.stateOf(m_carol.transport.localDevice), CallParticipantState::Ringing);
        QCOMPARE(m_bob.stateOf(m_alice.transport.localDevice), CallParticipantState::Joined);
        QCOMPARE(m_bob.stateOf(m_carol.transport.localDevice), CallParticipantState::Ringing);
        QVERIFY(aliceParticipants.count() >= 1);
        // Ringing opens nobody's microphone.
        QVERIFY(!m_bob.devices.capture->started);
        QVERIFY(!m_alice.devices.capture->started);

        // Bob picks up: the line opens between Alice and Bob; Carol, still
        // ringing, learns Bob is in.
        m_bob.engine->acceptCall();
        QCOMPARE(m_alice.engine->state(), CallState::Connecting);
        QCOMPARE(m_bob.engine->state(), CallState::Connecting);
        QCOMPARE(m_alice.stateOf(m_bob.transport.localDevice), CallParticipantState::Joined);
        QCOMPARE(m_alice.stateOf(m_carol.transport.localDevice), CallParticipantState::Ringing);
        QCOMPARE(m_carol.stateOf(m_bob.transport.localDevice), CallParticipantState::Joined);
        QCOMPARE(m_carol.engine->state(), CallState::Ringing);
        QVERIFY(m_alice.devices.capture->started);
        QVERIFY(m_bob.devices.capture->started);
        QVERIFY(m_alice.engine->sessionFor(m_bob.transport.localDevice) != nullptr);
        QVERIFY(m_alice.engine->sessionFor(m_carol.transport.localDevice) == nullptr);
        QCOMPARE(m_alice.engine->joinedParticipantCount(), 1);

        // Carol picks up too: she keys paths to both, and both key one to her.
        m_carol.engine->acceptCall();
        QCOMPARE(m_carol.engine->state(), CallState::Connecting);
        QVERIFY(m_carol.engine->sessionFor(m_alice.transport.localDevice) != nullptr);
        QVERIFY(m_carol.engine->sessionFor(m_bob.transport.localDevice) != nullptr);
        QVERIFY(m_alice.engine->sessionFor(m_carol.transport.localDevice) != nullptr);
        QVERIFY(m_bob.engine->sessionFor(m_carol.transport.localDevice) != nullptr);
        QCOMPARE(m_alice.engine->joinedParticipantCount(), 2);
        QCOMPARE(m_bob.engine->joinedParticipantCount(), 2);

        // Media, not answers, is what makes each end Active.
        exchangeMedia();
        QCOMPARE(m_alice.engine->state(), CallState::Active);
        QCOMPARE(m_bob.engine->state(), CallState::Active);
        QCOMPARE(m_carol.engine->state(), CallState::Active);
    }

    void everyoneHearsTheOthersMixed()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        m_bob.engine->acceptCall();
        m_carol.engine->acceptCall();

        // Three distinct tones. With the lossless codec, what each speaker
        // renders is exactly the sum of the other two, and nothing of itself.
        const AudioFrame aliceTone = toneFrame(300.0);
        const AudioFrame bobTone = toneFrame(500.0);
        const AudioFrame carolTone = toneFrame(700.0);
        AudioFrame aliceHeard;
        AudioFrame bobHeard;
        AudioFrame carolHeard;
        for (int i = 0; i < 8; ++i) {
            m_alice.speak(aliceTone);
            m_bob.speak(bobTone);
            m_carol.speak(carolTone);
            aliceHeard = m_alice.listen();
            bobHeard = m_bob.listen();
            carolHeard = m_carol.listen();
        }
        QCOMPARE(aliceHeard, sum(bobTone, carolTone));
        QCOMPARE(bobHeard, sum(aliceTone, carolTone));
        QCOMPARE(carolHeard, sum(aliceTone, bobTone));
        // Each member sends every frame separately to each other member: the
        // mesh forwards nothing through a third device.
        QCOMPARE(m_alice.transport.mediaRecipients.size(), 16);
        QVERIFY(m_bob.engine->isRemoteSpeaking());
        // Each pair keyed its own path: Bob's session with Alice refuses a
        // packet Alice sealed for Carol, so nothing Carol hears could be
        // replayed to Bob.
        QVERIFY(m_alice.engine->sessionFor(m_bob.transport.localDevice)->stats().packetsRejected
                == 0);
    }

    void aMemberWhoLeavesIsRemovedWhileTheRestContinue()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        m_bob.engine->acceptCall();
        m_carol.engine->acceptCall();
        exchangeMedia();
        QCOMPARE(m_alice.engine->state(), CallState::Active);

        // Bob hangs up: his device is done, the other two carry on without
        // him and no longer key a path to him.
        m_bob.engine->hangUp();
        QCOMPARE(m_bob.engine->state(), CallState::Ended);
        QCOMPARE(m_bob.engine->endReason(), CallEndReason::LocalHangup);
        QVERIFY(!m_bob.devices.capture->started);
        QCOMPARE(m_alice.engine->state(), CallState::Active);
        QCOMPARE(m_carol.engine->state(), CallState::Active);
        QCOMPARE(m_alice.stateOf(m_bob.transport.localDevice), CallParticipantState::Left);
        QCOMPARE(m_carol.stateOf(m_bob.transport.localDevice), CallParticipantState::Left);
        QVERIFY(m_alice.engine->sessionFor(m_bob.transport.localDevice) == nullptr);
        QVERIFY(m_alice.engine->sessionFor(m_carol.transport.localDevice) != nullptr);
        QCOMPARE(m_alice.engine->joinedParticipantCount(), 1);
        // Bob's participant list survives into Ended for the summary, then
        // clears on dismiss.
        QCOMPARE(m_bob.engine->participants().size(), 2);
        m_bob.engine->dismissEndedCall();
        QVERIFY(!m_bob.engine->isGroupCall());
        QVERIFY(m_bob.engine->participants().isEmpty());

        // Media keeps flowing between the two who stayed, and nothing goes to
        // Bob any more.
        const int toBobBefore = static_cast<int>(
            std::count(m_alice.transport.mediaRecipients.cbegin(),
                       m_alice.transport.mediaRecipients.cend(), m_bob.transport.localDevice));
        exchangeMedia();
        QCOMPARE(static_cast<int>(std::count(m_alice.transport.mediaRecipients.cbegin(),
                                             m_alice.transport.mediaRecipients.cend(),
                                             m_bob.transport.localDevice)),
                 toBobBefore);
        QCOMPARE(m_alice.engine->state(), CallState::Active);

        // When the last other member leaves, the call is over for the one left.
        m_carol.engine->hangUp();
        QCOMPARE(m_alice.engine->state(), CallState::Ended);
        QCOMPARE(m_alice.engine->endReason(), CallEndReason::RemoteHangup);
        QVERIFY(!m_alice.devices.capture->started);
    }

    void aDeclineIsMarkedAndTheCallGoesOnForTheOthers()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        m_carol.engine->declineCall();
        // Carol is marked on every device; Bob is still ringing so the call
        // is still alive.
        QCOMPARE(m_carol.engine->state(), CallState::Ended);
        QCOMPARE(m_carol.engine->endReason(), CallEndReason::Declined);
        QCOMPARE(m_alice.stateOf(m_carol.transport.localDevice), CallParticipantState::Declined);
        QCOMPARE(m_bob.stateOf(m_carol.transport.localDevice), CallParticipantState::Declined);
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);
        m_bob.engine->acceptCall();
        exchangeMedia();
        QCOMPARE(m_alice.engine->state(), CallState::Active);
        QCOMPARE(m_bob.engine->state(), CallState::Active);
        QVERIFY(m_alice.engine->sessionFor(m_carol.transport.localDevice) == nullptr);
    }

    void theCallEndsWhenEveryoneDeclines()
    {
        connectEndpoints();
        QSignalSpy ended(m_alice.engine.get(), &CallEngine::callEnded);
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        m_bob.engine->declineCall();
        QCOMPARE(m_alice.engine->state(), CallState::Ringing);
        m_carol.engine->declineCall();
        QCOMPARE(m_alice.engine->state(), CallState::Ended);
        QCOMPARE(m_alice.engine->endReason(), CallEndReason::Declined);
        QCOMPARE(ended.count(), 1);
    }

    void calleesStopRingingWhenTheCallerGivesUp()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        QCOMPARE(m_bob.engine->state(), CallState::Ringing);
        m_alice.engine->hangUp();
        // Nobody was in the call with them, so the ring simply stops.
        QCOMPARE(m_bob.engine->state(), CallState::Ended);
        QCOMPARE(m_bob.engine->endReason(), CallEndReason::RemoteHangup);
        QCOMPARE(m_carol.engine->state(), CallState::Ended);
        QVERIFY(!m_bob.devices.capture->started);

        // But if someone else had already joined, a still-ringing callee keeps
        // ringing and can still join them after the caller leaves.
        m_alice.engine->dismissEndedCall();
        m_bob.engine->dismissEndedCall();
        m_carol.engine->dismissEndedCall();
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        m_bob.engine->acceptCall();
        m_alice.engine->hangUp();
        QCOMPARE(m_carol.engine->state(), CallState::Ringing);
        QCOMPARE(m_carol.stateOf(m_alice.transport.localDevice), CallParticipantState::Left);
        QCOMPARE(m_bob.engine->state(), CallState::Connecting);
        m_carol.engine->acceptCall();
        exchangeMedia();
        QCOMPARE(m_bob.engine->state(), CallState::Active);
        QCOMPARE(m_carol.engine->state(), CallState::Active);
        QVERIFY(m_bob.engine->sessionFor(m_carol.transport.localDevice) != nullptr);
    }

    void theRingTimeoutGivesUpOnTheSilentAndKeepsTheRest()
    {
        CallEngine::Config config;
        config.ringTimeoutMs = 150;
        connectEndpoints(config);
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        m_bob.engine->acceptCall();
        exchangeMedia();
        QCOMPARE(m_alice.engine->state(), CallState::Active);

        // Carol never picks up: her own ring times out as a missed call and
        // she is marked No answer for the others, whose call continues.
        QTRY_COMPARE_WITH_TIMEOUT(m_carol.engine->state(), CallState::Ended, 2000);
        QCOMPARE(m_carol.engine->endReason(), CallEndReason::Unanswered);
        QTRY_COMPARE_WITH_TIMEOUT(m_alice.stateOf(m_carol.transport.localDevice),
                                  std::optional<CallParticipantState>(CallParticipantState::NoAnswer),
                                  2000);
        QCOMPARE(m_alice.engine->state(), CallState::Active);
        QCOMPARE(m_bob.engine->state(), CallState::Active);

        // A call nobody answers at all ends as No answer for the caller.
        m_alice.engine->hangUp();
        m_alice.engine->dismissEndedCall();
        m_bob.engine->dismissEndedCall();
        m_carol.engine->dismissEndedCall();
        m_bob.transport.mesh = nullptr; // Bob's answers would reach nobody anyway
        QSignalSpy ended(m_alice.engine.get(), &CallEngine::callEnded);
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        QVERIFY(ended.wait(2000));
        QCOMPARE(m_alice.engine->endReason(), CallEndReason::NoAnswer);
    }

    void aBusyMemberIsMarkedBusyAndRingsNobody()
    {
        connectEndpoints({}, /*withDave=*/true);
        // Carol is already on a one-to-one call with Dave.
        QVERIFY(m_carol.engine->placeCall(m_dave.asPeer(ConversationId::generate())));
        m_dave.engine->acceptCall();
        QCOMPARE(m_carol.engine->state(), CallState::Connecting);

        QSignalSpy carolAlerted(m_carol.engine.get(), &CallEngine::incomingCall);
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        QCOMPARE(carolAlerted.count(), 0);
        QCOMPARE(m_alice.stateOf(m_carol.transport.localDevice), CallParticipantState::Busy);
        QCOMPARE(m_carol.engine->state(), CallState::Connecting); // untouched
        QCOMPARE(m_bob.engine->state(), CallState::Ringing);
        // Dave's engine holds a one-to-one call and ignores the group offer's
        // consequences entirely.
        QVERIFY(!m_dave.engine->isGroupCall());
    }

    void cameraFramesReachEachMemberSeparately()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        m_bob.engine->acceptCall();
        m_carol.engine->acceptCall();
        exchangeMedia();

        // Captured by hand: a DeviceId has no default value, so it cannot ride
        // through a QSignalSpy's QVariant.
        QList<std::pair<QByteArray, QImage>> bobVideo;
        QList<std::pair<QByteArray, QImage>> carolVideo;
        connect(m_bob.engine.get(), &CallEngine::participantVideoFrame, this,
                [&](const DeviceId &device, const QImage &image) {
                    bobVideo.append({device.bytes(), image});
                });
        connect(m_carol.engine.get(), &CallEngine::participantVideoFrame, this,
                [&](const DeviceId &device, const QImage &image) {
                    carolVideo.append({device.bytes(), image});
                });
        QImage landscape(320, 180, QImage::Format_RGB32);
        landscape.fill(Qt::blue);
        m_alice.engine->sendVideoFrame(landscape);
        QCOMPARE(bobVideo.size(), 1);
        QCOMPARE(carolVideo.size(), 1);
        QCOMPARE(bobVideo.first().first, m_alice.transport.localDevice.bytes());
        QCOMPARE(bobVideo.first().second.size(), landscape.size());
        // Turning the camera off reaches everyone as a null frame.
        m_alice.engine->sendVideoFrame(QImage());
        QVERIFY(bobVideo.last().second.isNull());
        QVERIFY(carolVideo.last().second.isNull());
        // Video never disturbs the audio paths.
        QCOMPARE(m_bob.engine->state(), CallState::Active);
    }

    void oneSharedScreenIsEncodedOnceAndSealedForEveryMember()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        m_bob.engine->acceptCall();
        m_carol.engine->acceptCall();
        exchangeMedia();

        // Captured by hand: a DeviceId cannot ride through a QSignalSpy.
        QList<std::pair<QByteArray, ScreenCanvasPtr>> bobScreen;
        QList<std::pair<QByteArray, ScreenCanvasPtr>> carolScreen;
        connect(m_bob.engine.get(), &CallEngine::participantScreenFrame, this,
                [&](const DeviceId &device, const ScreenCanvasPtr &canvas) {
                    bobScreen.append({device.bytes(), canvas});
                });
        connect(m_carol.engine.get(), &CallEngine::participantScreenFrame, this,
                [&](const DeviceId &device, const ScreenCanvasPtr &canvas) {
                    carolScreen.append({device.bytes(), canvas});
                });

        const QImage desktop = desktopImage(QSize(1024, 640));
        QVERIFY(m_alice.engine->startScreenShare());
        for (int i = 0; i < 400; ++i) {
            m_alice.nowMs += 200;
            m_alice.engine->sendScreenFrame(ScreenFrameView::fromImage(desktop));
            if (!bobScreen.isEmpty() && bobScreen.last().second
                && bobScreen.last().second->isComplete())
                break;
        }
        QVERIFY(!bobScreen.isEmpty());
        QVERIFY(!carolScreen.isEmpty());
        QCOMPARE(bobScreen.first().first, m_alice.transport.localDevice.bytes());
        QCOMPARE(carolScreen.first().first, m_alice.transport.localDevice.bytes());

        // Both members reconstructed the same desktop from their own sealed
        // copies, and each holds its own canvas rather than sharing one.
        const ScreenCanvasPtr bobCanvas = bobScreen.last().second;
        const ScreenCanvasPtr carolCanvas = carolScreen.last().second;
        QVERIFY(bobCanvas && carolCanvas);
        QVERIFY(bobCanvas != carolCanvas);
        QCOMPARE(bobCanvas->size(), desktop.size());
        QCOMPARE(carolCanvas->size(), desktop.size());
        QCOMPARE(bobCanvas->image().pixel(500, 400), desktop.pixel(500, 400));
        QCOMPARE(carolCanvas->image().pixel(20, 22), desktop.pixel(20, 22));

        // The desktop was hashed and encoded ONCE for the whole mesh, not once
        // per member: that is the entire point of the shared encoder. Every
        // frame produced went to both members.
        const ScreenShareStats stats = m_alice.engine->screenShareStats();
        QVERIFY(stats.framesSent > 0);
        int bobPackets = 0;
        int carolPackets = 0;
        for (const DeviceId &recipient : m_alice.transport.mediaRecipients) {
            if (recipient.bytes() == m_bob.transport.localDevice.bytes())
                ++bobPackets;
            else if (recipient.bytes() == m_carol.transport.localDevice.bytes())
                ++carolPackets;
        }
        QCOMPARE(bobPackets, carolPackets);
        QVERIFY(bobPackets >= int(stats.framesSent));

        // Stopping clears it for everyone at once.
        m_alice.engine->stopScreenShare();
        QVERIFY(!bobScreen.last().second);
        QVERIFY(!carolScreen.last().second);
        QCOMPARE(m_bob.engine->state(), CallState::Active);
        QCOMPARE(m_carol.engine->state(), CallState::Active);
    }

    void aMemberLeavingMidShareTakesTheirViewOfItWithThem()
    {
        connectEndpoints();
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        m_bob.engine->acceptCall();
        m_carol.engine->acceptCall();
        exchangeMedia();

        QList<ScreenCanvasPtr> carolScreen;
        connect(m_carol.engine.get(), &CallEngine::participantScreenFrame, this,
                [&](const DeviceId &, const ScreenCanvasPtr &canvas) {
                    carolScreen.append(canvas);
                });
        // Alice's view of Bob's share, so that Bob leaving can be seen to clear
        // it on every other member's side too.
        QList<ScreenCanvasPtr> aliceScreen;
        connect(m_alice.engine.get(), &CallEngine::participantScreenFrame, this,
                [&](const DeviceId &, const ScreenCanvasPtr &canvas) {
                    aliceScreen.append(canvas);
                });

        const QImage desktop = desktopImage(QSize(800, 600));
        QVERIFY(m_bob.engine->startScreenShare());
        for (int i = 0; i < 400; ++i) {
            m_bob.nowMs += 200;
            m_bob.engine->sendScreenFrame(ScreenFrameView::fromImage(desktop));
            if (!carolScreen.isEmpty() && carolScreen.last() && carolScreen.last()->isComplete())
                break;
        }
        QVERIFY(!carolScreen.isEmpty() && carolScreen.last());
        QVERIFY(!aliceScreen.isEmpty() && aliceScreen.last());

        // Bob leaves mid-share. Everyone else's view of it closes, and Bob's
        // own sending half is released with the call.
        m_bob.engine->hangUp();
        QVERIFY2(!carolScreen.last(), "a departed member's share was left on screen");
        QVERIFY2(!aliceScreen.last(), "a departed member's share was left on screen");
        QVERIFY(!m_bob.engine->isScreenSharing());
        QCOMPARE(m_bob.engine->screenShareStats().outputSize, QSize());
        // The call carries on for the two who are left.
        QCOMPARE(m_alice.engine->state(), CallState::Active);
        QCOMPARE(m_carol.engine->state(), CallState::Active);
    }

    void groupCallsNeedALocalDeviceAndSomeoneToCall()
    {
        connectEndpoints();
        CallEngine::GroupCallRoute empty;
        empty.conversation = m_group;
        QVERIFY(!m_alice.engine->placeGroupCall(empty));
        QCOMPARE(m_alice.engine->state(), CallState::Idle);

        MeshTransport lonely;
        lonely.localDevice = m_bob.transport.localDevice; // stands in for Bob below
        OpenChat::CallTest::ScriptedAudioDevices devices;
        CallEngine::Config config;
        config.preferredCodec = AudioCodecKind::Pcm;
        CallEngine noDevice(config, lonely, devices.factory());
        QVERIFY(!noDevice.placeGroupCall(routeFrom(m_alice)));
        QCOMPARE(noDevice.state(), CallState::Idle);

        // A group offer to an engine with no device id is refused as busy
        // rather than rung, so the caller sees a clear outcome.
        lonely.mesh = &m_mesh;
        noDevice.groupRouteResolver = [this](const ConversationId &) { return routeFrom(m_alice); };
        m_mesh.byDevice[m_bob.transport.localDevice.bytes()] = &lonely;
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        QCOMPARE(m_alice.stateOf(m_bob.transport.localDevice), CallParticipantState::Busy);
        QCOMPARE(noDevice.state(), CallState::Idle);
    }

    void aRedeliveredGroupOfferDoesNotRingTwice()
    {
        connectEndpoints();
        QSignalSpy alerted(m_bob.engine.get(), &CallEngine::incomingCall);
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        QCOMPARE(alerted.count(), 1);
        // The durable path retried the offer.
        const QByteArray offer = m_alice.transport.sentSignals.first().second;
        const int ringingBefore = m_bob.transport.signalsOfType(CallSignalType::Ringing);
        m_bob.transport.onSignal(m_group, m_alice.transport.localDevice, offer);
        QCOMPARE(alerted.count(), 1);
        QCOMPARE(m_bob.transport.signalsOfType(CallSignalType::Ringing), ringingBefore + 1);
        QCOMPARE(m_bob.engine->state(), CallState::Ringing);
    }

    void twoMembersCallingAtOnceSettleOnOneCall()
    {
        connectEndpoints();
        // Neither hears the other's offer until both have dialled.
        m_alice.transport.mesh = nullptr;
        m_bob.transport.mesh = nullptr;
        QVERIFY(m_alice.engine->placeGroupCall(routeFrom(m_alice)));
        QVERIFY(m_bob.engine->placeGroupCall(routeFrom(m_bob)));
        m_alice.transport.mesh = &m_mesh;
        m_bob.transport.mesh = &m_mesh;
        const QByteArray aliceOffer = m_alice.transport.sentSignals.first().second;
        const QByteArray bobOffer = m_bob.transport.sentSignals.first().second;
        m_bob.transport.onSignal(m_group, m_alice.transport.localDevice, aliceOffer);
        m_alice.transport.onSignal(m_group, m_bob.transport.localDevice, bobOffer);
        // Exactly one of them abandoned their own call and is now ringing on
        // the other's; the lower call id won on both sides.
        const bool aliceWon = m_alice.engine->direction() == CallDirection::Outgoing;
        if (aliceWon) {
            QCOMPARE(m_bob.engine->direction(), CallDirection::Incoming);
            QCOMPARE(m_bob.engine->state(), CallState::Ringing);
        } else {
            QCOMPARE(m_alice.engine->direction(), CallDirection::Incoming);
            QCOMPARE(m_alice.engine->state(), CallState::Ringing);
            QCOMPARE(m_bob.engine->direction(), CallDirection::Outgoing);
        }
        QVERIFY(callOccupiesDevice(m_alice.engine->state()));
        QVERIFY(callOccupiesDevice(m_bob.engine->state()));
    }
};

QTEST_MAIN(GroupCallTest)
#include "tst_groupcall.moc"
