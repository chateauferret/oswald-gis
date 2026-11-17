#pragma once

#include <glm/glm.hpp>

#include <cstdint>

// FastNoise2-backed generators shared by brushes and scripts. Samples use float
// precision and return values in [-1, 1]. Both halves of the 64-bit seed are
// folded into FastNoise2's 32-bit seed; output differs from the original backend.
// Invalid/non-finite parameters or coordinates outside the supported lattice
// range throw std::invalid_argument.
namespace noise {

struct FractalParameters {
    double frequency = 1.0;
    double lacunarity = 2.0;
    double persistence = 0.5;
    int octaves = 1;
};

// FastNoise2 white noise sampled at integer positions, used to seed spectral fields.
double HashValue(std::int64_t x, std::int64_t y, std::int64_t z, std::uint64_t seed);

double Simplex(glm::dvec3 point, std::uint64_t seed);
double Value(glm::dvec3 point, std::uint64_t seed);

// Fractal sums are normalized by the total octave amplitude.
double SimplexFbm(glm::dvec3 point, std::uint64_t seed, const FractalParameters& fractal);
double ValueFbm(glm::dvec3 point, std::uint64_t seed, const FractalParameters& fractal);
double Ridged(glm::dvec3 point, std::uint64_t seed, const FractalParameters& fractal);

} // namespace noise
