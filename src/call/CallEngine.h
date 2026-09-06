#pragma once

#include "call/AudioIo.h"
#include "call/CallSession.h"
#include "call/CallScreenSession.h"
#include "call/CallVideoSession.h"
#include "call/CallSounds.h"
#include "call/CallSignal.h"
#include "call/CallTransport.h"
#include "call/CallTypes.h"
#include "domain/Identifiers.h"

#include <QObject>
#include <QString>
#include <QVector>
#include <vector>

#include <functional>
#include <memory>
#include <optional>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace OpenChat {

// Milliseconds from an arbitrary fixed origin on the monotonic clock. The
// default media clock: unlike the wall clock it never jumps when NTP corrects
// the system time, which would otherwise read as a burst of jitter.
[[nodiscard]] qint64 steadyClockMs() noexcept;

// What one member of a group call is doing, as every other member sees it.
enum class CallParticipantState {
    Ringing,  // offered, no answer yet
    Joined,   // in the call
    Declined, // refused the offer
    Left,     // hung up after joining
    Busy,     // was in another call
    NoAnswer, // never picked up before the ring timed out
};

[[nodiscard]] QString callParticipantStateName(CallParticipantState state);

// True in the states where a participant may still end up in the call.
[[nodiscard]] constexpr bool participantIsPending(CallParticipantState state) noexcept
{
    return state == CallParticipantState::Ringing || state == CallParticipantState::Joined;
}

// The lifecycle of one voice call at a time: who is being called, what state the
// call is in, and when audio starts and stops flowing.
//
// One at a time is a deliberate constraint, not a simplification. A device has
// one microphone and one speaker, so a second call cannot be carried anyway; the
// engine answers a competing offer with Busy and says so, rather than silently
// dropping it.
//
// A call is either with one peer (the conversation's other device) or with a
// group. A group call rings every member at once and carries media as a mesh:
// each pair of members keys its own path from the shared call secret, so audio
// never passes through a third device, and a member who leaves is simply
// dropped from everyone else's mesh while the rest carry on.
//
// Everything the engine touches is injected: the transport, the audio devices,
// and the timeouts. It owns no sockets and opens no devices itself, so the whole
// state machine — including its races — is exercisable without either.
class CallEngine final : public QObject
{
    Q_OBJECT

public:
    struct Config final {
        // How long an outgoing call rings before giving up.
        int ringTimeoutMs = 45'000;
        // How long an accepted call waits for the peer's first media packet
        // before declaring the media path dead.
        int connectTimeoutMs = 15'000;
        // How long an active call tolerates hearing nothing at all. Media is
        // sent continuously (silence included) so a gap this long means the path
        // is gone, not that the peer stopped talking.
        int mediaStallTimeoutMs = 20'000;
        // The codec offered to the peer. Opus unless a caller asks for PCM by
        // name, which only the tests and the diagnostics tool do.
        AudioCodecKind preferredCodec = preferredAudioCodec();
        // Milliseconds on a steady clock, read as each media packet arrives; the
        // jitter buffer sizes its cushion from how these scatter around the
        // sender's 20 ms grid. Injected so a test can drive a call in a tight
        // loop and still present arrivals as evenly spaced — on the real clock
        // that loop would look like a 4 s burst and the buffer would deepen to
        // absorb it.
        std::function<qint64()> mediaClock = steadyClockMs;
        // This device's id. A group call needs it to key each pair's media path
        // (both ends of a pair derive the same keys from the ordered pair of
        // device ids); without it group calls are refused and ordinary calls
        // are unaffected.
        std::optional<DeviceId> localDevice;
    };

    // A peer, as a call needs to address it: the shared conversation, the device
    // envelopes go to, and the name to show while it rings.
    struct CallPeer final {
        ConversationId conversation = ConversationId::generate();
        DeviceId device = DeviceId::generate();
        QString contactId;   // AccountId hex, the key the UI's roster uses
        QString displayName;
        QString avatarKey;
    };

    // A group, as a call needs to address it: the group conversation and every
    // other member's device.
    struct GroupCallRoute final {
        ConversationId conversation = ConversationId::generate();
        QString title;
        QVector<CallPeer> members;
    };

