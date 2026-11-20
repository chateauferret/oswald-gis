#include "Filter.hpp"
#include "ParallelFor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr float minimumElevation = -32767.0F;
constexpr float maximumElevation = 32767.0F;

struct IncidentEdges {
    std::vector<std::size_t> offsets;
    std::vector<std::uint32_t> indices;

    IncidentEdges(const SphericalGridSnapshot& grid, bool undirected = false)
        : offsets(grid.GetVertices().size() + 1, 0) {
        const auto& edges = grid.GetHalfEdges();
        for (const auto& edge : edges) {
            const auto source = static_cast<std::size_t>(edge.originVertex);
            const auto target = static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
            if (undirected && source >= target) continue;
            ++offsets[source + 1];
            if (undirected) ++offsets[target + 1];
        }
        std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());
        indices.resize(offsets.back());
        // Preserve the original half-edge order within each vertex's list.
        auto cursor = offsets;
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const auto source = static_cast<std::size_t>(edges[i].originVertex);
            const auto target = static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edges[i].nextEdge)].originVertex);
            if (undirected && source >= target) continue;
            indices[cursor[source]++] = static_cast<std::uint32_t>(i);
            if (undirected) indices[cursor[target]++] = static_cast<std::uint32_t>(i);
        }
    }

    std::span<const std::uint32_t> operator[](std::size_t vertex) const {
        return std::span<const std::uint32_t>(indices).subspan(
            offsets[vertex], offsets[vertex + 1] - offsets[vertex]);
    }
};

bool validOperation(FillOperation operation) {
    switch (operation) {
    case FillOperation::Replace:
    case FillOperation::Raise:
    case FillOperation::Lower:
    case FillOperation::Minimum:
    case FillOperation::Maximum:
        return true;
    }
    return false;
}

void validateFilterElevations(const SphericalGridSnapshot& grid,
                               std::span<const float> elevations, std::string_view name) {
    if (elevations.size() != grid.GetVertices().size()) {
        throw std::invalid_argument(std::string(name) +
            " elevation count must match the grid vertex count.");
    }
    for (const float elevation : elevations) {
        if (!std::isfinite(elevation) || elevation < minimumElevation ||
            elevation > maximumElevation) {
            throw std::invalid_argument(std::string(name) +
                " input elevations must be finite and in the range [-32767, 32767].");
        }
    }
}

std::vector<bool> erosionSinks(const SphericalGridSnapshot& grid,
                              std::span<const float> elevations) {
    std::vector<bool> sinks(elevations.size(), false);
    bool hasSink = false;
    for (std::size_t i = 0; i < elevations.size(); ++i) {
        sinks[i] = elevations[i] <= 0.0F ||
            (grid.HasSelection() && grid.GetVertices()[i].selectionWeight == 0.0F);
        hasSink |= sinks[i];
    }
    if (!hasSink && !elevations.empty()) {
        const float minimum = *std::min_element(elevations.begin(), elevations.end());
        for (std::size_t i = 0; i < elevations.size(); ++i) {
            sinks[i] = elevations[i] == minimum;
        }
    }
    return sinks;
}

void validateInciseFlowSettings(const InciseFlowSettings& settings) {
    const auto nonnegative = [](double value) { return std::isfinite(value) && value >= 0.0; };
    const auto positive = [](double value) { return std::isfinite(value) && value > 0.0; };
    if (!nonnegative(settings.incisionRate) || !nonnegative(settings.flowExponent) ||
        !nonnegative(settings.slopeExponent) || !nonnegative(settings.precipitation)) {
        throw std::invalid_argument(
            "Incision rate, flow exponent, slope exponent and precipitation must be finite and non-negative.");
    }
    if (!positive(settings.referenceArea)) {
        throw std::invalid_argument("Reference area must be finite and positive.");
    }
}

void validatePrecipitonSettings(const PrecipitonSettings& settings) {
    if (!std::isfinite(settings.diffusionCoefficient) || settings.diffusionCoefficient < 0.0) {
        throw std::invalid_argument(
            "Precipiton diffusion coefficient must be finite and non-negative.");
    }
}

void validateThermalWeatheringSettings(const ThermalWeatheringSettings& settings) {
    if (!std::isfinite(settings.angleOfReposeDegrees) ||
        settings.angleOfReposeDegrees < 0.0 || settings.angleOfReposeDegrees >= 90.0) {
        throw std::invalid_argument(
            "Thermal weathering angle of repose must be finite and in the range [0, 90) degrees.");
    }
}

void validateGlacialErosionSettings(const GlacialErosionSettings& settings) {
    const auto finite = [](double value) { return std::isfinite(value); };
    const auto nonnegative = [&](double value) { return finite(value) && value >= 0.0; };
    if (!finite(settings.equilibriumLineAltitude) ||
        !nonnegative(settings.elaLatitudeGradient) ||
        !nonnegative(settings.accumulationRate) || !nonnegative(settings.ablationRate) ||
        !nonnegative(settings.initialIceThickness) || !nonnegative(settings.durationYears) ||
        !nonnegative(settings.glacialErodibility) || !nonnegative(settings.iceViscosityFactor) ||
        !nonnegative(settings.thicknessExponent) || !nonnegative(settings.velocityExponent) ||
        !finite(settings.troughHalfWidth) || settings.troughHalfWidth <= 0.0) {
        throw std::invalid_argument(
            "Glacial parameters must be finite and non-negative; trough half-width must be positive.");
    }
}

