#include "BrushTool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {

constexpr double earthRadiusKilometres = 6371.0;
constexpr double earthRadiusMiles = 3958.7613;
constexpr float minimumElevation = -32767.0F;
constexpr float maximumElevation = 32767.0F;

bool validElevation(float value);

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

double planetRadius(BrushDistanceUnit unit) {
    return unit == BrushDistanceUnit::Kilometres ? earthRadiusKilometres
                                                   : earthRadiusMiles;
}

std::vector<std::pair<std::size_t, double>> brushWeights(
    const SphericalGridSnapshot& grid, glm::dvec3 center,
    const std::vector<float>& elevations, const BrushSettings& settings,
    double elapsedSeconds) {
    validate(grid, elevations, settings, elapsedSeconds);
    const double centerLength = glm::length(center);
    if (!std::isfinite(centerLength) || centerLength == 0.0) {
        throw std::invalid_argument("Brush center must be finite and nonzero.");
    }
    center /= centerLength;
    const double radius = planetRadius(settings.distanceUnit);
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

std::uint32_t latticeHash(std::int64_t x, std::int64_t y, std::int64_t z,
                          std::uint64_t seed) {
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
    const auto& gradient = gradients[latticeHash(i, j, k, seed) % gradients.size()];
    const double squared = attenuation * attenuation;
    return squared * squared *
           (gradient.x * offset.x + gradient.y * offset.y + gradient.z * offset.z);
}

double simplexNoise(glm::dvec3 point, std::uint64_t seed) {
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

double hashValue(std::int64_t x, std::int64_t y, std::int64_t z, std::uint64_t seed) {
    return static_cast<double>(latticeHash(x, y, z, seed)) /
               static_cast<double>(std::numeric_limits<std::uint32_t>::max()) * 2.0 - 1.0;
}

double interpolationCurve(double value, ValueNoiseSettings::Interpolation interpolation) {
    switch (interpolation) {
    case ValueNoiseSettings::Interpolation::Linear:
        return value;
    case ValueNoiseSettings::Interpolation::Cubic:
        return value * value * (3.0 - 2.0 * value);
    case ValueNoiseSettings::Interpolation::Quintic:
        return value * value * value * (value * (value * 6.0 - 15.0) + 10.0);
    }
    throw std::invalid_argument("Value noise interpolation mode is invalid.");
}

double valueNoise(glm::dvec3 point, std::uint64_t seed,
                  ValueNoiseSettings::Interpolation interpolation) {
    const auto x0 = static_cast<std::int64_t>(std::floor(point.x));
    const auto y0 = static_cast<std::int64_t>(std::floor(point.y));
    const auto z0 = static_cast<std::int64_t>(std::floor(point.z));
    const double x = interpolationCurve(point.x - x0, interpolation);
    const double y = interpolationCurve(point.y - y0, interpolation);
    const double z = interpolationCurve(point.z - z0, interpolation);
    const double c000 = hashValue(x0, y0, z0, seed);
    const double c100 = hashValue(x0 + 1, y0, z0, seed);
    const double c010 = hashValue(x0, y0 + 1, z0, seed);
    const double c110 = hashValue(x0 + 1, y0 + 1, z0, seed);
    const double c001 = hashValue(x0, y0, z0 + 1, seed);
    const double c101 = hashValue(x0 + 1, y0, z0 + 1, seed);
    const double c011 = hashValue(x0, y0 + 1, z0 + 1, seed);
    const double c111 = hashValue(x0 + 1, y0 + 1, z0 + 1, seed);
    const double z0Blend = std::lerp(std::lerp(std::lerp(c000, c100, x),
                                               std::lerp(c010, c110, x), y),
                                     std::lerp(std::lerp(c001, c101, x),
                                               std::lerp(c011, c111, x), y), z);
    return z0Blend;
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
            simplexNoise(domain, octaveSeed ^ 0x9e3779b97f4a7c15ULL),
            simplexNoise(domain, octaveSeed ^ 0xbf58476d1ce4e5b9ULL),
            simplexNoise(domain, octaveSeed ^ 0x94d049bb133111ebULL)) * amplitude;
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
std::vector<std::size_t> applyNoise(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& brushSettings, double elapsedSeconds, const NoiseSettings& parameters,
    Sampler sample) {
    validateNoiseSettings(parameters);
    const auto weights = brushWeights(grid, center, elevations, brushSettings, elapsedSeconds);
    const float lower = std::min(parameters.rangeBetween, parameters.rangeAnd);
    const float upper = std::max(parameters.rangeBetween, parameters.rangeAnd);
    std::vector<std::size_t> changed;
    changed.reserve(weights.size());
    for (const auto& [index, weight] : weights) {
        const float current = elevations[index];
        if (!validElevation(current)) {
            throw std::invalid_argument("Noise input elevations must be finite and in range.");
        }
        const auto position = warpedPosition(
            glm::normalize(glm::dvec3(grid.GetVertices()[index].position)), parameters);
        const float result = destinationForNoise(parameters.operation,
                                                 sample(position, index), current, lower, upper);
        const float updated = current + (result - current) * static_cast<float>(weight);
        if (updated != current) {
            elevations[index] = updated;
            changed.push_back(index);
        }
    }
    return changed;
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
                field[index(x, y, z)] = Complex(hashValue(x, y, z, seed), 0.0);
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
        return current + (result - current) * static_cast<float>(falloff);
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
        double frequency = parameters.frequency;
        double amplitude = 1.0;
        double totalAmplitude = 0.0;
        double value = 0.0;
        for (int octave = 0; octave < parameters.octaves; ++octave) {
            value += simplexNoise(position * frequency, parameters.seed) * amplitude;
            totalAmplitude += amplitude;
            frequency *= parameters.lacunarity;
            amplitude *= parameters.persistence;
        }
        value /= totalAmplitude;
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
            double frequency = parameters.frequency;
            double amplitude = 1.0;
            double totalAmplitude = 0.0;
            double value = 0.0;
            for (int octave = 0; octave < parameters.octaves; ++octave) {
                value += valueNoise(position * frequency, parameters.seed, parameters.interpolation) *
                         amplitude;
                totalAmplitude += amplitude;
                frequency *= parameters.lacunarity;
                amplitude *= parameters.persistence;
            }
            return std::clamp(value / totalAmplitude * parameters.amplitude, -1.0, 1.0);
        });
}