    // One other member of a group call, for the UI.
    struct Participant final {
        CallPeer peer;
        CallParticipantState state = CallParticipantState::Ringing;
        bool speaking = false;
        double level = 0.0;
    };

    CallEngine(Config config, CallTransport &transport, CallAudioIoFactory audioIo,
               QObject *parent = nullptr);
    ~CallEngine() override;

    CallEngine(const CallEngine &) = delete;
    CallEngine &operator=(const CallEngine &) = delete;

    [[nodiscard]] CallState state() const noexcept { return m_state; }
    [[nodiscard]] CallEndReason endReason() const noexcept { return m_endReason; }
    [[nodiscard]] CallDirection direction() const noexcept { return m_direction; }
    // The one peer of an ordinary call; in a group call, the member who placed
    // it (incoming) or the first member rung (outgoing).
    [[nodiscard]] const CallPeer &peer() const noexcept { return m_peer; }
    [[nodiscard]] bool isMuted() const noexcept { return m_muted; }
    // Milliseconds since the call became Active, or 0 outside a live call.
    [[nodiscard]] qint64 activeDurationMs() const;

    [[nodiscard]] double localLevel() const;
    [[nodiscard]] double remoteLevel() const;
    [[nodiscard]] bool isLocalSpeaking() const;
    [[nodiscard]] bool isRemoteSpeaking() const;

    // Group call surface. participants() is every other member with what they
    // are doing right now; empty outside a group call. Held through Ended so
    // the summary can still show who was there, cleared on dismiss.
    [[nodiscard]] bool isGroupCall() const noexcept { return m_group != nullptr; }
    [[nodiscard]] QString groupTitle() const;
    [[nodiscard]] QVector<Participant> participants() const;
    // How many members are in the call with us right now.
    [[nodiscard]] int joinedParticipantCount() const;

    // Non-null only between Connecting and Ended in an ordinary call; exposed
    // for diagnostics. A group call keeps one session per member instead.
    [[nodiscard]] const CallSession *session() const noexcept { return m_session.get(); }
    [[nodiscard]] const CallSession *sessionFor(const DeviceId &device) const;

    // Places a call. Fails (returning false, leaving state untouched) when a
    // call is already in progress or the call secret cannot be generated.
    [[nodiscard]] bool placeCall(const CallPeer &peer);
    // Places a group call: every member is offered the same call and rings at
    // once. Fails like placeCall, and also when the route has no members or
    // Config::localDevice is unset.
    [[nodiscard]] bool placeGroupCall(const GroupCallRoute &route);

    // Answers or refuses the ringing incoming call. Both are no-ops when there
    // is nothing ringing.
    void acceptCall();
    void declineCall();

    // Ends whatever is in progress, at any stage. Safe to call when idle. In a
    // group call this leaves the call: the others carry on without us.
    void hangUp();

    void setMuted(bool muted);
    void sendVideoFrame(const QImage &image);

    // --- Screen sharing ----------------------------------------------------
    //
    // A screen is a second video source alongside the camera, not a replacement
    // for it: both can run at once, and the far end can tell them apart because
    // they are separate streams with separate keys.

    // Arms the share. Frames are refused until this is called and again after
    // stopScreenShare(), so a capture callback still in flight when the user
    // stops cannot quietly start the whole thing up again. Returns false when
    // there is no live call to share into.
    [[nodiscard]] bool startScreenShare();
    // Encodes and sends one captured desktop frame to every peer in the call.
    // The view is read and released inside the call; nothing about it is kept.
    // Cheap to call at any rate — the encoder paces itself and returns without
    // touching a pixel when a frame arrives too early or nothing has changed.
    void sendScreenFrame(const ScreenFrameView &frame);
    // Tells every peer the share is over and releases every buffer the sending
    // half holds. Safe to call when not sharing.
    void stopScreenShare();
    [[nodiscard]] bool isScreenSharing() const noexcept { return m_screenSharing; }
    // The rate the capture should run at: the encoder's current rung, or a
    // trickle while nobody on the far end is displaying the share.
    [[nodiscard]] int screenShareTargetFps() const;
    // How large an incoming share is being displayed here. An empty size means
    // it is not on screen, which the sender uses to stop encoding for it.
    void setScreenViewSize(QSize size);
    void setScreenViewSize(const DeviceId &device, QSize size);
    // Development instrumentation for the one-to-one path.
    [[nodiscard]] ScreenShareStats screenShareStats() const;

