#pragma once

#include "SphericalGrid.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string_view>
#include <vector>

enum class BrushOperation {
    Replace,
    Raise,
    Lower,
    Minimum,
    Maximum
};

enum class BrushDistanceUnit {
    Kilometres,
    Miles
};

struct ElevationBrushSettings {
    BrushOperation operation = BrushOperation::Replace;
    BrushDistanceUnit distanceUnit = BrushDistanceUnit::Kilometres;
    double radius = 500.0;
    double falloffPercent = 25.0;
    double intensityPercent = 100.0;
    float value = 0.0F;
};

class BrushTool {
public:
    virtual ~BrushTool() = default;
    virtual std::string_view GetName() const = 0;
    virtual std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const ElevationBrushSettings& settings) const = 0;
};

class ElevationBrushTool final : public BrushTool {
public:
    std::string_view GetName() const override { return "Elevation Brush"; }
    std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const ElevationBrushSettings& settings) const override;
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
    ElevationBrushTool& GetElevationBrush();
    const ElevationBrushTool& GetElevationBrush() const;
    ElevationBrushSettings& GetSettings() { return settings_; }
    const ElevationBrushSettings& GetSettings() const { return settings_; }

private:
    std::vector<std::unique_ptr<BrushTool>> tools_;
    std::size_t selectedIndex_ = 0;
    ElevationBrushSettings settings_;
    bool active_ = true;
};
