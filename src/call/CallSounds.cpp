#include "call/CallSounds.h"

#include "media/ToneSynth.h"

#include <QMutexLocker>
#include <QtEndian>

#include <algorithm>

namespace OpenChat {

namespace {

using ToneSynth::Partial;
using ToneSynth::Segment;

// Interface sounds sit well below full scale: they are layered over speech, and
// a notification that drowns out the person talking is a bad notification.
constexpr double chimeGain = 0.30;
constexpr double ringGain = 0.22;

// The two-tone ringback every telephone network has used for decades: 440 Hz and
// 480 Hz together, ringing then resting. The cadence is shortened from the
// telephone standard's 2 s on / 4 s off, which is a long time to stare at a
// screen; this keeps the sound but returns every two seconds.
[[nodiscard]] QVector<qint16> makeRingback()
{
    const Segment ring{{Partial{440.0, 1.0}, Partial{480.0, 1.0}},
                       /*durationMs=*/900,
                       ringGain,
                       /*attackMs=*/20,
                       /*releaseMs=*/60};
    const Segment rest{{}, /*durationMs=*/1100};
    return ToneSynth::render({ring, rest});
}

// The incoming ring: a rising two-note chime struck twice, then a pause. Higher,
// shorter and rhythmically busier than the ringback, so which direction a call
// is going is obvious from the sound alone.
[[nodiscard]] QVector<qint16> makeIncomingRing()
{
    const auto strike = [](double fundamental) {
        return Segment{{Partial{fundamental, 1.0}, Partial{fundamental * 2.0, 0.35},
                        Partial{fundamental * 3.0, 0.12}},
                       /*durationMs=*/230,
                       chimeGain,
                       /*attackMs=*/5,
                       /*releaseMs=*/70,
                       /*decayHalfLifeMs=*/260.0};
    };
    const Segment gap{{}, /*durationMs=*/70};
    const Segment pause{{}, /*durationMs=*/1500};
    return ToneSynth::render({strike(659.25), gap, strike(880.0), gap,   // E5 -> A5
                              strike(659.25), gap, strike(880.0), pause});
}

// Answered and finished are the same interval in opposite directions, so they
// read as a matched pair: opening and closing.
[[nodiscard]] QVector<qint16> makeConnected()
{
    const auto note = [](double frequency, int durationMs) {
        return Segment{{Partial{frequency, 1.0}, Partial{frequency * 2.0, 0.28}},
                       durationMs,
                       chimeGain,
                       /*attackMs=*/4,
                       /*releaseMs=*/45,
                       /*decayHalfLifeMs=*/220.0};
    };
    return ToneSynth::render({note(587.33, 90), note(880.0, 170)}); // D5 -> A5
}

[[nodiscard]] QVector<qint16> makeEnded()
{
    const auto note = [](double frequency, int durationMs) {
        return Segment{{Partial{frequency, 1.0}, Partial{frequency * 2.0, 0.22}},
                       durationMs,
                       chimeGain,
                       /*attackMs=*/4,
                       /*releaseMs=*/70,
                       /*decayHalfLifeMs=*/240.0};
    };
    return ToneSynth::render({note(880.0, 90), note(587.33, 210)}); // A5 -> D5
}

// Mute and unmute are one short blip each, low and high. They fire while
// somebody may be mid-sentence, so they are the quietest and briefest sounds
// here.
[[nodiscard]] QVector<qint16> makeMuted()
{
    return ToneSynth::renderSegment({{Partial{392.0, 1.0}, Partial{196.0, 0.4}},
                                     /*durationMs=*/120,
                                     0.22,
                                     /*attackMs=*/4,
                                     /*releaseMs=*/60,
                                     /*decayHalfLifeMs=*/140.0});
}

[[nodiscard]] QVector<qint16> makeUnmuted()
{
    return ToneSynth::renderSegment({{Partial{783.99, 1.0}, Partial{1568.0, 0.25}},
                                     /*durationMs=*/120,
                                     0.22,
                                     /*attackMs=*/4,
                                     /*releaseMs=*/60,
                                     /*decayHalfLifeMs=*/140.0});
}

// Rendered once on first use and shared thereafter: a few hundred kilobytes of
// samples that never change.
[[nodiscard]] const QHash<int, QVector<qint16>> &soundBank()
{
    static const QHash<int, QVector<qint16>> bank = [] {
        QHash<int, QVector<qint16>> made;
        made.insert(static_cast<int>(CallSound::Ringback), makeRingback());
        made.insert(static_cast<int>(CallSound::IncomingRing), makeIncomingRing());
        made.insert(static_cast<int>(CallSound::Connected), makeConnected());
        made.insert(static_cast<int>(CallSound::Ended), makeEnded());
        made.insert(static_cast<int>(CallSound::Muted), makeMuted());
        made.insert(static_cast<int>(CallSound::Unmuted), makeUnmuted());
        return made;
    }();
    return bank;
}

[[nodiscard]] qint16 mixSamples(qint16 first, qint16 second)
{
    // Additive with saturation. The interface sounds are quiet enough that a
    // sum with speech stays in range; saturating rather than wrapping means a
    // loud coincidence dulls instead of tearing.
    const int sum = static_cast<int>(first) + static_cast<int>(second);
    return static_cast<qint16>(std::clamp(sum, -32768, 32767));
}

} // namespace

const QVector<qint16> &CallSoundBoard::samplesFor(CallSound sound)
{
    static const QVector<qint16> empty;
    const auto found = soundBank().constFind(static_cast<int>(sound));
    return found == soundBank().cend() ? empty : found.value();
}

QString CallSoundBoard::nameFor(CallSound sound)
{
    switch (sound) {
    case CallSound::Ringback:
        return QStringLiteral("ringback");
    case CallSound::IncomingRing:
        return QStringLiteral("incoming-ring");
    case CallSound::Connected:
        return QStringLiteral("connected");
    case CallSound::Ended:
        return QStringLiteral("ended");
    case CallSound::Muted:
        return QStringLiteral("muted");
    case CallSound::Unmuted:
        return QStringLiteral("unmuted");
    }
    return QStringLiteral("unknown");
}

QList<CallSound> CallSoundBoard::allSounds()
{
    return {CallSound::Ringback, CallSound::IncomingRing, CallSound::Connected,
            CallSound::Ended,    CallSound::Muted,        CallSound::Unmuted};
}

CallSoundBoard::CallSoundBoard()
{
    (void)soundBank(); // render up front, never on the audio thread
}

void CallSoundBoard::playOnce(CallSound sound)
{
    const QMutexLocker locked(&m_mutex);
    // Restart rather than layer: two copies of the same blip a few milliseconds
    // apart is a flam, not an emphasis.
    for (Voice &voice : m_oneShots) {
        if (voice.sound == sound) {
            voice.cursor = 0;
            return;
        }
    }
    m_oneShots.append(Voice{sound, 0, false});
}

void CallSoundBoard::startLoop(CallSound sound)
{
    const QMutexLocker locked(&m_mutex);
    if (m_loop && m_loop->sound == sound)
        return; // already ringing; do not restart mid-cadence
    m_loop = Voice{sound, 0, true};
}

void CallSoundBoard::stopLoop()
{
    const QMutexLocker locked(&m_mutex);
    m_loop.reset();
}

void CallSoundBoard::stopAll()
{
    const QMutexLocker locked(&m_mutex);
    m_loop.reset();
    m_oneShots.clear();
}

void CallSoundBoard::mixInto(AudioFrame &frame)
{
    if (!isFullAudioFrame(frame))
        return;
    const QMutexLocker locked(&m_mutex);
    if (!m_loop && m_oneShots.isEmpty())
        return;

    // Mixes one frame of `voice` into the frame and advances its cursor,
    // wrapping for a loop and returning false once a one-shot has run out.
    const auto mixVoice = [&frame](Voice &voice) {
        const QVector<qint16> &samples = samplesFor(voice.sound);
        if (samples.isEmpty())
            return false;
        auto *out = reinterpret_cast<uchar *>(frame.data());
        for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i) {
            if (voice.cursor >= samples.size()) {
                if (!voice.looping)
                    return false;
                voice.cursor = 0;
            }
            const qint16 mixed =
                mixSamples(qFromLittleEndian<qint16>(out), samples.at(voice.cursor));
            qToLittleEndian(mixed, out);
            out += 2;
            ++voice.cursor;
        }
        return true;
    };

    if (m_loop)
        (void)mixVoice(*m_loop);
    for (auto it = m_oneShots.begin(); it != m_oneShots.end();)
        it = mixVoice(*it) ? std::next(it) : m_oneShots.erase(it);
}

bool CallSoundBoard::isIdle() const
{
    const QMutexLocker locked(&m_mutex);
    return !m_loop && m_oneShots.isEmpty();
}

std::optional<CallSound> CallSoundBoard::loopingSound() const
{
    const QMutexLocker locked(&m_mutex);
    return m_loop ? std::optional<CallSound>(m_loop->sound) : std::nullopt;
}

int CallSoundBoard::remainingOneShotMs() const
{
    const QMutexLocker locked(&m_mutex);
    qsizetype longest = 0;
    for (const Voice &voice : m_oneShots)
        longest = std::max(longest, samplesFor(voice.sound).size() - voice.cursor);
    return static_cast<int>(CallAudioFormat::msForSamples(longest));
}

} // namespace OpenChat
