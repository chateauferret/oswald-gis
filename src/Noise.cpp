#include "Noise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace noise {
namespace {

double simplexCorner(std::int64_t i, std::int64_t j, std::int64_t k,
                     const glm::dvec3& offset, std::uint64_t seed) {
    static constexpr std::array<glm::ivec3, 12> gradients{{
        {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
        {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
        {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}
    }};
    const double attenuation = 0.6 - glm::dot(offset, offset);
    if (attenuation <= 0.0) {
        return 0.0;
    }
    const auto& gradient = gradients[LatticeHash(i, j, k, seed) % gradients.size()];
    const double squared = attenuation * attenuation;
    return squared * squared *
           (gradient.x * offset.x + gradient.y * offset.y + gradient.z * offset.z);
}

double interpolationCurve(double value, Interpolation interpolation) {
    switch (interpolation) {
    case Interpolation::Linear:
        return value;
    case Interpolation::Cubic:
        return value * value * (3.0 - 2.0 * value);
    case Interpolation::Quintic:
        return value * value * value * (value * (value * 6.0 - 15.0) + 10.0);
    }
    throw std::invalid_argument("Value noise interpolation mode is invalid.");
}

template <typename Sampler>
double fractalSum(glm::dvec3 point, const FractalParameters& fractal, Sampler sample) {
    double frequency = fractal.frequency;
    double amplitude = 1.0;
    double totalAmplitude = 0.0;
    double value = 0.0;
    for (int octave = 0; octave < fractal.octaves; ++octave) {
        value += sample(point * frequency) * amplitude;
        totalAmplitude += amplitude;
        frequency *= fractal.lacunarity;
        amplitude *= fractal.persistence;
    }
    return value / totalAmplitude;
}

void validateFractal(const FractalParameters& fractal) {
    if (!std::isfinite(fractal.frequency) || !std::isfinite(fractal.lacunarity) ||
        !std::isfinite(fractal.persistence) || fractal.octaves < 1 || fractal.octaves > 16) {
        throw std::invalid_argument(
            "Fractal noise needs finite frequency, lacunarity, persistence and 1-16 octaves.");
    }
}

} // namespace

std::uint32_t LatticeHash(std::int64_t x, std::int64_t y, std::int64_t z, std::uint64_t seed) {
    std::uint32_t hash = static_cast<std::uint32_t>(x) * 0x8da6b343U ^
                         static_cast<std::uint32_t>(y) * 0xd8163841U ^
                         static_cast<std::uint32_t>(z) * 0xcb1ab31fU;
    hash ^= static_cast<std::uint32_t>(seed) * 0x9e3779b9U;
    hash ^= static_cast<std::uint32_t>(seed >> 32) * 0x85ebca6bU;
    hash ^= hash >> 16;
    hash *= 0x7feb352dU;
    hash ^= hash >> 15;
    hash *= 0x846ca68bU;
    hash ^= hash >> 16;
    return hash;
}

double HashValue(std::int64_t x, std::int64_t y, std::int64_t z, std::uint64_t seed) {
    return static_cast<double>(LatticeHash(x, y, z, seed)) /
               static_cast<double>(std::numeric_limits<std::uint32_t>::max()) * 2.0 - 1.0;
}

double Simplex(glm::dvec3 point, std::uint64_t seed) {
    constexpr double skew = 1.0 / 3.0;
    constexpr double unskew = 1.0 / 6.0;
    const double skewed = (point.x + point.y + point.z) * skew;
    const auto i = static_cast<std::int64_t>(std::floor(point.x + skewed));
    const auto j = static_cast<std::int64_t>(std::floor(point.y + skewed));
    const auto k = static_cast<std::int64_t>(std::floor(point.z + skewed));
    const double unskewed = static_cast<double>(i + j + k) * unskew;
    const glm::dvec3 first(point.x - (static_cast<double>(i) - unskewed),
                           point.y - (static_cast<double>(j) - unskewed),
                           point.z - (static_cast<double>(k) - unskewed));

    glm::ivec3 firstOffset;
    glm::ivec3 secondOffset;
    if (first.x >= first.y) {
        if (first.y >= first.z) {
            firstOffset = {1, 0, 0};
            secondOffset = {1, 1, 0};
        } else if (first.x >= first.z) {
            firstOffset = {1, 0, 0};
            secondOffset = {1, 0, 1};
        } else {
            firstOffset = {0, 0, 1};
            secondOffset = {1, 0, 1};
        }
    } else if (first.y < first.z) {
        firstOffset = {0, 0, 1};
        secondOffset = {0, 1, 1};
    } else if (first.x < first.z) {
        firstOffset = {0, 1, 0};
        secondOffset = {0, 1, 1};
    } else {
        firstOffset = {1, 1, 0};
        secondOffset = {0, 1, 1};
    }
    const glm::dvec3 second = first - glm::dvec3(firstOffset) + unskew;
    const glm::dvec3 third = first - glm::dvec3(secondOffset) + 2.0 * unskew;
    const glm::dvec3 fourth = first - 1.0 + 3.0 * unskew;
    return 32.0 * (simplexCorner(i, j, k, first, seed) +
                   simplexCorner(i + firstOffset.x, j + firstOffset.y,
                                 k + firstOffset.z, second, seed) +
                   simplexCorner(i + secondOffset.x, j + secondOffset.y,
                                 k + secondOffset.z, third, seed) +
                   simplexCorner(i + 1, j + 1, k + 1, fourth, seed));
}

double Value(glm::dvec3 point, std::uint64_t seed, Interpolation interpolation) {
    const auto x0 = static_cast<std::int64_t>(std::floor(point.x));
    const auto y0 = static_cast<std::int64_t>(std::floor(point.y));
    const auto z0 = static_cast<std::int64_t>(std::floor(point.z));
    const double x = interpolationCurve(point.x - x0, interpolation);
    const double y = interpolationCurve(point.y - y0, interpolation);
    const double z = interpolationCurve(point.z - z0, interpolation);
    const double c000 = HashValue(x0, y0, z0, seed);
    const double c100 = HashValue(x0 + 1, y0, z0, seed);
    const double c010 = HashValue(x0, y0 + 1, z0, seed);
    const double c110 = HashValue(x0 + 1, y0 + 1, z0, seed);
    const double c001 = HashValue(x0, y0, z0 + 1, seed);
    const double c101 = HashValue(x0 + 1, y0, z0 + 1, seed);
    const double c011 = HashValue(x0, y0 + 1, z0 + 1, seed);
    const double c111 = HashValue(x0 + 1, y0 + 1, z0 + 1, seed);
    return std::lerp(std::lerp(std::lerp(c000, c100, x), std::lerp(c010, c110, x), y),
                     std::lerp(std::lerp(c001, c101, x), std::lerp(c011, c111, x), y), z);
}

double SimplexFbm(glm::dvec3 point, std::uint64_t seed, const FractalParameters& fractal) {
    validateFractal(fractal);
    return fractalSum(point, fractal, [seed](glm::dvec3 p) { return Simplex(p, seed); });
}

double ValueFbm(glm::dvec3 point, std::uint64_t seed, Interpolation interpolation,
                const FractalParameters& fractal) {
    validateFractal(fractal);
    return fractalSum(point, fractal,
                      [seed, interpolation](glm::dvec3 p) { return Value(p, seed, interpolation); });
}

double Ridged(glm::dvec3 point, std::uint64_t seed, const FractalParameters& fractal,
              double offset, double gainExponent) {
    validateFractal(fractal);
    double frequency = fractal.frequency;
    double amplitude = 1.0;
    double totalAmplitude = 0.0;
    double result = 0.0;
    double feedback = 1.0;
    for (int octave = 0; octave < fractal.octaves; ++octave) {
        const double base = Simplex(point * frequency, seed);
        const double ridge = std::clamp(offset - std::abs(base), 0.0, 1.0);
        const double signal = std::pow(ridge, gainExponent) * feedback;
        result += signal * amplitude;
        totalAmplitude += amplitude;
        feedback = std::clamp(signal * 2.0, 0.0, 1.0);
        frequency *= fractal.lacunarity;
        amplitude *= fractal.persistence;
    }
    return std::clamp(2.0 * result / totalAmplitude - 1.0, -1.0, 1.0);
}

} // namespace noise
