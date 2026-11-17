#include "Noise.hpp"

#include <FastNoise/FastNoise.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void rejects(Function function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid noise input was accepted.");
}

int nativeSeed(std::uint64_t seed) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(seed) +
        static_cast<std::uint32_t>(seed >> 32) * 0x9e3779b9U);
}

double nativeSample(const FastNoise::Generator& generator, glm::dvec3 point, std::uint64_t seed) {
    return generator.GenSingle3D(static_cast<float>(point.x), static_cast<float>(point.y),
                                 static_cast<float>(point.z), nativeSeed(seed));
}

void testNativeGenerators() {
    auto simplex = FastNoise::New<FastNoise::Simplex>();
    auto value = FastNoise::New<FastNoise::Value>();
    auto white = FastNoise::New<FastNoise::White>();
    auto fbm = FastNoise::New<FastNoise::FractalFBm>();
    auto valueFbm = FastNoise::New<FastNoise::FractalFBm>();
    auto ridged = FastNoise::New<FastNoise::FractalRidged>();
    check(simplex && value && white && fbm && valueFbm && ridged,
          "Native FastNoise2 generators could not be created.");
    simplex->SetScale(1.0F);
    value->SetScale(1.0F);
    fbm->SetSource(simplex);
    valueFbm->SetSource(value);
    ridged->SetSource(simplex);
    const auto normalized = [](double value) { return std::clamp(value, -1.0, 1.0); };
    for (const auto seed : {0ULL, 1ULL, 1ULL << 32, 0xffffffffffffffffULL}) {
        for (const auto point : {glm::dvec3(-0.25, 0.125, 1.75), glm::dvec3(3.2, -4.1, 0.7),
                                 glm::dvec3(0.0), glm::dvec3(-2.0, 3.0, 4.0)}) {
            check(noise::Simplex(point, seed) == normalized(nativeSample(*simplex, point, seed)),
                  "Simplex is not backed by the native FastNoise2 generator.");
            check(noise::Value(point, seed) == normalized(nativeSample(*value, point, seed)),
                  "Value is not backed by the native FastNoise2 generator.");
            const glm::dvec3 lattice = glm::floor(point);
            check(noise::HashValue(static_cast<std::int64_t>(lattice.x),
                                  static_cast<std::int64_t>(lattice.y),
                                  static_cast<std::int64_t>(lattice.z), seed) ==
                      normalized(nativeSample(*white, lattice, seed)),
                  "Spectral white noise is not backed by FastNoise2.");
            for (const noise::FractalParameters fractal : {
                     noise::FractalParameters{1.0, 2.0, 0.5, 1},
                     {4.0, 2.0, 0.5, 5}, {0.5, 1.0, 1.0, 16}, {0.0, 0.0, 0.0, 3},
                     {2.0, 0.5, 1.5, 4}}) {
                for (auto* generator : std::array<FastNoise::Fractal<>*, 3>{
                         fbm.get(), valueFbm.get(), ridged.get()}) {
                    generator->SetOctaveCount(fractal.octaves);
                    generator->SetLacunarity(static_cast<float>(fractal.lacunarity));
                    generator->SetGain(static_cast<float>(fractal.persistence));
                }
                double total = 0.0;
                double amplitude = 1.0;
                for (int octave = 0; octave < fractal.octaves; ++octave) {
                    total += amplitude;
                    amplitude *= fractal.persistence;
                }
                const auto domain = point * fractal.frequency;
                check(noise::SimplexFbm(point, seed, fractal) ==
                          normalized(nativeSample(*fbm, domain, seed) / total),
                      "Simplex fBm differs from normalized native FastNoise2 fBm.");
                check(noise::ValueFbm(point, seed, fractal) ==
                          normalized(nativeSample(*valueFbm, domain, seed) / total),
                      "Value fBm differs from normalized native FastNoise2 fBm.");
                check(noise::Ridged(point, seed, fractal) ==
                          normalized(nativeSample(*ridged, domain, seed) / total),
                      "Ridged noise differs from normalized native FastNoise2 ridged fractals.");
            }
        }
    }
}