bool validFlowDirection(const glm::dvec3& direction) {
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z)) {
        return false;
    }
    const double length = glm::length(direction);
    return std::isfinite(length) && length > 0.0;
}

void validateCratonicGroovingSettings(const CratonicGroovingSettings& settings) {
    if (!validFlowDirection(settings.iceFlowDirection) ||
        !std::isfinite(settings.erodibility) || settings.erodibility < 0.0 ||
        settings.erodibility > 1.0 ||
        !std::isfinite(settings.grooveCoefficient) || settings.grooveCoefficient < 0.0 ||
        !std::isfinite(settings.anisotropy) || settings.anisotropy < 0.0) {
        throw std::invalid_argument(
            "Cratonic grooving needs a finite nonzero flow direction, erodibility in [0, 1], "
            "and finite non-negative groove parameters.");
    }
}

void validateCratonicScourSettings(const CratonicScourSettings& settings) {
    if (!validFlowDirection(settings.iceFlowDirection) ||
        !std::isfinite(settings.erodibility) || settings.erodibility < 0.0 ||
        settings.erodibility > 1.0 ||
        !std::isfinite(settings.stossAbrasionCoefficient) ||
        settings.stossAbrasionCoefficient < 0.0 ||
        !std::isfinite(settings.pluckingRate) || settings.pluckingRate < 0.0 ||
        !std::isfinite(settings.cratonElevation) ||
        settings.cratonElevation < minimumElevation ||
        settings.cratonElevation > maximumElevation ||
        !std::isfinite(settings.planationAttenuation) ||
        settings.planationAttenuation < 0.0) {
        throw std::invalid_argument(
            "Cratonic scour needs a finite nonzero flow direction, erodibility in [0, 1], "
            "and finite non-negative rates with a supported craton elevation.");
    }
}

}

void FillFilter::SetSettings(const FillFilterSettings& settings) {
    if (!validOperation(settings.operation)) {
        throw std::invalid_argument("Fill filter operation is invalid.");
    }
    if (!std::isfinite(settings.value) || settings.value < minimumElevation ||
        settings.value > maximumElevation) {
        throw std::invalid_argument("Fill filter value must be finite and in the range [-32767, 32767].");
    }
    settings_ = settings;
}

std::vector<float> FillFilter::Apply(const SphericalGridSnapshot& grid,
                                    std::span<const float> elevations) const {
    if (!validOperation(settings_.operation)) {
        throw std::invalid_argument("Fill filter operation is invalid.");
    }
    if (!std::isfinite(settings_.value) || settings_.value < minimumElevation ||
        settings_.value > maximumElevation) {
        throw std::invalid_argument("Fill filter value must be finite and in the range [-32767, 32767].");
    }
    const auto& vertices = grid.GetVertices();
    if (elevations.size() != vertices.size()) {
        throw std::invalid_argument("Fill filter elevation count must match the grid vertex count.");
    }

    std::vector<float> result;
    result.reserve(elevations.size());
    for (std::size_t i = 0; i < elevations.size(); ++i) {
        const float current = elevations[i];
        if (!std::isfinite(current) || current < minimumElevation || current > maximumElevation) {
            throw std::invalid_argument("Fill filter input elevations must be finite and in the range [-32767, 32767].");
        }
        float target = current;
        switch (settings_.operation) {
        case FillOperation::Replace:
            target = settings_.value;
            break;
        case FillOperation::Raise:
            target = current + settings_.value;
            break;
        case FillOperation::Lower:
            target = current - settings_.value;
            break;
        case FillOperation::Minimum:
            target = std::min(current, settings_.value);
            break;
        case FillOperation::Maximum:
            target = std::max(current, settings_.value);
            break;
        }
        const float selectionWeight = grid.HasSelection()
                                          ? vertices[i].selectionWeight : 1.0F;
        result.push_back(std::clamp(current + (target - current) * selectionWeight,
                                    minimumElevation, maximumElevation));
    }
    return result;
}

