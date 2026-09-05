#include "media/ToneSynth.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace OpenChat::ToneSynth {

namespace {

// A raised-cosine ramp from 0 to 1 over `length` samples. Smoother at both ends
// than a straight line, which matters at the very short attack times used here:
// a linear ramp still has a corner, and a corner is audible as a tick.
[[nodiscard]] double raisedCosine(qsizetype position, qsizetype length)
{
    if (length <= 0)
        return 1.0;
    const double phase = static_cast<double>(position) / static_cast<double>(length);
    return 0.5 - 0.5 * std::cos(M_PI * std::clamp(phase, 0.0, 1.0));
}

[[nodiscard]] qint16 toS16(double value)
{
    const double scaled = std::round(std::clamp(value, -1.0, 1.0) * 32767.0);
    return static_cast<qint16>(scaled);
}

} // namespace

QVector<qint16> render(const QList<Segment> &segments)
{
    QVector<qint16> out;
    qsizetype total = 0;
    for (const Segment &segment : segments)
        total += CallAudioFormat::samplesForMs(std::max(0, segment.durationMs));
    out.reserve(total);

    for (const Segment &segment : segments) {
        const qsizetype length = CallAudioFormat::samplesForMs(std::max(0, segment.durationMs));
        if (length <= 0)
            continue;
        if (segment.partials.isEmpty()) {
            out.resize(out.size() + length); // silence
            continue;
        }

        // Clamp the ramps so a segment shorter than its own envelope still
        // starts and ends at silence rather than jumping mid-ramp.
        const qsizetype attack =
            std::min(CallAudioFormat::samplesForMs(std::max(0, segment.attackMs)), length / 2);
        const qsizetype release =
            std::min(CallAudioFormat::samplesForMs(std::max(0, segment.releaseMs)), length / 2);

        // Normalise by the summed partial amplitudes so adding a harmonic makes
        // the tone richer rather than louder (and cannot push it into clipping).
        double amplitudeSum = 0.0;
        for (const Partial &partial : segment.partials)
            amplitudeSum += std::abs(partial.amplitude);
        const double normalise = amplitudeSum > 0.0 ? 1.0 / amplitudeSum : 0.0;

        const double decayRate = segment.decayHalfLifeMs > 0.0
            ? std::log(2.0) / (segment.decayHalfLifeMs / 1000.0)
            : 0.0;

        for (qsizetype i = 0; i < length; ++i) {
            const double seconds = static_cast<double>(i) / CallAudioFormat::sampleRate;
            double sample = 0.0;
            for (const Partial &partial : segment.partials)
                sample += partial.amplitude * std::sin(2.0 * M_PI * partial.frequency * seconds);
            sample *= normalise * segment.gain;

            if (decayRate > 0.0)
                sample *= std::exp(-decayRate * seconds);
            if (i < attack)
                sample *= raisedCosine(i, attack);
            if (const qsizetype fromEnd = length - 1 - i; fromEnd < release)
                sample *= raisedCosine(fromEnd, release);

            out.append(toS16(sample));
        }
    }
    return out;
}

QVector<qint16> renderSegment(const Segment &segment)
{
    return render({segment});
}

} // namespace OpenChat::ToneSynth
