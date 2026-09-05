#pragma once

#include "media/AudioConvert.h"
#include "media/AudioTypes.h"
#include "media/WavFile.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QList>
#include <QRandomGenerator>
#include <QString>
#include <QVector>

#include <cmath>
#include <optional>

// MSVC's <cmath> only defines M_PI when this is set before it is included.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Shared helpers for the voice-call tests: locating a real audio file on the
// machine, synthesising reference signals, and measuring how close two pieces of
// audio are.
namespace OpenChat::AudioTest {

// Where to look for a genuine WAV to push through a call. The point of using a
// real file rather than only synthesised tones is that real audio has the
// properties synthesis does not: a non-trivial spectrum, a DC offset, clipping
// near the peaks, an arbitrary sample rate, and an arbitrary channel count. Any
// of those can break a pipeline that passes a sine wave.
//
// OPENCHAT_TEST_WAV overrides the search so a specific file can be pinned.
[[nodiscard]] inline QStringList systemWavCandidates()
{
    QStringList candidates;
    const QString pinned = qEnvironmentVariable("OPENCHAT_TEST_WAV").trimmed();
    if (!pinned.isEmpty())
        candidates.append(pinned);

    const QStringList roots{QStringLiteral("/usr/share/sounds"),
                            QStringLiteral("/usr/share/sound-theme"),
                            QStringLiteral("/usr/lib/libreoffice/share/gallery/sounds"),
                            QStringLiteral("/System/Library/Sounds"),
                            QStringLiteral("C:/Windows/Media")};
    for (const QString &root : roots) {
        QDir dir(root);
        if (!dir.exists())
            continue;
        // Deterministic order so a failure names the same file on a rerun.
        QDirIterator it(root, {QStringLiteral("*.wav"), QStringLiteral("*.WAV")}, QDir::Files,
                        QDirIterator::Subdirectories);
        QStringList found;
        while (it.hasNext())
            found.append(it.next());
        found.sort();
        candidates.append(found);
    }
    return candidates;
}

// The first candidate that actually decodes, with its path. Returns nullopt when
// the machine has no usable WAV at all, which the caller turns into a skip
// rather than a failure — the synthesised cases still cover the pipeline.
struct LoadedWav final {
    QString path;
    WavAudio audio;
};

[[nodiscard]] inline std::optional<LoadedWav> loadFirstSystemWav()
{
    for (const QString &path : systemWavCandidates()) {
        auto decoded = WavFile::readFile(path);
        if (!decoded.hasValue())
            continue;
        const WavAudio audio = std::move(decoded).value();
        // Something too short to fill a couple of frames tells us nothing about
        // a streaming pipeline.
        if (audio.durationMs() < 100)
            continue;
        return LoadedWav{path, audio};
    }
    return std::nullopt;
}

// A speech-like reference: several harmonically related tones with a slow
// amplitude envelope and pauses. Closer to voice than a pure sine (which every
// codec handles unrealistically well) and fully deterministic.
[[nodiscard]] inline QVector<qint16> syntheticSpeech(int milliseconds)
{
    const qsizetype count = CallAudioFormat::samplesForMs(milliseconds);
    QVector<qint16> samples;
    samples.reserve(count);
    constexpr double fundamental = 140.0; // a low speaking voice
    for (qsizetype i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / CallAudioFormat::sampleRate;
        // Syllable-rate envelope with real gaps, so silence handling is exercised.
        const double syllable = std::max(0.0, std::sin(2.0 * M_PI * 3.2 * t));
        const double envelope = std::pow(syllable, 1.5);
        const double voiced = std::sin(2.0 * M_PI * fundamental * t)
            + 0.5 * std::sin(2.0 * M_PI * fundamental * 2 * t)
            + 0.3 * std::sin(2.0 * M_PI * fundamental * 3 * t)
            + 0.15 * std::sin(2.0 * M_PI * fundamental * 5 * t);
        const double value = 0.28 * envelope * voiced;
        samples.append(static_cast<qint16>(std::clamp(value, -1.0, 1.0) * 32767.0));
    }
    return samples;
}

[[nodiscard]] inline QVector<qint16> tone(int milliseconds, double frequency, double amplitude)
{
    const qsizetype count = CallAudioFormat::samplesForMs(milliseconds);
    QVector<qint16> samples;
    samples.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / CallAudioFormat::sampleRate;
        samples.append(static_cast<qint16>(
            std::clamp(amplitude * std::sin(2.0 * M_PI * frequency * t), -1.0, 1.0) * 32767.0));
    }
    return samples;
}

// Deterministic full-scale noise: the hardest case for any lossy codec and the
// one most likely to expose a clipping or endianness bug.
[[nodiscard]] inline QVector<qint16> noise(int milliseconds, quint32 seed)
{
    QRandomGenerator generator(seed);
    const qsizetype count = CallAudioFormat::samplesForMs(milliseconds);
    QVector<qint16> samples;
    samples.reserve(count);
    for (qsizetype i = 0; i < count; ++i)
        samples.append(static_cast<qint16>(generator.bounded(-32768, 32768)));
    return samples;
}

// Signal-to-noise ratio in dB between a reference and a reproduction of it, at a
// given sample offset. Infinite (returned as 1e9) for an exact match.
[[nodiscard]] inline double snrDb(const QVector<qint16> &reference, const QVector<qint16> &actual,
                                  qsizetype offset = 0)
{
    const qsizetype count =
        std::min(reference.size(), actual.size() - offset);
    if (count <= 0)
        return -1e9;
    double signalEnergy = 0.0;
    double noiseEnergy = 0.0;
    for (qsizetype i = 0; i < count; ++i) {
        const double ref = reference.at(i);
        const double err = ref - actual.at(i + offset);
        signalEnergy += ref * ref;
        noiseEnergy += err * err;
    }
    if (signalEnergy <= 0.0)
        return noiseEnergy <= 0.0 ? 1e9 : -1e9;
    if (noiseEnergy <= 0.0)
        return 1e9;
    return 10.0 * std::log10(signalEnergy / noiseEnergy);
}

// The offset in [0, maxLag] at which `actual` best matches `reference`.
//
// A compressing codec delays its output — Opus by its 6.5 ms lookahead — so
// comparing sample 0 to sample 0 would score a perfect reproduction as noise.
// Searching for the alignment first is what makes the fidelity number mean
// "how faithful", rather than "how delayed".
[[nodiscard]] inline qsizetype bestAlignment(const QVector<qint16> &reference,
                                             const QVector<qint16> &actual, qsizetype maxLag)
{
    qsizetype bestOffset = 0;
    double bestScore = -1e18;
    for (qsizetype offset = 0; offset <= maxLag; ++offset) {
        if (actual.size() - offset < reference.size() / 2)
            break;
        const double score = snrDb(reference, actual, offset);
        if (score > bestScore) {
            bestScore = score;
            bestOffset = offset;
        }
    }
    return bestOffset;
}

// Peak absolute amplitude of a signal, normalised to full scale.
[[nodiscard]] inline double peakLevel(const QVector<qint16> &samples)
{
    double peak = 0.0;
    for (const qint16 sample : samples)
        peak = std::max(peak, std::abs(sample / 32768.0));
    return peak;
}

[[nodiscard]] inline double rmsLevel(const QVector<qint16> &samples)
{
    if (samples.isEmpty())
        return 0.0;
    double sum = 0.0;
    for (const qint16 sample : samples) {
        const double value = sample / 32768.0;
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

} // namespace OpenChat::AudioTest
