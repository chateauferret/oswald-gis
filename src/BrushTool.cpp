#include "BrushTool.hpp"

#include "Noise.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {

constexpr float minimumElevation = -32767.0F;
constexpr float maximumElevation = 32767.0F;

bool validElevation(float value);

// Blends in double precision and clamps, because float rounding of
// (destination - current) can push the result just past the valid range.
float blendElevation(float current, float destination, double weight) {
    const double blended = static_cast<double>(current) +
        (static_cast<double>(destination) - static_cast<double>(current)) * weight;
    return static_cast<float>(std::clamp(blended, static_cast<double>(minimumElevation),
                                         static_cast<double>(maximumElevation)));
}

void validate(const SphericalGridSnapshot& grid, const std::vector<float>& elevations,
              const BrushSettings& settings, double elapsedSeconds) {
    if (elevations.size() != grid.GetVertices().size()) {
        throw std::invalid_argument("Brush elevation count must match the grid vertex count.");
    }
    if (!std::isfinite(settings.radius) || settings.radius <= 0.0 ||
        !std::isfinite(settings.falloffPercent) || settings.falloffPercent < 0.0 ||
        settings.falloffPercent > 100.0 || !std::isfinite(elapsedSeconds) ||
        elapsedSeconds < 0.0) {
        throw std::invalid_argument("Brush radius, falloff, or elapsed time is invalid.");
    }
}

std::vector<std::pair<std::size_t, double>> brushWeights(
    const SphericalGridSnapshot& grid, glm::dvec3 center,
    const std::vector<float>& elevations, const BrushSettings& settings,
    double elapsedSeconds) {
    validate(grid, elevations, settings, elapsedSeconds);
    if (settings.coverage == BrushCoverage::Selection) {
        std::vector<std::pair<std::size_t, double>> result;
        const auto& vertices = grid.GetVertices();
        result.reserve(vertices.size());
        for (std::size_t index = 0; index < vertices.size(); ++index) {
            const double weight = grid.HasSelection() ? vertices[index].selectionWeight : 1.0;
            if (weight > 0.0) {
                result.emplace_back(index, weight);
            }
        }
        return result;
    }
    const double centerLength = glm::length(center);
    if (!std::isfinite(centerLength) || centerLength == 0.0) {
        throw std::invalid_argument("Brush center must be finite and nonzero.");
    }
    center /= centerLength;
    const double radius = PlanetRadiusInUnit(grid, settings.distanceUnit);
    const double outerRadius = settings.radius;
    const double innerRadius = outerRadius * (1.0 - settings.falloffPercent / 100.0);
    const auto candidates = grid.FindWithinRadius(center, outerRadius, radius);
    std::vector<std::pair<std::size_t, double>> result;
    result.reserve(candidates.size());
    const auto& vertices = grid.GetVertices();
    for (const auto index : candidates) {
        const glm::dvec3 point = glm::normalize(glm::dvec3(vertices[index].position));
        const double angle = std::atan2(glm::length(glm::cross(center, point)),
                                        glm::dot(center, point));
        const double distance = angle * radius;
        double weight = 1.0;
        if (innerRadius < outerRadius && distance > innerRadius) {
            weight = std::clamp((outerRadius - distance) / (outerRadius - innerRadius),
                                0.0, 1.0);
        }
        if (grid.HasSelection()) {
            weight *= vertices[index].selectionWeight;
        }
        if (weight > 0.0) {
            result.emplace_back(index, weight);
        }
    }
    return result;
}

float applyOperation(BrushOperation operation, float current, float value) {
    float destination = current;
    switch (operation) {
    case BrushOperation::Replace:
        destination = value;
        break;
    case BrushOperation::Minimum:
        destination = std::min(current, value);
        break;
    case BrushOperation::Maximum:
        destination = std::max(current, value);
        break;
    }
    return std::clamp(destination, minimumElevation, maximumElevation);
}

