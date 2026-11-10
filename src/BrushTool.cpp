#include "BrushTool.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

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
    validate(grid, elevations, settings, elapsedSeconds);
    const double planetRadius = settings.distanceUnit == BrushDistanceUnit::Kilometres
                                    ? earthRadiusKilometres : earthRadiusMiles;
    const double outerRadius = settings.radius;
    const double innerRadius = outerRadius * (1.0 - settings.falloffPercent / 100.0);
    const auto candidates = grid.FindWithinRadius(center, outerRadius, planetRadius);
    std::vector<std::size_t> changed;
    changed.reserve(candidates.size());

    const double centerLength = glm::length(center);
    if (!std::isfinite(centerLength) || centerLength == 0.0) {
        throw std::invalid_argument("Brush center must be finite and nonzero.");
    }
    center /= centerLength;
    const auto& vertices = grid.GetVertices();
    for (const auto index : candidates) {
        const glm::dvec3 point = glm::normalize(glm::dvec3(vertices[index].position));
        const double angle = std::atan2(glm::length(glm::cross(center, point)),
                                        glm::dot(center, point));
        const double distance = angle * planetRadius;
        double falloff = 1.0;
        if (innerRadius < outerRadius && distance > innerRadius) {
            falloff = std::clamp((outerRadius - distance) / (outerRadius - innerRadius), 0.0, 1.0);
        }
        if (grid.HasSelection()) {
            falloff *= vertices[index].selectionWeight;
        }
        const float current = elevations[index];
        const float updated = operation(current, falloff);
        if (updated != elevations[index]) {
            elevations[index] = updated;
            changed.push_back(index);
        }
    }
    return changed;
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

BrushToolManager::BrushToolManager() {
    RegisterTool(std::make_unique<ElevationBrushTool>());
    RegisterTool(std::make_unique<RaiseLowerBrushTool>());
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
