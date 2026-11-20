#pragma once

#include "Noise.hpp"
#include "SphericalGrid.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

enum class BrushOperation {
    Replace,
    Minimum,
    Maximum
};

enum class RaiseLowerOperation {
    Raise,
    Lower
};

enum class BrushDistanceUnit {
    Kilometres,
    Miles
};

inline double PlanetRadiusInUnit(const SphericalGridSnapshot& grid, BrushDistanceUnit unit) {
    const double radius = grid.GetOptions().planetRadius;
    return unit == BrushDistanceUnit::Kilometres ? radius : radius / 1.609344;
}

enum class BrushCoverage { Footprint, Selection };

struct BrushSettings {
    // Selection coverage ignores the footprint and uses only selection feathering.
    BrushCoverage coverage = BrushCoverage::Footprint;
    BrushOperation operation = BrushOperation::Replace;
    RaiseLowerOperation raiseLowerOperation = RaiseLowerOperation::Raise;
    BrushDistanceUnit distanceUnit = BrushDistanceUnit::Kilometres;
    double radius = 500.0; // Outer geodesic radius in the selected distance unit.
    double falloffPercent = 25.0; // Outer-radius percentage used for the tapered region.
    double intensityPercent = 100.0;
    double raiseLowerRate = 8192.0;
    float value = 0.0F;
};

struct NoiseSettings {
    enum class Operation {
        Replace,
        Add,
        Subtract,
        Minimum,
        Maximum
    };

    Operation operation = Operation::Replace;
    float rangeBetween = -32767.0F;
    float rangeAnd = 32767.0F;
    std::uint64_t seed = 0;
    bool domainWarpEnabled = false;
    float warpFrequency = 4.0F;
    float warpStrength = 1.25F;
    float warpLacunarity = 2.0F;
    float warpPersistence = 0.5F;
    int warpOctaves = 3;
};

struct FractalNoiseSettings : NoiseSettings {
    float frequency = 4.0F;
    float lacunarity = 2.0F;
    float persistence = 0.5F;
    int octaves = 4;
};

struct SimplexNoiseSettings : FractalNoiseSettings {
    using Operation = NoiseSettings::Operation;
    bool billowed = false;
};

struct ValueNoiseSettings : FractalNoiseSettings {
    float amplitude = 1.0F;
};

struct RidgeNoiseSettings : FractalNoiseSettings {};

struct PowerLawNoiseSettings : NoiseSettings {
    float frequency = 4.0F;
    float spectralExponent = 2.0F;
    int gridResolution = 32;
};

struct GaussianBlurSettings {
    BrushDistanceUnit distanceUnit = BrushDistanceUnit::Kilometres;
    double range = 100.0;
    float sigma = 25.0F;
};

struct ClampSettings {
    float minimum = -32767.0F;
    float maximum = 32767.0F;
};

// Per-vertex input passed to a noise sampler.
struct NoiseSample {
    glm::dvec3 position{0.0}; // Unit position after optional domain warping.
    glm::dvec3 location{0.0}; // Unwarped unit vertex position.
    std::size_t index = 0;
    float elevation = 0.0F; // Value before this dab.
    double weight = 0.0; // Falloff multiplied by selection weight, in (0, 1].
    double distance = 0.0; // Geodesic distance in brush units; zero for selection fills.
};

// Returns noise in [-1, 1]; values outside it are clamped.
using NoiseSampler = std::function<double(const NoiseSample&)>;
using NoiseBatchSampler =
    std::function<std::vector<double>(std::span<const NoiseSample>)>;

enum class BrushApplication { Noise, Elevation, Delta };

// Applies a sampler with the noise brushes' operation, output range, domain warp,
// falloff, and selection weighting. If sampling throws, elevations are unchanged.
std::vector<std::size_t> ApplyNoiseSampler(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& brushSettings, double elapsedSeconds, const NoiseSettings& parameters,
    const NoiseSampler& sample, BrushApplication application = BrushApplication::Noise);
std::vector<std::size_t> ApplyNoiseSamplerBatch(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& brushSettings, double elapsedSeconds, const NoiseSettings& parameters,
    const NoiseBatchSampler& sample, BrushApplication application = BrushApplication::Noise);

double SampleGaussian(const SphericalGridSnapshot& grid, const std::vector<float>& source,
                      std::size_t index, const GaussianBlurSettings& settings);

class BrushTool {
public:
    virtual ~BrushTool() = default;
    virtual std::string_view GetName() const = 0;
    // Continuous tools accumulate while held; others apply once per stroke.
    virtual bool IsContinuous() const = 0;
    virtual std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const BrushSettings& settings,
        double elapsedSeconds) const = 0;
};

