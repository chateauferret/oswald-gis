#include "ScriptedBrushTool.hpp"
#include "BrushScriptFile.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<float> elevationsOf(const SphericalGridSnapshot& grid) {
    std::vector<float> elevations;
    elevations.reserve(grid.GetVertices().size());
    for (const auto& vertex : grid.GetVertices()) {
        elevations.push_back(vertex.elevation);
    }
    return elevations;
}

BrushSettings wholeGlobe() {
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;
    return settings;
}

std::vector<float> paint(const ScriptedBrushTool& tool, const SphericalGridSnapshot& grid,
                         const BrushSettings& settings = wholeGlobe()) {
    auto values = elevationsOf(grid);
    tool.Apply(grid, {0.0, 0.0, 1.0}, values, settings, 0.0);
    return values;
}

void testDefaultMatchesSimplexBrush() {
    SphericalGrid model({2000, 0.0, 17});
    const auto& grid = *model.GetSnapshot();
    ScriptedBrushTool scripted;
    check(scripted.GetScript() == ScriptedBrushTool::DefaultScript, "Default script missing.");
    SimplexNoiseBrushTool simplex;
    simplex.GetSettings().octaves = 6;
    for (const std::uint64_t seed : {0ULL, 99ULL, 0xffffffffffffffffULL}) {
        scripted.GetSettings().seed = seed;
        simplex.GetSettings().seed = seed;
        scripted.GetSettings().domainWarpEnabled = true;
        simplex.GetSettings().domainWarpEnabled = true;
        auto expected = elevationsOf(grid);
        simplex.Apply(grid, {0.0, 0.0, 1.0}, expected, wholeGlobe(), 0.0);
        check(paint(scripted, grid) == expected,
              "Default script differs from the equivalent Simplex brush.");
    }
}

void testInputs() {
    SphericalGrid model({1000, 0.0, 17});
    const auto& grid = *model.GetSnapshot();
    ScriptedBrushTool tool;
    tool.GetSettings().output = ScriptedBrushSettings::Output::Elevation;

    const auto& vertices = grid.GetVertices();
    tool.SetScript("lat * 100 + lon");
    auto values = paint(tool, grid);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const glm::dvec3 p = glm::normalize(glm::dvec3(vertices[i].position));
        const double lat = glm::degrees(std::atan2(p.z, std::hypot(p.x, p.y)));
        const double lon = glm::degrees(std::atan2(p.y, p.x));
        check(std::abs(values[i] - (lat * 100.0 + lon)) < 0.01,
              "lat/lon inputs are incorrect.");
    }

    tool.SetScript("abs(x * x + y * y + z * z - 1) < 1e-9 ? 1 : 0");
    values = paint(tool, grid);
    check(std::all_of(values.begin(), values.end(), [](float v) { return v == 1.0F; }),
          "x/y/z inputs are not unit positions.");

    tool.SetScript("distance");
    values = paint(tool, grid);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const glm::dvec3 p = glm::normalize(glm::dvec3(vertices[i].position));
        const double expected = 6371.0 * std::acos(std::clamp(p.z, -1.0, 1.0));
        check(std::abs(values[i] - expected) < 0.5, "distance input is incorrect.");
    }

    tool.SetScript("elevation + 100");
    auto start = elevationsOf(grid);
    std::fill(start.begin(), start.end(), 250.0F);
    tool.Apply(grid, {0.0, 0.0, 1.0}, start, wholeGlobe(), 0.0);
    check(std::all_of(start.begin(), start.end(),
                      [](float v) { return std::abs(v - 350.0F) < 0.01F; }),
          "elevation input or Elevation output is incorrect.");

    tool.GetSettings().rangeBetween = 500.0F;
    tool.GetSettings().rangeAnd = -20.0F;
    tool.SetScript("upper * 1000 + lower");
    values = paint(tool, grid);
    check(std::all_of(values.begin(), values.end(), [](float v) { return v == 500.0F; }),
          "Elevation output was not clamped to the ordered range.");
    tool.SetScript("var r := radius; radius := 0; lower := 0; r == 20020 ? upper : lower");
    values = paint(tool, grid);
    check(std::all_of(values.begin(), values.end(), [](float v) { return v == 500.0F; }),
          "Input assignments leaked between vertices.");

    BrushSettings falloff = wholeGlobe();
    falloff.falloffPercent = 100.0;
    tool.GetSettings().rangeBetween = 0.0F;
    tool.GetSettings().rangeAnd = 1000.0F;
    tool.SetScript("weight * 1000");
    values = paint(tool, grid, falloff);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        // Replace blends by weight, so the result is weight^2 * 1000.
        const double weight = std::sqrt(std::max(0.0, values[i] / 1000.0));
        check(weight >= 0.0 && weight <= 1.0 + 1e-6, "weight input is out of range.");
    }
}

