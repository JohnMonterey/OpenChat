#include "cosmetics/ProceduralNoise.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace OpenChat::Procedural {

namespace {

struct Direction
{
    float x;
    float y;
};

// Sixteen evenly spaced unit gradients; a table avoids trigonometry per corner.
constexpr std::array<Direction, 16> kGradients = {{
    {1.0f, 0.0f},        {0.92388f, 0.38268f},  {0.70711f, 0.70711f},  {0.38268f, 0.92388f},
    {0.0f, 1.0f},        {-0.38268f, 0.92388f}, {-0.70711f, 0.70711f}, {-0.92388f, 0.38268f},
    {-1.0f, 0.0f},       {-0.92388f, -0.38268f}, {-0.70711f, -0.70711f}, {-0.38268f, -0.92388f},
    {0.0f, -1.0f},       {0.38268f, -0.92388f}, {0.70711f, -0.70711f}, {0.92388f, -0.38268f},
}};

inline float fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float cornerDot(int cellX, int cellY, int periodX, int periodY, std::uint32_t seed,
                       float dx, float dy)
{
    const std::uint32_t h =
        hash2(wrapIndex(cellX, periodX), wrapIndex(cellY, periodY), seed);
    const Direction g = kGradients[h & 15U];
    return g.x * dx + g.y * dy;
}

inline void featurePoint(std::uint32_t h, float jitter, float &x, float &y)
{
    x = 0.5f + jitter * (unit(h) - 0.5f);
    y = 0.5f + jitter * (unit(hash32(h ^ 0x5bd1e995U)) - 0.5f);
}

} // namespace

float gradientNoise(float x, float y, int periodX, int periodY, std::uint32_t seed)
{
    const float floorX = std::floor(x);
    const float floorY = std::floor(y);
    const int cellX = static_cast<int>(floorX);
    const int cellY = static_cast<int>(floorY);
    const float tx = x - floorX;
    const float ty = y - floorY;

    const float n00 = cornerDot(cellX, cellY, periodX, periodY, seed, tx, ty);
    const float n10 = cornerDot(cellX + 1, cellY, periodX, periodY, seed, tx - 1.0f, ty);
    const float n01 = cornerDot(cellX, cellY + 1, periodX, periodY, seed, tx, ty - 1.0f);
    const float n11 =
        cornerDot(cellX + 1, cellY + 1, periodX, periodY, seed, tx - 1.0f, ty - 1.0f);
    const float u = fade(tx);
    const float v = fade(ty);
    return 1.41421f * mix(mix(n00, n10, u), mix(n01, n11, u), v);
}

float fbm(float x, float y, int periodX, int periodY, int octaves, std::uint32_t seed, float gain)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    int frequency = 1;
    for (int octave = 0; octave < octaves; ++octave) {
        sum += amplitude
               * gradientNoise(x * static_cast<float>(frequency), y * static_cast<float>(frequency),
                               periodX * frequency, periodY * frequency,
                               seed + static_cast<std::uint32_t>(octave) * 0x9e3779b9U);
        total += amplitude;
        amplitude *= gain;
        frequency *= 2;
    }
    return sum / total;
}

float ridged(float x, float y, int periodX, int periodY, int octaves, std::uint32_t seed)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    int frequency = 1;
    for (int octave = 0; octave < octaves; ++octave) {
        const float n = gradientNoise(x * static_cast<float>(frequency),
                                      y * static_cast<float>(frequency), periodX * frequency,
                                      periodY * frequency,
                                      seed + static_cast<std::uint32_t>(octave) * 0x7f4a7c15U);
        const float crease = 1.0f - std::abs(n);
        sum += amplitude * crease * crease;
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2;
    }
    return sum / total;
}

CellSample cells(float x, float y, int periodX, int periodY, std::uint32_t seed, float jitter)
{
    const float floorX = std::floor(x);
    const float floorY = std::floor(y);
    const int cellX = static_cast<int>(floorX);
    const int cellY = static_cast<int>(floorY);
    const float fx = x - floorX;
    const float fy = y - floorY;

    // First pass: the nearest feature point.
    float nearestSquared = 1e9f;
    int nearestI = 0;
    int nearestJ = 0;
    float toNearestX = 0.0f;
    float toNearestY = 0.0f;
    std::uint32_t nearestId = 0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            const std::uint32_t h = hash2(wrapIndex(cellX + i, periodX),
                                          wrapIndex(cellY + j, periodY), seed);
            float px = 0.0f;
            float py = 0.0f;
            featurePoint(h, jitter, px, py);
            const float rx = static_cast<float>(i) + px - fx;
            const float ry = static_cast<float>(j) + py - fy;
            const float d = rx * rx + ry * ry;
            if (d < nearestSquared) {
                nearestSquared = d;
                nearestI = i;
                nearestJ = j;
                toNearestX = rx;
                toNearestY = ry;
                nearestId = h;
            }
        }
    }

    // Second pass: exact distance to the closest bisector around that point.
    float border = 1e9f;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            const int bi = nearestI + i;
            const int bj = nearestJ + j;
            const std::uint32_t h = hash2(wrapIndex(cellX + bi, periodX),
                                          wrapIndex(cellY + bj, periodY), seed);
            float px = 0.0f;
            float py = 0.0f;
            featurePoint(h, jitter, px, py);
            const float rx = static_cast<float>(bi) + px - fx;
            const float ry = static_cast<float>(bj) + py - fy;
            const float dx = rx - toNearestX;
            const float dy = ry - toNearestY;
            const float lengthSquared = dx * dx + dy * dy;
            if (lengthSquared < 1e-8f)
                continue;
            const float length = std::sqrt(lengthSquared);
            const float distance =
                (0.5f * (toNearestX + rx) * dx + 0.5f * (toNearestY + ry) * dy) / length;
            border = std::min(border, distance);
        }
    }

    CellSample sample;
    sample.nearest = std::sqrt(nearestSquared);
    sample.border = border;
    sample.id = nearestId;
    sample.offsetX = -toNearestX;
    sample.offsetY = -toNearestY;
    return sample;
}

void parallelRows(int height, const std::function<void(int)> &rowFunction)
{
    const unsigned available = std::max(1U, std::thread::hardware_concurrency());
    const unsigned workers = std::min(available, 8U);
    std::atomic<int> nextRow{0};
    const auto work = [&] {
        for (int y = nextRow++; y < height; y = nextRow++)
            rowFunction(y);
    };
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    for (unsigned i = 1; i < workers; ++i)
        threads.emplace_back(work);
    work();
    for (std::thread &thread : threads)
        thread.join();
}

} // namespace OpenChat::Procedural
