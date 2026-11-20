#include "ScriptedBrushTool.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

void ScriptParameter::ValidateValue(const ScriptParameterValue& value) const {
    if (type == Type::Choice) {
        const auto* text = std::get_if<std::string>(&value);
        if (text != nullptr && std::find(choices.begin(), choices.end(), *text) != choices.end()) {
            return;
        }
    } else if (const auto* number = std::get_if<double>(&value)) {
        if (std::isfinite(*number) && *number >= minimum && *number <= maximum &&
            (type != Type::Integer || *number == std::floor(*number))) {
            return;
        }
    }
    throw std::invalid_argument("Parameter '" + name + "' has an invalid value.");
}

void ScriptParameter::Validate() const {
    if (type == Type::Choice) {
        if (choices.empty()) {
            throw std::invalid_argument("Parameter '" + name + "' needs choices.");
        }
        for (std::size_t i = 0; i < choices.size(); ++i) {
            if (std::find(choices.begin(), choices.begin() + i, choices[i]) != choices.begin() + i) {
                throw std::invalid_argument("Parameter '" + name + "' has duplicate choices.");
            }
        }
    } else {
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum) {
            throw std::invalid_argument("Parameter '" + name + "' needs finite min < max.");
        }
        // Dear ImGui's double sliders require headroom for their range arithmetic.
        if (type == Type::Float && control == Control::Slider &&
            (minimum < -std::numeric_limits<double>::max() * 0.5 ||
             maximum > std::numeric_limits<double>::max() * 0.5)) {
            throw std::invalid_argument("Parameter '" + name +
                                       "' slider bounds exceed the supported double range.");
        }
        if (type == Type::Integer &&
            (minimum != std::floor(minimum) || maximum != std::floor(maximum) ||
             minimum < std::numeric_limits<int>::min() || maximum > std::numeric_limits<int>::max())) {
            throw std::invalid_argument("Parameter '" + name + "' needs 32-bit integer bounds.");
        }
        if (scale == Scale::Log && (type != Type::Float || minimum <= 0.0)) {
            throw std::invalid_argument("Parameter '" + name + "' needs positive float bounds for log scale.");
        }
    }
    ValidateValue(defaultValue);
}

ScriptedBrushTool::Inputs ScriptedBrushTool::DefineInputs(
    ScriptEngine& engine, std::shared_ptr<EvaluationContext> context) {
    engine.DefineFunction("gaussian", 3, 3, [context](std::span<const double> a) {
        if (!context || !context->grid || !context->elevations) {
            throw ScriptError("gaussian() is only available during brush evaluation.");
        }
        if (a[2] != 0.0 && a[2] != 1.0) {
            throw ScriptError("gaussian() distance unit must be 0 (kilometres) or 1 (miles).");
        }
        if (!std::isfinite(a[1]) || a[1] <= 0.0 || a[1] > std::numeric_limits<float>::max()) {
            throw ScriptError("gaussian() sigma must be a positive finite float.");
        }
        return SampleGaussian(*context->grid, *context->elevations, context->index,
            {a[2] == 0.0 ? BrushDistanceUnit::Kilometres : BrushDistanceUnit::Miles,
             a[0], static_cast<float>(a[1])});
    });
    return {&engine.DefineVariable("x"), &engine.DefineVariable("y"),
            &engine.DefineVariable("z"), &engine.DefineVariable("lat"),
            &engine.DefineVariable("lon"), &engine.DefineVariable("elevation"),
            &engine.DefineVariable("weight"), &engine.DefineVariable("distance"),
            &engine.DefineVariable("radius"), &engine.DefineVariable("lower"),
            &engine.DefineVariable("upper"), &engine.DefineVariable("elapsed"),
            &engine.DefineVariable("rate")};
}

void ScriptedBrushTool::ValidateParameters(const std::vector<ScriptParameter>& parameters) {
    ScriptEngine engine;
    DefineInputs(engine);
    for (const auto& parameter : parameters) {
        parameter.Validate();
        if (!parameter.units.empty()) {
            const auto unit = std::find_if(parameters.begin(), parameters.end(),
                [&](const auto& candidate) { return candidate.name == parameter.units; });
            if (parameter.type != ScriptParameter::Type::Float || unit == parameters.end() ||
                unit->type != ScriptParameter::Type::Choice ||
                unit->choices != std::vector<std::string>{"Kilometres", "Miles"}) {
                throw std::invalid_argument("Parameter '" + parameter.name +
                    "' units must reference a Kilometres/Miles choice parameter.");
            }
        }
        if (parameter.type == ScriptParameter::Type::Choice) {
            engine.DefineStringVariable(parameter.name);
        } else {
            engine.DefineVariable(parameter.name);
        }
    }
}

ScriptedBrushTool::ScriptedBrushTool(std::string name) : name_(std::move(name)) {
    SetScript(DefaultScript, {});
}