void testOutputsAndOperations() {
    SphericalGrid model({1000, 0.0, 17});
    const auto& grid = *model.GetSnapshot();
    ScriptedBrushTool tool;
    tool.GetSettings().rangeBetween = 100.0F;
    tool.GetSettings().rangeAnd = 300.0F;
    tool.SetScript("0");
    auto values = paint(tool, grid);
    check(std::all_of(values.begin(), values.end(), [](float v) { return v == 200.0F; }),
          "Normalised output was not mapped to the range.");
    tool.SetScript("5");
    values = paint(tool, grid);
    check(std::all_of(values.begin(), values.end(), [](float v) { return v == 300.0F; }),
          "Normalised output was not clamped.");

    tool.GetSettings().output = ScriptedBrushSettings::Output::Elevation;
    tool.GetSettings().operation = NoiseSettings::Operation::Add;
    tool.GetSettings().rangeBetween = 0.0F;
    tool.GetSettings().rangeAnd = 100.0F;
    tool.SetScript("10");
    auto start = elevationsOf(grid);
    std::fill(start.begin(), start.end(), 40.0F);
    tool.Apply(grid, {0.0, 0.0, 1.0}, start, wholeGlobe(), 0.0);
    check(std::all_of(start.begin(), start.end(),
                      [](float v) { return std::abs(v - 50.0F) < 0.01F; }),
          "Add operation was not applied.");
}

void testSeed() {
    SphericalGrid model({1000, 0.0, 17});
    const auto& grid = *model.GetSnapshot();
    ScriptedBrushTool tool;
    tool.SetScript("simplex(x * 3, y * 3, z * 3)");
    tool.GetSettings().seed = 1;
    const auto first = paint(tool, grid);
    check(paint(tool, grid) == first, "Scripted noise is not deterministic.");
    tool.GetSettings().seed = 2;
    check(paint(tool, grid) != first, "Brush seed did not reach the script.");
}

void testErrors() {
    SphericalGrid model({1000, 0.0, 17});
    const auto& grid = *model.GetSnapshot();
    ScriptedBrushTool tool;
    tool.SetScript("elevation");
    bool compileFailed = false;
    try {
        tool.SetScript("height + 1");
    } catch (const ScriptError& error) {
        compileFailed = !error.Diagnostics().empty();
    }
    check(compileFailed, "Unknown input compiled.");
    check(tool.GetScript() == "elevation", "Failed compile replaced the active script.");

    const auto original = elevationsOf(grid);
    for (const char* script : {"1 / 0", "fbm(x, y, z, 4, 2, 0.5, 0)",
                               "gaussian(0, 1, 0)", "gaussian(1, -1e300, 0)",
                               "gaussian(1, 1, 2)", "spectral(x, y, z, 1, 2, 12)",
                               "var i := 0; while (i >= 0) { i += 1; }; i"}) {
        tool.SetScript(script);
        auto values = original;
        bool failed = false;
        try {
            tool.Apply(grid, {0.0, 0.0, 1.0}, values, wholeGlobe(), 0.0);
        } catch (const ScriptError&) {
            failed = true;
        }
        check(failed, "Script runtime failure was not reported.");
        check(values == original, "Failed script modified elevations.");
    }
}

void testRegistration() {
    BrushToolManager manager;
    manager.RegisterTool(std::make_unique<ScriptedBrushTool>());
    check(manager.SelectTool("Custom"), "Custom brush could not be selected.");
    check(!manager.GetSelectedTool().IsContinuous(), "Scripted brush should be one-shot.");
}