    // Silences the call's own tones (ring, pick-up, hang-up, mute). The call
    // itself is unaffected; only the interface sounds stop.
    void setSoundsEnabled(bool enabled);
    [[nodiscard]] bool soundsEnabled() const noexcept { return m_soundsEnabled; }

    // Clears an Ended call back to Idle. The Ended state is held rather than
    // dropped so the UI can show why a call finished; this is the acknowledgement.
    void dismissEndedCall();

    // Called by the app when the peer's identity is resolved late (a handle that
    // arrives after the call started), so the ringing UI can be renamed.
    void updatePeerIdentity(const QString &displayName, const QString &avatarKey);
    void updateParticipantIdentity(const DeviceId &device, const QString &displayName,
                                   const QString &avatarKey);

    // Answers "which group is this conversation, and who is in it?" for an
    // incoming group offer, so the callee can ring with the members in view.
    // An offer on a conversation this returns nothing for is an ordinary call.
    std::function<std::optional<GroupCallRoute>(const ConversationId &)> groupRouteResolver;

signals:
    void stateChanged();
    void mutedChanged();
    void levelsChanged();
    void remoteVideoFrame(const QImage &image);
    // A group member's camera frame (null when their camera is off or gone).
    void participantVideoFrame(const OpenChat::DeviceId &device, const QImage &image);
    // The peer's shared screen. The canvas is a live surface written in place,
    // not a frame: holders keep the pointer and repaint its dirty rectangle. A
    // null pointer means the share ended.
    void remoteScreenFrame(const OpenChat::ScreenCanvasPtr &canvas);
    void participantScreenFrame(const OpenChat::DeviceId &device,
                                const OpenChat::ScreenCanvasPtr &canvas);
    // Who is in a group call, or what they are doing, changed.
    void participantsChanged();
    // A call arrived and is ringing. The UI raises the incoming-call surface.
    void incomingCall();
    // A call reached Ended. `reason` is also readable from endReason().
    void callEnded(OpenChat::CallEndReason reason);

private:
    // A group call's view of one other member, and the media path to them.
    struct Member final {
        Participant info;
        // The codec this member announced; each pair runs the narrower of the
        // two ends' choices, which both compute the same way.
        AudioCodecKind codec = AudioCodecKind::Pcm;
        std::unique_ptr<CallSession> session;
        std::unique_ptr<CallVideoSession> video;
        std::unique_ptr<CallScreenSession> screen;
        qint64 lastVideoMs = 0;
        qint64 lastScreenMs = 0;
        bool cameraOn = false;
        bool screenOn = false;
    };
    struct GroupCall final {
        ConversationId conversation = ConversationId::generate();
        QString title;
        std::vector<Member> members;
    };

    void onSignal(const ConversationId &conversation, const DeviceId &sender,
                  const QByteArray &payload);
    void onMedia(const ConversationId &conversation, const DeviceId &sender,
                 const QByteArray &packet);

    void handleOffer(const ConversationId &conversation, const DeviceId &sender,
                     const CallSignalMessage &message);
    void handleRinging(const CallSignalMessage &message);
    void handleAnswer(const CallSignalMessage &message);
    void handleHangup(const CallSignalMessage &message);

    // Group counterparts. Every signal in a group call is dispatched here once
    // the sender is known to be a member.
    void handleGroupOffer(const ConversationId &conversation, const DeviceId &sender,
                          const CallSignalMessage &message, const GroupCallRoute &route);
    void handleGroupAnswer(Member &member, const CallSignalMessage &message);
    void handleGroupHangup(Member &member, const CallSignalMessage &message);
    void onGroupMedia(Member &member, const QByteArray &packet);
    // Ends the group call when there is nobody left to be in it with.
    void settleGroup();
    [[nodiscard]] Member *memberFor(const DeviceId &device);
    [[nodiscard]] bool openMemberMedia(Member &member);
    void closeMemberMedia(Member &member);
    void broadcast(const CallSignalMessage &message);
    void setParticipantState(Member &member, CallParticipantState state);
    void onRingTimeout();

