#pragma once

#include "call/AudioIo.h"
#include "call/CallSession.h"
#include "call/CallVideoSession.h"
#include "call/CallSounds.h"
#include "call/CallSignal.h"
#include "call/CallTransport.h"
#include "call/CallTypes.h"
#include "domain/Identifiers.h"

#include <QObject>
#include <QString>

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

// The lifecycle of one voice call at a time: who is being called, what state the
// call is in, and when audio starts and stops flowing.
//
// One at a time is a deliberate constraint, not a simplification. A device has
// one microphone and one speaker, so a second call cannot be carried anyway; the
// engine answers a competing offer with Busy and says so, rather than silently
// dropping it.
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

    CallEngine(Config config, CallTransport &transport, CallAudioIoFactory audioIo,
               QObject *parent = nullptr);
    ~CallEngine() override;

    CallEngine(const CallEngine &) = delete;
    CallEngine &operator=(const CallEngine &) = delete;

    [[nodiscard]] CallState state() const noexcept { return m_state; }
    [[nodiscard]] CallEndReason endReason() const noexcept { return m_endReason; }
    [[nodiscard]] CallDirection direction() const noexcept { return m_direction; }
    [[nodiscard]] const CallPeer &peer() const noexcept { return m_peer; }
    [[nodiscard]] bool isMuted() const noexcept { return m_muted; }
    // Milliseconds since the call became Active, or 0 outside a live call.
    [[nodiscard]] qint64 activeDurationMs() const;

    [[nodiscard]] double localLevel() const;
    [[nodiscard]] double remoteLevel() const;
    [[nodiscard]] bool isLocalSpeaking() const;
    [[nodiscard]] bool isRemoteSpeaking() const;

    // Non-null only between Connecting and Ended; exposed for diagnostics.
    [[nodiscard]] const CallSession *session() const noexcept { return m_session.get(); }

    // Places a call. Fails (returning false, leaving state untouched) when a
    // call is already in progress or the call secret cannot be generated.
    [[nodiscard]] bool placeCall(const CallPeer &peer);

    // Answers or refuses the ringing incoming call. Both are no-ops when there
    // is nothing ringing.
    void acceptCall();
    void declineCall();

    // Ends whatever is in progress, at any stage. Safe to call when idle.
    void hangUp();

    void setMuted(bool muted);
    void sendVideoFrame(const QImage &image);

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

signals:
    void stateChanged();
    void mutedChanged();
    void levelsChanged();
    void remoteVideoFrame(const QImage &image);
    // A call arrived and is ringing. The UI raises the incoming-call surface.
    void incomingCall();
    // A call reached Ended. `reason` is also readable from endReason().
    void callEnded(OpenChat::CallEndReason reason);

private:
    void onSignal(const ConversationId &conversation, const DeviceId &sender,
                  const QByteArray &payload);
    void onMedia(const ConversationId &conversation, const DeviceId &sender,
                 const QByteArray &packet);

    void handleOffer(const ConversationId &conversation, const DeviceId &sender,
                     const CallSignalMessage &message);
    void handleRinging(const CallSignalMessage &message);
    void handleAnswer(const CallSignalMessage &message);
    void handleHangup(const CallSignalMessage &message);

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
    // Drops the microphone and the media session, and with them the call keys.
    // Leaves the speaker running so a departing sound can still be heard.
    void stopCapture();
    void closePlayback();
    void stopMedia();

    void setState(CallState state);
    void endCall(CallEndReason reason, bool notifyPeer);

    void onCapturedFrame(const AudioFrame &frame);
    [[nodiscard]] AudioFrame pullPlaybackFrame();

    Config m_config;
    CallTransport &m_transport;
    CallAudioIoFactory m_audioIo;

    std::unique_ptr<CallVideoSession> m_videoSession;
    QTimer *m_videoTimeout = nullptr;
    CallState m_state = CallState::Idle;
    CallEndReason m_endReason = CallEndReason::None;
    CallDirection m_direction = CallDirection::Outgoing;
    CallPeer m_peer;
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
