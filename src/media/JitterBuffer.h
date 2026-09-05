#pragma once

#include "media/AudioTypes.h"

#include <QByteArray>
#include <QVector>

#include <cstdint>
#include <functional>

namespace OpenChat {

// Statistics a call surfaces for diagnostics and that the tests assert against.
struct JitterBufferStats final {
    quint64 accepted = 0;   // frames queued for playout
    quint64 duplicates = 0; // same sequence delivered more than once
    quint64 late = 0;       // arrived after its playout slot had passed
    quint64 overflows = 0;  // arrived too far ahead to hold
    quint64 resets = 0;     // stream discontinuity forced a re-prime
    quint64 played = 0;     // frames handed to playback
    quint64 lost = 0;       // slots played as concealment because nothing arrived
    quint64 starved = 0;    // playout asked for a frame with the buffer empty
    quint64 inserted = 0;   // concealment slots added to deepen the queue
    quint64 dropped = 0;    // slots removed to shallow the queue
    int peakDepth = 0;      // deepest the queue ever got
    int targetDepth = 0;    // the depth the controller is currently steering to
    qint64 jitterMs = 0;    // spread of arrival timing over the recent window
};

// A fixed-cadence reordering buffer between a lossy network and a playback
// device that must be fed one 20 ms frame every 20 ms, no matter what.
//
// It absorbs the four things a real network does to a media stream — reorder,
// duplicate, drop, and arrive in bursts — and converts them into a single
// in-order frame sequence with explicit gaps the caller conceals. Sequence
// numbers are compared as wrapped 32-bit differences, so a call long enough to
// wrap the counter behaves exactly like one that has just started.
//
// The buffer primes before it plays: until the target number of frames is
// queued, pop() reports Starved. That initial latency is what buys the
// tolerance to reordering; without it the first out-of-order frame would be a
// loss.
//
// Given a clock, the buffer also steers its own depth. Two slow processes push a
// real call off a fixed depth. Network jitter over a WAN with a relay hop
// routinely exceeds the 80 ms a four-frame cushion covers, so a fixed buffer
// starves and re-primes over and over, which is audibly choppy; the target
// therefore follows the measured spread of arrival timing, growing while
// arrivals are erratic and shrinking once they settle. And two sound cards
// nominally at 48 kHz differ by tens of ppm, so over a long call the queue
// slowly fills or drains until it overflows or starves; the long-run average
// depth is therefore held against the target by removing or inserting one
// frame whenever it stays biased. Both are expressed as one rule — correct
// when the average depth sits away from the target — and both corrections are
// timed for a quiet stretch so they are not heard.
class JitterBuffer final
{
public:
    struct Config final {
        // Where the playout target starts, and the least it is ever allowed to
        // shrink to. Four frames is 80 ms: enough to reorder across on a clean
        // link, short enough to stay conversational.
        int targetDepth = 4;
        // The most the controller may deepen the target in pursuit of jitter.
        // 25 frames is 500 ms — beyond this a conversation stops feeling live,
        // so past it the link is better served by concealment than by delay.
        int maxTargetDepth = 25;
        // Hard ceiling on queued frames. A sender that races ahead (or a relay
        // that releases a burst) is bounded here rather than growing without
        // limit; 100 frames is 2 s of audio.
        int maxDepth = 100;
        // A sequence this far from what is expected is not reordering, it is a
        // different stream: the peer restarted its counter, or a stale call's
        // packets are arriving. Re-prime rather than stall for the gap.
        int resetDistance = 500;

        // Milliseconds on any steady clock, read at each arrival. Injected
        // rather than taken from the system so a test can script arrival timing
        // exactly. Leaving it unset switches the whole depth controller off and
        // the buffer holds `targetDepth` fixed — which is what the exact-audio
        // tests rely on, since a buffer that reshapes time cannot also promise
        // a constant playout offset.
        std::function<qint64()> clock;

        // Arrivals the jitter estimate remembers. 100 packets is 2 s: long
        // enough that a burst every second keeps the cushion it needs, short
        // enough that a one-off spike is forgotten within a phrase.
        int jitterWindow = 100;
        // How far (in frames) the average depth may sit from the target before
        // a correction is owed. Under 1 so that a permanent whole-frame offset
        // is corrected rather than tolerated forever; well above the residual
        // noise of the average, so an ordinary link never triggers it.
        double correctionBand = 0.75;
        // Fewest playout intervals between two corrections made for the sake of
        // quiet. 5 frames is 100 ms: the speaking detector needs a frame to
        // notice speech has resumed, and pacing the drops stops a long pause
        // from being eaten wholesale.
        int correctionSpacing = 5;
        // How long (in playout intervals) a correction waits for a quiet moment
        // before it is made regardless. 100 frames is 2 s: music has no pauses,
        // and a buffer left drifting for longer than this ends up starving,
        // which is far worse than one glitch.
        int correctionPatience = 100;
    };

