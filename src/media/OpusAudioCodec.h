#pragma once

#include "media/AudioCodec.h"

struct OpusEncoder;
struct OpusDecoder;

namespace OpenChat {

// Opus at the call's native 48 kHz mono, one 20 ms frame per packet.
//
// Configured for interactive voice: VoIP application mode, in-band FEC on, and a
// bitrate that keeps a call comfortable on a slow uplink. Loss is handled by
// asking the decoder to conceal (its own PLC) rather than by inserting silence,
// which is audibly far better across a short drop.
class OpusAudioCodec final : public AudioCodec
{
public:
    static constexpr int defaultBitrate = 24'000;
    // An Opus packet for one 20 ms mono frame never approaches this; it bounds
    // the encode buffer and rejects an absurd payload before decoding it.
    static constexpr int maxPacketBytes = 4'000;

    explicit OpusAudioCodec(int bitrate = defaultBitrate);
    ~OpusAudioCodec() override;

    OpusAudioCodec(const OpusAudioCodec &) = delete;
    OpusAudioCodec &operator=(const OpusAudioCodec &) = delete;

    // False when either the encoder or the decoder failed to initialise; such a
    // codec encodes and decodes to nothing rather than crashing.
    [[nodiscard]] bool isValid() const noexcept;

    [[nodiscard]] AudioCodecKind kind() const noexcept override { return AudioCodecKind::Opus; }
    [[nodiscard]] QByteArray encode(const AudioFrame &frame) override;
    [[nodiscard]] AudioFrame decode(QByteArrayView payload) override;
    [[nodiscard]] AudioFrame concealLoss() override;

private:
    OpusEncoder *m_encoder = nullptr;
    OpusDecoder *m_decoder = nullptr;
};

} // namespace OpenChat
