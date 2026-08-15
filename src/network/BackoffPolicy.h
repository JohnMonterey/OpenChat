#pragma once

#include <chrono>
#include <functional>

namespace OpenChat {

// Exponential backoff with full jitter for relay reconnect attempts.
//
// The delay for attempt n (0-based) is drawn uniformly from
//   [0, min(cap, base * multiplier^n)]
// which is the "full jitter" strategy: it spreads reconnect storms across the
// whole window while never exceeding the cap. The cap defaults to five minutes
// as required by the secure-networking design.
//
// The jitter source is injectable so tests can make the delay deterministic
// (return 0.0 for the floor, a value approaching 1.0 for the ceiling). The
// policy itself is stateless; the caller tracks the attempt counter.
class BackoffPolicy final
{
public:
    struct Config final {
        std::chrono::milliseconds base{std::chrono::seconds{1}};
        std::chrono::milliseconds cap{std::chrono::minutes{5}};
        double multiplier{2.0};
    };

    // Returns a fraction in [0, 1). The default uses a non-cryptographic system
    // RNG; that is appropriate because jitter only needs to be unpredictable
    // enough to de-synchronize clients, not secret.
    using JitterSource = std::function<double()>;

    // Default configuration (1s base, 5min cap, x2) with the system jitter
    // source. A nested aggregate with default member initializers cannot be
    // used as an in-class default argument, so the no-argument case is its own
    // constructor rather than `Config config = {}`.
    BackoffPolicy();
    explicit BackoffPolicy(Config config, JitterSource jitter = {});

    // Upper bound of the window for this attempt: min(cap, base*multiplier^n).
    // Overflow and non-finite intermediates are clamped to the cap.
    [[nodiscard]] std::chrono::milliseconds ceilingForAttempt(int attempt) const;

    // Full-jitter delay for this attempt, drawn from [0, ceilingForAttempt].
    [[nodiscard]] std::chrono::milliseconds delayForAttempt(int attempt) const;

    [[nodiscard]] const Config &config() const noexcept { return m_config; }

private:
    Config m_config;
    JitterSource m_jitter;
};

} // namespace OpenChat
