#include "Noise.hpp"

#include <FastNoise/FastNoise.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace noise {
namespace {

template <typename T>
auto newNode() {
    auto node = FastNoise::New<T>();
    if (!node) {
        throw std::runtime_error("FastNoise2 could not create a generator for this CPU.");
    }
    return node;
}

struct Generators {
    FastNoise::SmartNode<FastNoise::Simplex> simplex = newNode<FastNoise::Simplex>();
    FastNoise::SmartNode<FastNoise::Value> value = newNode<FastNoise::Value>();
    FastNoise::SmartNode<FastNoise::White> white = newNode<FastNoise::White>();
    FastNoise::SmartNode<FastNoise::FractalFBm> simplexFbm = newNode<FastNoise::FractalFBm>();
    FastNoise::SmartNode<FastNoise::FractalFBm> valueFbm = newNode<FastNoise::FractalFBm>();
    FastNoise::SmartNode<FastNoise::FractalRidged> ridged = newNode<FastNoise::FractalRidged>();

    Generators() {
        // FastNoise2 defaults to a feature scale of 100; our callers already scale positions.
        simplex->SetScale(1.0F);
        value->SetScale(1.0F);
        simplexFbm->SetSource(simplex);
        valueFbm->SetSource(value);
        ridged->SetSource(simplex);
    }
};

Generators& generators() {
    // Reuse nodes without allocating per vertex or sharing mutable fractal settings across threads.
    thread_local Generators nodes;
    return nodes;
}

int seed32(std::uint64_t seed) {
    const auto bits = static_cast<std::uint32_t>(seed) +
                      static_cast<std::uint32_t>(seed >> 32) * 0x9e3779b9U;
    return std::bit_cast<std::int32_t>(bits);
}

void validatePoint(glm::dvec3 point) {
    // Leave headroom for Simplex skewing before FastNoise2 converts to a signed 32-bit lattice.
    constexpr double maximumCoordinate = 1.0e8;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
        glm::any(glm::greaterThan(glm::abs(point), glm::dvec3(maximumCoordinate)))) {
        throw std::invalid_argument("Noise coordinates must be finite and within [-1e8, 1e8] at every octave.");
    }
}

double sample(const FastNoise::Generator& generator, glm::dvec3 point, std::uint64_t seed) {
    const auto value = generator.GenSingle3D(static_cast<float>(point.x),
        static_cast<float>(point.y), static_cast<float>(point.z), seed32(seed));
    if (!std::isfinite(value)) {
        throw std::runtime_error("FastNoise2 returned a non-finite noise sample.");
    }
    return value;
}

template <typename T>
double fractalSample(T& generator, glm::dvec3 point, std::uint64_t seed,
                     const FractalParameters& fractal) {
    constexpr double maximumFloat = std::numeric_limits<float>::max();
    if (!std::isfinite(fractal.frequency) || fractal.frequency < 0.0 ||
        !std::isfinite(fractal.lacunarity) || fractal.lacunarity < 0.0 ||
        fractal.lacunarity > maximumFloat ||
        !std::isfinite(fractal.persistence) || fractal.persistence < 0.0 ||
        fractal.persistence > maximumFloat || fractal.octaves < 1 || fractal.octaves > 16) {
        throw std::invalid_argument(
            "Fractal noise needs finite nonnegative frequency, lacunarity and persistence, "
            "float-representable lacunarity/persistence, and 1-16 octaves.");
    }
    point *= fractal.frequency;
    auto octavePoint = point;
    double amplitude = 1.0;
    double totalAmplitude = 1.0;
    for (int octave = 0; octave < fractal.octaves; ++octave) {
        validatePoint(octavePoint);
        if (octave + 1 < fractal.octaves) {
            octavePoint *= fractal.lacunarity;
            amplitude *= fractal.persistence;
            totalAmplitude += amplitude;
        }
    }
    if (!std::isfinite(totalAmplitude) || totalAmplitude > maximumFloat * 0.5) {
        throw std::invalid_argument("Fractal noise total amplitude exceeds the FastNoise2 float range.");
    }
    generator.SetOctaveCount(fractal.octaves);
    generator.SetLacunarity(static_cast<float>(fractal.lacunarity));
    generator.SetGain(static_cast<float>(fractal.persistence));
    return std::clamp(sample(generator, point, seed) / totalAmplitude, -1.0, 1.0);
}

} // namespace

double HashValue(std::int64_t x, std::int64_t y, std::int64_t z, std::uint64_t seed) {
    const glm::dvec3 point(x, y, z);
    validatePoint(point);
    return std::clamp(sample(*generators().white, point, seed), -1.0, 1.0);
}

double Simplex(glm::dvec3 point, std::uint64_t seed) {
    validatePoint(point);
    return std::clamp(sample(*generators().simplex, point, seed), -1.0, 1.0);
}

double Value(glm::dvec3 point, std::uint64_t seed) {
    validatePoint(point);
    return std::clamp(sample(*generators().value, point, seed), -1.0, 1.0);
}

glm::dvec3 DomainWarp(glm::dvec3 point, std::uint64_t seed,
                      const DomainWarpParameters& parameters) {
    constexpr double maximumFrequency = 1.0e4;
    if (!std::isfinite(parameters.frequency) || parameters.frequency <= 0.0 ||
        parameters.frequency > maximumFrequency ||
        !std::isfinite(parameters.strength) || parameters.strength < 0.0 ||
        parameters.strength > 10.0 ||
        !std::isfinite(parameters.lacunarity) || parameters.lacunarity < 1.0 ||
        parameters.lacunarity > 4.0 ||
        !std::isfinite(parameters.persistence) || parameters.persistence < 0.0 ||
        parameters.persistence > 1.0 || parameters.octaves < 1 ||
        parameters.octaves > 16) {
        throw std::invalid_argument("Domain warp parameters are invalid.");
    }
    validatePoint(point);
    if (parameters.strength == 0.0) {
        return point;
    }

    glm::dvec3 displacement(0.0);
    double frequency = parameters.frequency;
    double amplitude = 1.0;
    double totalAmplitude = 0.0;
    for (int octave = 0; octave < parameters.octaves; ++octave) {
        const glm::dvec3 domain = point * frequency;
        const auto octaveSeed = seed +
            (0x9e3779b97f4a7c15ULL * static_cast<std::uint64_t>(octave + 1));
        displacement += glm::dvec3(
            Simplex(domain, octaveSeed ^ 0x9e3779b97f4a7c15ULL),
            Simplex(domain, octaveSeed ^ 0xbf58476d1ce4e5b9ULL),
            Simplex(domain, octaveSeed ^ 0x94d049bb133111ebULL)) * amplitude;
        totalAmplitude += amplitude;
        frequency *= parameters.lacunarity;
        amplitude *= parameters.persistence;
    }
    return point + (displacement / totalAmplitude) * parameters.strength;
}

double SimplexFbm(glm::dvec3 point, std::uint64_t seed, const FractalParameters& fractal) {
    return fractalSample(*generators().simplexFbm, point, seed, fractal);
}

double ValueFbm(glm::dvec3 point, std::uint64_t seed, const FractalParameters& fractal) {
    return fractalSample(*generators().valueFbm, point, seed, fractal);
}

double Ridged(glm::dvec3 point, std::uint64_t seed, const FractalParameters& fractal) {
    return fractalSample(*generators().ridged, point, seed, fractal);
}

} // namespace noise
