#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

// Small, deterministic procedural-texture toolkit for cosmetic artwork.
//
// Every noise function here is periodic: sampling at x and at x + periodX gives
// the same value. A texture generated over exactly one period therefore tiles
// without a seam, which is what lets a bubble of any size repeat a cached tile.
// Periods are in lattice cells, so the coordinate passed in is "cells", not
// pixels; octave n of fbm uses period * 2^n and keeps the tiling.
namespace OpenChat::Procedural {

inline std::uint32_t hash32(std::uint32_t x)
{
    // "lowbias32" integer finaliser: cheap, well mixed, stable across platforms.
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline std::uint32_t hash2(std::int32_t x, std::int32_t y, std::uint32_t seed)
{
    return hash32(static_cast<std::uint32_t>(x) * 0x8da6b343U
                  ^ hash32(static_cast<std::uint32_t>(y) * 0xd8163841U ^ seed));
}

// Uniform in [0, 1).
inline float unit(std::uint32_t h)
{
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

inline int wrapIndex(int i, int period)
{
    const int m = i % period;
    return m < 0 ? m + period : m;
}

inline float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

inline float mix(float a, float b, float t)
{
    return a + (b - a) * t;
}

inline float smoothstep(float edge0, float edge1, float x)
{
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// Periodic gradient (Perlin-style) noise, roughly in [-1, 1].
float gradientNoise(float x, float y, int periodX, int periodY, std::uint32_t seed);

// Fractal sum of gradientNoise; the first octave has the given period.
float fbm(float x, float y, int periodX, int periodY, int octaves, std::uint32_t seed,
          float gain = 0.5f);

// Ridged multifractal: sharp creases where the underlying noise crosses zero.
float ridged(float x, float y, int periodX, int periodY, int octaves, std::uint32_t seed);

struct CellSample
{
    float nearest = 0.0f;   // distance to the nearest feature point (F1)
    float border = 0.0f;    // distance to the nearest Voronoi border
    std::uint32_t id = 0;   // stable id of the nearest cell (wrapped, so it tiles)
    float offsetX = 0.0f;   // vector from the nearest feature point to the sample
    float offsetY = 0.0f;
};

// Periodic Voronoi / Worley cells with an exact border distance, so cracks and
// seams drawn from it keep an even width.
CellSample cells(float x, float y, int periodX, int periodY, std::uint32_t seed,
                 float jitter = 0.85f);

struct Rgb
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

inline Rgb rgb(std::uint32_t hex)
{
    return {static_cast<float>((hex >> 16) & 0xff) / 255.0f,
            static_cast<float>((hex >> 8) & 0xff) / 255.0f,
            static_cast<float>(hex & 0xff) / 255.0f};
}

inline Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
inline Rgb operator-(Rgb a, Rgb b) { return {a.r - b.r, a.g - b.g, a.b - b.b}; }
inline Rgb operator*(Rgb a, float s) { return {a.r * s, a.g * s, a.b * s}; }
inline Rgb operator*(Rgb a, Rgb b) { return {a.r * b.r, a.g * b.g, a.b * b.b}; }

inline Rgb mix(Rgb a, Rgb b, float t)
{
    return {mix(a.r, b.r, t), mix(a.g, b.g, t), mix(a.b, b.b, t)};
}

// Screen blend: brightens without clipping the way plain addition does.
inline Rgb screen(Rgb base, Rgb light)
{
    return {1.0f - (1.0f - base.r) * (1.0f - light.r), 1.0f - (1.0f - base.g) * (1.0f - light.g),
            1.0f - (1.0f - base.b) * (1.0f - light.b)};
}

inline float luminance(Rgb c)
{
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// Cosine palette (a + b * cos(2π(c·t + d))): smooth hue cycles for iridescence.
inline Rgb cosinePalette(float t, Rgb a, Rgb b, Rgb c, Rgb d)
{
    constexpr float tau = 6.28318530718f;
    return {a.r + b.r * std::cos(tau * (c.r * t + d.r)), a.g + b.g * std::cos(tau * (c.g * t + d.g)),
            a.b + b.b * std::cos(tau * (c.b * t + d.b))};
}

// Runs rowFunction(y) for every row, spread over a few worker threads.
void parallelRows(int height, const std::function<void(int)> &rowFunction);

} // namespace OpenChat::Procedural
