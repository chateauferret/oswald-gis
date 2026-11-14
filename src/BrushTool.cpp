#include "BrushTool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {

constexpr double earthRadiusKilometres = 6371.0;
constexpr double earthRadiusMiles = 3958.7613;
constexpr float minimumElevation = -32767.0F;
constexpr float maximumElevation = 32767.0F;

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
    if (parameters.operation != SimplexNoiseSettings::Operation::Replace &&
        parameters.operation != SimplexNoiseSettings::Operation::Add &&
        parameters.operation != SimplexNoiseSettings::Operation::Subtract &&
        parameters.operation != SimplexNoiseSettings::Operation::Minimum &&
        parameters.operation != SimplexNoiseSettings::Operation::Maximum) {
        throw std::invalid_argument("Simplex noise operation is invalid.");
    }
    if (!validElevation(parameters.rangeBetween) || !validElevation(parameters.rangeAnd) ||
        !std::isfinite(parameters.frequency) || parameters.frequency <= 0.0F ||
        parameters.frequency > 1.0e6F || !std::isfinite(parameters.lacunarity) ||
        parameters.lacunarity < 1.0F || parameters.lacunarity > 4.0F ||
        !std::isfinite(parameters.persistence) || parameters.persistence < 0.0F ||
        parameters.persistence > 1.0F || parameters.octaves < 1 || parameters.octaves > 16) {
        throw std::invalid_argument("Simplex noise parameters are invalid.");
    }
    const auto weights = brushWeights(grid, center, elevations, settings, elapsedSeconds);
    const auto& vertices = grid.GetVertices();
    const float lowerBound = std::min(parameters.rangeBetween, parameters.rangeAnd);
    const float upperBound = std::max(parameters.rangeBetween, parameters.rangeAnd);
    std::vector<std::size_t> changed;
    changed.reserve(weights.size());
    for (const auto& [index, weight] : weights) {
        const float current = elevations[index];
        if (!validElevation(current)) {
            throw std::invalid_argument("Simplex noise input elevations must be finite and in range.");
        }
        const glm::dvec3 position = glm::normalize(glm::dvec3(vertices[index].position));
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
        const double normalizedNoise = (std::clamp(value, -1.0, 1.0) + 1.0) * 0.5;
        const float destination = static_cast<float>(
            lowerBound + normalizedNoise * (upperBound - lowerBound));
        float result = destination;
        switch (parameters.operation) {
        case SimplexNoiseSettings::Operation::Replace:
            break;
        case SimplexNoiseSettings::Operation::Add:
            result = current + destination;
            break;
        case SimplexNoiseSettings::Operation::Subtract:
            result = current - destination;
            break;
        case SimplexNoiseSettings::Operation::Minimum:
            result = std::min(current, destination);
            break;
        case SimplexNoiseSettings::Operation::Maximum:
            result = std::max(current, destination);
            break;
        }
        result = std::clamp(result, minimumElevation, maximumElevation);
        const float updated = current + (result - current) * static_cast<float>(weight);
        if (updated != current) {
            elevations[index] = updated;
            changed.push_back(index);
        }
    }
    return changed;
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