std::vector<std::size_t> RidgeNoiseBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    const auto& parameters = settings_;
    validateFractalSettings(parameters);
    if (!std::isfinite(parameters.ridgeOffset) || parameters.ridgeOffset < 0.0F ||
        parameters.ridgeOffset > 2.0F || !std::isfinite(parameters.gainExponent) ||
        parameters.gainExponent <= 0.0F || parameters.gainExponent > 8.0F) {
        throw std::invalid_argument("Ridge noise offset or gain exponent is invalid.");
    }
    return applyNoise(grid, center, elevations, settings, elapsedSeconds, parameters,
        [&](glm::dvec3 position, std::size_t) {
            double frequency = parameters.frequency;
            double amplitude = 1.0;
            double totalAmplitude = 0.0;
            double result = 0.0;
            double feedback = 1.0;
            for (int octave = 0; octave < parameters.octaves; ++octave) {
                const double base = simplexNoise(position * frequency, parameters.seed);
                const double ridge = std::clamp(parameters.ridgeOffset - std::abs(base), 0.0, 1.0);
                const double signal = std::pow(ridge, parameters.gainExponent) * feedback;
                result += signal * amplitude;
                totalAmplitude += amplitude;
                feedback = std::clamp(signal * 2.0, 0.0, 1.0);
                frequency *= parameters.lacunarity;
                amplitude *= parameters.persistence;
            }
            return std::clamp(2.0 * result / totalAmplitude - 1.0, -1.0, 1.0);
        });
}

std::vector<std::size_t> PowerLawNoiseBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    const auto& parameters = settings_;
    validateNoiseSettings(parameters);
    if (!std::isfinite(parameters.frequency) || parameters.frequency <= 0.0F ||
        parameters.frequency > 1.0e6F || !std::isfinite(parameters.spectralExponent) ||
        parameters.spectralExponent < -4.0F || parameters.spectralExponent > 8.0F ||
        parameters.gridResolution < 8 || parameters.gridResolution > 128 ||
        (parameters.gridResolution & (parameters.gridResolution - 1)) != 0) {
        throw std::invalid_argument("Power-law spectral noise parameters are invalid.");
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
    return applyNoise(grid, center, elevations, settings, elapsedSeconds, parameters,
        [&](glm::dvec3 position, std::size_t) {
            return sampleSpectralField(*field, parameters.gridResolution, position,
                                       parameters.frequency);
        });
}

std::vector<std::size_t> GaussianBlurBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    const double radius = planetRadius(settings_.distanceUnit);
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
    const auto& vertices = grid.GetVertices();
    std::vector<std::size_t> changed;
    changed.reserve(weights.size());
    for (const auto& [index, brushWeight] : weights) {
        const glm::dvec3 point = glm::normalize(glm::dvec3(vertices[index].position));
        const auto neighbors = grid.FindWithinRadius(point, settings_.range, radius);
        double weightedSum = 0.0;
        double weightSum = 0.0;
        for (const auto neighbor : neighbors) {
            const glm::dvec3 neighborPoint =
                glm::normalize(glm::dvec3(vertices[neighbor].position));
            const double angle = std::atan2(glm::length(glm::cross(point, neighborPoint)),
                          glm::dot(point, neighborPoint));
            const double distance = angle * radius;
            const double ratio = distance / settings_.sigma;
            const double gaussianWeight = std::exp(-0.5 * ratio * ratio);
            weightedSum += static_cast<double>(source[neighbor]) * gaussianWeight;
            weightSum += gaussianWeight;
        }
        const float destination = static_cast<float>(weightedSum / weightSum);
        const float updated = source[index] +
            (destination - source[index]) * static_cast<float>(brushWeight);
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
        return current + (destination - current) * static_cast<float>(weight);
    });
}

BrushToolManager::BrushToolManager() {
    RegisterTool(std::make_unique<ElevationBrushTool>());
    RegisterTool(std::make_unique<RaiseLowerBrushTool>());
    RegisterTool(std::make_unique<SimplexNoiseBrushTool>());
    RegisterTool(std::make_unique<ValueNoiseBrushTool>());
    RegisterTool(std::make_unique<RidgeNoiseBrushTool>());
    RegisterTool(std::make_unique<PowerLawNoiseBrushTool>());
    RegisterTool(std::make_unique<GaussianBlurBrushTool>());
    RegisterTool(std::make_unique<ClampBrushTool>());
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

ElevationBrushTool& BrushToolManager::GetElevationBrush() {
    auto* brush = dynamic_cast<ElevationBrushTool*>(tools_.at(selectedIndex_).get());
    if (brush == nullptr) {
        throw std::logic_error("The selected brush is not an elevation brush.");
    }
    return *brush;
}

const ElevationBrushTool& BrushToolManager::GetElevationBrush() const {
    auto* brush = dynamic_cast<const ElevationBrushTool*>(tools_.at(selectedIndex_).get());
    if (brush == nullptr) {
        throw std::logic_error("The selected brush is not an elevation brush.");
    }
    return *brush;
}
