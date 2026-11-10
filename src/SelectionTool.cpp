#include "SelectionTool.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {

constexpr double earthRadiusKilometres = 6371.0;
constexpr double earthRadiusMiles = 3958.7613;

glm::dvec3 unitDirection(glm::dvec3 direction) {
    const double length = glm::length(direction);
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z) || !std::isfinite(length) || length == 0.0) {
        throw std::invalid_argument("Selection directions must be finite and nonzero.");
    }
    return direction / length;
}

double sphericalWinding(glm::dvec3 point, std::span<const glm::dvec3> polygon) {
    double winding = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[i];
        const auto b = polygon[(i + 1) % polygon.size()];
        if (glm::length(glm::cross(point, a)) < 1e-12 ||
            glm::length(glm::cross(point, b)) < 1e-12) {
            if (glm::dot(point, a) > 0.0 || glm::dot(point, b) > 0.0) {
                return std::numeric_limits<double>::infinity();
            }
        }
        const double numerator = glm::dot(point, glm::cross(a, b));
        const double denominator = glm::dot(a, b) -
                                   glm::dot(point, a) * glm::dot(point, b);
        winding += std::atan2(numerator, denominator);
    }
    return winding;
}

} // namespace

std::vector<float> SelectByValue(const SphericalGridSnapshot& grid,
                                 double lowerBound, double upperBound,
                                 double featherPercent) {
    if (!std::isfinite(lowerBound) || !std::isfinite(upperBound) ||
        lowerBound < -32767.0 || upperBound > 32767.0 || lowerBound > upperBound) {
        throw std::invalid_argument(
            "Selection bounds must be in [-32767, 32767], with lower bound <= upper bound.");
    }
    if (!std::isfinite(featherPercent) || featherPercent < 0.0 || featherPercent > 100.0) {
        throw std::invalid_argument("Selection feather percentage must be in [0, 100].");
    }
    const double midpoint = (lowerBound + upperBound) / 2.0;
    const double halfPlateau = (upperBound - lowerBound) / 2.0 * (1.0 - featherPercent / 100.0);
    const double innerLower = midpoint - halfPlateau;
    const double innerUpper = midpoint + halfPlateau;
    std::vector<float> weights;
    weights.reserve(grid.GetVertices().size());
    for (const auto& vertex : grid.GetVertices()) {
        const double value = vertex.elevation;
        double weight = 0.0;
        if (value >= innerLower && value <= innerUpper) {
            weight = 1.0;
        } else if (value > lowerBound && value < innerLower) {
            weight = (value - lowerBound) / (innerLower - lowerBound);
        } else if (value > innerUpper && value < upperBound) {
            weight = (upperBound - value) / (upperBound - innerUpper);
        }
        weights.push_back(static_cast<float>(weight));
    }
    return weights;
}

std::vector<std::size_t> RadiusSelectionTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center,
    std::vector<float>& selectionWeights, const SelectionSettings& settings) const {
    if (selectionWeights.size() != grid.GetVertices().size()) {
        throw std::invalid_argument("Selection weight count must match the grid vertex count.");
    }
    if (!std::isfinite(settings.radius) || settings.radius <= 0.0 ||
        !std::isfinite(settings.falloffPercent) || settings.falloffPercent < 0.0 ||
        settings.falloffPercent > 100.0) {
        throw std::invalid_argument("Selection radius or falloff is invalid.");
    }
    center = unitDirection(center);
    const double planetRadius = settings.distanceUnit == BrushDistanceUnit::Kilometres
                                    ? earthRadiusKilometres : earthRadiusMiles;
    const double innerRadius = settings.radius * (1.0 - settings.falloffPercent / 100.0);
    const auto candidates = grid.FindWithinRadius(center, settings.radius, planetRadius);
    std::vector<std::size_t> changed;
    changed.reserve(candidates.size());
    const auto& vertices = grid.GetVertices();
    for (const auto index : candidates) {
        const auto point = unitDirection(glm::dvec3(vertices[index].position));
        const double distance = std::atan2(glm::length(glm::cross(center, point)),
                                           glm::dot(center, point)) * planetRadius;
        double weight = 1.0;
        if (innerRadius < settings.radius && distance > innerRadius) {
            weight = std::clamp((settings.radius - distance) /
                                    (settings.radius - innerRadius),
                                0.0, 1.0);
        }
        if (weight > selectionWeights[index]) {
            selectionWeights[index] = static_cast<float>(weight);
            changed.push_back(index);
        }
    }
    return changed;
}

std::vector<float> SelectSphericalPolygon(const SphericalGridSnapshot& grid,
                                          std::span<const glm::dvec3> polygon) {
    if (polygon.size() < 3) {
        throw std::invalid_argument("A spherical selection polygon requires at least three points.");
    }
    std::vector<glm::dvec3> normalizedPolygon;
    normalizedPolygon.reserve(polygon.size());
    for (const auto point : polygon) {
        normalizedPolygon.push_back(unitDirection(point));
    }
    glm::dvec3 reference(0.0);
    for (const auto point : normalizedPolygon) {
        reference += point;
    }
    reference = unitDirection(reference);
    const double referenceWinding = sphericalWinding(reference, normalizedPolygon);
    if (!std::isfinite(referenceWinding) ||
        std::abs(referenceWinding) <= std::numbers::pi) {
        throw std::invalid_argument("Spherical polygon does not define a stable interior.");
    }
    std::vector<float> weights;
    weights.reserve(grid.GetVertices().size());
    for (const auto& vertex : grid.GetVertices()) {
        const double winding = sphericalWinding(
            unitDirection(glm::dvec3(vertex.position)), normalizedPolygon);
        const bool inside = std::isinf(winding) ||
            (std::abs(winding) > std::numbers::pi && winding * referenceWinding > 0.0);
        weights.push_back(inside ? 1.0F : 0.0F);
    }
    return weights;
}
