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

struct PrecipitonSettings {
    double diffusionCoefficient = 1.0;
};

class PrecipitonFilter final : public Filter {
public:
    std::string_view GetName() const override { return "Precipiton diffusion"; }
    std::vector<float> Apply(const SphericalGridSnapshot& grid,
                             std::span<const float> elevations) const override;
    const PrecipitonSettings& GetSettings() const { return settings_; }
    void SetSettings(const PrecipitonSettings& settings);

private:
    PrecipitonSettings settings_;
};

struct ThermalWeatheringSettings {
    double angleOfReposeDegrees = 35.0;
};

class ThermalWeatheringFilter final : public Filter {
public:
    std::string_view GetName() const override { return "Thermal weathering"; }
    std::vector<float> Apply(const SphericalGridSnapshot& grid,
                             std::span<const float> elevations) const override;
    const ThermalWeatheringSettings& GetSettings() const { return settings_; }
    void SetSettings(const ThermalWeatheringSettings& settings);

private:
    ThermalWeatheringSettings settings_;
};

struct GlacialErosionSettings {
    double equilibriumLineAltitude = 2500.0;
    double elaLatitudeGradient = 20.0;
    double accumulationRate = 0.01;
    double ablationRate = 0.01;
    double initialIceThickness = 0.0;
    double durationYears = 1.0;
    double glacialErodibility = 0.001;
    double iceViscosityFactor = 0.01;
    double thicknessExponent = 1.0;
    double velocityExponent = 2.0;
    double troughHalfWidth = 100.0;
};

class GlacialErosionFilter final : public Filter {
public:
    std::string_view GetName() const override { return "Glacial erosion"; }
    std::vector<float> Apply(const SphericalGridSnapshot& grid,
                             std::span<const float> elevations) const override;
    const GlacialErosionSettings& GetSettings() const { return settings_; }
    void SetSettings(const GlacialErosionSettings& settings);

private:
    GlacialErosionSettings settings_;
};

struct CratonicGroovingSettings {
    glm::dvec3 iceFlowDirection{1.0, 0.0, 0.0};
    double erodibility = 0.5;
    double grooveCoefficient = 1.0;
    double anisotropy = 1.0;
};

class CratonicGroovingFilter final : public Filter {
public:
    std::string_view GetName() const override { return "Cratonic directional grooving"; }
    std::vector<float> Apply(const SphericalGridSnapshot& grid,
                             std::span<const float> elevations) const override;
    const CratonicGroovingSettings& GetSettings() const { return settings_; }
    void SetSettings(const CratonicGroovingSettings& settings);

private:
    CratonicGroovingSettings settings_;
};

struct CratonicScourSettings {
    glm::dvec3 iceFlowDirection{1.0, 0.0, 0.0};
    double erodibility = 0.5;
    double stossAbrasionCoefficient = 0.01;
    double pluckingRate = 0.1;
    double cratonElevation = 0.0;
    double planationAttenuation = 0.001;
};

class CratonicScourFilter final : public Filter {
public:
    std::string_view GetName() const override { return "Cratonic scour and planation"; }
    std::vector<float> Apply(const SphericalGridSnapshot& grid,
                             std::span<const float> elevations) const override;
    const CratonicScourSettings& GetSettings() const { return settings_; }
    void SetSettings(const CratonicScourSettings& settings);

private:
    CratonicScourSettings settings_;
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
