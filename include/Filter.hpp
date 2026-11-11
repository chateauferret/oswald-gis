#pragma once

#include "SphericalGrid.hpp"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

enum class FillOperation {
    Replace,
    Raise,
    Lower,
    Minimum,
    Maximum
};

struct FillFilterSettings {
    FillOperation operation = FillOperation::Replace;
    float value = 0.0F;
};

class Filter {
public:
    virtual ~Filter() = default;
    virtual std::string_view GetName() const = 0;
    virtual std::vector<float> Apply(const SphericalGridSnapshot& grid,
                                     std::span<const float> elevations) const = 0;
};

class FillFilter final : public Filter {
public:
    std::string_view GetName() const override { return "Fill"; }
    std::vector<float> Apply(const SphericalGridSnapshot& grid,
                             std::span<const float> elevations) const override;
    const FillFilterSettings& GetSettings() const { return settings_; }
    void SetSettings(const FillFilterSettings& settings);

private:
    FillFilterSettings settings_;
};

class FilterManager {
public:
    FilterManager();

    void RegisterFilter(std::unique_ptr<Filter> filter);
    Filter& GetFilter(std::string_view name);
    const Filter& GetFilter(std::string_view name) const;
    const std::vector<std::unique_ptr<Filter>>& GetFilters() const { return filters_; }

private:
    std::vector<std::unique_ptr<Filter>> filters_;
};
