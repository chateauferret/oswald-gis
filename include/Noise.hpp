#pragma once

#include <glm/glm.hpp>

#include <cstdint>

// Deterministic, seeded noise primitives shared by the noise brushes and the
// script engine. Every function returns a value in [-1, 1].
namespace noise {

enum class Interpolation { Linear, Cubic, Quintic };

struct FractalParameters {
    double frequency = 1.0;
    double lacunarity = 2.0;
    double persistence = 0.5;
    int octaves = 1;
};

std::uint32_t LatticeHash(std::int64_t x, std::int64_t y, std::int64_t z, std::uint64_t seed);
double HashValue(std::int64_t x, std::int64_t y, std::int64_t z, std::uint64_t seed);

double Simplex(glm::dvec3 point, std::uint64_t seed);
double Value(glm::dvec3 point, std::uint64_t seed, Interpolation interpolation);

// Fractal sums are normalized by the total octave amplitude.
double SimplexFbm(glm::dvec3 point, std::uint64_t seed, const FractalParameters& fractal);
double ValueFbm(glm::dvec3 point, std::uint64_t seed, Interpolation interpolation,
                const FractalParameters& fractal);
double Ridged(glm::dvec3 point, std::uint64_t seed, const FractalParameters& fractal,
              double offset, double gainExponent);

} // namespace noise