std::vector<float> FillBasinsFilter::Apply(const SphericalGridSnapshot& grid,
                                          std::span<const float> elevations) const {
    const auto& vertices = grid.GetVertices();
    validateFilterElevations(grid, elevations, "Fill basins");

    std::vector<float> result(elevations.begin(), elevations.end());
    auto resolved = erosionSinks(grid, elevations);
    using QueueEntry = std::pair<float, std::size_t>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;
    const auto seed = [&](std::size_t index) {
        resolved[index] = true;
        queue.emplace(result[index], index);
    };
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (resolved[i]) {
            seed(i);
        }
    }

    const auto& edges = grid.GetHalfEdges();
    std::vector<std::int32_t> firstEdges(vertices.size(), -1);
    for (std::size_t i = 0; i < edges.size(); ++i) {
        firstEdges[static_cast<std::size_t>(edges[i].originVertex)] = static_cast<std::int32_t>(i);
    }
    while (!queue.empty()) {
        const auto [height, index] = queue.top();
        queue.pop();
        const auto first = firstEdges[index];
        auto edgeIndex = first;
        do {
            const auto& edge = edges[static_cast<std::size_t>(edgeIndex)];
            const auto neighbor = static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
            if (!resolved[neighbor]) {
                if (result[neighbor] <= height) {
                    // At high elevations epsilon can round away in Float32.
                    result[neighbor] = std::max(height + 1.0e-4F,
                        std::nextafter(height, std::numeric_limits<float>::infinity()));
                    if (result[neighbor] > maximumElevation) {
                        throw std::overflow_error(
                            "Fill basins cannot establish drainage within the maximum elevation of 32767 m.");
                    }
                }
                seed(neighbor);
            }
            edgeIndex = edges[static_cast<std::size_t>(edge.twinEdge)].nextEdge;
        } while (edgeIndex != first);
    }
    if (grid.HasSelection()) {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            result[i] = elevations[i] + (result[i] - elevations[i]) * vertices[i].selectionWeight;
        }
    }
    return result;
}

void AbsoluteMagnitudeNoiseFilter::SetAmplitudeFeet(float amplitude) {
    if (!std::isfinite(amplitude) || amplitude < 0.0F || amplitude > 500.0F) {
        throw std::invalid_argument("Noise amplitude must be finite and between 0 and 500 feet.");
    }
    amplitudeFeet_ = amplitude;
}

std::vector<float> AbsoluteMagnitudeNoiseFilter::Apply(
    const SphericalGridSnapshot& grid, std::span<const float> elevations) const {
    validateFilterElevations(grid, elevations, "Absolute Magnitude Noise");
    std::vector<float> result(elevations.begin(), elevations.end());
    if (amplitudeFeet_ == 0.0F) {
        return result;
    }
    std::random_device device;
    std::mt19937 generator(device());
    const double amplitudeMetres = static_cast<double>(amplitudeFeet_) * 0.3048;
    std::uniform_real_distribution<double> noise(-amplitudeMetres, amplitudeMetres);
    for (std::size_t i = 0; i < elevations.size(); ++i) {
        const double weight = grid.HasSelection() ? grid.GetVertices()[i].selectionWeight : 1.0;
        if (weight == 0.0) {
            continue;
        }
        result[i] = static_cast<float>(std::clamp(elevations[i] + noise(generator) * weight,
            static_cast<double>(minimumElevation), static_cast<double>(maximumElevation)));
    }
    return result;
}

void InciseFlowFilter::SetSettings(const InciseFlowSettings& settings) {
    validateInciseFlowSettings(settings);
    settings_ = settings;
}

std::vector<float> InciseFlowFilter::Apply(const SphericalGridSnapshot& grid,
                                          std::span<const float> elevations) const {
    validateInciseFlowSettings(settings_);
    validateFilterElevations(grid, elevations, "Incise flow");
    std::vector<float> result(elevations.begin(), elevations.end());
    if (settings_.incisionRate == 0.0 || settings_.precipitation == 0.0) {
        return result;
    }
    const auto& vertices = grid.GetVertices();
    const auto sinks = erosionSinks(grid, elevations);
    const auto& edges = grid.GetHalfEdges();
    const std::size_t noReceiver = vertices.size();
    std::vector<std::size_t> receivers(vertices.size(), noReceiver);
    std::vector<double> slopes(vertices.size(), 0.0);
    const IncidentEdges incident(grid);
    parallelFor(vertices.size(), 1024, [&](std::size_t vertex) {
        for (const auto edgeIndex : incident[vertex]) {
            const auto& edge = edges[edgeIndex];
            const auto source = static_cast<std::size_t>(edge.originVertex);
            const auto target = static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
            if (sinks[source] || elevations[source] <= elevations[target]) {
                continue;
            }
            const auto from = glm::normalize(glm::dvec3(vertices[source].position));
            const auto to = glm::normalize(glm::dvec3(vertices[target].position));
            const double distance = grid.GetOptions().planetRadius *
                std::acos(std::clamp(glm::dot(from, to), -1.0, 1.0));
            if (!std::isfinite(distance) || distance <= 0.0) {
                throw std::runtime_error("Incise flow encountered an invalid geodesic edge distance.");
            }
            const double slope = (static_cast<double>(elevations[source]) - elevations[target]) / distance;
            if (!std::isfinite(slope)) {
                throw std::overflow_error("Incise flow slope exceeds the supported numeric range.");
            }
            if (slope > slopes[source] || (slope == slopes[source] && target < receivers[source])) {
                slopes[source] = slope;
                receivers[source] = target;
            }
        }
    });

    std::vector<std::size_t> order(vertices.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return elevations[left] == elevations[right] ? left < right : elevations[left] > elevations[right];
    });
    std::vector<double> area(vertices.size(), settings_.precipitation);
    for (const auto index : order) {
        if (receivers[index] != noReceiver) {
            auto& downstreamArea = area[receivers[index]];
            downstreamArea += area[index];
            if (!std::isfinite(downstreamArea)) {
                throw std::overflow_error("Incise flow accumulated runoff exceeds the supported numeric range.");
            }
        }
    }

    parallelFor(vertices.size(), 1024, [&](std::size_t i) {
        if (receivers[i] == noReceiver) {
            return;
        }
        const double incision = settings_.incisionRate *
            std::pow(area[i] / settings_.referenceArea, settings_.flowExponent) *
            std::pow(slopes[i], settings_.slopeExponent);
        if (!std::isfinite(incision)) {
            throw std::overflow_error("Incise flow elevation change exceeds the supported numeric range.");
        }
        const double target = std::clamp(elevations[i] - incision,
            static_cast<double>(minimumElevation), static_cast<double>(maximumElevation));
        const double weight = grid.HasSelection() ? vertices[i].selectionWeight : 1.0;
        result[i] = static_cast<float>(elevations[i] + (target - elevations[i]) * weight);
    });
    return result;
}