void testShippedBrushes(bool selected) {
    const auto library = BrushScriptLibrary::Scan(OSWALD_BRUSH_SCRIPT_DIR);
    check(library.errors.empty(), "Shipped brush definitions did not parse.");
    for (const auto& entry : library.entries) {
        ScriptedBrushTool tool(entry.file.name);
        tool.SetScript(entry.file.script, entry.file.parameters);
    }

    SphericalGrid model({1000, 0.0, 17});
    if (selected) {
        std::vector<float> selection(model.GetSnapshot()->GetVertices().size());
        for (std::size_t i = 0; i < selection.size(); ++i) {
            selection[i] = static_cast<float>(i % 4) / 3.0F;
        }
        model.SetSelectionWeights(selection);
    }
    const auto& grid = *model.GetSnapshot();
    const auto load = [&](const char* name) {
        const auto file = BrushScriptFile::Load(std::filesystem::path(OSWALD_BRUSH_SCRIPT_DIR) / name);
        auto tool = std::make_unique<ScriptedBrushTool>(file.name);
        tool->SetScript(file.script, file.parameters);
        tool->GetSettings().output = *file.output;
        tool->GetSettings().application = file.application;
        tool->GetSettings().continuous = file.continuous;
        return tool;
    };
    const auto set = [](ScriptedBrushTool& tool, const std::string& name, ScriptParameterValue value) {
        const auto definitions = tool.GetParameters();
        for (std::size_t i = 0; i < definitions.size(); ++i) {
            if (definitions[i].name == name) {
                tool.SetParameterValue(i, value);
                return;
            }
        }
        throw std::runtime_error("Missing shipped brush parameter: " + name);
    };
    const auto compare = [&](ScriptedBrushTool& script, const BrushTool& native,
                             BrushSettings settings = wholeGlobe()) {
        std::vector<float> expected(grid.GetVertices().size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            expected[i] = static_cast<float>(std::sin(i * 0.3) * 32000);
        }
        auto actual = expected;
        auto referenceSettings = settings;
        if (settings.coverage == BrushCoverage::Selection) {
            referenceSettings.coverage = BrushCoverage::Footprint;
            referenceSettings.radius = wholeGlobe().radius;
            referenceSettings.falloffPercent = 0;
        }
        native.Apply(grid, {0, 0, 1}, expected, referenceSettings, 0.5);
        script.Apply(grid, {0, 0, 1}, actual, settings, 0.5);
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (std::abs(actual[i] - expected[i]) > 0.01F) {
                throw std::runtime_error(std::string(script.GetName()) + " differs from the native reference.");
            }
        }
        check(script.IsContinuous() == native.IsContinuous(), "Continuous behavior changed.");
    };
    BrushSettings tapered = wholeGlobe();
    tapered.falloffPercent = 100;
    BrushSettings selectionFill;
    selectionFill.coverage = BrushCoverage::Selection;
    selectionFill.radius = 0.1;
    selectionFill.falloffPercent = 100;
    for (auto settings : {wholeGlobe(), tapered, selectionFill}) {
        auto elevation = load("elevation.toml");
        ElevationBrushTool nativeElevation;
        settings.value = 1234;
        set(*elevation, "Elevation", 1234.0);
        for (const auto& [name, operation] : std::vector<std::pair<std::string, BrushOperation>>{
                 {"Replace", BrushOperation::Replace}, {"Minimum", BrushOperation::Minimum},
                 {"Maximum", BrushOperation::Maximum}}) {
            settings.operation = operation;
            set(*elevation, "Operation", name);
            compare(*elevation, nativeElevation, settings);
        }
        auto raiseLower = load("raise_lower.toml");
        RaiseLowerBrushTool nativeRaiseLower;
        settings.intensityPercent = 75;
        settings.raiseLowerRate = 16000;
        set(*raiseLower, "Intensity", 75.0);
        for (const auto& [name, operation] : std::vector<std::pair<std::string, RaiseLowerOperation>>{
                 {"Raise", RaiseLowerOperation::Raise}, {"Lower", RaiseLowerOperation::Lower}}) {
            settings.raiseLowerOperation = operation;
            set(*raiseLower, "Operation", name);
            compare(*raiseLower, nativeRaiseLower, settings);
        }
        auto clamp = load("clamp.toml");
        ClampBrushTool nativeClamp;
        nativeClamp.GetSettings() = {-1000, 2000};
        set(*clamp, "Minimum", -1000.0);
        set(*clamp, "Maximum", 2000.0);
        compare(*clamp, nativeClamp, settings);

        auto blur = load("gaussian_blur.toml");
        GaussianBlurBrushTool nativeBlur;
        nativeBlur.GetSettings().range = 1500;
        nativeBlur.GetSettings().sigma = 700;
        set(*blur, "Range", 1500.0);
        set(*blur, "Sigma", 700.0);
        compare(*blur, nativeBlur, settings);
        set(*blur, "Units", std::string("Miles"));
        compare(*blur, nativeBlur, settings);

        auto simplex = load("simplex.toml");
        SimplexNoiseBrushTool nativeSimplex;
        auto value = load("value.toml");
        ValueNoiseBrushTool nativeValue;
        auto ridge = load("ridge.toml");
        RidgeNoiseBrushTool nativeRidge;
        auto spectral = load("spectral.toml");
        PowerLawNoiseBrushTool nativeSpectral;
        set(*spectral, "Resolution", std::string("8"));
        nativeSpectral.GetSettings().gridResolution = 8;
        for (const bool warped : {false, true}) {
            for (const auto& [script, native, common] :
                 std::vector<std::tuple<ScriptedBrushTool*, BrushTool*, NoiseSettings*>>{
                     {simplex.get(), &nativeSimplex, &nativeSimplex.GetSettings()},
                     {value.get(), &nativeValue, &nativeValue.GetSettings()},
                     {ridge.get(), &nativeRidge, &nativeRidge.GetSettings()},
                     {spectral.get(), &nativeSpectral, &nativeSpectral.GetSettings()}}) {
                script->GetSettings().seed = common->seed = 99;
                script->GetSettings().domainWarpEnabled = common->domainWarpEnabled = warped;
                script->GetSettings().operation = common->operation = NoiseSettings::Operation::Add;
                script->GetSettings().rangeBetween = common->rangeBetween = -1000;
                script->GetSettings().rangeAnd = common->rangeAnd = 2000;
                compare(*script, *native, settings);
            }
        }
        nativeSimplex.GetSettings().billowed = true;
        set(*simplex, "Billowed", std::string("Yes"));
        compare(*simplex, nativeSimplex, settings);
    }
}

