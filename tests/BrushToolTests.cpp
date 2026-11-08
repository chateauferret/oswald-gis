#include "BrushTool.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string_view>
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

void testOperationsAndClamping() {
    SphericalFibonacciGrid model({1000, 0.0, 17});
    const auto& grid = *model.GetSnapshot();
    ElevationBrushTool brush;
    auto values = elevationsOf(grid);
    ElevationBrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;
    settings.value = 120.0F;

    settings.operation = BrushOperation::Replace;
    brush.Apply(grid, {0, 0, 1}, values, settings);
    check(values[0] == 120.0F, "Replace did not write the target elevation.");

    settings.operation = BrushOperation::Raise;
    settings.value = 25.0F;
    brush.Apply(grid, {0, 0, 1}, values, settings);
    check(values[0] == 145.0F, "Raise did not add its value.");

    settings.operation = BrushOperation::Lower;
    settings.value = 200.0F;
    brush.Apply(grid, {0, 0, 1}, values, settings);
    check(values[0] == -55.0F, "Lower did not subtract its value.");

    settings.operation = BrushOperation::Minimum;
    settings.value = -100.0F;
    brush.Apply(grid, {0, 0, 1}, values, settings);
    check(values[0] == -100.0F, "Minimum did not select the lower elevation.");

    settings.operation = BrushOperation::Maximum;
    settings.value = 32767.0F;
    brush.Apply(grid, {0, 0, 1}, values, settings);
    check(values[0] == 32767.0F, "Maximum did not select the higher elevation.");

    settings.operation = BrushOperation::Raise;
    settings.value = 32767.0F;
    brush.Apply(grid, {0, 0, 1}, values, settings);
    check(values[0] == 32767.0F, "Brush exceeded the maximum elevation.");
}

void testIntensityAndGeodesicFalloff() {
    SphericalFibonacciGrid model({1000, 0.0, 19});
    const auto& grid = *model.GetSnapshot();
    const auto& vertices = grid.GetVertices();
    const glm::dvec3 center(vertices[0].position);
    const glm::dvec3 neighbor(vertices[1].position);
    const double neighborAngle = std::atan2(glm::length(glm::cross(center, neighbor)),
                                             glm::dot(center, neighbor));
    const double neighborDistance = neighborAngle * 6371.0;
    ElevationBrushTool brush;
    auto values = elevationsOf(grid);
    ElevationBrushSettings settings;
    settings.operation = BrushOperation::Replace;
    settings.radius = neighborDistance * 0.75;
    settings.falloffPercent = 100.0;
    settings.intensityPercent = 50.0;
    settings.value = 100.0F;
    const auto changed = brush.Apply(grid, center, values, settings);
    check(values[0] == 50.0F, "Intensity percentage was not applied at the brush center.");
    const float expectedNeighbor = 100.0F * 0.5F * (2.0F / 3.0F);
    check(std::abs(values[1] - expectedNeighbor) < 1e-3F,
          "Geodesic outer falloff was not linearly tapered.");
    check(std::find(changed.begin(), changed.end(), 0) != changed.end() &&
          std::find(changed.begin(), changed.end(), 1) != changed.end(),
          "Brush did not report changed grid vertices.");

    values = elevationsOf(grid);
    settings.intensityPercent = 0.0;
    check(brush.Apply(grid, center, values, settings).empty(),
          "Zero-intensity brush unexpectedly changed elevations.");
}

void testValidation() {
    SphericalFibonacciGrid model({1000, 0.0, 23});
    const auto& grid = *model.GetSnapshot();
    ElevationBrushTool brush;
    auto values = elevationsOf(grid);
    ElevationBrushSettings settings;
    auto invalid = [&](const char* message) {
        try {
            brush.Apply(grid, {0, 0, 1}, values, settings);
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error(message);
    };
    settings.radius = 0.0;
    invalid("Zero brush radius was accepted.");
    settings.radius = 1.0;
    settings.falloffPercent = 101.0;
    invalid("Out-of-range falloff was accepted.");
    settings.falloffPercent = 0.0;
    settings.intensityPercent = -1.0;
    invalid("Negative intensity was accepted.");
    settings.intensityPercent = 100.0;
    settings.value = 32768.0F;
    invalid("Out-of-range elevation value was accepted.");
    settings.value = 0.0F;
    values.pop_back();
    invalid("A mismatched elevation buffer was accepted.");
}

class TestBrush final : public BrushTool {
public:
    std::string_view GetName() const override { return "Test Brush"; }
    std::vector<std::size_t> Apply(const SphericalGridSnapshot&, glm::dvec3,
                                   std::vector<float>&, const ElevationBrushSettings&) const override {
        return {};
    }
};

void testToolManager() {
    BrushToolManager manager;
    check(manager.GetSelectedTool().GetName() == "Elevation Brush",
          "Manager did not select its default elevation brush.");
    manager.RegisterTool(std::make_unique<TestBrush>());
    check(manager.SelectTool("Test Brush") && manager.GetSelectedTool().GetName() == "Test Brush",
          "Manager could not select a registered tool.");
    check(!manager.SelectTool("Missing Brush"), "Manager selected an unregistered tool.");
    bool duplicateRejected = false;
    try {
        manager.RegisterTool(std::make_unique<TestBrush>());
    } catch (const std::invalid_argument&) {
        duplicateRejected = true;
    }
    check(duplicateRejected, "Manager accepted duplicate tool names.");
}

} // namespace

int main() {
    try {
        testOperationsAndClamping();
        testIntensityAndGeodesicFalloff();
        testValidation();
        testToolManager();
        std::cout << "Brush tool tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Brush tool test failed: " << error.what() << '\n';
        return 1;
    }
}