void PrecipitonFilter::SetSettings(const PrecipitonSettings& settings) {
    validatePrecipitonSettings(settings);
    settings_ = settings;
}

std::vector<float> PrecipitonFilter::Apply(const SphericalGridSnapshot& grid,
                                           std::span<const float> elevations) const {
    validatePrecipitonSettings(settings_);
    validateFilterElevations(grid, elevations, "Precipiton diffusion");
    std::vector<float> result(elevations.begin(), elevations.end());
    if (settings_.diffusionCoefficient == 0.0 || elevations.empty()) {
        return result;
    }

    const auto& vertices = grid.GetVertices();
    const auto sinks = erosionSinks(grid, elevations);
    std::vector<double> laplacian(elevations.size(), 0.0);
    const auto& edges = grid.GetHalfEdges();
    const IncidentEdges incident(grid);
    parallelFor(vertices.size(), 1024, [&](std::size_t vertex) {
        for (const auto edgeIndex : incident[vertex]) {
            const auto& edge = edges[edgeIndex];
            const auto source = static_cast<std::size_t>(edge.originVertex);
            if (sinks[source]) {
                continue;
            }
            const auto target = static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
            const auto from = glm::normalize(glm::dvec3(vertices[source].position));
            const auto to = glm::normalize(glm::dvec3(vertices[target].position));
            const double distance = grid.GetOptions().planetRadius *
                std::acos(std::clamp(glm::dot(from, to), -1.0, 1.0));
            if (!std::isfinite(distance) || distance <= 0.0) {
                throw std::runtime_error(
                    "Precipiton diffusion encountered an invalid geodesic edge distance.");
            }
            const double contribution =
                (static_cast<double>(elevations[target]) - elevations[source]) / distance;
            if (!std::isfinite(contribution)) {
                throw std::overflow_error(
                    "Precipiton diffusion Laplacian exceeds the supported numeric range.");
            }
            laplacian[source] += contribution;
            if (!std::isfinite(laplacian[source])) {
                throw std::overflow_error(
                    "Precipiton diffusion Laplacian exceeds the supported numeric range.");
            }
        }
    });

    parallelFor(vertices.size(), 1024, [&](std::size_t i) {
        if (sinks[i]) {
            return;
        }
        const double change = settings_.diffusionCoefficient * laplacian[i];
        if (!std::isfinite(change)) {
            throw std::overflow_error(
                "Precipiton diffusion elevation change exceeds the supported numeric range.");
        }
        const double target = std::clamp(static_cast<double>(elevations[i]) + change,
            static_cast<double>(minimumElevation), static_cast<double>(maximumElevation));
        const double weight = grid.HasSelection() ? vertices[i].selectionWeight : 1.0;
        result[i] = static_cast<float>(elevations[i] + (target - elevations[i]) * weight);
    });
    return result;
}

void ThermalWeatheringFilter::SetSettings(const ThermalWeatheringSettings& settings) {
    validateThermalWeatheringSettings(settings);
    settings_ = settings;
}

