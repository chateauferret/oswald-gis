#include "ScriptedBrushTool.hpp"

#include <algorithm>
#include <cmath>

ScriptedBrushTool::ScriptedBrushTool() {
    inputs_.x = &engine_.DefineVariable("x");
    inputs_.y = &engine_.DefineVariable("y");
    inputs_.z = &engine_.DefineVariable("z");
    inputs_.latitude = &engine_.DefineVariable("lat");
    inputs_.longitude = &engine_.DefineVariable("lon");
    inputs_.elevation = &engine_.DefineVariable("elevation");
    inputs_.weight = &engine_.DefineVariable("weight");
    inputs_.distance = &engine_.DefineVariable("distance");
    inputs_.radius = &engine_.DefineVariable("radius");
    inputs_.lower = &engine_.DefineVariable("lower");
    inputs_.upper = &engine_.DefineVariable("upper");
    SetScript(DefaultScript);
}

std::string ScriptedBrushTool::GetScript() const {
    std::lock_guard lock(mutex_);
    return script_->GetSource();
}

void ScriptedBrushTool::SetScript(std::string_view source) {
    std::lock_guard lock(mutex_);
    auto compiled = engine_.Compile(source);
    script_.emplace(std::move(compiled));
}

std::vector<std::size_t> ScriptedBrushTool::Apply(
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    std::lock_guard lock(mutex_);
    const auto& parameters = settings_;
    const double lower = std::min(parameters.rangeBetween, parameters.rangeAnd);
    const double upper = std::max(parameters.rangeBetween, parameters.rangeAnd);
    if (parameters.output != ScriptedBrushSettings::Output::Normalized &&
        parameters.output != ScriptedBrushSettings::Output::Elevation) {
        throw std::invalid_argument("Scripted brush output mode is invalid.");
    }
    engine_.SetNoiseSeed(parameters.seed);
    return ApplyNoiseSampler(grid, center, elevations, settings, elapsedSeconds, parameters,
        [&](const NoiseSample& sample) {
            *inputs_.x = sample.position.x;
            *inputs_.y = sample.position.y;
            *inputs_.z = sample.position.z;
            *inputs_.latitude = glm::degrees(std::atan2(
                sample.location.z, std::hypot(sample.location.x, sample.location.y)));
            *inputs_.longitude = glm::degrees(std::atan2(sample.location.y, sample.location.x));
            *inputs_.elevation = sample.elevation;
            *inputs_.weight = sample.weight;
            *inputs_.distance = sample.distance;
            *inputs_.radius = settings.radius;
            *inputs_.lower = lower;
            *inputs_.upper = upper;
            const double value = script_->Evaluate();
            if (!std::isfinite(value)) {
                throw ScriptError("The brush script returned a non-finite value.");
            }
            if (parameters.output == ScriptedBrushSettings::Output::Normalized ||
                upper == lower) {
                return value;
            }
            return (std::clamp(value, lower, upper) - lower) / (upper - lower) * 2.0 - 1.0;
        });
}
