#pragma once

#include <algorithm>
#include <cmath>

namespace OpenChat::CaseMotion {
inline constexpr int durationMs = 7200;
inline constexpr int tileCount = 80;
inline constexpr int startIndex = 8;
inline constexpr int firstWinnerIndex = 56;
inline constexpr int winnerVariants = 5;
inline constexpr int minimumTickMs = 28;
// Integral of a short linear acceleration followed by a quadratic velocity
// falloff. Both velocity joins are continuous; the last 20% travels ~0.4 cards.
inline double progress(double t)
{
    constexpr double ramp = 0.035;
    constexpr double area = ramp / 2 + (1 - ramp) / 3;
    t = std::clamp(t, 0.0, 1.0);
    if (t < ramp)
        return t * t / (2 * ramp * area);
    const double u = (t - ramp) / (1 - ramp);
    return (ramp / 2 + (1 - ramp) * (1 - std::pow(1 - u, 3)) / 3) / area;
}
inline int selectedIndex(double position) { return int(std::floor(position + 0.5)); }
}