std::string ScriptedBrushTool::GetScript() const {
    std::lock_guard lock(mutex_);
    return script_->GetSource();
}

std::optional<std::string> ScriptedBrushTool::GetGpuScript() const {
    std::lock_guard lock(mutex_);
    return gpuScript_;
}

void ScriptedBrushTool::SetGpuScript(std::optional<std::string> source) {
    if (source && (source->empty() || source->size() > 64 * 1024)) {
        throw std::invalid_argument("GPU brush body must contain between 1 byte and 64 KiB of GLSL.");
    }
    std::lock_guard lock(mutex_);
    gpuScript_ = std::move(source);
}

bool ScriptedBrushTool::HasGpuScript() const {
    std::lock_guard lock(mutex_);
    return gpuScript_.has_value();
}

std::vector<std::size_t> ScriptedBrushTool::ApplyGpu(
    BrushScriptComputeBackend& compute,
    const SphericalGridSnapshot& grid, glm::dvec3 center, std::vector<float>& elevations,
    const BrushSettings& settings, double elapsedSeconds) const {
    std::lock_guard lock(mutex_);
    if (!gpuScript_) {
        throw std::logic_error("This brush does not define a GPU GLSL body.");
    }
    const auto& parameters = settings_;
    const double lower = std::min(parameters.rangeBetween, parameters.rangeAnd);
    const double upper = std::max(parameters.rangeBetween, parameters.rangeAnd);
    if (parameters.output != ScriptedBrushSettings::Output::Normalized &&
        parameters.output != ScriptedBrushSettings::Output::Elevation) {
        throw std::invalid_argument("Scripted brush output mode is invalid.");
    }
    if (!std::isfinite(settings.raiseLowerRate) || settings.raiseLowerRate <= 0.0) {
        throw std::invalid_argument("Brush rate must be finite and positive.");
    }
    if (elevations.size() != grid.GetVertices().size()) {
        throw std::invalid_argument("Brush elevation count must match the grid vertex count.");
    }

    std::vector<GpuScriptParameter> gpuParameters;
    gpuParameters.reserve(parameters_.size());
    for (std::size_t i = 0; i < parameters_.size(); ++i) {
        const auto& definition = parameters_[i];
        const auto& value = parameterValues_[i];
        float numericValue = 0.0F;
        if (definition.type == ScriptParameter::Type::Choice) {
            const auto& choice = std::get<std::string>(value);
            const auto found = std::find(definition.choices.begin(), definition.choices.end(), choice);
            if (found == definition.choices.end()) {
                throw std::invalid_argument("GPU brush choice parameter has an invalid value.");
            }
            numericValue = static_cast<float>(std::distance(definition.choices.begin(), found));
        } else {
            numericValue = static_cast<float>(std::get<double>(value));
            if (!std::isfinite(numericValue)) {
                throw std::invalid_argument("GPU brush numeric parameter exceeds the GLSL float range.");
            }
        }
        gpuParameters.push_back({definition.name, numericValue});
    }
    const auto common = parameters.application == BrushApplication::Noise ?
        static_cast<const NoiseSettings&>(parameters) : NoiseSettings{};
    std::vector<GpuScriptTerrainVertex> terrainVertices;
    std::vector<float> terrainElevations;
    terrainVertices.reserve(grid.GetVertices().size());
    terrainElevations.reserve(grid.GetVertices().size());
    for (std::size_t i = 0; i < grid.GetVertices().size(); ++i) {
        const auto position = glm::normalize(glm::dvec3(grid.GetVertices()[i].position));
        terrainVertices.push_back({{
            static_cast<float>(position.x), static_cast<float>(position.y),
            static_cast<float>(position.z), 0.0F}});
        terrainElevations.push_back(elevations[i]);
    }
    return ApplyNoiseSamplerBatch(grid, center, elevations, settings, elapsedSeconds, common,
        [&](std::span<const NoiseSample> samples) {
            std::vector<GpuScriptInput> inputs;
            inputs.reserve(samples.size());
            const auto toFloat = [](double value) {
                const float result = static_cast<float>(value);
                if (!std::isfinite(result)) {
                    throw std::invalid_argument("GPU brush input exceeds the GLSL float range.");
                }
                return result;
            };
            for (const auto& sample : samples) {
                GpuScriptInput input{};
                input.positionAndLatitude = {
                    toFloat(sample.position.x), toFloat(sample.position.y),
                    toFloat(sample.position.z),
                    toFloat(glm::degrees(std::atan2(sample.location.z,
                        std::hypot(sample.location.x, sample.location.y))))};
                input.terrain = {
                    toFloat(glm::degrees(std::atan2(sample.location.y, sample.location.x))),
                    sample.elevation, toFloat(sample.weight), toFloat(sample.distance)};
                input.brush = {toFloat(settings.radius), toFloat(lower), toFloat(upper),
                               toFloat(elapsedSeconds)};
                input.timing = {toFloat(settings.raiseLowerRate), 0.0F, 0.0F, 0.0F};
                if (sample.index > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::length_error("GPU brush vertex index exceeds the GLSL range.");
                }
                input.vertexIndex = static_cast<std::uint32_t>(sample.index);
                inputs.push_back(input);
            }
            auto values = compute.EvaluateBrushScript(*gpuScript_, inputs, gpuParameters,
                parameters.seed, terrainVertices, terrainElevations,
                grid.GetOptions().planetRadius);
            if (parameters.application == BrushApplication::Noise &&
                parameters.output == ScriptedBrushSettings::Output::Elevation &&
                upper != lower) {
                for (auto& value : values) {
                    value = (std::clamp(static_cast<double>(value), lower, upper) - lower) /
                        (upper - lower) * 2.0 - 1.0;
                }
            }
            return std::vector<double>(values.begin(), values.end());
        }, parameters.application);
}