template <typename Operation>
std::vector<std::size_t> applyBrush(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds, Operation operation) {
    const auto weights = brushWeights(grid, center, elevations, settings, elapsedSeconds);
    std::vector<std::size_t> changed;
    changed.reserve(weights.size());
    for (const auto& [index, falloff] : weights) {
        const float current = elevations[index];
        const float updated = operation(current, falloff);
        if (updated != elevations[index]) {
            elevations[index] = updated;
            changed.push_back(index);
        }
    }
    return changed;
}

bool validNoiseOperation(NoiseSettings::Operation operation) {
    return operation == NoiseSettings::Operation::Replace ||
           operation == NoiseSettings::Operation::Add ||
           operation == NoiseSettings::Operation::Subtract ||
           operation == NoiseSettings::Operation::Minimum ||
           operation == NoiseSettings::Operation::Maximum;
}

void validateNoiseSettings(const NoiseSettings& settings) {
    if (!validNoiseOperation(settings.operation) || !validElevation(settings.rangeBetween) ||
        !validElevation(settings.rangeAnd) ||
        !std::isfinite(settings.warpFrequency) || settings.warpFrequency <= 0.0F ||
        settings.warpFrequency > 1.0e4F || !std::isfinite(settings.warpStrength) ||
        settings.warpStrength < 0.0F || settings.warpStrength > 10.0F ||
        !std::isfinite(settings.warpLacunarity) || settings.warpLacunarity < 1.0F ||
        settings.warpLacunarity > 4.0F || !std::isfinite(settings.warpPersistence) ||
        settings.warpPersistence < 0.0F || settings.warpPersistence > 1.0F ||
        settings.warpOctaves < 1 || settings.warpOctaves > 16) {
        throw std::invalid_argument("Noise brush parameters are invalid.");
    }
}

void validateFractalSettings(const FractalNoiseSettings& settings) {
    validateNoiseSettings(settings);
    if (!std::isfinite(settings.frequency) || settings.frequency <= 0.0F ||
        settings.frequency > 1.0e6F || !std::isfinite(settings.lacunarity) ||
        settings.lacunarity < 1.0F || settings.lacunarity > 4.0F ||
        !std::isfinite(settings.persistence) || settings.persistence < 0.0F ||
        settings.persistence > 1.0F || settings.octaves < 1 || settings.octaves > 16) {
        throw std::invalid_argument("Fractal noise parameters are invalid.");
    }
}

noise::FractalParameters fractalOf(const FractalNoiseSettings& settings) {
    return {settings.frequency, settings.lacunarity, settings.persistence, settings.octaves};
}

glm::dvec3 warpedPosition(glm::dvec3 position, const NoiseSettings& settings) {
    if (!settings.domainWarpEnabled || settings.warpStrength == 0.0F) {
        return position;
    }
    glm::dvec3 displacement(0.0);
    double frequency = settings.warpFrequency;
    double amplitude = 1.0;
    double totalAmplitude = 0.0;
    for (int octave = 0; octave < settings.warpOctaves; ++octave) {
        const glm::dvec3 domain = position * frequency;
        const auto octaveSeed = settings.seed +
            (0x9e3779b97f4a7c15ULL * static_cast<std::uint64_t>(octave + 1));
        displacement += glm::dvec3(
            noise::Simplex(domain, octaveSeed ^ 0x9e3779b97f4a7c15ULL),
            noise::Simplex(domain, octaveSeed ^ 0xbf58476d1ce4e5b9ULL),
            noise::Simplex(domain, octaveSeed ^ 0x94d049bb133111ebULL)) * amplitude;
        totalAmplitude += amplitude;
        frequency *= settings.warpLacunarity;
        amplitude *= settings.warpPersistence;
    }
    displacement /= totalAmplitude;
    return position + displacement * static_cast<double>(settings.warpStrength);
}

float destinationForNoise(NoiseSettings::Operation operation, double noise,
                          float current, float lower, float upper) {
    const double normalized = (std::clamp(noise, -1.0, 1.0) + 1.0) * 0.5;
    const float generated = static_cast<float>(lower + normalized * (upper - lower));
    float result = generated;
    switch (operation) {
    case NoiseSettings::Operation::Replace: break;
    case NoiseSettings::Operation::Add: result = current + generated; break;
    case NoiseSettings::Operation::Subtract: result = current - generated; break;
    case NoiseSettings::Operation::Minimum: result = std::min(current, generated); break;
    case NoiseSettings::Operation::Maximum: result = std::max(current, generated); break;
    }
    return std::clamp(result, minimumElevation, maximumElevation);
}

