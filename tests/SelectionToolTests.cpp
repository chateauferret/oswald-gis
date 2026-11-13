#include "SelectionTool.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expectInvalid(auto&& operation) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Expected std::invalid_argument.");
}

void testRadiusSelectionAndWeights() {
    SphericalGrid model({1000, 0.0, 81});
    const auto snapshot = model.GetSnapshot();
    const auto& vertices = snapshot->GetVertices();
    const glm::dvec3 center(vertices[0].position);
    const glm::dvec3 neighbor(vertices[1].position);
    const double neighborDistance = std::atan2(
        glm::length(glm::cross(center, neighbor)), glm::dot(center, neighbor)) * 6371.0;

    SelectionSettings settings;
    settings.radius = neighborDistance * 1.5;
    settings.falloffPercent = 50.0;
    std::vector<float> weights(vertices.size(), 0.0F);
    const RadiusSelectionTool tool;
    const auto changed = tool.Apply(*snapshot, center, weights, settings);
    check(weights[0] == 1.0F, "The center was not fully selected.");
    check(std::abs(weights[1] - 2.0F / 3.0F) < 1e-3F,
          "Radius selection did not feather linearly at the outer edge.");
    check(std::find(changed.begin(), changed.end(), 0) != changed.end(),
          "Radius selection did not report changed vertices.");

    weights[1] = 0.9F;
    tool.Apply(*snapshot, center, weights, settings);
    check(weights[1] == 0.9F, "A new radius dab reduced an existing stroke weight.");
    expectInvalid([&] { tool.Apply(*snapshot, center, weights, {settings.distanceUnit, 0, 25}); });
    expectInvalid([&] {
        std::vector<float> wrongSize(1);
        tool.Apply(*snapshot, center, wrongSize, settings);
    });
}

void testSphericalPolygonSelection() {
    SphericalGrid model({1000, 0.0, 82});
    const auto snapshot = model.GetSnapshot();
    std::vector<glm::dvec3> cap;
    constexpr double colatitude = 0.65;
    for (int i = 0; i < 8; ++i) {
        const double longitude = 2.0 * std::numbers::pi * i / 8.0;
        cap.emplace_back(std::sin(colatitude) * std::cos(longitude),
                         std::sin(colatitude) * std::sin(longitude),
                         std::cos(colatitude));
    }
    const auto weights = SelectSphericalPolygon(*snapshot, cap);
    check(weights.size() == snapshot->GetVertices().size(),
          "Polygon selection returned an incorrect weight count.");
        check(weights[snapshot->FindNearest({0, 0, 1})] == 1.0F,
            "Polygon around the north pole missed the nearest north-pole site.");
        check(weights[snapshot->FindNearest({0, 0, -1})] == 0.0F,
            "Polygon around the north pole selected the nearest south-pole site.");
    check(std::count(weights.begin(), weights.end(), 1.0F) > 0 &&
          std::count(weights.begin(), weights.end(), 0.0F) > 0,
          "Polygon selection did not divide the sphere.");
    expectInvalid([&] {
        const std::vector<glm::dvec3> tooFew{{0, 0, 1}, {0, 1, 0}};
        SelectSphericalPolygon(*snapshot, tooFew);
    });
    expectInvalid([&] {
        const std::vector<glm::dvec3> invalid{{0, 0, 1}, {0, 1, 0}, {0, 0, 0}};
        SelectSphericalPolygon(*snapshot, invalid);
    });
}

