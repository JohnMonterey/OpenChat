#include "media/AudioConvert.h"

#include <QtEndian>

#include <cmath>
#include <limits>

namespace OpenChat::AudioConvert {

namespace {

[[nodiscard]] qint16 clampToS16(double value)
{
    if (!std::isfinite(value))
        return 0;
    const double rounded = std::round(value);
    if (rounded >= 32767.0)
        return 32767;
    if (rounded <= -32768.0)
        return -32768;
    return static_cast<qint16>(rounded);
}

} // namespace

QVector<qint16> downmixToMono(const QVector<qint16> &interleaved, int channels)
{
    if (channels <= 1)
        return interleaved;
    const qsizetype frames = interleaved.size() / channels;
    QVector<qint16> mono;
    mono.reserve(frames);
    for (qsizetype frame = 0; frame < frames; ++frame) {
        // Sum in a wide type: 64 channels at full scale would overflow qint16
        // and even qint32 headroom is worth not relying on here.
        qint64 sum = 0;
        for (int channel = 0; channel < channels; ++channel)
            sum += interleaved.at(frame * channels + channel);
        mono.append(clampToS16(static_cast<double>(sum) / channels));
    }
    return mono;
}

QVector<qint16> resampleMono(const QVector<qint16> &samples, int fromRate, int toRate)
{
    if (fromRate <= 0 || toRate <= 0 || samples.isEmpty())
        return {};
    if (fromRate == toRate)
        return samples;

    // Output length is chosen so the last output sample lands at or before the
    // last input sample; interpolation therefore never reads past the end.
    const double ratio = static_cast<double>(fromRate) / static_cast<double>(toRate);
    const qsizetype outCount =
        static_cast<qsizetype>(std::llround(static_cast<double>(samples.size()) / ratio));
    if (outCount <= 0)
        return {};

    QVector<qint16> out;
    out.reserve(outCount);
    for (qsizetype i = 0; i < outCount; ++i) {
        const double position = static_cast<double>(i) * ratio;
        const auto index = static_cast<qsizetype>(position);
        if (index + 1 >= samples.size()) {
            out.append(samples.last());
            continue;
        }
        const double fraction = position - static_cast<double>(index);
        const double interpolated = samples.at(index) * (1.0 - fraction)
                                    + samples.at(index + 1) * fraction;
        out.append(clampToS16(interpolated));
    }
    return out;
}

QVector<qint16> toCallFormat(const WavAudio &audio)
{
    if (audio.channels <= 0 || audio.sampleRate <= 0)
        return {};
    // Downmix first, then resample: mixing at the source rate keeps the cheaper
    // interpolation pass working on a single channel.
    return resampleMono(downmixToMono(audio.samples, audio.channels), audio.sampleRate,
                        CallAudioFormat::sampleRate);
}

QList<AudioFrame> toFrames(const QVector<qint16> &samples, bool padTail)
{
    QList<AudioFrame> frames;
    if (samples.isEmpty())
        return frames;
    constexpr qsizetype perFrame = CallAudioFormat::samplesPerFrame;
    const qsizetype whole = samples.size() / perFrame;
    frames.reserve(whole + 1);
    for (qsizetype i = 0; i < whole; ++i) {
        AudioFrame frame(CallAudioFormat::bytesPerFrame, Qt::Uninitialized);
        auto *cursor = reinterpret_cast<uchar *>(frame.data());
        for (qsizetype s = 0; s < perFrame; ++s) {
            qToLittleEndian(samples.at(i * perFrame + s), cursor);
            cursor += 2;
        }
        frames.append(std::move(frame));
    }
    if (const qsizetype remainder = samples.size() % perFrame; remainder != 0 && padTail) {
        AudioFrame frame = silentAudioFrame();
        auto *cursor = reinterpret_cast<uchar *>(frame.data());
        for (qsizetype s = 0; s < remainder; ++s) {
            qToLittleEndian(samples.at(whole * perFrame + s), cursor);
            cursor += 2;
        }
        frames.append(std::move(frame));
    }
    return frames;
}

QVector<qint16> fromFrames(const QList<AudioFrame> &frames)
{
    QVector<qint16> out;
    out.reserve(frames.size() * CallAudioFormat::samplesPerFrame);
    for (const AudioFrame &frame : frames) {
        const qsizetype count = frame.size() / 2;
        const auto *cursor = reinterpret_cast<const uchar *>(frame.constData());
        for (qsizetype s = 0; s < count; ++s) {
            out.append(qFromLittleEndian<qint16>(cursor));
            cursor += 2;
        }
    }
    return out;
}

QVector<qint16> samplesOf(const AudioFrame &frame)
{
    const qsizetype count = frame.size() / 2;
    QVector<qint16> out;
    out.reserve(count);
    const auto *cursor = reinterpret_cast<const uchar *>(frame.constData());
    for (qsizetype s = 0; s < count; ++s) {
        out.append(qFromLittleEndian<qint16>(cursor));
        cursor += 2;
    }
    return out;
}

AudioFrame frameOf(const QVector<qint16> &samples)
{
    AudioFrame frame(samples.size() * 2, Qt::Uninitialized);
    auto *cursor = reinterpret_cast<uchar *>(frame.data());
    for (const qint16 sample : samples) {
        qToLittleEndian(sample, cursor);
        cursor += 2;
    }
    return frame;
}

double frameRms(const AudioFrame &frame)
{
    const qsizetype count = frame.size() / 2;
    if (count == 0)
        return 0.0;
    const auto *cursor = reinterpret_cast<const uchar *>(frame.constData());
    double sumOfSquares = 0.0;
    for (qsizetype s = 0; s < count; ++s) {
        const double value = qFromLittleEndian<qint16>(cursor) / 32768.0;
        sumOfSquares += value * value;
        cursor += 2;
    }
    return std::sqrt(sumOfSquares / static_cast<double>(count));
}

double framePeak(const AudioFrame &frame)
{
    const qsizetype count = frame.size() / 2;
    const auto *cursor = reinterpret_cast<const uchar *>(frame.constData());
    double peak = 0.0;
    for (qsizetype s = 0; s < count; ++s) {
        peak = std::max(peak, std::abs(qFromLittleEndian<qint16>(cursor) / 32768.0));
        cursor += 2;
    }
    return peak;
}

} // namespace OpenChat::AudioConvert