template <typename Sampler>
std::vector<std::size_t> applyNoiseSamples(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& brushSettings, double elapsedSeconds, const NoiseSettings& parameters,
    Sampler&& sample, BrushApplication application = BrushApplication::Noise) {
    validateNoiseSettings(parameters);
    const auto weights = brushWeights(grid, center, elevations, brushSettings, elapsedSeconds);
    center = glm::normalize(center);
    const double radius = PlanetRadiusInUnit(grid, brushSettings.distanceUnit);
    const float lower = std::min(parameters.rangeBetween, parameters.rangeAnd);
    const float upper = std::max(parameters.rangeBetween, parameters.rangeAnd);
    // Sample every vertex before writing so a throwing sampler leaves elevations intact.
    std::vector<std::pair<std::size_t, float>> updates;
    updates.reserve(weights.size());
    for (const auto& [index, weight] : weights) {
        const float current = elevations[index];
        if (!validElevation(current)) {
            throw std::invalid_argument("Noise input elevations must be finite and in range.");
        }
        NoiseSample noiseSample;
        noiseSample.location = glm::normalize(glm::dvec3(grid.GetVertices()[index].position));
        noiseSample.position = warpedPosition(noiseSample.location, parameters);
        noiseSample.index = index;
        noiseSample.elevation = current;
        noiseSample.weight = weight;
        noiseSample.distance = brushSettings.coverage == BrushCoverage::Selection ? 0.0 :
            radius * std::atan2(
            glm::length(glm::cross(center, noiseSample.location)),
            glm::dot(center, noiseSample.location));
        const double value = sample(noiseSample);
        if (!std::isfinite(value)) {
            throw std::invalid_argument("Brush sampler returned a non-finite value.");
        }
        float updated;
        if (application == BrushApplication::Noise) {
            const float result = destinationForNoise(parameters.operation, value, current, lower, upper);
            updated = blendElevation(current, result, weight);
        } else if (application == BrushApplication::Delta) {
            updated = static_cast<float>(std::clamp(current + value * weight,
                                                    -32767.0, 32767.0));
        } else {
            updated = blendElevation(current, static_cast<float>(std::clamp(value, -32767.0, 32767.0)),
                                     weight);
        }
        if (updated != current) {
            updates.emplace_back(index, updated);
        }
    }
    std::vector<std::size_t> changed;
    changed.reserve(updates.size());
    for (const auto& [index, updated] : updates) {
        elevations[index] = updated;
        changed.push_back(index);
    }
    return changed;
}

template <typename Sampler>
std::vector<std::size_t> applyNoise(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& brushSettings, double elapsedSeconds, const NoiseSettings& parameters,
    Sampler sample) {
    return applyNoiseSamples(grid, center, elevations, brushSettings, elapsedSeconds, parameters,
                             [&](const NoiseSample& noiseSample) {
                                 return sample(noiseSample.position, noiseSample.index);
                             });
}

using Complex = std::complex<double>;

void fft(std::vector<Complex>& values, bool inverse) {
    const std::size_t count = values.size();
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        std::size_t bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * std::numbers::pi / length;
        const Complex root(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < count; start += length) {
            Complex factor(1.0, 0.0);
            for (std::size_t offset = 0; offset < length / 2; ++offset) {
                const Complex even = values[start + offset];
                const Complex odd = values[start + offset + length / 2] * factor;
                values[start + offset] = even + odd;
                values[start + offset + length / 2] = even - odd;
                factor *= root;
            }
        }
    }
    if (inverse) {
        for (auto& value : values) value /= static_cast<double>(count);
    }
}