void testGridChannelAndBrushMasking() {
    SphericalGrid model({1000, 0.0, 83});
    auto snapshot = model.GetSnapshot();
    std::vector<float> weights(snapshot->GetVertices().size(), 0.0F);
    weights[0] = 0.5F;
    model.SetSelectionWeights(weights);
    snapshot = model.GetSnapshot();
    check(snapshot->HasSelection() && snapshot->GetVertices()[0].selectionWeight == 0.5F,
          "Selection channel was not stored in the grid snapshot.");

    std::vector<float> elevations(snapshot->GetVertices().size(), 0.0F);
    BrushSettings settings;
    settings.radius = 20020.0;
    settings.falloffPercent = 0.0;
    settings.value = 100.0F;
    ElevationBrushTool brush;
    brush.Apply(*snapshot, {0, 0, 1}, elevations, settings, 0.1);
    check(elevations[0] == 50.0F, "Brush effect was not multiplied by selection weight.");
    check(elevations[1] == 0.0F, "Brush changed an unselected vertex.");

    model.SetElevations(elevations);
    snapshot = model.GetSnapshot();
    check(snapshot->HasSelection() && snapshot->GetVertices()[0].selectionWeight == 0.5F,
          "Elevation updates discarded selection state.");
    expectInvalid([&] {
        std::vector<float> invalid(weights.size(), 1.1F);
        model.SetSelectionWeights(invalid);
    });
    model.SetSelectionWeights(std::vector<float>(weights.size(), 0.0F), false);
    snapshot = model.GetSnapshot();
    check(!snapshot->HasSelection(), "Clearing selection left its active flag set.");
}

void testValueSelection() {
    SphericalGrid model({1000, 0.0, 84});
    std::vector<float> elevations(model.GetSnapshot()->GetVertices().size(), 0.0F);
    const std::vector<float> samples{-120, -100, -90, -80, -60, -40, -20, 0,
                                     20, 40, 60, 80, 90, 100, 120};
    std::copy(samples.begin(), samples.end(), elevations.begin());
    model.SetElevations(elevations);
    const auto snapshot = model.GetSnapshot();
    for (const double feather : {0.0, 25.0, 50.0, 75.0, 100.0}) {
        const auto weights = SelectByValue(*snapshot, -100, 100, feather);
        check(weights.size() == elevations.size(), "Value selection has the wrong size.");
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const double distance = std::abs(samples[i]);
            const double expected = distance > 100.0 ? 0.0
                : distance <= 100.0 - feather ? 1.0 : (100.0 - distance) / feather;
            check(std::abs(weights[i] - expected) < 1e-6,
                  "Value selection does not match the specified plateau and linear taper.");
        }
    }
    const auto asymmetric = SelectByValue(*snapshot, -100, 60, 50);
    check(asymmetric[5] == 1.0F && asymmetric[7] == 1.0F &&
          asymmetric[2] == 0.25F && asymmetric[9] == 0.5F &&
          asymmetric[10] == 0.0F,
          "Value selection did not center its plateau on the range midpoint.");
    for (const double feather : {0.0, 50.0, 100.0}) {
        const auto equalBounds = SelectByValue(*snapshot, 20, 20, feather);
        for (std::size_t i = 0; i < elevations.size(); ++i) {
            check(equalBounds[i] == (elevations[i] == 20.0F ? 1.0F : 0.0F),
                  "Equal bounds should select only exact matches.");
        }
    }
    expectInvalid([&] { SelectByValue(*snapshot, 100, -100, 50); });
    expectInvalid([&] { SelectByValue(*snapshot, -32768, 0, 50); });
    expectInvalid([&] { SelectByValue(*snapshot, 0, 32768, 50); });
    expectInvalid([&] { SelectByValue(*snapshot, -100, 100, -1); });
    expectInvalid([&] { SelectByValue(*snapshot, -100, 100, 101); });
    expectInvalid([&] {
        SelectByValue(*snapshot, std::numeric_limits<double>::quiet_NaN(), 100, 50);
    });
    expectInvalid([&] {
        SelectByValue(*snapshot, -100, 100, std::numeric_limits<double>::infinity());
    });
    check(model.GetSnapshot() == snapshot && !snapshot->HasSelection(),
          "Computing selection weights modified the model.");
}

} // namespace

int main() {
    try {
        testRadiusSelectionAndWeights();
        testSphericalPolygonSelection();
        testGridChannelAndBrushMasking();
        testValueSelection();
        std::cout << "Selection tool tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Selection tool test failure: " << error.what() << '\n';
        return 1;
    }
}
