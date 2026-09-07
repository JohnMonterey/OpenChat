#include "call/CallSounds.h"

#include "media/WavFile.h"

#include <QMutexLocker>
#include <QString>
#include <QtEndian>

#include <algorithm>

namespace OpenChat {

namespace {

// Every call sound ships as a mono 48 kHz WAV under assets/sounds, embedded
// into the binary as a Qt resource so there is no file to go missing at
// runtime. The asset is the single source of truth: nothing here re-derives
// or re-mixes the audio, so what plays is exactly what is on disk in the
// repository and exactly what tools/render_call_sounds.cpp exports.
[[nodiscard]] QVector<qint16> loadChime(const QString &name)
{
    auto decoded = WavFile::readFile(QStringLiteral(":/openchat/sounds/") + name
                                     + QStringLiteral(".wav"));
    Q_ASSERT_X(decoded.hasValue(), "loadChime", qPrintable(name));
    if (!decoded.hasValue())
        return {};
    const WavAudio &audio = decoded.value();
    Q_ASSERT_X(audio.sampleRate == CallAudioFormat::sampleRate, "loadChime",
              "asset is not 48 kHz");
    Q_ASSERT_X(audio.channels == CallAudioFormat::channels, "loadChime",
              "asset is not mono");
    return audio.samples;
}

// Loaded once on first use and shared thereafter: a few hundred kilobytes of
// samples that never change.
[[nodiscard]] const QHash<int, QVector<qint16>> &soundBank()
{
    static const QHash<int, QVector<qint16>> bank = [] {
        QHash<int, QVector<qint16>> made;
        made.insert(static_cast<int>(CallSound::Ringback), loadChime(QStringLiteral("ringback")));
        made.insert(static_cast<int>(CallSound::IncomingRing),
                    loadChime(QStringLiteral("incoming-ring")));
        made.insert(static_cast<int>(CallSound::Connected),
                    loadChime(QStringLiteral("connected")));
        made.insert(static_cast<int>(CallSound::Ended), loadChime(QStringLiteral("ended")));
        made.insert(static_cast<int>(CallSound::Muted), loadChime(QStringLiteral("muted")));
        made.insert(static_cast<int>(CallSound::Unmuted), loadChime(QStringLiteral("unmuted")));
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
    (void)soundBank(); // load up front, never on the audio thread
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
