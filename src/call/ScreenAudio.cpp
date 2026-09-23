#include "call/ScreenAudio.h"

#include <QMutexLocker>
#include <QtEndian>

#include <opus.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace OpenChat {

namespace {

// Frames below this peak count as a quiet moment, when the jitter buffer may
// add or drop a frame to follow the far end's clock without being heard.
constexpr int quietPeak = 96; // about -50 dBFS

[[nodiscard]] qint64 fallbackClockMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] QByteArray headerFor(const CallId &callId, quint32 sequence)
{
    QByteArray header;
    header.reserve(ScreenAudioSession::headerBytes);
    header.append(char(ScreenAudioSession::wireVersion));
    header.append(char(0));
    header.append(callId.bytes());
    char sequenceBytes[4];
    qToBigEndian(sequence, sequenceBytes);
    header.append(sequenceBytes, 4);
    return header;
}

[[nodiscard]] bool isQuiet(const StereoFrame &frame)
{
    const auto *samples = reinterpret_cast<const qint16 *>(frame.constData());
    const qsizetype count = frame.size() / 2;
    for (qsizetype i = 0; i < count; ++i) {
        if (std::abs(int(samples[i])) > quietPeak)
            return false;
    }
    return true;
}

} // namespace

void mixStereoInto(StereoFrame &mix, const StereoFrame &frame, double gain)
{
    if (!isFullStereoFrame(mix) || !isFullStereoFrame(frame))
        return;
    auto *out = reinterpret_cast<qint16 *>(mix.data());
    const auto *in = reinterpret_cast<const qint16 *>(frame.constData());
    const int count = ScreenAudioFormat::samplesPerFrame * ScreenAudioFormat::channels;
    // Fixed point, so a gain of exactly 1 adds the samples unchanged.
    const int scale = int(std::lround(std::clamp(gain, 0.0, 4.0) * 4096.0));
    for (int i = 0; i < count; ++i) {
        const int added = scale == 4096 ? int(in[i]) : (int(in[i]) * scale) / 4096;
        out[i] = qint16(std::clamp(int(out[i]) + added, int(std::numeric_limits<qint16>::min()),
                                   int(std::numeric_limits<qint16>::max())));
    }
}

// --- Encoder -------------------------------------------------------------------

ScreenAudioEncoder::ScreenAudioEncoder()
{
    int error = OPUS_OK;
    m_encoder = opus_encoder_create(ScreenAudioFormat::sampleRate, ScreenAudioFormat::channels,
                                    OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK) {
        m_encoder = nullptr;
        return;
    }
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bitrate));
    // 8 rather than 10: indistinguishable on this material, and this runs on
    // the call's thread fifty times a second.
    opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(8));
    opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1));
    // Films and games are neither speech nor music; Opus decides per frame.
    opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_AUTO));
    // No FEC and no DTX, for the voice path's reasons: FEC is only useful to a
    // decoder that waits for the next packet, and a steady stream of frames is
    // what tells the far end the share's sound is still on.
    opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(m_encoder, OPUS_SET_DTX(0));
}

ScreenAudioEncoder::~ScreenAudioEncoder()
{
    if (m_encoder != nullptr)
        opus_encoder_destroy(m_encoder);
}

QByteArray ScreenAudioEncoder::encode(const StereoFrame &frame)
{
    if (m_encoder == nullptr || !isFullStereoFrame(frame))
        return {};
    QByteArray packet(ScreenAudioSession::maxPayloadBytes, Qt::Uninitialized);
    const opus_int32 written =
        opus_encode(m_encoder, reinterpret_cast<const opus_int16 *>(frame.constData()),
                    ScreenAudioFormat::samplesPerFrame,
                    reinterpret_cast<unsigned char *>(packet.data()),
                    opus_int32(ScreenAudioSession::maxPayloadBytes));
    if (written <= 0)
        return {};
    packet.resize(written);
    return packet;
}

// --- Session -------------------------------------------------------------------

std::unique_ptr<ScreenAudioSession> ScreenAudioSession::create(const CallId &callId,
                                                               CallDirection direction,
                                                               QByteArrayView callSecret,
                                                               std::function<qint64()> clock)
{
    const std::optional<CallMediaKeySchedule> schedule =
        CallMediaKeySchedule::deriveScreenAudio(callSecret, callId);
    if (!schedule)
        return nullptr;
    int error = OPUS_OK;
    OpusDecoder *decoder =
        opus_decoder_create(ScreenAudioFormat::sampleRate, ScreenAudioFormat::channels, &error);
    if (error != OPUS_OK || decoder == nullptr)
        return nullptr;
    if (!clock)
        clock = fallbackClockMs;
    return std::unique_ptr<ScreenAudioSession>(
        new ScreenAudioSession(callId, schedule->sendKeys(direction), schedule->receiveKeys(direction),
                               std::move(clock), decoder));
}