    void send(const CallSignalMessage &message);
    void send(const CallSignalMessage &message, const CallPeer &peer);

    // Opens the speaker on its own, without a media session behind it. The ring
    // tones play long before there is any call audio, so playback comes up as
    // soon as a call starts rather than when it is answered. Idempotent, and
    // non-fatal: a machine that cannot open its speaker still places calls, it
    // just makes them silently.
    bool openPlayback();

    // Builds the media session and opens the microphone. Playback must already
    // be up. Returns false (and ends the call as SetupFailed) if either fails.
    [[nodiscard]] bool startMedia();
    // The group form: opens the microphone and one session per joined member.
    [[nodiscard]] bool startGroupMedia();
    [[nodiscard]] bool startCapture();
    // Drops the microphone and the media session, and with them the call keys.
    // Leaves the speaker running so a departing sound can still be heard.
    void stopCapture();
    void closePlayback();
    void stopMedia();

    void setState(CallState state);
    void endCall(CallEndReason reason, bool notifyPeer);

    // What one peer's screen packet turned into. Shared by the one-to-one and
    // the group path, which differ only in which signal carries the result.
    struct ScreenPacketOutcome final {
        bool changed = false; // there is a new picture to show
        bool ended = false;   // the peer stopped sharing
        ScreenCanvasPtr canvas;
    };
    [[nodiscard]] ScreenPacketOutcome handleScreenPacket(CallScreenSession &session,
                                                         const QByteArray &packet,
                                                         bool &activeFlag, qint64 &lastSeenMs);
    // Sends each receiving session's periodic report back to its sender, and
    // folds every peer's reported capability into the one shared encoder.
    void pumpScreenFeedback();
    void applyScreenEncoderPolicy();
    // Drops the sending half of every screen session and the encoder with it.
    void releaseScreenSender();

    void onCapturedFrame(const AudioFrame &frame);
    [[nodiscard]] AudioFrame pullPlaybackFrame();
    void refreshParticipantLevels();

    Config m_config;
    CallTransport &m_transport;
    CallAudioIoFactory m_audioIo;

    std::unique_ptr<CallVideoSession> m_videoSession;
    QTimer *m_videoTimeout = nullptr;
    // One encoder for the whole call: a mesh has one desktop and several peers,
    // so the pixels are hashed and the tiles encoded once, and only the AES
    // seal is repeated for each of them.
    std::shared_ptr<ScreenTileEncoder> m_screenEncoder;
    std::unique_ptr<CallScreenSession> m_screenSession;
    QTimer *m_screenTimeout = nullptr;
    QTimer *m_screenFeedbackTimer = nullptr;
    bool m_screenSharing = false;
    bool m_remoteScreenActive = false;
    CallState m_state = CallState::Idle;
    CallEndReason m_endReason = CallEndReason::None;
    CallDirection m_direction = CallDirection::Outgoing;
    CallPeer m_peer;
    std::unique_ptr<GroupCall> m_group;
    std::optional<CallId> m_callId;
    QByteArray m_secret;
    AudioCodecKind m_codec = AudioCodecKind::Pcm;
    bool m_muted = false;
    bool m_soundsEnabled = true;
    qint64 m_activeSinceMs = 0;
    qint64 m_lastMediaMs = 0;

    std::unique_ptr<CallSession> m_session;
    std::unique_ptr<AudioCaptureSource> m_capture;
    std::unique_ptr<AudioPlaybackSink> m_playback;
    CallSoundBoard m_sounds;

    QTimer *m_ringTimer = nullptr;
    QTimer *m_stallTimer = nullptr;
    QTimer *m_levelTimer = nullptr;
    // Holds the speaker open after a call ends, just long enough for the
    // hang-up sound to finish playing through it.
    QTimer *m_playbackTailTimer = nullptr;
};

} // namespace OpenChat
