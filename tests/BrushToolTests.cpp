#include "BrushTool.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
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
    SphericalGrid model({1000, 0.0, 17});
    const auto& grid = *model.GetSnapshot();
    ElevationBrushTool brush;
    auto values = elevationsOf(grid);
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;
    settings.value = 120.0F;

    settings.operation = BrushOperation::Replace;
    brush.Apply(grid, {0, 0, 1}, values, settings, 1.0);
    check(values[0] == 120.0F, "Replace did not write the target elevation.");

    settings.operation = BrushOperation::Minimum;
    settings.value = -100.0F;
    brush.Apply(grid, {0, 0, 1}, values, settings, 1.0);
    check(values[0] == -100.0F, "Minimum did not select the lower elevation.");

    settings.operation = BrushOperation::Maximum;
    settings.value = 32767.0F;
    brush.Apply(grid, {0, 0, 1}, values, settings, 1.0);
    check(values[0] == 32767.0F, "Maximum did not select the higher elevation.");
}

void testIntensityAndGeodesicFalloff() {
    SphericalGrid model({1000, 0.0, 19});
    const auto& grid = *model.GetSnapshot();
    const auto& vertices = grid.GetVertices();
    const glm::dvec3 center(vertices[0].position);
    const glm::dvec3 neighbor(vertices[1].position);
    const double neighborAngle = std::atan2(glm::length(glm::cross(center, neighbor)),
                                             glm::dot(center, neighbor));
    const double neighborDistance = neighborAngle * 6371.0;
    ElevationBrushTool brush;
    auto values = elevationsOf(grid);
    BrushSettings settings;
    settings.operation = BrushOperation::Replace;
    settings.radius = neighborDistance * 1.5;
    settings.falloffPercent = 50.0;
    settings.intensityPercent = 50.0;
    settings.value = 100.0F;
    const auto changed = brush.Apply(grid, center, values, settings, 0.25);
    check(values[0] == 100.0F, "Intensity changed the immediate Replace operation.");
    const float expectedNeighbor = 100.0F * (2.0F / 3.0F);
    check(std::abs(values[1] - expectedNeighbor) < 1e-3F,
          "Geodesic outer falloff was not linearly tapered.");
    check(std::find(changed.begin(), changed.end(), 0) != changed.end() &&
          std::find(changed.begin(), changed.end(), 1) != changed.end(),
          "Brush did not report changed grid vertices.");

    values = elevationsOf(grid);
    settings.operation = BrushOperation::Maximum;
    settings.value = 100.0F;
    settings.intensityPercent = 0.0;
    brush.Apply(grid, center, values, settings, 0.0);
    check(values[0] == 100.0F, "Maximum did not apply its full result in one application.");
    check(std::abs(values[1] - expectedNeighbor) < 1e-3F,
          "One-shot Maximum did not respect linear falloff.");
    check(!brush.IsContinuous(), "Elevation brush should apply once per stroke.");
    check(RaiseLowerBrushTool().IsContinuous(), "Raise/Lower brush should be continuous.");
}

void testRaiseLowerBrush() {
    SphericalGrid model({1000, 0.0, 20});
    const auto& grid = *model.GetSnapshot();
    const auto center = glm::normalize(glm::dvec3(grid.GetVertices()[0].position));
    const auto neighbor = glm::normalize(glm::dvec3(grid.GetVertices()[1].position));
    const double distance = std::atan2(glm::length(glm::cross(center, neighbor)),
                                      glm::dot(center, neighbor)) * 6371.0;
    RaiseLowerBrushTool brush;
    BrushSettings settings;
    settings.radius = distance * 1.5;
    settings.falloffPercent = 50.0;
    settings.intensityPercent = 50.0;
    settings.value = std::numeric_limits<float>::quiet_NaN();
    auto values = elevationsOf(grid);
    check(settings.raiseLowerRate == 8192.0, "Default Raise/Lower speed is incorrect.");
    brush.Apply(grid, center, values, settings, 0.25);
    check(std::abs(values[0] - 1024.0F) < 1e-3F,
          "Raise speed depends on a value rather than intensity and time.");
    check(std::abs(values[1] - 1024.0F * (2.0F / 3.0F)) < 1e-2F,
          "Raise/Lower brush did not respect linear geodesic falloff.");
    settings.raiseLowerRate = 100.0;
    values = elevationsOf(grid);
    brush.Apply(grid, center, values, settings, 0.25);
    check(std::abs(values[0] - 12.5F) < 1e-4F, "Configured Raise/Lower speed was ignored.");
    settings.intensityPercent = 0.0;
    check(brush.Apply(grid, center, values, settings, 1.0).empty(),
          "Zero-intensity Raise/Lower brush changed elevations.");
    settings.intensityPercent = 100.0;
    check(brush.Apply(grid, center, values, settings, 0.0).empty(),
          "Zero elapsed time changed elevations.");

    settings.raiseLowerRate = 8192.0;
    settings.intensityPercent = 25.0;
    settings.falloffPercent = 0.0;
    for (const auto operation : {RaiseLowerOperation::Raise, RaiseLowerOperation::Lower}) {
        settings.raiseLowerOperation = operation;
        const float direction = operation == RaiseLowerOperation::Raise ? 1.0F : -1.0F;
        for (const int frames : {1, 30, 120}) {
            values = elevationsOf(grid);
            for (int frame = 0; frame < frames; ++frame) {
                brush.Apply(grid, center, values, settings, 1.0 / frames);
            }
            check(std::abs(values[0] - direction * 2048.0F) < 0.01F,
                  "Raise/Lower speed depends on frame rate.");
        }
        settings.intensityPercent = 100.0;
        values = elevationsOf(grid);
        brush.Apply(grid, center, values, settings, 4.0);
        check(values[0] == direction * 32767.0F,
              "Raise/Lower brush did not clamp at the elevation limit after four seconds.");
        check(brush.Apply(grid, center, values, settings, 1.0).empty(),
              "Raise/Lower brush changed already-clamped elevations.");
        settings.intensityPercent = 25.0;
    }
}