void transformAxis(std::vector<Complex>& field, int resolution, int axis, bool inverse) {
    std::vector<Complex> line(static_cast<std::size_t>(resolution));
    const auto index = [resolution](int x, int y, int z) {
        return (static_cast<std::size_t>(z) * resolution + y) * resolution + x;
    };
    for (int a = 0; a < resolution; ++a) {
        for (int b = 0; b < resolution; ++b) {
            for (int i = 0; i < resolution; ++i) {
                const int x = axis == 0 ? i : a;
                const int y = axis == 1 ? i : axis == 0 ? a : b;
                const int z = axis == 2 ? i : b;
                line[static_cast<std::size_t>(i)] = field[index(x, y, z)];
            }
            fft(line, inverse);
            for (int i = 0; i < resolution; ++i) {
                const int x = axis == 0 ? i : a;
                const int y = axis == 1 ? i : axis == 0 ? a : b;
                const int z = axis == 2 ? i : b;
                field[index(x, y, z)] = line[static_cast<std::size_t>(i)];
            }
        }
    }
}

std::shared_ptr<const std::vector<float>> buildSpectralField(int resolution,
                                                             std::uint64_t seed,
                                                             float exponent) {
    const std::size_t count = static_cast<std::size_t>(resolution) * resolution * resolution;
    std::vector<Complex> field(count);
    const auto index = [resolution](int x, int y, int z) {
        return (static_cast<std::size_t>(z) * resolution + y) * resolution + x;
    };
    for (int z = 0; z < resolution; ++z) {
        for (int y = 0; y < resolution; ++y) {
            for (int x = 0; x < resolution; ++x) {
                field[index(x, y, z)] = Complex(noise::HashValue(x, y, z, seed), 0.0);
            }
        }
    }
    transformAxis(field, resolution, 0, false);
    transformAxis(field, resolution, 1, false);
    transformAxis(field, resolution, 2, false);
    for (int z = 0; z < resolution; ++z) {
        const int kz = z <= resolution / 2 ? z : z - resolution;
        for (int y = 0; y < resolution; ++y) {
            const int ky = y <= resolution / 2 ? y : y - resolution;
            for (int x = 0; x < resolution; ++x) {
                const int kx = x <= resolution / 2 ? x : x - resolution;
                const double magnitude = std::sqrt(static_cast<double>(kx * kx + ky * ky + kz * kz));
                if (magnitude == 0.0) {
                    field[index(x, y, z)] = Complex(0.0, 0.0);
                } else {
                    field[index(x, y, z)] *= std::pow(magnitude, -static_cast<double>(exponent) / 2.0);
                }
            }
        }
    }
    transformAxis(field, resolution, 0, true);
    transformAxis(field, resolution, 1, true);
    transformAxis(field, resolution, 2, true);
    double maximum = 0.0;
    for (const auto& value : field) maximum = std::max(maximum, std::abs(value.real()));
    auto result = std::make_shared<std::vector<float>>(count, 0.0F);
    if (maximum > 0.0) {
        for (std::size_t i = 0; i < count; ++i) {
            (*result)[i] = static_cast<float>(std::clamp(field[i].real() / maximum, -1.0, 1.0));
        }
    }
    return result;
}

double sampleSpectralField(const std::vector<float>& field, int resolution,
                           glm::dvec3 position, double frequency) {
    const auto coordinate = [resolution, frequency](double value) {
        double result = (value * 0.5 + 0.5) * resolution * frequency;
        result = std::fmod(result, static_cast<double>(resolution));
        return result < 0.0 ? result + resolution : result;
    };
    const glm::dvec3 p(coordinate(position.x), coordinate(position.y), coordinate(position.z));
    const int x0 = static_cast<int>(std::floor(p.x));
    const int y0 = static_cast<int>(std::floor(p.y));
    const int z0 = static_cast<int>(std::floor(p.z));
    const double fx = p.x - x0;
    const double fy = p.y - y0;
    const double fz = p.z - z0;
    const auto wrap = [resolution](int value) { return (value % resolution + resolution) % resolution; };
    const auto at = [&](int x, int y, int z) {
        return static_cast<double>(field[(static_cast<std::size_t>(wrap(z)) * resolution +
                                          wrap(y)) * resolution + wrap(x)]);
    };
    const double z0Value = std::lerp(std::lerp(std::lerp(at(x0, y0, z0), at(x0 + 1, y0, z0), fx),
                                                std::lerp(at(x0, y0 + 1, z0), at(x0 + 1, y0 + 1, z0), fx), fy),
                                      std::lerp(std::lerp(at(x0, y0, z0 + 1), at(x0 + 1, y0, z0 + 1), fx),
                                                std::lerp(at(x0, y0 + 1, z0 + 1), at(x0 + 1, y0 + 1, z0 + 1), fx), fy), fz);
    return z0Value;
}