std::vector<float> ThermalWeatheringFilter::Apply(
    const SphericalGridSnapshot& grid, std::span<const float> elevations) const {
    validateThermalWeatheringSettings(settings_);
    validateFilterElevations(grid, elevations, "Thermal weathering");
    std::vector<float> result(elevations.begin(), elevations.end());
    if (elevations.empty()) {
        return result;
    }

    const double radians = settings_.angleOfReposeDegrees *
        (std::acos(-1.0) / 180.0);
    const double reposeSlope = std::tan(radians);
    if (!std::isfinite(reposeSlope)) {
        throw std::overflow_error(
            "Thermal weathering repose slope exceeds the supported numeric range.");
    }
    const auto& vertices = grid.GetVertices();
    const auto sinks = erosionSinks(grid, elevations);
    std::vector<double> displacement(elevations.size(), 0.0);
    const auto& edges = grid.GetHalfEdges();
    const IncidentEdges incident(grid, true);
    parallelFor(vertices.size(), 1024, [&](std::size_t vertex) {
        for (const auto edgeIndex : incident[vertex]) {
            const auto& edge = edges[edgeIndex];
            const auto first = static_cast<std::size_t>(edge.originVertex);
            const auto second = static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
            if (sinks[first] || sinks[second]) {
                continue;
            }
            const double firstHeight = elevations[first];
            const double secondHeight = elevations[second];
            if (firstHeight == secondHeight) {
                continue;
            }
            const std::size_t high = firstHeight > secondHeight ? first : second;
            const std::size_t low = firstHeight > secondHeight ? second : first;
            const auto from = glm::normalize(glm::dvec3(vertices[high].position));
            const auto to = glm::normalize(glm::dvec3(vertices[low].position));
            const double distance = grid.GetOptions().planetRadius *
                std::acos(std::clamp(glm::dot(from, to), -1.0, 1.0));
            if (!std::isfinite(distance) || distance <= 0.0) {
                throw std::runtime_error(
                    "Thermal weathering encountered an invalid geodesic edge distance.");
            }
            const double excess = (static_cast<double>(elevations[high]) - elevations[low]) -
                distance * reposeSlope;
            if (!std::isfinite(excess)) {
                throw std::overflow_error(
                    "Thermal weathering slope excess exceeds the supported numeric range.");
            }
            if (excess <= 0.0) {
                continue;
            }
            const double transfer = 0.5 * excess;
            if (vertex == high) {
                displacement[vertex] -= transfer;
            } else {
                displacement[vertex] += transfer;
            }
            if (!std::isfinite(displacement[vertex])) {
                throw std::overflow_error(
                    "Thermal weathering displacement exceeds the supported numeric range.");
            }
        }
    });

    parallelFor(vertices.size(), 1024, [&](std::size_t i) {
        if (sinks[i] || displacement[i] == 0.0) {
            return;
        }
        const double target = std::clamp(
            static_cast<double>(elevations[i]) + displacement[i],
            static_cast<double>(minimumElevation), static_cast<double>(maximumElevation));
        const double weight = grid.HasSelection() ? vertices[i].selectionWeight : 1.0;
        result[i] = static_cast<float>(elevations[i] + (target - elevations[i]) * weight);
    });
    return result;
}

void GlacialErosionFilter::SetSettings(const GlacialErosionSettings& settings) {
    validateGlacialErosionSettings(settings);
    settings_ = settings;
}

