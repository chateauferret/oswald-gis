#pragma once

#include "BrushTool.hpp"
#include "ScriptEngine.hpp"
#include "ScriptParameter.hpp"

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
    BrushApplication application = BrushApplication::Noise;
    bool continuous = false;
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
//   distance    geodesic distance in brush units; zero for selection fills
//   radius      outer brush radius, in brush distance units
//   lower, upper  ordered Between/And range
//   elapsed     seconds since the previous dab (for continuous scripts)
//   rate        configurable Raise/Lower speed, in elevation units per second
// gaussian(range, sigma, unit) samples the pre-dab neighbourhood; unit is 0 for
// kilometres or 1 for miles. Direct elevation/delta scripts ignore noise controls.
// Noise functions use the brush seed. See ScriptEngine for the full function list.
class ScriptedBrushTool final : public BrushTool {
public:
    static constexpr std::string_view DefaultScript = "fbm(x, y, z, 4, 2, 0.5, 6)";

    explicit ScriptedBrushTool(std::string name = "Custom");

    std::string_view GetName() const override { return name_; }
    bool IsContinuous() const override { return settings_.continuous; }
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
    // Atomically replaces the script and its parameters; omitted values use the defaults.
    void SetScript(std::string_view source, const std::vector<ScriptParameter>& parameters,
                   const std::vector<ScriptParameterValue>& values = {});
    static void ValidateParameters(const std::vector<ScriptParameter>& parameters);
    std::vector<ScriptParameter> GetParameters() const;
    std::vector<ScriptParameterValue> GetParameterValues() const;
    void SetParameterValue(std::size_t index, const ScriptParameterValue& value);

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
        double* elapsed = nullptr;
        double* rate = nullptr;
    };

    struct EvaluationContext {
        const SphericalGridSnapshot* grid = nullptr;
        const std::vector<float>* elevations = nullptr;
        std::size_t index = 0;
    };
    static Inputs DefineInputs(ScriptEngine& engine,
                               std::shared_ptr<EvaluationContext> context = {});
    std::string name_;
    ScriptedBrushSettings settings_;
    // Evaluation writes the bound inputs, so Apply mutates the engine under mutex_.
    mutable std::mutex mutex_;
    mutable ScriptEngine engine_;
    Inputs inputs_;
    std::shared_ptr<EvaluationContext> context_;
    std::optional<CompiledScript> script_;
    std::vector<ScriptParameter> parameters_;
    std::vector<ScriptParameterValue> parameterValues_;
    std::vector<std::variant<double*, std::string*>> parameterBindings_;
};
