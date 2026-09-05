#include "media/AudioCodec.h"

#include "media/OpusAudioCodec.h"
#include "media/PcmAudioCodec.h"

namespace OpenChat {

bool isAudioCodecAvailable(AudioCodecKind kind) noexcept
{
    switch (kind) {
    case AudioCodecKind::Pcm:
    case AudioCodecKind::Opus:
        return true;
    }
    return false;
}

std::unique_ptr<AudioCodec> makeAudioCodec(AudioCodecKind kind)
{
    switch (kind) {
    case AudioCodecKind::Pcm:
        return std::make_unique<PcmAudioCodec>();
    case AudioCodecKind::Opus: {
        auto codec = std::make_unique<OpusAudioCodec>();
        // A codec whose libopus state failed to allocate would silently drop the
        // whole call's audio; refuse it here so the call fails to set up
        // instead of connecting in silence.
        if (!codec->isValid())
            return nullptr;
        return codec;
    }
    }
    return nullptr;
}

AudioCodecKind preferredAudioCodec() noexcept
{
    return AudioCodecKind::Opus;
}

QString audioCodecName(AudioCodecKind kind)
{
    switch (kind) {
    case AudioCodecKind::Pcm:
        return QStringLiteral("PCM/48k");
    case AudioCodecKind::Opus:
        return QStringLiteral("Opus/48k");
    }
    return QStringLiteral("unknown");
}

} // namespace OpenChat