    enum class PushResult {
        Accepted,
        Duplicate,
        Late,
        Overflow,
        Reset, // accepted, but the stream restarted and the queue was flushed
    };

    enum class PopKind {
        Frame,    // payload holds the next packet in order
        Lost,     // its slot arrived at playout time with nothing in it
        Starved,  // the buffer is empty (or still priming); play silence
        Inserted, // one extra slot of concealment to deepen the queue; the
                  // cursor did not move, and the same sequence is due next
    };

    struct PopResult final {
        PopKind kind = PopKind::Starved;
        QByteArray payload;
        quint32 sequence = 0;
        // A frame removed to shallow the queue on this pop, when there was one.
        // Never rendered, but a stateful decoder should still be shown it so
        // the frame that follows decodes against the right history.
        QByteArray skipped;
    };

    // Two constructors rather than a defaulted argument: a nested aggregate's
    // member initialisers are not usable in the enclosing class's default
    // arguments, so `JitterBuffer()` spells the default explicitly.
    JitterBuffer()
        : JitterBuffer(Config{})
    {
    }
    explicit JitterBuffer(Config config);

    [[nodiscard]] PushResult push(quint32 sequence, const QByteArray &payload);

    // Takes the next playout slot. Called exactly once per frame interval; the
    // caller turns Lost/Starved/Inserted into concealment or silence.
    //
    // `quiet` says whether playback is currently in a low-energy stretch. Depth
    // corrections are made at the first quiet opportunity, or without one only
    // when the alternative is starving or the wait has gone on too long. A
    // caller with no level information leaves it true and gets corrections as
    // soon as they are owed.
    [[nodiscard]] PopResult pop(bool quiet = true);

    [[nodiscard]] int depth() const noexcept { return m_count; }
    [[nodiscard]] int targetDepth() const noexcept { return m_target; }
    [[nodiscard]] bool isPriming() const noexcept { return m_priming; }
    [[nodiscard]] bool isAdaptive() const noexcept { return static_cast<bool>(m_config.clock); }
    [[nodiscard]] const JitterBufferStats &stats() const noexcept { return m_stats; }

    void reset();

private:
    struct Slot final {
        quint32 sequence = 0;
        QByteArray payload;
        bool filled = false;
    };

    // Wrapped comparison: how far `sequence` is ahead of `reference`, negative
    // when behind. Correct across the 32-bit rollover, which plain subtraction
    // of unsigned values is not.
    [[nodiscard]] static qint32 distance(quint32 sequence, quint32 reference) noexcept
    {
        return static_cast<qint32>(sequence - reference);
    }

    [[nodiscard]] Slot &slotFor(quint32 sequence) noexcept
    {
        return m_slots[sequence % static_cast<quint32>(m_slots.size())];
    }
    [[nodiscard]] bool holds(quint32 sequence) noexcept
    {
        const Slot &slot = slotFor(sequence);
        return slot.filled && slot.sequence == sequence;
    }

    // Playout slots between the cursor and the newest frame held, holes
    // included. This — not the count of filled slots — is the latency the
    // newest audio is waiting through, which is what the controller steers.
    [[nodiscard]] int span() const noexcept;

    void primeFrom(quint32 sequence);
    void observeArrival(quint32 sequence);
    void setTarget(int target);

    Config m_config;
    QVector<Slot> m_slots;
    int m_count = 0;
    quint32 m_nextSequence = 0;
    // The furthest-ahead sequence currently held. While priming it decides
    // whether the playout start can still be moved back to an earlier packet
    // without pushing what is already queued out of the ring; once playing it
    // is the far end of the span the controller measures.
    quint32 m_highestStored = 0;
    bool m_started = false; // a first packet has been seen, so m_nextSequence is real
    bool m_priming = true;  // filling to the target before playout begins
    // True once the playout cursor has advanced for this stream. From that point
    // the cursor is authoritative and must never move backwards, even while
    // re-priming after an underrun — audio that has already been rendered cannot
    // be un-rendered, and rewinding for a late straggler would replay its slots
    // as concealed gaps.
    bool m_playoutStarted = false;

    // --- the depth controller; all of it idle without a clock ---
    int m_target = 0;
    // The sequence arrival timing is measured against. Transit offsets are
    // "arrival time minus where this sequence sits in the sender's 20 ms grid",
    // and the grid needs an origin; the first packet of a stream is it.
    quint32 m_streamBase = 0;
    QVector<qint64> m_transitsMs; // ring of the last jitterWindow transit offsets
    int m_transitCount = 0;
    int m_transitNext = 0;
    // Long-run average of span() at pop time. Nudged by exactly one frame each
    // time a correction is made, so it reads as though the correction had
    // always been in place rather than lagging behind it and firing twice.
    double m_averageSpan = 0.0;
    bool m_averageSeeded = false;
    int m_sinceCorrection = 0; // pops since the last correction
    int m_biasedFor = 0;       // consecutive pops a correction has been owed
    JitterBufferStats m_stats;
};

} // namespace OpenChat