void testSelectionFill() {
    SphericalGrid model({1000, 0.0, 17});
    const auto count = model.GetSnapshot()->GetVertices().size();
    std::vector<float> selection(count);
    for (std::size_t i = 0; i < count; ++i) {
        selection[i] = static_cast<float>(i % 5) / 4.0F;
    }
    BrushSettings settings;
    settings.coverage = BrushCoverage::Selection;
    settings.radius = 0.1;
    settings.falloffPercent = 100;
    ScriptedBrushTool tool;
    tool.GetSettings().application = BrushApplication::Elevation;
    tool.SetScript("distance == 0 and radius == 0.1 ? 1000 : -1000");
    for (const bool selected : {false, true}) {
        model.SetSelectionWeights(selection, selected);
        const auto snapshot = model.GetSnapshot();
        std::vector<float> values(count, 100.0F);
        const auto changed = tool.Apply(*snapshot, {0, 0, 1}, values, settings, 1.0);
        std::size_t expectedChanges = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const float weight = selected ? selection[i] : 1.0F;
            check(values[i] == 100.0F + 900.0F * weight,
                  "Selection fill did not apply feathering exactly once or ignored coverage.");
            expectedChanges += weight > 0;
        }
        check(changed.size() == expectedChanges, "Selection fill returned incorrect changed indices.");
    }

    tool.GetSettings().application = BrushApplication::Delta;
    tool.GetSettings().continuous = true;
    tool.SetScript("rate * elapsed");
    std::vector<float> values(count, 32000.0F);
    const auto snapshot = model.GetSnapshot();
    tool.Apply(*snapshot, {0, 0, 1}, values, settings, 1.0);
    for (std::size_t i = 0; i < count; ++i) {
        check(values[i] == static_cast<float>(
                  std::min(32767.0, 32000.0 + settings.raiseLowerRate * selection[i])),
              "Continuous fill did not apply one second before weighted saturation.");
    }
    const auto before = values;
    tool.SetScript("weight < 1 ? 100 : sqrt(-1)");
    try {
        tool.Apply(*snapshot, {0, 0, 1}, values, settings, 1.0);
        check(false, "Invalid fill script did not fail.");
    } catch (const ScriptError&) {
        check(values == before, "Failed selection fill partially changed elevations.");
    }
    std::fill(selection.begin(), selection.end(), 0.0F);
    model.SetSelectionWeights(selection);
    check(tool.Apply(*model.GetSnapshot(), {0, 0, 1}, values, settings, 1.0).empty() &&
          values == before, "Empty active selection was filled.");
}

