#pragma once

#include "SphericalGrid.hpp"

#include <glm/glm.hpp>

#include <memory>
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

struct BrushSettings {
    BrushOperation operation = BrushOperation::Replace;
    RaiseLowerOperation raiseLowerOperation = RaiseLowerOperation::Raise;
    BrushDistanceUnit distanceUnit = BrushDistanceUnit::Kilometres;
    double radius = 500.0; // Outer geodesic radius in the selected distance unit.
    double falloffPercent = 25.0; // Outer-radius percentage used for the tapered region.
    double intensityPercent = 100.0;
    double raiseLowerRate = 8192.0;
    float value = 0.0F;
};

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

class BrushToolManager {
public:
    BrushToolManager();

    void RegisterTool(std::unique_ptr<BrushTool> tool);
    bool SelectTool(std::string_view name);
    void SetActive(bool active) { active_ = active; }
    bool IsActive() const { return active_; }
    BrushTool& GetSelectedTool() { return *tools_.at(selectedIndex_); }
    const BrushTool& GetSelectedTool() const { return *tools_.at(selectedIndex_); }
    const std::vector<std::unique_ptr<BrushTool>>& GetTools() const { return tools_; }
    ElevationBrushTool& GetElevationBrush();
    const ElevationBrushTool& GetElevationBrush() const;
    BrushSettings& GetSettings() { return settings_; }
    const BrushSettings& GetSettings() const { return settings_; }

private:
    std::vector<std::unique_ptr<BrushTool>> tools_;
    std::size_t selectedIndex_ = 0;
    BrushSettings settings_;
    bool active_ = true;
};