bool validElevation(float value) {
    return std::isfinite(value) && value >= minimumElevation && value <= maximumElevation;
}

} // namespace

std::vector<std::size_t> ApplyNoiseSampler(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& brushSettings, double elapsedSeconds, const NoiseSettings& parameters,
    const NoiseSampler& sample, BrushApplication application) {
    if (!sample) {
        throw std::invalid_argument("Noise sampler is empty.");
    }
    return applyNoiseSamples(grid, center, elevations, brushSettings, elapsedSeconds, parameters,
                             sample, application);
}

std::vector<std::size_t> ElevationBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    if (!std::isfinite(settings.value) || settings.value < minimumElevation ||
        settings.value > maximumElevation) {
        throw std::invalid_argument("Brush elevation value is invalid.");
    }
    if (settings.operation != BrushOperation::Replace &&
        settings.operation != BrushOperation::Minimum &&
        settings.operation != BrushOperation::Maximum) {
        throw std::invalid_argument("Elevation brush operation is invalid.");
    }
    return applyBrush(grid, center, elevations, settings, elapsedSeconds,
                      [&](float current, double falloff) {
        const float result = applyOperation(settings.operation, current, settings.value);
        return blendElevation(current, result, falloff);
    });
}

std::vector<std::size_t> RaiseLowerBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    if (!std::isfinite(settings.raiseLowerRate) || settings.raiseLowerRate <= 0.0) {
        throw std::invalid_argument("Raise/Lower brush speed must be finite and positive.");
    }
    if (settings.raiseLowerOperation != RaiseLowerOperation::Raise &&
        settings.raiseLowerOperation != RaiseLowerOperation::Lower) {
        throw std::invalid_argument("Raise/Lower brush operation is invalid.");
    }
    if (!std::isfinite(settings.intensityPercent) || settings.intensityPercent < 0.0 ||
        settings.intensityPercent > 100.0) {
        throw std::invalid_argument("Raise/Lower brush intensity is invalid.");
    }
    const double direction = settings.raiseLowerOperation == RaiseLowerOperation::Raise ? 1.0 : -1.0;
    return applyBrush(grid, center, elevations, settings, elapsedSeconds,
                      [&](float current, double falloff) {
        const double amount = settings.raiseLowerRate *
                              (settings.intensityPercent / 100.0 * falloff) * elapsedSeconds;
        return static_cast<float>(std::clamp(current + direction * amount,
                                            static_cast<double>(minimumElevation),
                                            static_cast<double>(maximumElevation)));
    });
}

std::vector<std::size_t> SimplexNoiseBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    const auto& parameters = settings_;
    validateFractalSettings(parameters);
    if (!std::isfinite(parameters.frequency) || parameters.frequency <= 0.0F ||
        parameters.frequency > 1.0e6F || !std::isfinite(parameters.lacunarity) ||
        parameters.lacunarity < 1.0F || parameters.lacunarity > 4.0F ||
        !std::isfinite(parameters.persistence) || parameters.persistence < 0.0F ||
        parameters.persistence > 1.0F || parameters.octaves < 1 || parameters.octaves > 16) {
        throw std::invalid_argument("Simplex noise parameters are invalid.");
    }
    return applyNoise(grid, center, elevations, settings, elapsedSeconds, parameters,
        [&](glm::dvec3 position, std::size_t) {
        double value = noise::SimplexFbm(position, parameters.seed, fractalOf(parameters));
        if (parameters.billowed) {
            value = 2.0 * std::abs(value) - 1.0;
        }
        return std::clamp(value, -1.0, 1.0);
    });
}

