#pragma once

#include <QByteArray>

#include <cstdint>

namespace OpenChat {

// The one audio format every voice call speaks on the wire and at both device
// boundaries. Capture is converted INTO it and playback is converted OUT of it,
// so the pipeline in between (codec, crypto, jitter buffer) only ever sees
// frames of exactly this shape and a frame's byte length alone identifies it.
//
// 48 kHz mono is the Opus native rate, so the encoder never resamples; 20 ms is
// the codec's default frame and the granularity the jitter buffer schedules at.
struct CallAudioFormat final {
    static constexpr int sampleRate = 48'000;
    static constexpr int channels = 1;
    static constexpr int bytesPerSample = 2; // signed 16-bit little-endian
    static constexpr int frameDurationMs = 20;

    // Samples in one 20 ms frame (960) and its size in bytes (1920).
    static constexpr int samplesPerFrame = sampleRate / 1000 * frameDurationMs;
    static constexpr int bytesPerFrame = samplesPerFrame * bytesPerSample * channels;

    [[nodiscard]] static constexpr qint64 msForSamples(qint64 samples) noexcept
    {
        return samples * 1000 / sampleRate;
    }
    [[nodiscard]] static constexpr qint64 samplesForMs(qint64 ms) noexcept
    {
        return ms * sampleRate / 1000;
    }
};

// A 20 ms mono S16LE frame: exactly CallAudioFormat::bytesPerFrame bytes. Kept
// as raw bytes because that is what both the audio device and the codec want,
// and because it lets the whole pipeline move frames without converting.
using AudioFrame = QByteArray;

[[nodiscard]] inline bool isFullAudioFrame(const AudioFrame &frame) noexcept
{
    return frame.size() == CallAudioFormat::bytesPerFrame;
}

// A frame of digital silence, used for muted capture and for the concealment
// tail once a loss burst outlasts what the concealer can plausibly invent.
[[nodiscard]] inline AudioFrame silentAudioFrame()
{
    return AudioFrame(CallAudioFormat::bytesPerFrame, '\0');
}

} // namespace OpenChat