class ElevationBrushTool final : public BrushTool {
public:
    std::string_view GetName() const override { return "Elevation Brush"; }
    bool IsContinuous() const override { return false; }
    std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const BrushSettings& settings,
        double elapsedSeconds) const override;
};

class RaiseLowerBrushTool final : public BrushTool {
public:
    std::string_view GetName() const override { return "Raise/Lower Brush"; }
    bool IsContinuous() const override { return true; }
    std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const BrushSettings& settings,
        double elapsedSeconds) const override;
};

class SimplexNoiseBrushTool final : public BrushTool {
public:
    std::string_view GetName() const override { return "Simplex Noise"; }
    bool IsContinuous() const override { return false; }
    std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const BrushSettings& settings,
        double elapsedSeconds) const override;
    SimplexNoiseSettings& GetSettings() { return settings_; }
    const SimplexNoiseSettings& GetSettings() const { return settings_; }

private:
    SimplexNoiseSettings settings_;
};

class ValueNoiseBrushTool final : public BrushTool {
public:
    std::string_view GetName() const override { return "Value Noise"; }
    bool IsContinuous() const override { return false; }
    std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const BrushSettings& settings,
        double elapsedSeconds) const override;
    ValueNoiseSettings& GetSettings() { return settings_; }
    const ValueNoiseSettings& GetSettings() const { return settings_; }

private:
    ValueNoiseSettings settings_;
};

class RidgeNoiseBrushTool final : public BrushTool {
public:
    std::string_view GetName() const override { return "Ridge Noise"; }
    bool IsContinuous() const override { return false; }
    std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const BrushSettings& settings,
        double elapsedSeconds) const override;
    RidgeNoiseSettings& GetSettings() { return settings_; }
    const RidgeNoiseSettings& GetSettings() const { return settings_; }

private:
    RidgeNoiseSettings settings_;
};

class PowerLawNoiseBrushTool final : public BrushTool {
public:
    double Sample(glm::dvec3 position, const PowerLawNoiseSettings& parameters) const;
    std::string_view GetName() const override { return "Power-Law Spectral Noise"; }
    bool IsContinuous() const override { return false; }
    std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const BrushSettings& settings,
        double elapsedSeconds) const override;
    PowerLawNoiseSettings& GetSettings() { return settings_; }
    const PowerLawNoiseSettings& GetSettings() const { return settings_; }

private:
    PowerLawNoiseSettings settings_;
    mutable std::mutex cacheMutex_;
    mutable std::shared_ptr<const std::vector<float>> cachedField_;
    mutable std::uint64_t cachedSeed_ = 0;
    mutable int cachedResolution_ = 0;
    mutable float cachedExponent_ = 0.0F;
};

class GaussianBlurBrushTool final : public BrushTool {
public:
    std::string_view GetName() const override { return "Gaussian Blur"; }
    bool IsContinuous() const override { return false; }
    std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const BrushSettings& settings,
        double elapsedSeconds) const override;
    GaussianBlurSettings& GetSettings() { return settings_; }
    const GaussianBlurSettings& GetSettings() const { return settings_; }

private:
    GaussianBlurSettings settings_;
};

class ClampBrushTool final : public BrushTool {
public:
    std::string_view GetName() const override { return "Clamp"; }
    bool IsContinuous() const override { return false; }
    std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const BrushSettings& settings,
        double elapsedSeconds) const override;
    ClampSettings& GetSettings() { return settings_; }
    const ClampSettings& GetSettings() const { return settings_; }

private:
    ClampSettings settings_;
};

class BrushToolManager {
public:
    BrushToolManager() = default;
    // Retains the named tool instance (e.g. the Custom editor) after the replacement tools.
    void ReplaceTools(std::vector<std::unique_ptr<BrushTool>> tools, std::string_view retain = {});

    void RegisterTool(std::unique_ptr<BrushTool> tool);
    bool SelectTool(std::string_view name);
    void SetActive(bool active) { active_ = active; }
    bool IsActive() const { return active_; }
    BrushTool& GetSelectedTool() { return *tools_.at(selectedIndex_); }
    const BrushTool& GetSelectedTool() const { return *tools_.at(selectedIndex_); }
    const std::vector<std::unique_ptr<BrushTool>>& GetTools() const { return tools_; }
    BrushSettings& GetSettings() { return settings_; }
    const BrushSettings& GetSettings() const { return settings_; }

private:
    std::vector<std::unique_ptr<BrushTool>> tools_;
    std::size_t selectedIndex_ = 0;
    BrushSettings settings_;
    bool active_ = true;
};