std::vector<std::size_t> ValueNoiseBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    const auto& parameters = settings_;
    validateFractalSettings(parameters);
    if (!std::isfinite(parameters.amplitude) || parameters.amplitude < 0.0F ||
        parameters.amplitude > 2.0F) {
        throw std::invalid_argument("Value noise amplitude must be in [0, 2].");
    }
    return applyNoise(grid, center, elevations, settings, elapsedSeconds, parameters,
        [&](glm::dvec3 position, std::size_t) {
            const double value = noise::ValueFbm(position, parameters.seed, fractalOf(parameters));
            return std::clamp(value * parameters.amplitude, -1.0, 1.0);
        });
}

std::vector<std::size_t> RidgeNoiseBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    const auto& parameters = settings_;
    validateFractalSettings(parameters);
    return applyNoise(grid, center, elevations, settings, elapsedSeconds, parameters,
        [&](glm::dvec3 position, std::size_t) {
            return noise::Ridged(position, parameters.seed, fractalOf(parameters));
        });
}

std::vector<std::size_t> PowerLawNoiseBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    const auto& parameters = settings_;
    validateNoiseSettings(parameters);
    return applyNoise(grid, center, elevations, settings, elapsedSeconds, parameters,
        [&](glm::dvec3 position, std::size_t) { return Sample(position, parameters); });
}

double PowerLawNoiseBrushTool::Sample(glm::dvec3 position,
                                      const PowerLawNoiseSettings& parameters) const {
    if (!std::isfinite(parameters.frequency) || parameters.frequency <= 0.0F ||
        parameters.frequency > 1.0e6F || !std::isfinite(parameters.spectralExponent) ||
        parameters.spectralExponent < -4.0F || parameters.spectralExponent > 8.0F ||
        parameters.gridResolution < 8 || parameters.gridResolution > 128 ||
        (parameters.gridResolution & (parameters.gridResolution - 1)) != 0) {
        throw std::invalid_argument("Power-law spectral noise parameters are invalid.");
    }
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        glm::any(glm::greaterThan(glm::abs(position), glm::dvec3(1e8)))) {
        throw std::invalid_argument("Spectral noise coordinates must be finite and within +/-1e8.");
    }
    std::shared_ptr<const std::vector<float>> field;
    {
        std::lock_guard lock(cacheMutex_);
        if (!cachedField_ || cachedSeed_ != parameters.seed ||
            cachedResolution_ != parameters.gridResolution ||
            cachedExponent_ != parameters.spectralExponent) {
            cachedField_ = buildSpectralField(parameters.gridResolution, parameters.seed,
                                              parameters.spectralExponent);
            cachedSeed_ = parameters.seed;
            cachedResolution_ = parameters.gridResolution;
            cachedExponent_ = parameters.spectralExponent;
        }
        field = cachedField_;
    }
    return sampleSpectralField(*field, parameters.gridResolution, position, parameters.frequency);
}

double SampleGaussian(const SphericalGridSnapshot& grid, const std::vector<float>& source,
                      std::size_t index, const GaussianBlurSettings& settings) {
    if (source.size() != grid.GetVertices().size() || index >= source.size() ||
        !std::isfinite(settings.range) || settings.range <= 0.0 ||
        !std::isfinite(settings.sigma) || settings.sigma <= 0.0F) {
        throw std::invalid_argument("Gaussian blur needs matching elevations and positive finite range/sigma.");
    }
    const auto& vertices = grid.GetVertices();
    const double radius = PlanetRadiusInUnit(grid, settings.distanceUnit);
    const glm::dvec3 point = glm::normalize(glm::dvec3(vertices[index].position));
    const auto neighbors = grid.FindWithinRadius(point, settings.range, radius);
    double weightedSum = 0.0;
    double weightSum = 0.0;
    for (const auto neighbor : neighbors) {
        if (!validElevation(source[neighbor])) {
            throw std::invalid_argument("Gaussian blur input elevations must be finite and in range.");
        }
        const glm::dvec3 neighborPoint = glm::normalize(glm::dvec3(vertices[neighbor].position));
        const double distance = radius * std::atan2(glm::length(glm::cross(point, neighborPoint)),
                                                    glm::dot(point, neighborPoint));
        const double ratio = distance / settings.sigma;
        const double gaussianWeight = std::exp(-0.5 * ratio * ratio);
        weightedSum += static_cast<double>(source[neighbor]) * gaussianWeight;
        weightSum += gaussianWeight;
    }
    if (!(weightSum > 0.0)) {
        throw std::runtime_error("Gaussian blur neighbourhood has no positive weight.");
    }
    return weightedSum / weightSum;
}