void testParameters() {
    const auto file = BrushScriptFile::Parse(
        "name='parameters'\nscript=\"mode == 'one' ? amount + count : -amount\"\n"
        "[[parameter]]\nname='amount'\ntype='float'\ncontrol='slider'\n"
        "min=0.1\nmax=100\nscale='log'\ndefault=4\n"
        "[[parameter]]\nname='count'\ntype='integer'\ncontrol='spin'\nmin=0\nmax=30\ndefault=5\n"
        "[[parameter]]\nname='mode'\ntype='choice'\nchoices=['one','two']\ndefault='one'\n");
    SphericalGrid model({1000, 0.0, 17});
    const auto& grid = *model.GetSnapshot();
    ScriptedBrushTool tool;
    tool.GetSettings().output = ScriptedBrushSettings::Output::Elevation;
    const auto expect = [&](float expected) {
        const auto values = paint(tool, grid);
        check(std::all_of(values.begin(), values.end(),
                          [=](float v) { return std::abs(v - expected) < 0.001F; }),
              "Script did not evaluate using the current parameter values.");
    };
    tool.SetScript(file.script, file.parameters);
    check(tool.GetParameters() == file.parameters, "Parameter definitions were not installed.");
    expect(9.0F);
    tool.SetParameterValue(0, 20.0);
    tool.SetParameterValue(1, 7.0);
    expect(27.0F);
    tool.SetParameterValue(2, std::string("two"));
    expect(-20.0F);
    const auto currentValues = tool.GetParameterValues();
    tool.SetScript("mode := 'one'; amount += 1; amount");
    expect(21.0F);
    expect(21.0F);
    check(tool.GetParameterValues() == currentValues, "Script assignments changed control values.");
    tool.SetScript(file.script, file.parameters, currentValues);
    expect(-20.0F);

    for (const auto& [index, value] : std::vector<std::pair<std::size_t, ScriptParameterValue>>{
             {0, 0.0}, {0, std::string("bad")}, {1, 2.5}, {2, std::string("bad")}, {2, 0.0}}) {
        bool failed = false;
        try {
            tool.SetParameterValue(index, value);
        } catch (const std::invalid_argument&) {
            failed = true;
        }
        check(failed && tool.GetParameterValues() == currentValues,
              "Invalid parameter update changed active control values.");
    }
    try {
        tool.SetScript("unknown", {});
        check(false, "Invalid replacement script compiled.");
    } catch (const ScriptError&) {
        check(tool.GetParameters() == file.parameters &&
              tool.GetParameterValues() == currentValues && tool.GetScript() == file.script,
              "Failed replacement changed the active script or parameters.");
    }
    expect(-20.0F);
    tool.SetScript("1", {});
    check(tool.GetParameters().empty() && tool.GetParameterValues().empty(),
          "Loading a parameterless script did not clear the old parameters.");
    try {
        tool.SetScript("amount");
        check(false, "A removed parameter remained available.");
    } catch (const ScriptError&) {
        expect(1.0F);
    }
    tool.SetScript(file.script, file.parameters);
    expect(9.0F);
}

} // namespace

int main() {
    try {
        testDefaultMatchesSimplexBrush();
        testInputs();
        testOutputsAndOperations();
        testSeed();
        testErrors();
        testRegistration();
        testParameters();
        testSelectionFill();
        testShippedBrushes(false);
        testShippedBrushes(true);
        std::cout << "Scripted brush tool tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scripted brush tool test failed: " << error.what() << '\n';
        return 1;
    }
}
