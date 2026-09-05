#include "media/OpusAudioCodec.h"

#include "media/AudioConvert.h"

#include <opus.h>

namespace OpenChat {

OpusAudioCodec::OpusAudioCodec(int bitrate)
{
    int encoderError = OPUS_OK;
    m_encoder = opus_encoder_create(CallAudioFormat::sampleRate, CallAudioFormat::channels,
                                    OPUS_APPLICATION_VOIP, &encoderError);
    if (encoderError != OPUS_OK) {
        m_encoder = nullptr;
    } else {
        opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bitrate));
        // In-band FEC lets the decoder reconstruct a lost frame from the next
        // packet; it costs bitrate only when the encoder is told loss is likely.
        opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(10));
        opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }

    int decoderError = OPUS_OK;
    m_decoder = opus_decoder_create(CallAudioFormat::sampleRate, CallAudioFormat::channels,
                                    &decoderError);
    if (decoderError != OPUS_OK)
        m_decoder = nullptr;
}

OpusAudioCodec::~OpusAudioCodec()
{
    if (m_encoder != nullptr)
        opus_encoder_destroy(m_encoder);
    if (m_decoder != nullptr)
        opus_decoder_destroy(m_decoder);
}

bool OpusAudioCodec::isValid() const noexcept
{
    return m_encoder != nullptr && m_decoder != nullptr;
}

QByteArray OpusAudioCodec::encode(const AudioFrame &frame)
{
    if (m_encoder == nullptr || !isFullAudioFrame(frame))
        return {};
    QByteArray packet(maxPacketBytes, Qt::Uninitialized);
    const opus_int32 written = opus_encode(
        m_encoder, reinterpret_cast<const opus_int16 *>(frame.constData()),
        CallAudioFormat::samplesPerFrame, reinterpret_cast<unsigned char *>(packet.data()),
        maxPacketBytes);
    if (written <= 0)
        return {};
    packet.resize(written);
    return packet;
}

AudioFrame OpusAudioCodec::decode(QByteArrayView payload)
{
    if (m_decoder == nullptr || payload.isEmpty() || payload.size() > maxPacketBytes)
        return {};
    AudioFrame frame(CallAudioFormat::bytesPerFrame, Qt::Uninitialized);
    const int decoded = opus_decode(
        m_decoder, reinterpret_cast<const unsigned char *>(payload.data()),
        static_cast<opus_int32>(payload.size()), reinterpret_cast<opus_int16 *>(frame.data()),
        CallAudioFormat::samplesPerFrame, /*decode_fec=*/0);
    // A packet that decodes to a different frame length is not something this
    // pipeline can splice into a fixed-cadence stream; treat it as a loss.
    if (decoded != CallAudioFormat::samplesPerFrame)
        return {};
    return frame;
}

AudioFrame OpusAudioCodec::concealLoss()
{
    if (m_decoder == nullptr)
        return silentAudioFrame();
    AudioFrame frame(CallAudioFormat::bytesPerFrame, Qt::Uninitialized);
    // A null packet asks Opus to run its own packet-loss concealment, which
    // extrapolates the previous frame instead of dropping to silence.
    const int decoded = opus_decode(m_decoder, nullptr, 0,
                                    reinterpret_cast<opus_int16 *>(frame.data()),
                                    CallAudioFormat::samplesPerFrame, /*decode_fec=*/0);
    if (decoded != CallAudioFormat::samplesPerFrame)
        return silentAudioFrame();
    return frame;
}

} // namespace OpenChat