namespace {

[[nodiscard]] JitterBuffer::Config screenAudioJitter(std::function<qint64()> clock)
{
    JitterBuffer::Config config;
    // 120 ms to start with: a little more cushion than voice, because nobody
    // is waiting on a reply, and about what the picture beside it takes to
    // arrive, which keeps the two roughly together.
    config.targetDepth = 6;
    config.clock = std::move(clock);
    return config;
}

} // namespace

ScreenAudioSession::ScreenAudioSession(const CallId &callId, CallMediaKeys sendKeys,
                                       CallMediaKeys receiveKeys, std::function<qint64()> clock,
                                       OpusDecoder *decoder)
    : m_callId(callId)
    , m_clock(std::move(clock))
    , m_sealer(std::move(sendKeys))
    , m_opener(std::move(receiveKeys))
    , m_decoder(decoder)
    , m_jitter(screenAudioJitter(m_clock))
{
}

ScreenAudioSession::~ScreenAudioSession()
{
    if (m_decoder != nullptr)
        opus_decoder_destroy(m_decoder);
}

QByteArray ScreenAudioSession::seal(QByteArrayView opusPacket)
{
    const QMutexLocker locked(&m_mutex);
    if (opusPacket.isEmpty() || opusPacket.size() > maxPayloadBytes)
        return {};
    const QByteArray header = headerFor(m_callId, m_nextSequence);
    const QByteArray sealed = m_sealer.seal(m_nextSequence, opusPacket, header);
    if (sealed.isEmpty())
        return {};
    ++m_nextSequence;
    ++m_stats.framesSent;
    QByteArray packet = header + sealed;
    m_stats.bytesSent += quint64(packet.size());
    return packet;
}

ScreenAudioSession::ReceiveResult ScreenAudioSession::receive(QByteArrayView packet)
{
    const QMutexLocker locked(&m_mutex);
    ++m_stats.packetsReceived;
    const auto reject = [this](ReceiveResult result) {
        ++m_stats.packetsRejected;
        return result;
    };
    if (packet.size() <= headerBytes + CallMediaSealer::tagBytes || packet.size() > maxPacketBytes)
        return reject(ReceiveResult::Malformed);
    const auto *bytes = reinterpret_cast<const uchar *>(packet.data());
    if (bytes[0] != wireVersion || bytes[1] != 0)
        return reject(ReceiveResult::Malformed);
    if (packet.sliced(2, CallId::byteCount) != QByteArrayView(m_callId.bytes()))
        return reject(ReceiveResult::WrongCall);
    const quint32 sequence = qFromBigEndian<quint32>(bytes + 18);

    const quint64 replaysBefore = m_opener.replayCount();
    const std::optional<QByteArray> payload =
        m_opener.open(sequence, packet.sliced(headerBytes), packet.first(headerBytes));
    if (!payload) {
        return reject(m_opener.replayCount() > replaysBefore ? ReceiveResult::Replay
                                                             : ReceiveResult::Unauthentic);
    }

    // Sound that stopped and started again is a new stretch: whatever the
    // buffer still holds from before is stale, and playing it first would
    // put the old tail in front of the new sound.
    const qint64 now = m_clock();
    if (m_receivedAny && now - m_lastPacketMs > activeWindowMs)
        m_jitter.reset();
    m_receivedAny = true;
    m_lastPacketMs = now;

    switch (m_jitter.push(sequence, *payload)) {
    case JitterBuffer::PushResult::Accepted:
    case JitterBuffer::PushResult::Reset:
        return ReceiveResult::Queued;
    case JitterBuffer::PushResult::Duplicate:
        return reject(ReceiveResult::Duplicate);
    case JitterBuffer::PushResult::Late:
        return reject(ReceiveResult::Late);
    case JitterBuffer::PushResult::Overflow:
        return reject(ReceiveResult::Overflow);
    }
    return reject(ReceiveResult::Malformed);
}

