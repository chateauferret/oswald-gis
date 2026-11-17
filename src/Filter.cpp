#include "Filter.hpp"

#include <algorithm>
#include <cmath>
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
    for (const auto& edge : edges) {
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

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (receivers[i] == noReceiver) {
            continue;
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
    }
    return result;
}

FilterManager::FilterManager() {
    RegisterFilter(std::make_unique<FillFilter>());
    RegisterFilter(std::make_unique<FillBasinsFilter>());
    RegisterFilter(std::make_unique<InciseFlowFilter>());
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
