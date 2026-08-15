#include "network/BackoffPolicy.h"

#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

namespace OpenChat {

namespace {

double defaultJitter()
{
    // generateDouble() yields a value in [0, 1).
    return QRandomGenerator::system()->generateDouble();
}

} // namespace

BackoffPolicy::BackoffPolicy()
    : BackoffPolicy(Config{}, JitterSource{})
{
}

BackoffPolicy::BackoffPolicy(Config config, JitterSource jitter)
    : m_config(config)
    , m_jitter(jitter ? std::move(jitter) : JitterSource(defaultJitter))
{
    if (m_config.base <= std::chrono::milliseconds::zero())
        m_config.base = std::chrono::milliseconds{1};
    if (m_config.cap < m_config.base)
        m_config.cap = m_config.base;
    if (!(m_config.multiplier > 1.0))
        m_config.multiplier = 2.0;
}

std::chrono::milliseconds BackoffPolicy::ceilingForAttempt(int attempt) const
{
    if (attempt < 0)
        attempt = 0;

    const double capMs = static_cast<double>(m_config.cap.count());
    const double baseMs = static_cast<double>(m_config.base.count());

    // base * multiplier^attempt, computed in double and clamped. std::pow on a
    // large exponent yields +inf; std::min then selects the finite cap.
    const double scaled = baseMs * std::pow(m_config.multiplier, static_cast<double>(attempt));
    double windowMs = scaled;
    if (!std::isfinite(windowMs) || windowMs > capMs)
        windowMs = capMs;
    if (windowMs < baseMs)
        windowMs = baseMs;

    return std::chrono::milliseconds{static_cast<long long>(windowMs)};
}

std::chrono::milliseconds BackoffPolicy::delayForAttempt(int attempt) const
{
    const auto ceiling = ceilingForAttempt(attempt);

    double fraction = m_jitter ? m_jitter() : 0.0;
    // Defend against a misbehaving jitter source.
    if (!std::isfinite(fraction) || fraction < 0.0)
        fraction = 0.0;
    else if (fraction >= 1.0)
        fraction = std::nextafter(1.0, 0.0);

    const auto delayMs = static_cast<long long>(fraction * static_cast<double>(ceiling.count()));
    return std::chrono::milliseconds{delayMs};
}

} // namespace OpenChat