StereoFrame ScreenAudioSession::conceal()
{
    StereoFrame frame(ScreenAudioFormat::bytesPerFrame, Qt::Uninitialized);
    const int decoded = opus_decode(m_decoder, nullptr, 0, reinterpret_cast<opus_int16 *>(frame.data()),
                                    ScreenAudioFormat::samplesPerFrame, 0);
    ++m_stats.framesConcealed;
    return decoded == ScreenAudioFormat::samplesPerFrame ? frame : silentStereoFrame();
}

StereoFrame ScreenAudioSession::nextFrame()
{
    const QMutexLocker locked(&m_mutex);
    const JitterBuffer::PopResult next = m_jitter.pop(m_lastQuiet);
    // A frame dropped to shed latency still goes through the decoder, which
    // predicts each frame from the one before.
    if (!next.skipped.isEmpty()) {
        StereoFrame scratch(ScreenAudioFormat::bytesPerFrame, Qt::Uninitialized);
        const int skipped =
            opus_decode(m_decoder, reinterpret_cast<const unsigned char *>(next.skipped.constData()),
                        opus_int32(next.skipped.size()), reinterpret_cast<opus_int16 *>(scratch.data()),
                        ScreenAudioFormat::samplesPerFrame, 0);
        Q_UNUSED(skipped)
    }
    StereoFrame frame;
    switch (next.kind) {
    case JitterBuffer::PopKind::Frame: {
        frame = StereoFrame(ScreenAudioFormat::bytesPerFrame, Qt::Uninitialized);
        const int decoded =
            opus_decode(m_decoder, reinterpret_cast<const unsigned char *>(next.payload.constData()),
                        opus_int32(next.payload.size()), reinterpret_cast<opus_int16 *>(frame.data()),
                        ScreenAudioFormat::samplesPerFrame, 0);
        if (decoded != ScreenAudioFormat::samplesPerFrame)
            frame = conceal();
        break;
    }
    case JitterBuffer::PopKind::Lost:
    case JitterBuffer::PopKind::Inserted:
        frame = conceal();
        break;
    case JitterBuffer::PopKind::Starved:
        return silentStereoFrame();
    }
    ++m_stats.framesPlayed;
    m_lastQuiet = isQuiet(frame);
    return frame;
}

bool ScreenAudioSession::isActive() const
{
    const QMutexLocker locked(&m_mutex);
    return m_receivedAny && m_clock() - m_lastPacketMs <= activeWindowMs;
}

void ScreenAudioSession::resetReceiver()
{
    const QMutexLocker locked(&m_mutex);
    m_jitter.reset();
    m_receivedAny = false;
    m_lastPacketMs = 0;
    m_lastQuiet = true;
    opus_decoder_ctl(m_decoder, OPUS_RESET_STATE);
}

ScreenAudioSession::Stats ScreenAudioSession::stats() const
{
    const QMutexLocker locked(&m_mutex);
    return m_stats;
}

// --- Mixer ---------------------------------------------------------------------

void ScreenAudioMixer::setVolume(double volume)
{
    m_volume.store(std::clamp(volume, 0.0, 1.0));
}

void ScreenAudioMixer::add(const QByteArray &key, std::shared_ptr<ScreenAudioSession> session)
{
    const QMutexLocker locked(&m_mutex);
    if (session)
        m_sessions[key] = std::move(session);
    else
        m_sessions.erase(key);
}

void ScreenAudioMixer::remove(const QByteArray &key)
{
    const QMutexLocker locked(&m_mutex);
    m_sessions.erase(key);
}

void ScreenAudioMixer::clear()
{
    const QMutexLocker locked(&m_mutex);
    m_sessions.clear();
}

StereoFrame ScreenAudioMixer::pull()
{
    std::vector<std::shared_ptr<ScreenAudioSession>> sessions;
    {
        const QMutexLocker locked(&m_mutex);
        sessions.reserve(m_sessions.size());
        for (const auto &entry : m_sessions)
            sessions.push_back(entry.second);
    }
    const double gain = m_volume.load();
    StereoFrame mix;
    for (const std::shared_ptr<ScreenAudioSession> &session : sessions) {
        if (!session->isActive())
            continue;
        // Pulled even when muted, so the buffer keeps draining in time.
        const StereoFrame frame = session->nextFrame();
        if (gain <= 0.0)
            continue;
        if (mix.isEmpty())
            mix = silentStereoFrame();
        mixStereoInto(mix, frame, gain);
    }
    return mix;
}

} // namespace OpenChat
