#include "call/ScreenAudioCapture.h"

#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace OpenChat {

namespace {

constexpr qsizetype samplesPerFrame =
    qsizetype(ScreenAudioFormat::samplesPerFrame) * ScreenAudioFormat::channels;
// A pump that fell this far behind (a suspended laptop, a stalled thread)
// starts again from now rather than emitting the whole backlog at once.
constexpr qint64 maxCatchUpMs = 200;

[[nodiscard]] qint16 toSample(float value)
{
    const float scaled = std::clamp(value, -1.0F, 1.0F) * 32767.0F;
    return qint16(std::lround(scaled));
}

} // namespace

// --- Framer ----------------------------------------------------------------------

void ScreenAudioFramer::push(int source, const qint16 *interleaved, qsizetype frames)
{
    if (interleaved == nullptr || frames <= 0)
        return;
    const QMutexLocker locked(&m_mutex);
    Source &entry = m_sources[source];
    entry.samples.insert(entry.samples.end(), interleaved,
                         interleaved + frames * ScreenAudioFormat::channels);
    // Ahead of the clock by more than the ceiling: drop the oldest back to
    // the cushion. One audible skip every few minutes beats delay that grows
    // for as long as the share lasts.
    const qsizetype ceiling = samplesPerFrame * ceilingFrames;
    if (qsizetype(entry.samples.size()) > ceiling) {
        const qsizetype keep = samplesPerFrame * cushionFrames;
        const qsizetype drop = qsizetype(entry.samples.size()) - keep;
        entry.samples.erase(entry.samples.begin(), entry.samples.begin() + drop);
        m_trimmed += quint64(drop / samplesPerFrame);
    }
}

void ScreenAudioFramer::pushSilence(int source, qsizetype frames)
{
    if (frames <= 0)
        return;
    const std::vector<qint16> zeros(size_t(frames) * ScreenAudioFormat::channels, 0);
    push(source, zeros.data(), frames);
}

void ScreenAudioFramer::removeSource(int source)
{
    const QMutexLocker locked(&m_mutex);
    m_sources.erase(source);
}

void ScreenAudioFramer::pump(qint64 nowMs, const std::function<void(const StereoFrame &)> &deliver)
{
    std::vector<StereoFrame> ready;
    {
        const QMutexLocker locked(&m_mutex);
        if (m_nextDueMs < 0 || nowMs - m_nextDueMs > maxCatchUpMs)
            m_nextDueMs = nowMs;
        while (nowMs >= m_nextDueMs) {
            StereoFrame frame = silentStereoFrame();
            auto *out = reinterpret_cast<qint16 *>(frame.data());
            for (auto &[id, source] : m_sources) {
                const qsizetype held = qsizetype(source.samples.size());
                if (!source.primed) {
                    // Wait for the cushion before contributing, so the pieces
                    // that follow have room to arrive late.
                    if (held < samplesPerFrame * cushionFrames)
                        continue;
                    source.primed = true;
                }
                const qsizetype take = std::min(held, samplesPerFrame);
                for (qsizetype i = 0; i < take; ++i) {
                    const int sum = int(out[i]) + int(source.samples[size_t(i)]);
                    out[i] = qint16(std::clamp(sum, int(std::numeric_limits<qint16>::min()),
                                               int(std::numeric_limits<qint16>::max())));
                }
                source.samples.erase(source.samples.begin(), source.samples.begin() + take);
                // Ran dry: nothing is playing, or the source is late. Either
                // way it waits for a fresh cushion before contributing again.
                if (take < samplesPerFrame)
                    source.primed = false;
            }
            ready.push_back(std::move(frame));
            m_nextDueMs += ScreenAudioFormat::frameDurationMs;
            ++m_emitted;
        }
    }
    // Outside the lock: the receiver may take its time.
    for (const StereoFrame &frame : ready)
        deliver(frame);
}

quint64 ScreenAudioFramer::trimmedFrames() const
{
    const QMutexLocker locked(&m_mutex);
    return m_trimmed;
}

quint64 ScreenAudioFramer::emittedFrames() const
{
    const QMutexLocker locked(&m_mutex);
    return m_emitted;
}

// --- Converter -------------------------------------------------------------------

StereoConverter::StereoConverter(int sampleRate, int channels, Sample sample)
    : m_rate(sampleRate > 0 ? sampleRate : 0)
    , m_channels(channels > 0 ? channels : 0)
    , m_sample(sample)
{
}

void StereoConverter::convert(const void *data, qsizetype frames, std::vector<qint16> &out)
{
    if (!isValid() || data == nullptr || frames <= 0)
        return;
    const auto channel = [&](qsizetype frame, int index) -> float {
        const int source = std::min(index, m_channels - 1); // mono spreads to both
        const qsizetype at = frame * m_channels + source;
        if (m_sample == Sample::Float32)
            return static_cast<const float *>(data)[at];
        return float(static_cast<const qint16 *>(data)[at]) / 32768.0F;
    };

    if (m_rate == ScreenAudioFormat::sampleRate) {
        out.reserve(out.size() + size_t(frames) * 2);
        for (qsizetype frame = 0; frame < frames; ++frame) {
            out.push_back(toSample(channel(frame, 0)));
            out.push_back(toSample(channel(frame, 1)));
        }
        return;
    }

    // Linear interpolation between input frames. `m_position` is where the
    // next output frame falls, in input frames, measured from the frame
    // before this block (the carried `m_last`) — position 0 is that frame.
    const double step = double(m_rate) / double(ScreenAudioFormat::sampleRate);
    const auto input = [&](qsizetype index, int side) -> float {
        // Index -1 is the carried frame.
        if (index < 0)
            return m_last[side];
        return channel(index, side);
    };
    if (!m_haveLast) {
        m_last[0] = channel(0, 0);
        m_last[1] = channel(0, 1);
        m_haveLast = true;
    }
    // Input indices here run from -1 (carried) to frames - 1.
    while (m_position <= double(frames)) {
        const double at = m_position - 1.0; // relative to this block's first frame
        const auto base = qsizetype(std::floor(at));
        const double fraction = at - double(base);
        if (base + 1 > frames - 1)
            break;
        for (int side = 0; side < 2; ++side) {
            const float a = input(base, side);
            const float b = input(base + 1, side);
            out.push_back(toSample(float(a + (b - a) * fraction)));
        }
        m_position += step;
    }
    m_last[0] = channel(frames - 1, 0);
    m_last[1] = channel(frames - 1, 1);
    m_position -= double(frames);
}

} // namespace OpenChat
