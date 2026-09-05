#pragma once

#include "media/AudioTypes.h"

#include <QByteArray>
#include <QString>

#include <memory>

namespace OpenChat {

// The codecs a call may negotiate. The value is what travels in the call offer,
// so it is a stable wire enum: append, never renumber.
enum class AudioCodecKind : quint8 {
    // Uncompressed 48 kHz mono S16LE. Lossless end to end, and the only codec
    // whose output can be compared to its input byte for byte — which is why it
    // is what the pipeline's fidelity tests run through. Never chosen for a real
    // call unless asked for by name: at 768 kbit/s it would swamp a domestic
    // uplink, and a build that reached it by accident would look like it
    // worked while flooding the relay.
    Pcm = 0,
    // Opus at 48 kHz mono. Perceptually transparent for speech at a fraction of
    // the bandwidth. The build guarantees it (see cmake/dependencies/Opus.cmake),
    // so every client offers it and every client can accept it.
    Opus = 1,
};

// Frame-in / frame-out codec over the single CallAudioFormat frame shape. Both
// directions are stateful in the general case (Opus carries inter-frame state),
// so a call holds one encoder and one decoder and never shares them.
class AudioCodec
{
public:
    virtual ~AudioCodec() = default;

    [[nodiscard]] virtual AudioCodecKind kind() const noexcept = 0;

    // Compresses exactly one CallAudioFormat frame. Returns an empty payload if
    // the frame is the wrong length or the encoder fails; the caller treats that
    // as "nothing to send" rather than as a call failure.
    [[nodiscard]] virtual QByteArray encode(const AudioFrame &frame) = 0;

    // Expands one payload back to a full frame. An empty or rejected payload
    // yields an empty frame, which the caller replaces with concealment.
    [[nodiscard]] virtual AudioFrame decode(QByteArrayView payload) = 0;

    // Produces the frame that best masks one lost packet, advancing whatever
    // internal state the codec keeps. The base implementation returns silence.
    [[nodiscard]] virtual AudioFrame concealLoss() { return silentAudioFrame(); }
};

// True when `kind` is a codec this build knows how to run. Both listed kinds
// always are; what this guards against is a value off the wire that a newer
// peer's enum has and ours does not.
[[nodiscard]] bool isAudioCodecAvailable(AudioCodecKind kind) noexcept;

// Builds a codec, or nullptr for a kind this build does not know or a codec
// whose library state could not be allocated. The same call makes both an
// encoder and a decoder; they must not be shared.
[[nodiscard]] std::unique_ptr<AudioCodec> makeAudioCodec(AudioCodecKind kind);

// The codec a new call offers. Always Opus: PCM exists for the tests and the
// diagnostics tool, never as something a call arrives at by default.
[[nodiscard]] AudioCodecKind preferredAudioCodec() noexcept;

[[nodiscard]] QString audioCodecName(AudioCodecKind kind);

} // namespace OpenChat