std::vector<float> GlacialErosionFilter::Apply(
    const SphericalGridSnapshot& grid, std::span<const float> elevations) const {
    validateGlacialErosionSettings(settings_);
    validateFilterElevations(grid, elevations, "Glacial erosion");
    std::vector<float> result(elevations.begin(), elevations.end());
    if (elevations.empty()) {
        return result;
    }

    constexpr double iceDensity = 917.0;
    constexpr double mantleDensity = 3300.0;
    constexpr double metresToDegrees = 180.0 / 3.14159265358979323846;
    const auto& vertices = grid.GetVertices();
    const auto sinks = erosionSinks(grid, elevations);
    const std::size_t count = vertices.size();

    struct Neighbor {
        std::size_t index;
        double distance;
        glm::dvec3 tangent;
    };
    std::vector<std::vector<Neighbor>> neighbors(count);
    const auto& edges = grid.GetHalfEdges();
    const IncidentEdges incident(grid);
    parallelFor(count, 1024, [&](std::size_t vertex) {
        for (const auto edgeIndex : incident[vertex]) {
            const auto& edge = edges[edgeIndex];
            const auto source = static_cast<std::size_t>(edge.originVertex);
            const auto target = static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
            const auto from = glm::normalize(glm::dvec3(vertices[source].position));
            const auto to = glm::normalize(glm::dvec3(vertices[target].position));
            const double distance = grid.GetOptions().planetRadius *
                std::acos(std::clamp(glm::dot(from, to), -1.0, 1.0));
            if (!std::isfinite(distance) || distance <= 0.0) {
                throw std::runtime_error(
                    "Glacial erosion encountered an invalid geodesic edge distance.");
            }
            const glm::dvec3 tangentVector = to - glm::dot(from, to) * from;
            const double tangentLength = glm::length(tangentVector);
            if (!std::isfinite(tangentLength) || tangentLength <= 0.0) {
                throw std::runtime_error("Glacial erosion encountered an invalid edge direction.");
            }
            neighbors[source].push_back({target, distance, tangentVector / tangentLength});
        }
    });

    std::vector<double> iceThickness(count, settings_.initialIceThickness);
    parallelFor(count, 1024, [&](std::size_t i) {
        const auto position = glm::normalize(glm::dvec3(vertices[i].position));
        const double latitude = std::asin(std::clamp(position.z, -1.0, 1.0)) * metresToDegrees;
        const double ela = settings_.equilibriumLineAltitude -
            settings_.elaLatitudeGradient * std::abs(latitude);
        const double surface = static_cast<double>(elevations[i]) + iceThickness[i];
        const double balance = settings_.accumulationRate * std::max(0.0, surface - ela) -
            settings_.ablationRate * std::max(0.0, ela - surface);
        const double updatedThickness = std::max(0.0,
            iceThickness[i] + settings_.durationYears * balance);
        if (!std::isfinite(updatedThickness)) {
            throw std::overflow_error(
                "Glacial ice mass balance exceeds the supported numeric range.");
        }
        iceThickness[i] = updatedThickness;
    });

    std::vector<double> surface(count);
    parallelFor(count, 1024, [&](std::size_t i) {
        surface[i] = static_cast<double>(elevations[i]) + iceThickness[i];
        if (!std::isfinite(surface[i])) {
            throw std::overflow_error("Glacial ice surface exceeds the supported numeric range.");
        }
    });

    std::vector<double> erosionRate(count, 0.0);
    std::vector<glm::dvec3> flowDirection(count, glm::dvec3(0.0));
    parallelFor(count, 1024, [&](std::size_t i) {
        if (sinks[i] || iceThickness[i] <= 0.0) {
            return;
        }
        double maximumSlope = 0.0;
        double fallbackSlope = 0.0;
        glm::dvec3 weightedFlow(0.0);
        glm::dvec3 fallbackDirection(0.0);
        for (const auto& neighbor : neighbors[i]) {
            if (surface[i] <= surface[neighbor.index]) {
                continue;
            }
            const double slope = (surface[i] - surface[neighbor.index]) / neighbor.distance;
            if (!std::isfinite(slope)) {
                throw std::overflow_error("Glacial ice slope exceeds the supported numeric range.");
            }
            weightedFlow += neighbor.tangent * slope;
            if (slope > fallbackSlope) {
                fallbackSlope = slope;
                fallbackDirection = neighbor.tangent;
            }
            maximumSlope = std::max(maximumSlope, slope);
        }
        if (maximumSlope <= 0.0) {
            return;
        }
        const double flowLength = glm::length(weightedFlow);
        if (!std::isfinite(flowLength)) {
            throw std::overflow_error("Glacial flow direction exceeds the supported numeric range.");
        }
        flowDirection[i] = flowLength > 1.0e-12 ? weightedFlow / flowLength : fallbackDirection;
        const double velocity = settings_.iceViscosityFactor * iceThickness[i] *
            maximumSlope * maximumSlope * maximumSlope;
        const double rate = settings_.glacialErodibility *
            std::pow(iceThickness[i], settings_.thicknessExponent) *
            std::pow(velocity, settings_.velocityExponent);
        if (!std::isfinite(velocity) || !std::isfinite(rate)) {
            throw std::overflow_error("Glacial erosion rate exceeds the supported numeric range.");
        }
        erosionRate[i] = rate;
    });

    constexpr double crossTrackLimit = 0.70710678118654752440;
    const auto spreadWeight = [&](std::size_t source, const Neighbor& neighbor) {
        if (sinks[neighbor.index]) {
            return 0.0;
        }
        const double alongFlow = glm::dot(flowDirection[source], neighbor.tangent);
        if (std::abs(alongFlow) > crossTrackLimit) {
            return 0.0;
        }
        const double perpendicularDistance = neighbor.distance *
            std::sqrt(std::max(0.0, 1.0 - alongFlow * alongFlow));
        const double ratio = perpendicularDistance / settings_.troughHalfWidth;
        return std::max(0.0, 1.0 - ratio * ratio);
    };
    std::vector<double> depth(count, 0.0);
    std::vector<double> weightSums(count, 1.0);
    parallelFor(count, 1024, [&](std::size_t source) {
        depth[source] = erosionRate[source] * settings_.durationYears;
        if (!std::isfinite(depth[source])) {
            throw std::overflow_error("Glacial erosion depth exceeds the supported numeric range.");
        }
        if (depth[source] <= 0.0) {
            return;
        }
        for (const auto& neighbor : neighbors[source]) {
            const double weight = spreadWeight(source, neighbor);
            if (weight > 0.0) {
                weightSums[source] += weight;
            }
        }
    });

    std::vector<double> erosionDepth(count, 0.0);
    parallelFor(count, 1024, [&](std::size_t target) {
        std::vector<std::size_t> sources{target};
        sources.reserve(neighbors[target].size() + 1);
        for (const auto& neighbor : neighbors[target]) {
            sources.push_back(neighbor.index);
        }
        // Match the source order of the original scatter accumulation.
        std::sort(sources.begin(), sources.end());
        for (const auto source : sources) {
            if (depth[source] <= 0.0) continue;
            double weight = 1.0;
            if (source != target) {
                const auto& sourceNeighbors = neighbors[source];
                const auto found = std::find_if(sourceNeighbors.begin(), sourceNeighbors.end(),
                    [&](const Neighbor& neighbor) { return neighbor.index == target; });
                if (found == sourceNeighbors.end()) {
                    throw std::runtime_error("Glacial erosion encountered an asymmetric edge.");
                }
                weight = spreadWeight(source, *found);
                if (weight <= 0.0) continue;
            }
            erosionDepth[target] += depth[source] * (weight / weightSums[source]);
            if (!std::isfinite(erosionDepth[target])) {
                throw std::overflow_error(
                    "Glacial trough erosion exceeds the supported numeric range.");
            }
        }
    });

    const double reboundRatio = iceDensity / mantleDensity;
    parallelFor(count, 1024, [&](std::size_t i) {
        if (sinks[i]) {
            return;
        }
        const double subsidence = reboundRatio * iceThickness[i];
        const double target = static_cast<double>(elevations[i]) - erosionDepth[i] - subsidence;
        if (!std::isfinite(subsidence) || !std::isfinite(target)) {
            throw std::overflow_error(
                "Glacial bedrock displacement exceeds the supported numeric range.");
        }
        const double clamped = std::clamp(target,
            static_cast<double>(minimumElevation), static_cast<double>(maximumElevation));
        const double weight = grid.HasSelection() ? vertices[i].selectionWeight : 1.0;
        result[i] = static_cast<float>(elevations[i] + (clamped - elevations[i]) * weight);
    });
    return result;
}