void ScriptedBrushTool::SetScript(std::string_view source) {
    std::lock_guard lock(mutex_);
    auto compiled = engine_.Compile(source);
    script_.emplace(std::move(compiled));
}

void ScriptedBrushTool::SetScript(std::string_view source,
                                  const std::vector<ScriptParameter>& parameters,
                                  const std::vector<ScriptParameterValue>& values) {
    std::lock_guard lock(mutex_);
    if (!values.empty() && values.size() != parameters.size()) {
        throw std::invalid_argument("Script parameter value count does not match definitions.");
    }
    ValidateParameters(parameters);
    ScriptEngine engine;
    auto context = std::make_shared<EvaluationContext>();
    auto inputs = DefineInputs(engine, context);
    auto definitions = parameters;
    std::vector<ScriptParameterValue> currentValues;
    std::vector<std::variant<double*, std::string*>> bindings;
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const auto& parameter = parameters[i];
        parameter.Validate();
        const auto& value = values.empty() ? parameter.defaultValue : values[i];
        parameter.ValidateValue(value);
        currentValues.push_back(value);
        if (parameter.type == ScriptParameter::Type::Choice) {
            bindings.emplace_back(&engine.DefineStringVariable(parameter.name, std::get<std::string>(value)));
        } else {
            bindings.emplace_back(&engine.DefineVariable(parameter.name, std::get<double>(value)));
        }
    }
    auto compiled = engine.Compile(source);
    engine_ = std::move(engine);
    inputs_ = inputs;
    context_ = std::move(context);
    script_.emplace(std::move(compiled));
    parameters_ = std::move(definitions);
    parameterValues_ = std::move(currentValues);
    parameterBindings_ = std::move(bindings);
}

std::vector<ScriptParameter> ScriptedBrushTool::GetParameters() const {
    std::lock_guard lock(mutex_);
    return parameters_;
}

std::vector<ScriptParameterValue> ScriptedBrushTool::GetParameterValues() const {
    std::lock_guard lock(mutex_);
    return parameterValues_;
}

void ScriptedBrushTool::SetParameterValue(std::size_t index, const ScriptParameterValue& value) {
    std::lock_guard lock(mutex_);
    parameters_.at(index).ValidateValue(value);
    parameterValues_.at(index) = value;
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
    if (!std::isfinite(settings.raiseLowerRate) || settings.raiseLowerRate <= 0.0) {
        throw std::invalid_argument("Brush rate must be finite and positive.");
    }
    context_->grid = &grid;
    context_->elevations = &elevations;
    struct ResetContext {
        EvaluationContext& context;
        ~ResetContext() { context = {}; }
    } reset{*context_};
    const auto common = parameters.application == BrushApplication::Noise ?
        static_cast<const NoiseSettings&>(parameters) : NoiseSettings{};
    return ApplyNoiseSampler(grid, center, elevations, settings, elapsedSeconds, common,
        [&](const NoiseSample& sample) {
            context_->index = sample.index;
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
            *inputs_.elapsed = elapsedSeconds;
            *inputs_.rate = settings.raiseLowerRate;
            // Discard script assignments to parameters between vertices, just like inputs.
            for (std::size_t i = 0; i < parameterBindings_.size(); ++i) {
                if (auto* number = std::get_if<double*>(&parameterBindings_[i])) {
                    **number = std::get<double>(parameterValues_[i]);
                } else {
                    *std::get<std::string*>(parameterBindings_[i]) =
                        std::get<std::string>(parameterValues_[i]);
                }
            }
            const double value = script_->Evaluate();
            if (!std::isfinite(value)) {
                throw ScriptError("The brush script returned a non-finite value.");
            }
            if (parameters.application != BrushApplication::Noise ||
                parameters.output == ScriptedBrushSettings::Output::Normalized ||
                upper == lower) {
                return value;
            }
            return (std::clamp(value, lower, upper) - lower) / (upper - lower) * 2.0 - 1.0;
        }, parameters.application);
}
