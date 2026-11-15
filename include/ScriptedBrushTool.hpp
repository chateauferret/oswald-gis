#pragma once

#include "BrushTool.hpp"
#include "ScriptEngine.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>

struct ScriptedBrushSettings : NoiseSettings {
    enum class Output {
        Normalized, // Script result in [-1, 1] is mapped to the Between/And range.
        Elevation // Script result is an elevation, clamped to the Between/And range.
    };

    Output output = Output::Normalized;
};

// Brush whose generated value is computed by a user script for each affected
// vertex. The value is then combined with the current elevation using the
// shared noise operation, falloff, and selection weighting.
//
// Read-only per-vertex inputs (assignments are discarded after each vertex):
//   x, y, z     unit vertex position, domain-warped when warping is enabled
//   lat, lon    unwarped vertex latitude and longitude, in degrees
//   elevation   current active-layer value before this dab
//   weight      falloff multiplied by selection weight, in (0, 1]
//   distance    geodesic distance from the brush centre, in brush distance units
//   radius      outer brush radius, in brush distance units
//   lower, upper  ordered Between/And range
// Noise functions use the brush seed. See ScriptEngine for the full function list.
class ScriptedBrushTool final : public BrushTool {
public:
    static constexpr std::string_view DefaultScript = "fbm(x, y, z, 4, 2, 0.5, 6)";

    ScriptedBrushTool();

    std::string_view GetName() const override { return "Scripted Brush"; }
    bool IsContinuous() const override { return false; }
    // Throws ScriptError when the script fails or returns a non-finite value.
    std::vector<std::size_t> Apply(
        const SphericalGridSnapshot& grid, glm::dvec3 center,
        std::vector<float>& elevations, const BrushSettings& settings,
        double elapsedSeconds) const override;

    ScriptedBrushSettings& GetSettings() { return settings_; }
    const ScriptedBrushSettings& GetSettings() const { return settings_; }

    std::string GetScript() const;
    // Compiles and installs a script. On ScriptError the previous script remains active.
    void SetScript(std::string_view source);

private:
    struct Inputs {
        double* x = nullptr;
        double* y = nullptr;
        double* z = nullptr;
        double* latitude = nullptr;
        double* longitude = nullptr;
        double* elevation = nullptr;
        double* weight = nullptr;
        double* distance = nullptr;
        double* radius = nullptr;
        double* lower = nullptr;
        double* upper = nullptr;
    };

    ScriptedBrushSettings settings_;
    // Evaluation writes the bound inputs, so Apply mutates the engine under mutex_.
    mutable std::mutex mutex_;
    mutable ScriptEngine engine_;
    Inputs inputs_;
    std::optional<CompiledScript> script_;
};