void CratonicGroovingFilter::SetSettings(const CratonicGroovingSettings& settings) {
    validateCratonicGroovingSettings(settings);
    settings_ = settings;
}

std::vector<float> CratonicGroovingFilter::Apply(
    const SphericalGridSnapshot& grid, std::span<const float> elevations) const {
    validateCratonicGroovingSettings(settings_);
    validateFilterElevations(grid, elevations, "Cratonic directional grooving");
    std::vector<float> result(elevations.begin(), elevations.end());
    if (elevations.empty() || settings_.grooveCoefficient == 0.0 ||
        settings_.erodibility == 0.0) {
        return result;
    }

    const auto& vertices = grid.GetVertices();
    const auto sinks = erosionSinks(grid, elevations);
    const auto flowDirection = glm::normalize(settings_.iceFlowDirection);
    std::vector<double> parallelDistanceSum(vertices.size(), 0.0);
    std::vector<std::size_t> neighborCounts(vertices.size(), 0);
    const auto& edges = grid.GetHalfEdges();
    const IncidentEdges incident(grid);
    parallelFor(vertices.size(), 1024, [&](std::size_t vertex) {
        for (const auto edgeIndex : incident[vertex]) {
            const auto& edge = edges[edgeIndex];
            const auto source = static_cast<std::size_t>(edge.originVertex);
            const auto target = static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
            const auto from = glm::normalize(glm::dvec3(vertices[source].position));
            const auto to = glm::normalize(glm::dvec3(vertices[target].position));
            const double dot = std::clamp(glm::dot(from, to), -1.0, 1.0);
            const double distance = grid.GetOptions().planetRadius * std::acos(dot);
            const glm::dvec3 tangent = to - dot * from;
            const double tangentLength = glm::length(tangent);
            if (!std::isfinite(distance) || distance <= 0.0 ||
                !std::isfinite(tangentLength) || tangentLength <= 0.0) {
                throw std::runtime_error(
                    "Cratonic grooving encountered an invalid geodesic edge direction.");
            }
            glm::dvec3 localFlow = flowDirection - glm::dot(flowDirection, from) * from;
            const double flowLength = glm::length(localFlow);
            if (flowLength > 0.0) {
                localFlow /= flowLength;
                const double parallelDistance = std::abs(
                    glm::dot(tangent / tangentLength * distance, localFlow));
                parallelDistanceSum[source] += parallelDistance;
                if (!std::isfinite(parallelDistanceSum[source])) {
                    throw std::overflow_error(
                        "Cratonic groove alignment exceeds the supported numeric range.");
                }
            }
            ++neighborCounts[source];
        }
    });

    parallelFor(vertices.size(), 1024, [&](std::size_t i) {
        if (sinks[i] || neighborCounts[i] == 0) {
            return;
        }
        const double meanParallelDistance =
            parallelDistanceSum[i] / static_cast<double>(neighborCounts[i]);
        const double depth = settings_.grooveCoefficient * settings_.erodibility *
            (1.0 + settings_.anisotropy * meanParallelDistance);
        if (!std::isfinite(depth)) {
            throw std::overflow_error(
                "Cratonic groove depth exceeds the supported numeric range.");
        }
        const double target = std::clamp(static_cast<double>(elevations[i]) - depth,
            static_cast<double>(minimumElevation), static_cast<double>(maximumElevation));
        const double weight = grid.HasSelection() ? vertices[i].selectionWeight : 1.0;
        result[i] = static_cast<float>(elevations[i] + (target - elevations[i]) * weight);
    });
    return result;
}

void CratonicScourFilter::SetSettings(const CratonicScourSettings& settings) {
    validateCratonicScourSettings(settings);
    settings_ = settings;
}

