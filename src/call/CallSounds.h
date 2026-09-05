#pragma once

#include "media/AudioTypes.h"

#include <QHash>
#include <QMutex>
#include <QVector>

#include <optional>

namespace OpenChat {

// The sounds a call makes. Two of them loop while the call waits for a human;
// the rest are one-shots that mark a moment.
enum class CallSound {
    // Outgoing: the classic two-tone ringback, on a slow ring/pause cycle. It
    // means "their phone is ringing" and it loops until they answer or we give
    // up, so the caller is never left listening to nothing wondering whether
    // anything is happening.
    Ringback,
    // Incoming: an insistent chime pattern, loops until answered or missed.
    // Deliberately different in both pitch and rhythm from the ringback, so the
    // two are never confused across a room.
    IncomingRing,
    // Answered: a short rising pair. The moment the line opens.
    Connected,
    // Finished: the same interval falling. The moment it closes.
    Ended,
    // The microphone going off and coming back. Low for muted, high for live,
    // so the direction is audible without looking.
    Muted,
    Unmuted,
};

// Generates the call sounds once and mixes whichever are playing into the
// outgoing audio stream.
//
// Mixing into the call's own playback rather than opening a second output is
// what keeps a call to one audio stream: no second device to fail, no fighting
// over the speaker, and the ring, the pick-up chirp and the far end's voice all
// arrive on the same clock. It also means the whole thing is testable by pulling
// frames and looking at them.
//
// Thread-safe: the engine starts and stops sounds on its own thread while the
// audio device pulls mixed frames on another.
class CallSoundBoard final
{
public:
    CallSoundBoard();

    // Plays `sound` once from the beginning. Repeating a one-shot restarts it
    // rather than layering it over itself.
    void playOnce(CallSound sound);

    // Starts (or replaces) the looping sound. Looping is exclusive: a call is
    // either ringing out or ringing in, never both.
    void startLoop(CallSound sound);
    void stopLoop();

    // Stops everything immediately, loop and one-shots alike.
    void stopAll();

    // Mixes every active sound into `frame` in place and advances all of them by
    // one frame. A frame of the wrong length is left untouched.
    void mixInto(AudioFrame &frame);

    [[nodiscard]] bool isIdle() const;
    [[nodiscard]] std::optional<CallSound> loopingSound() const;

    // How long the active one-shots still have to run. The engine uses this to
    // hold the speaker open just long enough for the hang-up sound to finish.
    [[nodiscard]] int remainingOneShotMs() const;

    // The rendered samples for one sound. Exposed so the sounds can be
    // inspected, measured and written out for review without a device.
    [[nodiscard]] static const QVector<qint16> &samplesFor(CallSound sound);
    [[nodiscard]] static QString nameFor(CallSound sound);
    [[nodiscard]] static QList<CallSound> allSounds();

private:
    struct Voice final {
        CallSound sound = CallSound::Connected;
        qsizetype cursor = 0;
        bool looping = false;
    };

    mutable QMutex m_mutex;
    std::optional<Voice> m_loop;
    QList<Voice> m_oneShots;
};

} // namespace OpenChat
