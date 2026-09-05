#include "media/JitterBuffer.h"

#include <algorithm>

namespace OpenChat {

namespace {

// Gain of the running average of depth. 1/64 over 20 ms frames is a time
// constant of about 1.3 s: slow enough that the ±1 tick of noise a single
// reordered packet causes is flattened to a few hundredths of a frame, fast
// enough that a real bias shows within a couple of seconds.
constexpr double spanAverageGain = 1.0 / 64.0;

} // namespace

JitterBuffer::JitterBuffer(Config config)
    : m_config(std::move(config))
{
    // Clamp the configuration into a shape the ring can honour rather than
    // trusting it: a maxDepth below the target would prime forever, and a
    // ceiling below the floor would leave the controller nowhere to go.
    m_config.maxDepth = std::max(1, m_config.maxDepth);
    m_config.targetDepth = std::clamp(m_config.targetDepth, 0, m_config.maxDepth);
    m_config.maxTargetDepth =
        std::clamp(m_config.maxTargetDepth, m_config.targetDepth, m_config.maxDepth);
    m_config.resetDistance = std::max(m_config.maxDepth, m_config.resetDistance);
    m_config.jitterWindow = std::max(2, m_config.jitterWindow);
    m_config.correctionSpacing = std::max(1, m_config.correctionSpacing);
    m_config.correctionPatience = std::max(m_config.correctionSpacing, m_config.correctionPatience);
    m_slots.resize(m_config.maxDepth);
    m_transitsMs.resize(m_config.jitterWindow);
    setTarget(m_config.targetDepth);
}

void JitterBuffer::reset()
{
    for (Slot &slot : m_slots) {
        slot.filled = false;
        slot.payload.clear();
    }
    m_count = 0;
    m_started = false;
    m_priming = true;
    m_playoutStarted = false;
    m_transitCount = 0;
    m_transitNext = 0;
    m_averageSeeded = false;
    m_sinceCorrection = 0;
    m_biasedFor = 0;
    m_stats.jitterMs = 0;
    setTarget(m_config.targetDepth);
}

void JitterBuffer::primeFrom(quint32 sequence)
{
    for (Slot &slot : m_slots) {
        slot.filled = false;
        slot.payload.clear();
    }
    m_count = 0;
    m_nextSequence = sequence;
    m_highestStored = sequence;
    m_started = true;
    m_priming = true;
    m_playoutStarted = false;
    // A new stream means a new sender grid, so the old transit offsets are
    // meaningless and must not be compared with the new ones. The target itself
    // is kept: the network did not change just because the peer restarted, and
    // re-learning it from nothing would mean re-starving on the same jitter.
    m_streamBase = sequence;
    m_transitCount = 0;
    m_transitNext = 0;
    m_averageSeeded = false;
    m_sinceCorrection = 0;
    m_biasedFor = 0;
}

void JitterBuffer::setTarget(int target)
{
    m_target = target;
    m_stats.targetDepth = target;
}

int JitterBuffer::span() const noexcept
{
    if (m_count == 0)
        return 0;
    return distance(m_highestStored + 1, m_nextSequence);
}

void JitterBuffer::observeArrival(quint32 sequence)
{
    if (!isAdaptive())
        return;
    // Where this packet landed relative to the sender's 20 ms grid. On a perfect
    // link every packet has the same offset; the spread of the offsets over the
    // window is exactly how much later than the fastest a packet can arrive,
    // which is exactly how much cushion playout needs.
    const qint64 transit = m_config.clock()
        - static_cast<qint64>(distance(sequence, m_streamBase)) * CallAudioFormat::frameDurationMs;
    m_transitsMs[m_transitNext] = transit;
    m_transitNext = (m_transitNext + 1) % m_config.jitterWindow;
    m_transitCount = std::min(m_transitCount + 1, m_config.jitterWindow);
    if (m_transitCount < 2)
        return;

    const auto end = m_transitsMs.cbegin() + m_transitCount;
    const auto [lowest, highest] = std::minmax_element(m_transitsMs.cbegin(), end);
    const qint64 spread = *highest - *lowest;
    m_stats.jitterMs = spread;

    // The slowest packet arrives `spread` after the fastest, so it needs that
    // many whole frames of cushion plus the one it is going to play from. The
    // floor is the configured depth, not one: a clean link is still reordered
    // across, and a jitter estimate says nothing about that.
    constexpr qint64 frameMs = CallAudioFormat::frameDurationMs;
    const int needed = static_cast<int>((spread + frameMs - 1) / frameMs) + 1;
    setTarget(std::clamp(needed, m_config.targetDepth, m_config.maxTargetDepth));
}

JitterBuffer::PushResult JitterBuffer::push(quint32 sequence, const QByteArray &payload)
{
    // Records `payload` as the only queued frame of a freshly primed stream.
    const auto restartOn = [this, sequence, &payload] {
        primeFrom(sequence);
        slotFor(sequence) = Slot{sequence, payload, true};
        m_count = 1;
        ++m_stats.accepted;
        m_stats.peakDepth = std::max(m_stats.peakDepth, m_count);
        observeArrival(sequence);
        if (m_count >= m_target)
            m_priming = false;
    };

    if (!m_started) {
        restartOn();
        return PushResult::Accepted;
    }

    qint32 ahead = distance(sequence, m_nextSequence);

    // Still priming, and this packet is older than where playout was going to
    // start. The start was only ever a guess — it was pinned to whichever packet
    // happened to arrive first, and on a reordering link that need not be the
    // earliest. Nothing has played yet, so move the start back to this packet
    // rather than discarding it as late; otherwise a call that opens with two
    // packets swapped loses its first frame every time.
    //
    // Only possible before anything has played, and while the whole queued span
    // still fits the ring.
    if (m_priming && !m_playoutStarted && ahead < 0 && ahead > -m_config.resetDistance
        && distance(m_highestStored, sequence) < static_cast<qint32>(m_slots.size())) {
        m_nextSequence = sequence;
        ahead = 0;
    }

    // Far outside the window in either direction: this is a different stream,
    // not a reordering. Re-prime on it so a peer that restarted (or a call that
    // resumed after a long stall) recovers instead of discarding everything.
    if (ahead <= -m_config.resetDistance || ahead >= m_config.resetDistance) {
        restartOn();
        ++m_stats.resets;
        return PushResult::Reset;
    }

    if (ahead < 0) {
        // Late packets are the clearest evidence there is that the cushion is
        // too shallow, so they are measured even though they are not kept.
        observeArrival(sequence);
        ++m_stats.late;
        return PushResult::Late;
    }

    if (ahead >= static_cast<qint32>(m_slots.size())) {
        // Too far ahead to address in the ring. Dropping the newest keeps the
        // frames already queued (which play sooner) rather than evicting them.
        ++m_stats.overflows;
        return PushResult::Overflow;
    }

    Slot &slot = slotFor(sequence);
    if (slot.filled && slot.sequence == sequence) {
        // Not measured: a redelivery says nothing about the path the original
        // took, and counting it would inflate the spread with the relay's retry
        // interval.
        ++m_stats.duplicates;
        return PushResult::Duplicate;
    }

    // The ring is indexed modulo its size, and `ahead` is inside it, so a filled
    // slot with a different sequence cannot be in the live window — it is a
    // stale entry left by an earlier wrap. Overwriting it is correct.
    if (!slot.filled)
        ++m_count;
    slot = Slot{sequence, payload, true};
    if (distance(sequence, m_highestStored) > 0)
        m_highestStored = sequence;
    ++m_stats.accepted;
    m_stats.peakDepth = std::max(m_stats.peakDepth, m_count);
    observeArrival(sequence);

    if (m_priming && m_count >= m_target)
        m_priming = false;
    return PushResult::Accepted;
}

JitterBuffer::PopResult JitterBuffer::pop(bool quiet)
{
    if (!m_started || m_priming || m_count == 0) {
        // Starving mid-stream drops back to priming: resuming playout the
        // instant one frame lands would only starve again on the next slot.
        // The depth average restarts with playout, since the depth the re-prime
        // delivers is the only honest sample of the new steady state.
        if (m_started && m_count == 0) {
            m_priming = true;
            m_averageSeeded = false;
        }
        ++m_stats.starved;
        return PopResult{PopKind::Starved, {}, m_nextSequence, {}};
    }

    QByteArray skipped;
    if (isAdaptive()) {
        const int current = span();
        if (m_averageSeeded)
            m_averageSpan += (current - m_averageSpan) * spanAverageGain;
        else
            m_averageSpan = current;
        m_averageSeeded = true;
        ++m_sinceCorrection;

        const double bias = m_averageSpan - m_target;
        const bool nextEmpty = !holds(m_nextSequence);
        // A correction made for quiet's sake is paced; one that has waited the
        // whole patience out goes ahead in whatever is playing. Counted after
        // this pop is added to the wait, so "patience" is the number of pops a
        // correction can be refused, exactly as configured.
        const auto due = [this, quiet] {
            return (quiet && m_sinceCorrection >= m_config.correctionSpacing)
                || m_biasedFor >= m_config.correctionPatience;
        };

        if (bias <= -m_config.correctionBand) {
            ++m_biasedFor;
            // Free when the slot is empty anyway: this pop was going to be
            // concealment regardless, and holding the cursor gives the missing
            // frame one more interval to turn up. Urgent when only one frame is
            // actually in hand — frames, not slots, because a span of three can
            // be one frame and two holes — since the next pop would then starve,
            // and a re-prime costs a whole target's worth of silence against one
            // frame here.
            const bool free = nextEmpty;
            const bool urgent = m_count <= 1;
            if (free || urgent || due()) {
                ++m_stats.inserted;
                m_averageSpan += 1.0;
                m_sinceCorrection = 0;
                m_biasedFor = 0;
                return PopResult{PopKind::Inserted, {}, m_nextSequence, {}};
            }
        } else if (bias >= m_config.correctionBand && current >= 2) {
            ++m_biasedFor;
            // Free when the slot is empty: skipping a hole removes a frame of
            // latency and a frame of concealment at once. Never taken with only
            // one slot left, because that slot is the one about to play.
            if (nextEmpty || due()) {
                Slot &victim = slotFor(m_nextSequence);
                if (victim.filled && victim.sequence == m_nextSequence) {
                    skipped = std::move(victim.payload);
                    victim.filled = false;
                    victim.payload.clear();
                    --m_count;
                }
                ++m_nextSequence;
                m_playoutStarted = true;
                ++m_stats.dropped;
                m_averageSpan -= 1.0;
                m_sinceCorrection = 0;
                m_biasedFor = 0;
            }
        } else {
            m_biasedFor = 0;
        }
    }

    Slot &slot = slotFor(m_nextSequence);
    if (slot.filled && slot.sequence == m_nextSequence) {
        PopResult result{PopKind::Frame, slot.payload, m_nextSequence, std::move(skipped)};
        slot.filled = false;
        slot.payload.clear();
        --m_count;
        ++m_nextSequence;
        m_playoutStarted = true;
        ++m_stats.played;
        return result;
    }

    // The slot is empty but later frames are queued, so this one is not merely
    // late — waiting for it would stall playout behind audio already in hand.
    // Report the gap and move on; the caller conceals it.
    PopResult result{PopKind::Lost, {}, m_nextSequence, std::move(skipped)};
    ++m_nextSequence;
    m_playoutStarted = true;
    ++m_stats.lost;
    return result;
}

} // namespace OpenChat