std::vector<float> CratonicScourFilter::Apply(
    const SphericalGridSnapshot& grid, std::span<const float> elevations) const {
    validateCratonicScourSettings(settings_);
    validateFilterElevations(grid, elevations, "Cratonic scour and planation");
    std::vector<float> result(elevations.begin(), elevations.end());
    if (elevations.empty()) {
        return result;
    }

    const auto& vertices = grid.GetVertices();
    const auto sinks = erosionSinks(grid, elevations);
    const auto flowDirection = glm::normalize(settings_.iceFlowDirection);
    std::vector<double> laplacian(vertices.size(), 0.0);
    std::vector<glm::dvec3> gradient(vertices.size(), glm::dvec3(0.0));
    std::vector<std::size_t> neighborCounts(vertices.size(), 0);
    const auto& edges = grid.GetHalfEdges();
    const IncidentEdges incident(grid);
    parallelFor(vertices.size(), 1024, [&](std::size_t vertex) {
        for (const auto edgeIndex : incident[vertex]) {
            const auto& edge = edges[edgeIndex];
            const auto source = static_cast<std::size_t>(edge.originVertex);
            const auto target = static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
            const auto from = glm::normalize(glm::dvec3(vertices[source].position));
            const auto to = glm::normalize(glm::dvec3(vertices[target].position));
            const double dot = std::clamp(glm::dot(from, to), -1.0, 1.0);
            const double distance = grid.GetOptions().planetRadius * std::acos(dot);
            const glm::dvec3 tangent = to - dot * from;
            const double tangentLength = glm::length(tangent);
            if (!std::isfinite(distance) || distance <= 0.0 ||
                !std::isfinite(tangentLength) || tangentLength <= 0.0) {
                throw std::runtime_error(
                    "Cratonic scour encountered an invalid geodesic edge direction.");
            }
            const double elevationDifference =
                static_cast<double>(elevations[target]) - elevations[source];
            laplacian[source] += elevationDifference;
            gradient[source] += (elevationDifference / distance) * (tangent / tangentLength);
            ++neighborCounts[source];
            if (!std::isfinite(laplacian[source]) ||
                !std::isfinite(gradient[source].x) || !std::isfinite(gradient[source].y) ||
                !std::isfinite(gradient[source].z)) {
                throw std::overflow_error(
                    "Cratonic stoss/lee gradient exceeds the supported numeric range.");
            }
        }
    });

    parallelFor(vertices.size(), 1024, [&](std::size_t i) {
        if (sinks[i] || neighborCounts[i] == 0) {
            return;
        }
        const auto position = glm::normalize(glm::dvec3(vertices[i].position));
        glm::dvec3 localFlow = flowDirection - glm::dot(flowDirection, position) * position;
        const double flowLength = glm::length(localFlow);
        double change = 0.0;
        if (flowLength > 0.0) {
            localFlow /= flowLength;
            const double alpha = glm::dot(
                gradient[i] / static_cast<double>(neighborCounts[i]), localFlow);
            if (!std::isfinite(alpha)) {
                throw std::overflow_error(
                    "Cratonic stoss/lee slope exceeds the supported numeric range.");
            }
            if (alpha > 0.0) {
                change += settings_.stossAbrasionCoefficient * laplacian[i];
            } else if (alpha < 0.0) {
                change -= settings_.pluckingRate * settings_.erodibility * std::abs(alpha);
            }
        }
        double elevation = static_cast<double>(elevations[i]) + change;
        const double heightAboveCraton =
            std::max(0.0, elevation - settings_.cratonElevation);
        elevation = settings_.cratonElevation +
            (elevation - settings_.cratonElevation) /
                (1.0 + settings_.planationAttenuation * heightAboveCraton);
        if (!std::isfinite(elevation)) {
            throw std::overflow_error(
                "Cratonic planation exceeds the supported numeric range.");
        }
        const double target = std::clamp(elevation,
            static_cast<double>(minimumElevation), static_cast<double>(maximumElevation));
        const double weight = grid.HasSelection() ? vertices[i].selectionWeight : 1.0;
        result[i] = static_cast<float>(elevations[i] + (target - elevations[i]) * weight);
    });
    return result;
}

FilterManager::FilterManager() {
    RegisterFilter(std::make_unique<FillFilter>());
    RegisterFilter(std::make_unique<FillBasinsFilter>());
    RegisterFilter(std::make_unique<InciseFlowFilter>());
    RegisterFilter(std::make_unique<PrecipitonFilter>());
    RegisterFilter(std::make_unique<ThermalWeatheringFilter>());
    RegisterFilter(std::make_unique<GlacialErosionFilter>());
    RegisterFilter(std::make_unique<CratonicGroovingFilter>());
    RegisterFilter(std::make_unique<CratonicScourFilter>());
    RegisterFilter(std::make_unique<AbsoluteMagnitudeNoiseFilter>());
}

void FilterManager::RegisterFilter(std::unique_ptr<Filter> filter) {
    if (!filter) {
        throw std::invalid_argument("Cannot register an empty filter.");
    }
    if (filter->GetName().empty()) {
        throw std::invalid_argument("Filter names must not be empty.");
    }
    if (std::any_of(filters_.begin(), filters_.end(), [&](const auto& registered) {
            return registered->GetName() == filter->GetName();
        })) {
        throw std::invalid_argument("Filter names must be unique.");
    }
    filters_.push_back(std::move(filter));
}

Filter& FilterManager::GetFilter(std::string_view name) {
    const auto found = std::find_if(filters_.begin(), filters_.end(), [&](const auto& filter) {
        return filter->GetName() == name;
    });
    if (found == filters_.end()) {
        throw std::out_of_range("Filter is not registered: " + std::string(name));
    }
    return **found;
}

const Filter& FilterManager::GetFilter(std::string_view name) const {
    const auto found = std::find_if(filters_.begin(), filters_.end(), [&](const auto& filter) {
        return filter->GetName() == name;
    });
    if (found == filters_.end()) {
        throw std::out_of_range("Filter is not registered: " + std::string(name));
    }
    return **found;
}
