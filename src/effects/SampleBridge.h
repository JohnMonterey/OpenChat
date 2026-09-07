#pragma once

#include "media/AudioTypes.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OpenChat::SampleBridge {

// The only conversion in the hot path: the call's 16-bit integer frames to and
// from the 32-bit float every plugin ABI speaks.
//
// Both CLAP and VST3 are float and PLANAR -- one buffer per channel, never
// interleaved. The pipeline is mono, so planar and interleaved coincide and
// there is no deinterleave step at all: one float[960] is the whole bridge.
//
// The scale is 32768 in BOTH directions. libsndfile's well-known convention
// divides by 32768 and multiplies back by 32767, and swept across all 65 536
// int16 values that loses one LSB on every single sample. Symmetrical 32768 is
// exactly lossless on the round trip. That matters here specifically, and not
// as a purity argument: MicrophoneProcessor documents that a gain of 1.0
// "leaves the signal untouched (and the bytes bit-identical)" and tst_voicecall
// asserts it, so an empty or bypassed chain has to keep that promise.

inline void frameToFloat(const AudioFrame &in, float *out) noexcept
{
    // constData(), never data(): the non-const overload DETACHES when the
    // QByteArray is shared, which is a 1920-byte allocation and a memcpy at
    // precisely the point we were trying not to have one.
    const char *src = in.constData();
    for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i) {
        qint16 sample;
        std::memcpy(&sample, src + i * CallAudioFormat::bytesPerSample, sizeof sample);
        out[i] = static_cast<float>(qFromLittleEndian(sample)) * (1.0f / 32768.0f);
    }
}

inline void floatToFrame(const float *in, char *out) noexcept
{
    for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i) {
        float value = in[i] * 32768.0f;
        // NaN first, by name, and to silence rather than to the clamp floor.
        // Every comparison against NaN is false, so a comparison-based clamp --
        // and std::clamp is one -- maps NaN to the LOW bound: a full-scale
        // negative click on every affected sample. This is not hypothetical.
        // ZamEQ2, unmodified and installed from a distribution package, writes
        // NaN into its output with factory defaults on its first 960-sample
        // block.
        if (std::isnan(value))
            value = 0.0f;
        // Also catches the infinities. Needed for ordinary use and not only for
        // pathology: a saturator or tube emulation at high drive pushes peaks
        // well past 1.0, and that is the plugin working correctly.
        value = std::clamp(value, -32768.0f, 32767.0f);
        const qint16 sample = qToLittleEndian(static_cast<qint16>(std::lrintf(value)));
        std::memcpy(out + i * CallAudioFormat::bytesPerSample, &sample, sizeof sample);
    }
}

// True when every sample of one frame is finite.
//
// One pass over 960 floats, around a microsecond. Worth spending because a
// chain that has gone NaN stays NaN: clamping it silently forever would hide a
// dead plugin behind plausible-sounding silence, and the user would hear
// nothing wrong while the far end heard nothing at all.
[[nodiscard]] inline bool isFinite(const float *buffer) noexcept
{
    for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i) {
        if (!std::isfinite(buffer[i]))
            return false;
    }
    return true;
}

} // namespace OpenChat::SampleBridge
