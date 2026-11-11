#include "Filter.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

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

FilterManager::FilterManager() {
    RegisterFilter(std::make_unique<FillFilter>());
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
