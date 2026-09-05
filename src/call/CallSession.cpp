#include "call/CallSession.h"

#include <QMutexLocker>

namespace OpenChat {

std::unique_ptr<CallSession> CallSession::create(const Config &config, QByteArrayView callSecret)
{
    const std::optional<CallMediaKeySchedule> schedule =
        CallMediaKeySchedule::derive(callSecret, config.callId);
    if (!schedule)
        return nullptr;
    std::unique_ptr<AudioCodec> encoder = makeAudioCodec(config.codec);
    std::unique_ptr<AudioCodec> decoder = makeAudioCodec(config.codec);
    if (!encoder || !decoder)
        return nullptr;
    return std::unique_ptr<CallSession>(new CallSession(
        config, schedule->sendKeys(config.direction), schedule->receiveKeys(config.direction),
        std::move(encoder), std::move(decoder)));
}

CallSession::CallSession(Config config, CallMediaKeys sendKeys, CallMediaKeys receiveKeys,
                         std::unique_ptr<AudioCodec> encoder, std::unique_ptr<AudioCodec> decoder)
    : m_config(std::move(config))
    , m_sealer(std::move(sendKeys))
    , m_opener(std::move(receiveKeys))
    , m_encoder(std::move(encoder))
    , m_decoder(std::move(decoder))
    , m_jitter(m_config.jitter)
    , m_localMeter(m_config.levels)
    , m_remoteMeter(m_config.levels)
{
}

QByteArray CallSession::processCapturedFrame(const AudioFrame &frame)
{
    const QMutexLocker locked(&m_mutex);
    if (!isFullAudioFrame(frame))
        return {};
    ++m_stats.framesCaptured;

    // Mute substitutes silence before the encoder sees the frame, so muted audio
    // is not merely flagged on the wire — it is never encoded at all. The level
    // meter follows the same substitution, so a muted caller reads as silent.
    if (m_muted)
        m_localMeter.updateSilent();
    else
        m_localMeter.update(frame);

    const QByteArray payload = m_encoder->encode(m_muted ? silentAudioFrame() : frame);
    if (payload.isEmpty())
        return {};

    CallMediaPacket packet;
    packet.callId = m_config.callId;
    packet.sequence = m_nextSequence;
    packet.flags = m_muted ? CallMediaPacket::flagMuted : 0;

    // The header is built before sealing because it IS the associated data: the
    // sequence and call id the receiver routes on are covered by the same tag as
    // the audio, so neither can be altered in flight.
    const QByteArray header = packet.header();
    packet.sealed = m_sealer.seal(m_nextSequence, payload, header);
    if (packet.sealed.isEmpty())
        return {};

    ++m_nextSequence;
    ++m_stats.framesSent;
    const QByteArray encoded = packet.encode();
    m_stats.bytesSent += static_cast<quint64>(encoded.size());
    return encoded;
}

CallSession::ReceiveResult CallSession::processIncomingPacket(QByteArrayView packetBytes)
{
    const QMutexLocker locked(&m_mutex);
    ++m_stats.packetsReceived;
    m_stats.bytesReceived += static_cast<quint64>(packetBytes.size());

    const std::optional<CallMediaPacket> packet = CallMediaPacket::decode(packetBytes);
    if (!packet) {
        ++m_stats.packetsRejected;
        return ReceiveResult::Malformed;
    }
    // Checked before the AEAD so media left over from a previous call (or aimed
    // at a concurrent one) is discarded on identity, not on a failed tag.
    if (packet->callId != m_config.callId) {
        ++m_stats.packetsRejected;
        return ReceiveResult::WrongCall;
    }

    // The opener refuses a replay before it spends a decryption, so the change
    // in its replay counter is what separates the two rejection reasons.
    const quint64 replaysBefore = m_opener.replayCount();
    const std::optional<QByteArray> payload =
        m_opener.open(packet->sequence, packet->sealed, packet->header());
    if (!payload) {
        ++m_stats.packetsRejected;
        return m_opener.replayCount() > replaysBefore ? ReceiveResult::Replay
                                                      : ReceiveResult::Unauthentic;
    }

    switch (m_jitter.push(packet->sequence, *payload)) {
    case JitterBuffer::PushResult::Accepted:
    case JitterBuffer::PushResult::Reset:
        return ReceiveResult::Queued;
    case JitterBuffer::PushResult::Duplicate:
        ++m_stats.packetsRejected;
        return ReceiveResult::Duplicate;
    case JitterBuffer::PushResult::Late:
        ++m_stats.packetsRejected;
        return ReceiveResult::Late;
    case JitterBuffer::PushResult::Overflow:
        ++m_stats.packetsRejected;
        return ReceiveResult::Overflow;
    }
    return ReceiveResult::Malformed;
}

AudioFrame CallSession::nextPlaybackFrame()
{
    const QMutexLocker locked(&m_mutex);
    // The remote level meter is the buffer's ear: it has heard everything that
    // has played so far, and its hangover means "not speaking" holds only once
    // the far end has been quiet for a good fraction of a second — the moment a
    // frame can be added or removed without anyone noticing.
    const JitterBuffer::PopResult next = m_jitter.pop(!m_remoteMeter.isSpeaking());
    // A frame the buffer removed to shed latency is still decoded, because Opus
    // predicts each frame from the last: skipping it in the decoder as well
    // would make the frame that follows decode against the wrong history and
    // put a click exactly where the correction was meant to be silent.
    if (!next.skipped.isEmpty())
        (void)m_decoder->decode(next.skipped);
    AudioFrame frame;
    switch (next.kind) {
    case JitterBuffer::PopKind::Frame:
        frame = m_decoder->decode(next.payload);
        if (frame.isEmpty()) {
            // The packet authenticated but the codec could not use it. Conceal
            // rather than emit nothing: playback must still get a frame.
            frame = m_decoder->concealLoss();
            ++m_stats.framesConcealed;
        }
        break;
    case JitterBuffer::PopKind::Lost:
    case JitterBuffer::PopKind::Inserted:
        // Either a slot nothing arrived for, or one the buffer added to deepen
        // itself; to the codec they are the same request — invent the frame
        // that most plausibly continues what was last heard.
        frame = m_decoder->concealLoss();
        ++m_stats.framesConcealed;
        break;
    case JitterBuffer::PopKind::Starved:
        // Nothing buffered at all: the call has not started, has stalled, or has
        // ended. Silence is the honest output; concealment here would invent
        // audio out of an arbitrarily old frame.
        frame = silentAudioFrame();
        ++m_stats.framesSilent;
        break;
    }
    if (!isFullAudioFrame(frame))
        frame = silentAudioFrame();

    ++m_stats.framesPlayed;
    m_remoteMeter.update(frame);
    return frame;
}

void CallSession::setMuted(bool muted)
{
    const QMutexLocker locked(&m_mutex);
    m_muted = muted;
}

bool CallSession::isMuted() const
{
    const QMutexLocker locked(&m_mutex);
    return m_muted;
}

double CallSession::localLevel() const
{
    const QMutexLocker locked(&m_mutex);
    return m_localMeter.level();
}

double CallSession::remoteLevel() const
{
    const QMutexLocker locked(&m_mutex);
    return m_remoteMeter.level();
}

bool CallSession::isLocalSpeaking() const
{
    const QMutexLocker locked(&m_mutex);
    return m_localMeter.isSpeaking();
}

bool CallSession::isRemoteSpeaking() const
{
    const QMutexLocker locked(&m_mutex);
    return m_remoteMeter.isSpeaking();
}

CallSession::Stats CallSession::stats() const
{
    const QMutexLocker locked(&m_mutex);
    return m_stats;
}

JitterBufferStats CallSession::jitterStats() const
{
    const QMutexLocker locked(&m_mutex);
    return m_jitter.stats();
}

quint64 CallSession::replayCount() const
{
    const QMutexLocker locked(&m_mutex);
    return m_opener.replayCount();
}

} // namespace OpenChat