void testOuterRadiusProfiles() {
    SphericalGrid model({1000, 0.0, 21});
    const auto& grid = *model.GetSnapshot();
    const auto& vertices = grid.GetVertices();
    const glm::dvec3 center = glm::normalize(glm::dvec3(vertices[0].position));
    ElevationBrushTool brush;
    for (const auto unit : {BrushDistanceUnit::Kilometres, BrushDistanceUnit::Miles}) {
        const double planetRadius = unit == BrushDistanceUnit::Kilometres
                                        ? 6371.0 : 3958.7613;
        for (const double percent : {0.0, 25.0, 50.0, 100.0}) {
            BrushSettings settings;
            settings.distanceUnit = unit;
            settings.radius = planetRadius * 0.5;
            settings.falloffPercent = percent;
            settings.intensityPercent = 50.0;
            settings.value = 100.0F;
            auto values = elevationsOf(grid);
            const auto changed = brush.Apply(grid, center, values, settings, 1.0);
            std::vector<std::size_t> expectedChanged;
            bool testedCore = false;
            bool testedTaper = false;
            bool testedOutside = false;
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                const auto point = glm::normalize(glm::dvec3(vertices[i].position));
                const double angle = std::atan2(glm::length(glm::cross(center, point)),
                                                glm::dot(center, point));
                const double fraction = angle * planetRadius / settings.radius;
                double weight = 0.0;
                if (fraction > 1.0) {
                    testedOutside = true;
                } else if (fraction <= 1.0 - percent / 100.0) {
                    weight = 1.0;
                    testedCore = true;
                } else {
                    weight = (1.0 - fraction) / (percent / 100.0);
                    testedTaper = true;
                }
                const float expected = static_cast<float>(100.0 * weight);
                check(std::isfinite(values[i]) && std::abs(values[i] - expected) < 1e-3F,
                      "Brush did not match the fixed-outer-radius profile.");
                if (expected > 0.0F) {
                    expectedChanged.push_back(i);
                }
            }
            check(changed == expectedChanged, "Brush changed vertices outside its profile.");
            check(testedCore && testedOutside, "Profile test missed the core or outer boundary.");
            check(testedTaper == (percent > 0.0),
                  "Only the cylinder profile should have no tapered region.");
        }
    }
}

void testValidation() {
    SphericalGrid model({1000, 0.0, 23});
    const auto& grid = *model.GetSnapshot();
    ElevationBrushTool brush;
    auto values = elevationsOf(grid);
    BrushSettings settings;
    auto invalid = [&](const char* message) {
        try {
            brush.Apply(grid, {0, 0, 1}, values, settings, 1.0);
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
    brush.Apply(grid, {0, 0, 1}, values, settings, 1.0);
    settings.intensityPercent = 100.0;
    settings.value = 32768.0F;
    invalid("Out-of-range elevation value was accepted.");
    settings.value = 0.0F;
    values.pop_back();
    invalid("A mismatched elevation buffer was accepted.");

    values = elevationsOf(grid);
    RaiseLowerBrushTool raiseLower;
    settings.intensityPercent = -1.0;
    bool intensityRejected = false;
    try {
        raiseLower.Apply(grid, {0, 0, 1}, values, settings, 1.0);
    } catch (const std::invalid_argument&) {
        intensityRejected = true;
    }
    check(intensityRejected, "Negative Raise/Lower intensity was accepted.");
    settings.intensityPercent = 100.0;
    for (const double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()}) {
        settings.raiseLowerRate = rate;
        bool rejected = false;
        try {
            raiseLower.Apply(grid, {0, 0, 1}, values, settings, 1.0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "Invalid Raise/Lower speed was accepted.");
    }
}

class TestBrush final : public BrushTool {
public:
    std::string_view GetName() const override { return "Test Brush"; }
    bool IsContinuous() const override { return false; }
    std::vector<std::size_t> Apply(const SphericalGridSnapshot&, glm::dvec3,
                                   std::vector<float>&, const BrushSettings&,
                                   double) const override {
        return {};
    }
};

void testToolManager() {
    BrushToolManager manager;
    check(manager.GetSelectedTool().GetName() == "Elevation Brush",
          "Manager did not select its default elevation brush.");
    check(manager.GetTools().size() == 2, "Manager did not register both built-in brushes.");
    manager.GetSettings().value = 1234.0F;
    manager.GetSettings().operation = BrushOperation::Maximum;
    check(manager.SelectTool("Raise/Lower Brush") &&
          dynamic_cast<RaiseLowerBrushTool*>(&manager.GetSelectedTool()) != nullptr,
          "Manager did not select the Raise/Lower brush.");
    manager.GetSettings().raiseLowerOperation = RaiseLowerOperation::Lower;
    check(manager.SelectTool("Elevation Brush") &&
          manager.GetSettings().value == 1234.0F &&
          manager.GetSettings().operation == BrushOperation::Maximum &&
          manager.GetSettings().raiseLowerOperation == RaiseLowerOperation::Lower,
          "Switching brushes did not preserve their settings.");
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
        testRaiseLowerBrush();
        testOuterRadiusProfiles();
        testValidation();
        testToolManager();
        std::cout << "Brush tool tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Brush tool test failed: " << error.what() << '\n';
        return 1;
    }
}
