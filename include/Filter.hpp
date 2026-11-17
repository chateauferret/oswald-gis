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

class FillBasinsFilter final : public Filter {
public:
    std::string_view GetName() const override { return "Fill basins"; }
    std::vector<float> Apply(const SphericalGridSnapshot& grid,
                             std::span<const float> elevations) const override;
};

class AbsoluteMagnitudeNoiseFilter final : public Filter {
public:
    std::string_view GetName() const override { return "Absolute Magnitude Noise"; }
    std::vector<float> Apply(const SphericalGridSnapshot& grid,
                             std::span<const float> elevations) const override;
    float GetAmplitudeFeet() const { return amplitudeFeet_; }
    void SetAmplitudeFeet(float amplitude);

private:
    float amplitudeFeet_ = 100.0F;
};

struct InciseFlowSettings {
    double incisionRate = 1.0;
    double referenceArea = 1.0;
    double flowExponent = 0.5;
    double slopeExponent = 1.0;
    double precipitation = 1.0;
};

class InciseFlowFilter final : public Filter {
public:
    std::string_view GetName() const override { return "Incise flow"; }
    std::vector<float> Apply(const SphericalGridSnapshot& grid,
                             std::span<const float> elevations) const override;
    const InciseFlowSettings& GetSettings() const { return settings_; }
    void SetSettings(const InciseFlowSettings& settings);

private:
    InciseFlowSettings settings_;
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
