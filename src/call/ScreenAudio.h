#pragma once

#include "call/CallMediaCrypto.h"
#include "call/CallTypes.h"
#include "media/JitterBuffer.h"

#include <QByteArray>
#include <QMutex>

#include <atomic>
#include <functional>
#include <map>
#include <memory>

struct OpusEncoder;
struct OpusDecoder;

namespace OpenChat {

// The sound of a shared screen: what the sharer's computer is playing, sent
// beside the picture as a stream of its own.
//
// It is not the voice path. Voice is mono, tuned for speech and gated when
// nobody talks; a shared screen carries films, games and music, in stereo, and
// must not be gated or noise-suppressed away. So it has its own format, its own
// codec settings, its own keys and its own wire version (5), and it is mixed
// into the speaker after the voices, in stereo.
//
// Nothing here touches a device or a socket. The platform capture that feeds
// it lives behind ScreenAudioCapture; the engine moves the packets.

struct ScreenAudioFormat final {
    static constexpr int sampleRate = 48'000;
    static constexpr int channels = 2;
    static constexpr int bytesPerSample = 2; // signed 16-bit little-endian, interleaved L R
    static constexpr int frameDurationMs = 20;
    // Per channel: 960. The frame is 3840 bytes.
    static constexpr int samplesPerFrame = sampleRate / 1000 * frameDurationMs;
    static constexpr int bytesPerFrame = samplesPerFrame * channels * bytesPerSample;
};

// One 20 ms interleaved stereo frame: exactly ScreenAudioFormat::bytesPerFrame.
using StereoFrame = QByteArray;

[[nodiscard]] inline bool isFullStereoFrame(const StereoFrame &frame) noexcept
{
    return frame.size() == ScreenAudioFormat::bytesPerFrame;
}

[[nodiscard]] inline StereoFrame silentStereoFrame()
{
    return StereoFrame(ScreenAudioFormat::bytesPerFrame, '\0');
}

// Adds `frame`, scaled by `gain`, into `mix`, saturating rather than wrapping.
// Frames of the wrong size are ignored.
void mixStereoInto(StereoFrame &mix, const StereoFrame &frame, double gain = 1.0);

// Stereo Opus for whatever a computer plays. Music mode rather than VoIP, no
// speech-only tricks, and a rate that keeps music intact: 128 kbit/s is a
// small fraction of what the picture beside it costs.
class ScreenAudioEncoder final
{
public:
    static constexpr int bitrate = 128'000;

    ScreenAudioEncoder();
    ~ScreenAudioEncoder();

    ScreenAudioEncoder(const ScreenAudioEncoder &) = delete;
    ScreenAudioEncoder &operator=(const ScreenAudioEncoder &) = delete;

    [[nodiscard]] bool isValid() const noexcept { return m_encoder != nullptr; }
    // One Opus packet for one full frame; empty for a wrong-size frame or a
    // failed encode.
    [[nodiscard]] QByteArray encode(const StereoFrame &frame);

private:
    OpusEncoder *m_encoder = nullptr;
};

// One peer's screen-sound path: the sealing half for our share, the opening,
// reordering and decoding half for theirs.
//
// Wire shape, the same header as a voice frame under its own version:
//
//   0       version (5)
//   1       flags (none yet; must be 0)
//   2..17   call id
//   18..21  sequence, big-endian
//   22..    AES-256-GCM(Opus packet) || tag
//
// Internally synchronised: packets arrive on the engine's thread, frames are
// pulled by the playback device's.
class ScreenAudioSession final
{
public:
    static constexpr quint8 wireVersion = 5;
    static constexpr qsizetype headerBytes = 22;
    // The most a single Opus frame can be.
    static constexpr qsizetype maxPayloadBytes = 1'275;
    static constexpr qsizetype maxPacketBytes =
        headerBytes + maxPayloadBytes + CallMediaSealer::tagBytes;
    // A share sends a frame every 20 ms, silence included, for as long as its
    // sound is on; this long without one and it is taken to have stopped.
    static constexpr qint64 activeWindowMs = 1'000;

    enum class ReceiveResult {
        Queued,
        Malformed,
        WrongCall,
        Unauthentic,
        Replay,
        Late,
        Duplicate,
        Overflow,
    };

    struct Stats final {
        quint64 framesSent = 0;
        quint64 bytesSent = 0;
        quint64 packetsReceived = 0;
        quint64 packetsRejected = 0;
        quint64 framesPlayed = 0;
        quint64 framesConcealed = 0;
    };

    // Null when the secret cannot be expanded into keys or Opus cannot be set
    // up. `clock` is the call's media clock, in milliseconds.
    [[nodiscard]] static std::unique_ptr<ScreenAudioSession>
    create(const CallId &callId, CallDirection direction, QByteArrayView callSecret,
           std::function<qint64()> clock);
    ~ScreenAudioSession();

    ScreenAudioSession(const ScreenAudioSession &) = delete;
    ScreenAudioSession &operator=(const ScreenAudioSession &) = delete;

    // Seals one encoded frame for this peer. Empty on failure.
    [[nodiscard]] QByteArray seal(QByteArrayView opusPacket);

    [[nodiscard]] ReceiveResult receive(QByteArrayView packet);

    // The frame the speaker should play now. Silence while nothing is queued.
    [[nodiscard]] StereoFrame nextFrame();

    // True while the far end is sharing sound: a frame arrived recently.
    [[nodiscard]] bool isActive() const;

    // Forget the far end's stream, e.g. when their share stops.
    void resetReceiver();

    [[nodiscard]] Stats stats() const;

private:
    ScreenAudioSession(const CallId &callId, CallMediaKeys sendKeys, CallMediaKeys receiveKeys,
                       std::function<qint64()> clock, OpusDecoder *decoder);

    [[nodiscard]] StereoFrame conceal();

    CallId m_callId;
    std::function<qint64()> m_clock;
    CallMediaSealer m_sealer;
    CallMediaOpener m_opener;
    OpusDecoder *m_decoder = nullptr;
    JitterBuffer m_jitter;
    quint32 m_nextSequence = 0;
    qint64 m_lastPacketMs = 0;
    bool m_receivedAny = false;
    bool m_lastQuiet = true;
    Stats m_stats;
    mutable QMutex m_mutex;
};

// Every share's sound, summed for the speaker. Sessions are added and removed
// on the engine's thread and pulled on the playback device's; a session's
// pointer is shared, so one removed mid-pull still finishes its frame.
class ScreenAudioMixer final
{
public:
    // How loud shared sound plays, 0 (muted) to 1 (as sent).
    void setVolume(double volume);
    [[nodiscard]] double volume() const { return m_volume.load(); }

    void add(const QByteArray &key, std::shared_ptr<ScreenAudioSession> session);
    void remove(const QByteArray &key);
    void clear();

    // One frame of every active share's sound, or an empty array when no share
    // is sending any (or all of it is muted), so the caller can skip the work.
    // Every session is still advanced, muted or not, so their buffers keep
    // time with the speaker.
    [[nodiscard]] StereoFrame pull();

private:
    mutable QMutex m_mutex;
    std::map<QByteArray, std::shared_ptr<ScreenAudioSession>> m_sessions;
    std::atomic<double> m_volume{1.0};
};

} // namespace OpenChat