void testSeedsAndBounds() {
    const glm::dvec3 point(0.25, -0.75, 1.125);
    const auto baseline = noise::Simplex(point, 0);
    check(noise::Simplex(point, 1) != baseline &&
          noise::Simplex(point, 1ULL << 32) != baseline &&
          noise::Simplex(point, 0xffffffffffffffffULL) != baseline,
          "Noise ignored seed bits or mapped zero and maximum seeds to the same seed.");
    for (int i = 0; i < 100; ++i) {
        const auto p = point * static_cast<double>(i);
        for (double value : {noise::Simplex(p, i), noise::Value(p, i),
                             noise::SimplexFbm(p, i, {4, 2, 0.5, 5}),
                             noise::ValueFbm(p, i, {4, 2, 0.5, 5}),
                             noise::Ridged(p, i, {4, 2, 0.5, 5}),
                             noise::HashValue(i, -i, i * 2, i)}) {
            check(std::isfinite(value) && value >= -1.0 && value <= 1.0,
                  "Noise sample was non-finite or outside [-1, 1].");
        }
    }
}

void testInvalidInputs() {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    for (const auto point : {glm::dvec3(nan, 0, 0), glm::dvec3(0, inf, 0),
                             glm::dvec3(0, 0, -inf), glm::dvec3(1e9)}) {
        rejects([&] { noise::Simplex(point, 0); });
        rejects([&] { noise::Value(point, 0); });
        rejects([&] { noise::SimplexFbm(point, 0, {}); });
    }
    rejects([] { noise::HashValue(std::numeric_limits<std::int64_t>::max(), 0, 0, 0); });
    for (const noise::FractalParameters fractal : {
             noise::FractalParameters{nan, 2, 0.5, 4}, {1, inf, 0.5, 4},
             {1, 2, nan, 4}, {-1, 2, 0.5, 4}, {1, -2, 0.5, 4}, {1, 2, -0.5, 4},
             {1, 2, 0.5, 0}, {1, 2, 0.5, 17}, {1, 1e300, 0.5, 4},
             {1, 2, 1e300, 4}, {1, 1, 1e30, 16}, {1e7, 4, 0.5, 16}}) {
        rejects([&] { noise::SimplexFbm(glm::dvec3(1), 0, fractal); });
        rejects([&] { noise::ValueFbm(glm::dvec3(1), 0, fractal); });
        rejects([&] { noise::Ridged(glm::dvec3(1), 0, fractal); });
    }
}

void testThreadIsolation() {
    const auto run = [](std::uint64_t seed, noise::FractalParameters fractal) {
        double result = 0.0;
        for (int i = 0; i < 200; ++i) {
            const glm::dvec3 point(i * 0.1, -i * 0.2, i * 0.03);
            result += noise::SimplexFbm(point, seed, fractal) +
                      noise::ValueFbm(point, seed, fractal) + noise::Ridged(point, seed, fractal);
        }
        return result;
    };
    const noise::FractalParameters first{4, 2, 0.5, 6};
    const noise::FractalParameters second{2, 1.5, 0.75, 3};
    const auto expectedFirst = run(1, first);
    const auto expectedSecond = run(99, second);
    auto a = std::async(std::launch::async, run, 1, first);
    auto b = std::async(std::launch::async, run, 99, second);
    check(a.get() == expectedFirst && b.get() == expectedSecond &&
          run(1, first) == expectedFirst,
          "Cached FastNoise2 generators leaked settings between calls or threads.");
}

} // namespace

int main() {
    try {
        testNativeGenerators();
        testSeedsAndBounds();
        testInvalidInputs();
        testThreadIsolation();
        std::cout << "Noise tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Noise test failed: " << error.what() << '\n';
        return 1;
    }
}
