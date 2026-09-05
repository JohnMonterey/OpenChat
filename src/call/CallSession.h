#pragma once

#include "call/CallMediaCrypto.h"
#include "call/CallMediaPacket.h"
#include "call/CallTypes.h"
#include "media/AudioCodec.h"
#include "media/AudioTypes.h"
#include "media/JitterBuffer.h"
#include "media/SpeechLevelMeter.h"

#include <QByteArray>
#include <QMutex>

#include <memory>
#include <optional>

namespace OpenChat {

// Everything a live call does to audio, with nothing that touches a device, a
// socket, or a clock.
//
// One direction of the pipeline is captured PCM in and a wire packet out
// (encode, seal, frame); the other is a wire packet in and playback PCM out
// (parse, open, reorder, decode or conceal). Keeping both halves in one
// dependency-free object is what makes a call testable: wire two sessions'
// outputs to each other's inputs, push a real audio file through one, and read
// the other's playback — no audio hardware and no network involved.
//
// The session is deliberately not a QObject and holds no timer. Whoever owns it
// decides when a frame is captured and when playback is due; the session only
// answers, once per call, what those frames should be.
//
// It IS internally synchronised, because the two halves genuinely run on
// different threads in a live call: capture is marshalled onto the owning
// thread, but playback must answer the audio device synchronously and therefore
// runs on whatever thread that device pulls from. The lock is held only for the
// few microseconds a frame takes to encode or decode.
class CallSession final
{
public:
    struct Config final {
        CallId callId = CallId::generate();
        CallDirection direction = CallDirection::Outgoing;
        AudioCodecKind codec = AudioCodecKind::Pcm;
        JitterBuffer::Config jitter{};
        SpeechLevelMeter::Config levels{};
    };

    // Why a received packet was not queued. Every rejection is counted, because
    // a call that sounds wrong is usually a call whose packets are being dropped
    // for one specific reason.
    enum class ReceiveResult {
        Queued,
        Malformed,   // not a well-formed packet of the current version
        WrongCall,   // authenticated shape, but for a different call id
        Unauthentic, // failed AEAD verification
        Replay,      // a sequence already accepted
        Late,        // valid, but its playout slot had already passed
        Duplicate,   // already queued for playout
        Overflow,    // too far ahead of playout to hold
    };

    struct Stats final {
        quint64 framesCaptured = 0;
        quint64 framesSent = 0;
        quint64 framesPlayed = 0;
        quint64 framesConcealed = 0; // a gap the codec papered over
        quint64 framesSilent = 0;    // playback with nothing buffered at all
        quint64 packetsReceived = 0;
        quint64 packetsRejected = 0;
        quint64 bytesSent = 0;
        quint64 bytesReceived = 0;
    };

    // Builds a session, or nullptr when the codec is unavailable in this build
    // or the secret cannot be expanded into media keys. A call that cannot key
    // itself must fail rather than fall back to sending audio in the clear.
    [[nodiscard]] static std::unique_ptr<CallSession> create(const Config &config,
                                                             QByteArrayView callSecret);

    // Consumes one captured frame and returns the packet to send, or an empty
    // array if there is nothing to send (a wrong-length frame, or an encoder
    // that produced nothing). While muted the audio is replaced with silence
    // before it is encoded, so muting removes the sound rather than merely
    // hiding it — nothing recognisable ever leaves the machine.
    [[nodiscard]] QByteArray processCapturedFrame(const AudioFrame &frame);

    // Takes one packet off the wire. Only Queued means it will be heard.
    [[nodiscard]] ReceiveResult processIncomingPacket(QByteArrayView packet);

    // The frame playback should render now. Always exactly one full frame: a
    // gap is concealed by the codec, and a buffer with nothing in it yields
    // silence, so the playback device is never starved of a frame to play.
    [[nodiscard]] AudioFrame nextPlaybackFrame();

    void setMuted(bool muted);
    [[nodiscard]] bool isMuted() const;

    [[nodiscard]] const CallId &callId() const noexcept { return m_config.callId; }
    [[nodiscard]] AudioCodecKind codec() const noexcept { return m_config.codec; }

    [[nodiscard]] double localLevel() const;
    [[nodiscard]] double remoteLevel() const;
    [[nodiscard]] bool isLocalSpeaking() const;
    [[nodiscard]] bool isRemoteSpeaking() const;

    // Snapshots rather than references: the underlying counters are mutated
    // under the lock by whichever thread is running the pipeline.
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] JitterBufferStats jitterStats() const;
    [[nodiscard]] quint64 replayCount() const;

private:
    CallSession(Config config, CallMediaKeys sendKeys, CallMediaKeys receiveKeys,
                std::unique_ptr<AudioCodec> encoder, std::unique_ptr<AudioCodec> decoder);

    Config m_config;
    CallMediaSealer m_sealer;
    CallMediaOpener m_opener;
    // Separate codec objects: Opus carries state across frames in each
    // direction, so sharing one would corrupt both streams.
    std::unique_ptr<AudioCodec> m_encoder;
    std::unique_ptr<AudioCodec> m_decoder;
    JitterBuffer m_jitter;
    SpeechLevelMeter m_localMeter;
    SpeechLevelMeter m_remoteMeter;
    quint32 m_nextSequence = 0;
    bool m_muted = false;
    Stats m_stats;
    // Guards everything above. Mutable so the const observers can take it.
    mutable QMutex m_mutex;
};

} // namespace OpenChat