std::vector<std::size_t> GaussianBlurBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    if (!std::isfinite(settings_.range) || settings_.range <= 0.0 ||
        !std::isfinite(settings_.sigma) || settings_.sigma <= 0.0F) {
        throw std::invalid_argument("Gaussian blur range and sigma must be finite and positive.");
    }
    const auto weights = brushWeights(grid, center, elevations, settings, elapsedSeconds);
    const std::vector<float> source = elevations;
    for (const float value : source) {
        if (!validElevation(value)) {
            throw std::invalid_argument("Gaussian blur input elevations must be finite and in range.");
        }
    }
    std::vector<std::size_t> changed;
    changed.reserve(weights.size());
    for (const auto& [index, brushWeight] : weights) {
        const float destination = static_cast<float>(SampleGaussian(grid, source, index, settings_));
        const float updated = blendElevation(source[index], destination, brushWeight);
        if (updated != source[index]) {
            elevations[index] = updated;
            changed.push_back(index);
        }
    }
    return changed;
}

std::vector<std::size_t> ClampBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    if (!std::isfinite(settings_.minimum) || !std::isfinite(settings_.maximum) ||
        settings_.minimum < minimumElevation || settings_.maximum > maximumElevation ||
        settings_.minimum > settings_.maximum) {
        throw std::invalid_argument("Clamp minimum and maximum must be ordered within [-32767, 32767].");
    }
    return applyBrush(grid, center, elevations, settings, elapsedSeconds,
    [&](float current, double weight) {
        if (!validElevation(current)) {
            throw std::invalid_argument("Clamp input elevations must be finite and in range.");
        }
        const float destination = std::clamp(current, settings_.minimum, settings_.maximum);
        return blendElevation(current, destination, weight);
    });
}

void BrushToolManager::ReplaceTools(std::vector<std::unique_ptr<BrushTool>> tools, std::string_view retain) {
    const auto selected = tools_.empty() ? std::string{} : std::string(GetSelectedTool().GetName());
    BrushToolManager replacement;
    for (auto& tool : tools) {
        replacement.RegisterTool(std::move(tool));
    }
    if (!retain.empty()) {
        if (std::any_of(replacement.tools_.begin(), replacement.tools_.end(),
                        [&](const auto& tool) { return tool->GetName() == retain; })) {
            throw std::invalid_argument("Replacement tools duplicate the retained brush.");
        }
        const auto retained = std::find_if(tools_.begin(), tools_.end(),
            [&](const auto& tool) { return tool->GetName() == retain; });
        if (retained == tools_.end()) {
            throw std::invalid_argument("The retained brush is not registered.");
        }
        replacement.RegisterTool(std::move(*retained));
    }
    if (replacement.tools_.empty()) {
        throw std::invalid_argument("The brush registry needs at least one tool.");
    }
    replacement.SelectTool(selected);
    tools_ = std::move(replacement.tools_);
    selectedIndex_ = replacement.selectedIndex_;
}

void BrushToolManager::RegisterTool(std::unique_ptr<BrushTool> tool) {
    if (!tool) {
        throw std::invalid_argument("Cannot register an empty brush tool.");
    }
    if (std::any_of(tools_.begin(), tools_.end(), [&](const auto& registered) {
            return registered->GetName() == tool->GetName();
        })) {
        throw std::invalid_argument("Brush tool names must be unique.");
    }
    tools_.push_back(std::move(tool));
}

bool BrushToolManager::SelectTool(std::string_view name) {
    const auto found = std::find_if(tools_.begin(), tools_.end(), [&](const auto& tool) {
        return tool->GetName() == name;
    });
    if (found == tools_.end()) {
        return false;
    }
    selectedIndex_ = static_cast<std::size_t>(std::distance(tools_.begin(), found));
    return true;
}
