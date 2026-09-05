#pragma once

#include "media/AudioCodec.h"

namespace OpenChat {

// The identity codec: a frame is its own payload. It exists so a call has a
// codec that is always present and provably lossless, which makes it the
// reference the compressing codecs are measured against.
class PcmAudioCodec final : public AudioCodec
{
public:
    [[nodiscard]] AudioCodecKind kind() const noexcept override { return AudioCodecKind::Pcm; }

    [[nodiscard]] QByteArray encode(const AudioFrame &frame) override
    {
        return isFullAudioFrame(frame) ? frame : QByteArray();
    }

    [[nodiscard]] AudioFrame decode(QByteArrayView payload) override
    {
        return payload.size() == CallAudioFormat::bytesPerFrame ? payload.toByteArray()
                                                                : AudioFrame();
    }
};

} // namespace OpenChat
