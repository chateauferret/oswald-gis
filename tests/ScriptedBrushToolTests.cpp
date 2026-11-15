#include "ScriptedBrushTool.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
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
    check(manager.SelectTool("Scripted Brush"), "Scripted brush could not be selected.");
    check(!manager.GetSelectedTool().IsContinuous(), "Scripted brush should be one-shot.");
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
        std::cout << "Scripted brush tool tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scripted brush tool test failed: " << error.what() << '\n';
        return 1;
    }
}
