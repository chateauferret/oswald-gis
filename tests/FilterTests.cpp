#include "Filter.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expectInvalid(auto&& operation, const char* message) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message);
}

void testFillOperationsAndSelectionWeights() {
    SphericalGrid model({1000, 0.0, 91});
    const auto size = model.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(size, 10.0F);
    model.SetElevations(elevations);
    std::vector<float> weights(size, 0.0F);
    weights[1] = 0.5F;
    weights[2] = 1.0F;
    model.SetSelectionWeights(weights);
    const auto snapshot = model.GetSnapshot();
    FillFilter filter;

    const std::vector<std::pair<FillOperation, float>> cases{
        {FillOperation::Replace, 100.0F},
        {FillOperation::Raise, 20.0F},
        {FillOperation::Lower, 20.0F},
        {FillOperation::Minimum, 0.0F},
        {FillOperation::Maximum, 20.0F}
    };
    const std::vector<std::vector<float>> expected{
        {10.0F, 55.0F, 100.0F},
        {10.0F, 20.0F, 30.0F},
        {10.0F, 0.0F, -10.0F},
        {10.0F, 5.0F, 0.0F},
        {10.0F, 15.0F, 20.0F}
    };
    for (std::size_t operation = 0; operation < cases.size(); ++operation) {
        filter.SetSettings({cases[operation].first, cases[operation].second});
        const auto result = filter.Apply(*snapshot, elevations);
        for (std::size_t i = 0; i < expected[operation].size(); ++i) {
            check(std::abs(result[i] - expected[operation][i]) < 1e-5F,
                  "Fill filter operation did not respect its selection weight.");
        }
        check(result[3] == elevations[3],
              "Fill filter changed a vertex with zero selection weight.");
        check(elevations[2] == 10.0F,
              "Fill filter modified its input elevation buffer.");
    }
}

void testGlobalApplicationAndClamping() {
    SphericalGrid model({1000, 0.0, 92});
    const auto size = model.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(size, 10.0F);
    model.SetElevations(elevations);
    auto snapshot = model.GetSnapshot();
    FillFilter filter;
    filter.SetSettings({FillOperation::Lower, 2.0F});
    auto result = filter.Apply(*snapshot, elevations);
    check(result.front() == 8.0F && result.back() == 8.0F,
          "Fill filter without a selection did not affect every vertex globally.");

    elevations[0] = 32760.0F;
    filter.SetSettings({FillOperation::Raise, 100.0F});
    result = filter.Apply(*snapshot, elevations);
    check(result[0] == 32767.0F, "Fill filter did not clamp the upper elevation bound.");
    elevations[0] = -32760.0F;
    filter.SetSettings({FillOperation::Lower, 100.0F});
    result = filter.Apply(*snapshot, elevations);
    check(result[0] == -32767.0F, "Fill filter did not clamp the lower elevation bound.");
}

void testValidationAndPluginRegistration() {
    SphericalGrid model({1000, 0.0, 93});
    const auto snapshot = model.GetSnapshot();
    std::vector<float> elevations(snapshot->GetVertices().size(), 0.0F);
    FillFilter filter;
    expectInvalid([&] { filter.SetSettings({FillOperation::Replace, 32768.0F}); },
                  "Fill filter accepted a value above the supported range.");
    expectInvalid([&] { filter.SetSettings({FillOperation::Replace,
                                             std::numeric_limits<float>::quiet_NaN()}); },
                  "Fill filter accepted a non-finite value.");
    expectInvalid([&] {
        filter.SetSettings({static_cast<FillOperation>(99), 0.0F});
    }, "Fill filter accepted an invalid operation.");
    expectInvalid([&] { filter.Apply(*snapshot, std::span<const float>(elevations).first(1)); },
                  "Fill filter accepted a mismatched elevation buffer.");

    FilterManager manager;
        check(manager.GetFilters().size() == 9 &&
          manager.GetFilter("Fill").GetName() == "Fill" &&
          manager.GetFilter("Fill basins").GetName() == "Fill basins" &&
          manager.GetFilter("Incise flow").GetName() == "Incise flow" &&
            manager.GetFilter("Precipiton diffusion").GetName() == "Precipiton diffusion" &&
              manager.GetFilter("Thermal weathering").GetName() == "Thermal weathering" &&
              manager.GetFilter("Glacial erosion").GetName() == "Glacial erosion" &&
              manager.GetFilter("Cratonic directional grooving").GetName() ==
                  "Cratonic directional grooving" &&
              manager.GetFilter("Cratonic scour and planation").GetName() ==
                  "Cratonic scour and planation" &&
          manager.GetFilter("Absolute Magnitude Noise").GetName() == "Absolute Magnitude Noise",
          "Filter manager did not register or return the built-in plugins.");
    expectInvalid([&] { manager.RegisterFilter(std::make_unique<FillFilter>()); },
                  "Filter manager accepted a duplicate filter name.");
    expectInvalid([&] { manager.RegisterFilter(nullptr); },
                  "Filter manager accepted an empty plugin.");
}

std::vector<std::size_t> neighbors(const SphericalGridSnapshot& grid, std::size_t index) {
    std::vector<std::size_t> result;
    const auto& edges = grid.GetHalfEdges();
    for (const auto& edge : edges) {
        if (static_cast<std::size_t>(edge.originVertex) == index) {
            result.push_back(static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edge.nextEdge)].originVertex));
        }
    }
    return result;
}

void checkDrainage(const SphericalGridSnapshot& grid, const std::vector<float>& original,
                   const std::vector<float>& filled, float sinkHeight = 0.0F) {
    for (std::size_t i = 0; i < original.size(); ++i) {
        check(filled[i] >= original[i], "Fill basins lowered terrain.");
        if (original[i] <= sinkHeight) {
            check(filled[i] == original[i], "Fill basins changed a designated sink.");
        } else {
            const auto adjacent = neighbors(grid, i);
            check(std::any_of(adjacent.begin(), adjacent.end(), [&](std::size_t neighbor) {
                return filled[neighbor] < filled[i];
            }), "Fill basins left a non-sink vertex without a downhill drainage path.");
        }
    }
}

void testSphericalFillBasins() {
    SphericalGrid model({1000, 0.0, 94});
    const auto snapshot = model.GetSnapshot();
    const auto size = snapshot->GetVertices().size();
    FillBasinsFilter filter;
    std::vector<float> elevations(size, -20.0F);
    const auto ring = neighbors(*snapshot, 0);
    for (const auto index : ring) {
        elevations[index] = 100.0F;
    }
    elevations[0] = 1.0F;
    elevations[ring.front()] = 50.0F;
    auto filled = filter.Apply(*snapshot, elevations);
    check(filled[0] == 50.0F + 1.0e-4F,
          "Fill basins did not use the lowest spill point and specified epsilon.");
    check(elevations[0] == 1.0F, "Fill basins modified its input buffer.");
    checkDrainage(*snapshot, elevations, filled);
    check(filter.Apply(*snapshot, filled) == filled, "Fill basins was not idempotent.");

    // A long plateau needs strictly descending paths even where Float32 ULP exceeds epsilon.
    std::fill(elevations.begin(), elevations.end(), 5000.0F);
    elevations[0] = -100.0F;
    elevations[ring.front()] = 0.0F;
    filled = filter.Apply(*snapshot, elevations);
    checkDrainage(*snapshot, elevations, filled);
    check(std::any_of(filled.begin(), filled.end(), [](float height) { return height > 5000.0F; }),
          "Fill basins did not drain a high-elevation plateau.");

    // The global minimum substitutes for sea-level sinks on an entirely dry globe.
    std::fill(elevations.begin(), elevations.end(), 100.0F);
    elevations[0] = 1.0F;
    filled = filter.Apply(*snapshot, elevations);
    checkDrainage(*snapshot, elevations, filled, 1.0F);
    std::fill(elevations.begin(), elevations.end(), 100.0F);
    check(filter.Apply(*snapshot, elevations) == elevations,
          "Fill basins changed tied global-minimum sinks on a uniform dry globe.");
    std::fill(elevations.begin(), elevations.end(), -5.0F);
    check(filter.Apply(*snapshot, elevations) == elevations,
          "Fill basins filled the sea floor.");
}

void testFillBasinsSelectionAndValidation() {
    SphericalGrid model({1000, 0.0, 95});
    const auto size = model.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(size, 100.0F);
    elevations[0] = 1.0F;
    model.SetElevations(elevations);
    std::vector<float> weights(size, 0.0F);
    FillBasinsFilter filter;
    for (const float weight : {1.0F, 0.5F, 0.0F}) {
        weights[0] = weight;
        model.SetSelectionWeights(weights);
        const auto snapshot = model.GetSnapshot();
        const auto result = filter.Apply(*snapshot, elevations);
        const float expected = 1.0F + ((100.0F + 1.0e-4F) - 1.0F) * weight;
        check(result[0] == expected, "Fill basins did not blend at a fixed selection boundary.");
        check(std::equal(result.begin() + 1, result.end(), elevations.begin() + 1),
              "Fill basins modified unselected terrain.");
    }
    weights[0] = 1.0F;
    model.SetSelectionWeights(weights);
    const auto snapshot = model.GetSnapshot();
    elevations[0] = -1.0F;
    check(filter.Apply(*snapshot, elevations) == elevations,
          "Fill basins filled a selected endorheic sea-floor sink.");
    elevations[0] = 0.0F;
    check(filter.Apply(*snapshot, elevations) == elevations,
          "Fill basins changed a selected sea-level sink.");
    expectInvalid([&] { filter.Apply(*snapshot, std::span<const float>(elevations).first(1)); },
                  "Fill basins accepted a mismatched input buffer.");
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(), 32768.0F, -32768.0F}) {
        elevations[0] = invalid;
        expectInvalid([&] { filter.Apply(*snapshot, elevations); },
                      "Fill basins accepted invalid elevations.");
    }
    std::fill(elevations.begin(), elevations.end(), 32767.0F);
    elevations[0] = 1.0F;
    bool overflow = false;
    try {
        filter.Apply(*snapshot, elevations);
    } catch (const std::overflow_error&) {
        overflow = true;
    }
    check(overflow && elevations[0] == 1.0F,
          "Fill basins did not explicitly reject unrepresentable drainage without changing input.");
}

void checkIncisionFormula(const SphericalGridSnapshot& grid, const std::vector<float>& elevations,
                          const InciseFlowSettings& settings) {
    const auto size = elevations.size();
    const auto& vertices = grid.GetVertices();
    std::vector<bool> sinks(size, false);
    bool hasOutlet = false;
    for (std::size_t i = 0; i < size; ++i) {
        sinks[i] = elevations[i] <= 0.0F ||
            (grid.HasSelection() && vertices[i].selectionWeight == 0.0F);
        hasOutlet |= sinks[i];
    }
    if (!hasOutlet) {
        const auto minimum = *std::min_element(elevations.begin(), elevations.end());
        for (std::size_t i = 0; i < size; ++i) {
            sinks[i] = elevations[i] == minimum;
        }
    }
    std::vector<double> slopes(size, 0.0);
    std::vector<std::size_t> receivers(size, size);
    for (std::size_t i = 0; i < size; ++i) {
        if (sinks[i]) continue;
        for (const auto neighbor : neighbors(grid, i)) {
            const auto from = glm::normalize(glm::dvec3(vertices[i].position));
            const auto to = glm::normalize(glm::dvec3(vertices[neighbor].position));
            const auto distance = grid.GetOptions().planetRadius *
                std::acos(std::clamp(glm::dot(from, to), -1.0, 1.0));
            const auto slope = (static_cast<double>(elevations[i]) - elevations[neighbor]) / distance;
            if (slope > slopes[i] ||
                (slope > 0.0 && slope == slopes[i] && neighbor < receivers[i])) {
                slopes[i] = slope;
                receivers[i] = neighbor;
            }
        }
    }
    // Trace each rainfall contribution independently, rather than using the filter's sorted accumulation.
    std::vector<double> area(size, 0.0);
    bool hasConfluence = false;
    for (std::size_t source = 0; source < size; ++source) {
        for (auto index = source; index != size; index = receivers[index]) {
            area[index] += settings.precipitation;
        }
    }
    InciseFlowFilter filter;
    filter.SetSettings(settings);
    const auto original = elevations;
    const auto result = filter.Apply(grid, elevations);
    bool incised = false;
    for (std::size_t i = 0; i < size; ++i) {
        double expected = elevations[i];
        if (receivers[i] != size) {
            hasConfluence |= area[i] > settings.precipitation * 1.5;
            const double delta = settings.incisionRate *
                std::pow(area[i] / settings.referenceArea, settings.flowExponent) *
                std::pow(slopes[i], settings.slopeExponent);
            const double target = std::clamp(elevations[i] - delta, -32767.0, 32767.0);
            const double weight = grid.HasSelection() ? vertices[i].selectionWeight : 1.0;
            expected += (target - elevations[i]) * weight;
        }
        check(std::abs(result[i] - static_cast<float>(expected)) < 0.002F,
              "Incise flow disagreed with the stream-power formula and traced catchment runoff.");
        check(result[i] <= elevations[i] && result[i] >= -32767.0F,
              "Incise flow raised terrain or exceeded the elevation limits.");
        if (sinks[i] || receivers[i] == size) {
            check(result[i] == elevations[i], "Incise flow changed a sink or unresolved depression.");
        }
        incised |= result[i] < elevations[i];
    }
    check(incised && hasConfluence, "Incise flow fixture did not exercise incision and accumulated runoff.");
    check(original == elevations, "Incise flow modified its input buffer.");
}

void testInciseFlowFormulaAndSelection() {
    SphericalGrid model({1000, 0.0, 96});
    const auto size = model.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(size);
    for (std::size_t i = 0; i < size; ++i) {
        elevations[i] = i % 17 == 0 ? -200.0F : i % 19 == 0 ? 0.0F :
            200.0F + static_cast<float>((i * 193) % 4000);
    }
    model.SetPlanetRadius(1200.0);
    InciseFlowSettings settings{2.3, 3.7, 0.65, 1.4, 0.8};
    checkIncisionFormula(*model.GetSnapshot(), elevations, settings);
    model.SetElevations(elevations);
    std::vector<float> weights(size, 0.0F);
    for (std::size_t i = 0; i < size; ++i) {
        weights[i] = i % 5 == 0 ? 0.0F : i % 3 == 0 ? 0.25F : 1.0F;
    }
    model.SetSelectionWeights(weights);
    checkIncisionFormula(*model.GetSnapshot(), elevations, settings);
    settings.flowExponent = 0.0;
    settings.slopeExponent = 0.0;
    checkIncisionFormula(*model.GetSnapshot(), elevations, settings);
    settings.incisionRate = 1.0e8;
    checkIncisionFormula(*model.GetSnapshot(), elevations, settings);

    model.SetSelectionWeights(weights, false);
    for (auto& elevation : elevations) {
        elevation = std::max(elevation, 1.0F);
    }
    checkIncisionFormula(*model.GetSnapshot(), elevations, InciseFlowSettings{});
}

void testInciseFlowSinksAndParameterScaling() {
    SphericalGrid model({1000, 0.0, 97});
    const auto size = model.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(size, -20.0F);
    elevations[0] = 1000.0F;
    InciseFlowFilter filter;
    auto settings = filter.GetSettings();
    settings.incisionRate = 10.0;
    settings.flowExponent = 1.0;
    filter.SetSettings(settings);
    const auto snapshot = model.GetSnapshot();
    const auto baseline = filter.Apply(*snapshot, elevations);
    const float incision = elevations[0] - baseline[0];
    check(incision > 0.0F, "Isolated headwater was not incised.");
    check(std::equal(baseline.begin() + 1, baseline.end(), elevations.begin() + 1),
          "Incise flow changed underwater sinks.");
    const auto checkScale = [&](double InciseFlowSettings::* member, double factor, double expected) {
        auto changed = settings;
        changed.*member *= factor;
        filter.SetSettings(changed);
        const auto result = filter.Apply(*snapshot, elevations);
        check(std::abs((elevations[0] - result[0]) - incision * expected) < 0.0002,
              "Incise flow did not respect parameter scaling or geodesic distance units.");
    };
    checkScale(&InciseFlowSettings::incisionRate, 2.0, 2.0);
    checkScale(&InciseFlowSettings::precipitation, 2.0, 2.0);
    checkScale(&InciseFlowSettings::referenceArea, 2.0, 0.5);
    checkScale(&InciseFlowSettings::incisionRate, 0.0, 0.0);
    checkScale(&InciseFlowSettings::precipitation, 0.0, 0.0);
    filter.SetSettings(settings);
    model.SetPlanetRadius(snapshot->GetOptions().planetRadius * 2.0);
    const auto scaled = filter.Apply(*model.GetSnapshot(), elevations);
    check(std::abs((elevations[0] - scaled[0]) - incision * 0.5) < 0.0002,
          "Incise flow ignored the planet-wide radius.");
    std::fill(elevations.begin(), elevations.end(), 10.0F);
    check(filter.Apply(*snapshot, elevations) == elevations,
          "Incise flow changed a uniform dry globe of global-minimum sinks.");
    elevations[0] = 1.0F;
    check(filter.Apply(*snapshot, elevations)[0] == 1.0F,
          "Incise flow changed a dry globe's minimum sink.");
    elevations[0] = 0.0F;
    check(filter.Apply(*snapshot, elevations)[0] == 0.0F,
          "Incise flow changed a sea-level sink.");
    std::fill(elevations.begin(), elevations.end(), -10.0F);
    check(filter.Apply(*snapshot, elevations) == elevations,
          "Incise flow eroded the sea floor.");
    std::fill(elevations.begin(), elevations.end(), 1000.0F);
    elevations[0] = 1.0F;
    model.SetSelectionWeights(std::vector<float>(size, 0.0F), true);
    check(filter.Apply(*model.GetSnapshot(), elevations) == elevations,
          "Incise flow changed an active empty selection.");
    std::vector<float> weights(size, 0.0F);
    weights[0] = 1.0F;
    model.SetSelectionWeights(weights);
    check(filter.Apply(*model.GetSnapshot(), elevations) == elevations,
          "Incise flow automatically filled an unresolved selected pit.");
}

void testInciseFlowValidation() {
    InciseFlowFilter filter;
    const auto defaults = filter.GetSettings();
    const std::vector<double InciseFlowSettings::*> parameters{
        &InciseFlowSettings::incisionRate, &InciseFlowSettings::referenceArea,
        &InciseFlowSettings::flowExponent, &InciseFlowSettings::slopeExponent,
        &InciseFlowSettings::precipitation
    };
    for (const auto member : parameters) {
        for (const auto invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity()}) {
            auto settings = defaults;
            settings.*member = invalid;
            expectInvalid([&] { filter.SetSettings(settings); },
                          "Incise flow accepted an invalid parameter.");
            check(filter.GetSettings().*member == defaults.*member,
                  "Invalid incision settings replaced accepted settings.");
        }
    }
    for (const auto member : {&InciseFlowSettings::referenceArea}) {
        auto settings = defaults;
        settings.*member = 0.0;
        expectInvalid([&] { filter.SetSettings(settings); },
                      "Incise flow accepted a zero denominator parameter.");
    }
    SphericalGrid model({1000, 0.0, 98});
    model.SetPlanetRadius(1.0);
    const auto snapshot = model.GetSnapshot();
    std::vector<float> elevations(snapshot->GetVertices().size(), 1000.0F);
    elevations[0] = -20.0F;
    expectInvalid([&] { filter.Apply(*snapshot, std::span<const float>(elevations).first(1)); },
                  "Incise flow accepted a mismatched elevation count.");
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(), 32768.0F, -32768.0F}) {
        elevations[0] = invalid;
        expectInvalid([&] { filter.Apply(*snapshot, elevations); },
                      "Incise flow accepted an invalid input elevation.");
    }
    elevations[0] = -20.0F;
    for (const bool runoffOverflow : {false, true}) {
        auto settings = defaults;
        if (runoffOverflow) {
            settings.precipitation = std::numeric_limits<double>::max();
        } else {
            settings.incisionRate = std::numeric_limits<double>::max();
        }
        filter.SetSettings(settings);
        bool overflow = false;
        const auto original = elevations;
        try {
            filter.Apply(*snapshot, elevations);
        } catch (const std::overflow_error&) {
            overflow = true;
        }
        check(overflow && elevations == original,
              "Incise flow failed to report numeric overflow without changing input.");
    }
    }

    void testPrecipitonDiffusion() {
        SphericalGrid model({1000, 0.0, 96});
        const auto size = model.GetSnapshot()->GetVertices().size();
        model.SetPlanetRadius(1200.0);
        std::vector<float> elevations(size);
        for (std::size_t i = 0; i < size; ++i) {
            elevations[i] = i % 23 == 0 ? -100.0F :
                500.0F + static_cast<float>((i * 193) % 4000);
        }
        PrecipitonFilter filter;
        const PrecipitonSettings settings{0.75};
        filter.SetSettings(settings);

        const auto verify = [&](const SphericalGridSnapshot& snapshot) {
            const auto& vertices = snapshot.GetVertices();
            std::vector<double> laplacian(size, 0.0);
            const auto& edges = snapshot.GetHalfEdges();
            for (const auto& edge : edges) {
                const auto source = static_cast<std::size_t>(edge.originVertex);
                if (elevations[source] <= 0.0F ||
                    (snapshot.HasSelection() && vertices[source].selectionWeight == 0.0F)) {
                    continue;
                }
                const auto target = static_cast<std::size_t>(
                    edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
                const auto from = glm::normalize(glm::dvec3(vertices[source].position));
                const auto to = glm::normalize(glm::dvec3(vertices[target].position));
                const double distance = snapshot.GetOptions().planetRadius *
                    std::acos(std::clamp(glm::dot(from, to), -1.0, 1.0));
                laplacian[source] += (static_cast<double>(elevations[target]) - elevations[source]) /
                    distance;
            }
            const auto original = elevations;
            const auto result = filter.Apply(snapshot, elevations);
            bool raised = false;
            bool lowered = false;
            for (std::size_t i = 0; i < size; ++i) {
                double expected = elevations[i];
                if (elevations[i] > 0.0F &&
                    (!snapshot.HasSelection() || vertices[i].selectionWeight > 0.0F)) {
                    const double target = std::clamp(elevations[i] + settings.diffusionCoefficient * laplacian[i],
                        -32767.0, 32767.0);
                    const double weight = snapshot.HasSelection() ? vertices[i].selectionWeight : 1.0;
                    expected += (target - elevations[i]) * weight;
                }
                check(std::abs(result[i] - static_cast<float>(expected)) < 0.001F,
                      "Precipiton diffusion disagreed with the distance-weighted Laplacian.");
                check(result[i] >= -32767.0F && result[i] <= 32767.0F,
                      "Precipiton diffusion exceeded supported elevation limits.");
                if (elevations[i] <= 0.0F ||
                    (snapshot.HasSelection() && vertices[i].selectionWeight == 0.0F)) {
                    check(result[i] == elevations[i], "Precipiton diffusion changed a fixed sink.");
                }
                raised |= result[i] > elevations[i];
                lowered |= result[i] < elevations[i];
            }
            check(raised && lowered, "Precipiton diffusion fixture did not exercise smoothing.");
            check(original == elevations, "Precipiton diffusion modified its input buffer.");
        };

        verify(*model.GetSnapshot());
        std::vector<float> weights(size, 1.0F);
        for (std::size_t i = 0; i < size; ++i) {
            if (i % 5 == 0) weights[i] = 0.0F;
            else if (i % 3 == 0) weights[i] = 0.25F;
        }
        model.SetSelectionWeights(weights);
        verify(*model.GetSnapshot());

        filter.SetSettings({0.0});
        check(filter.Apply(*model.GetSnapshot(), elevations) == elevations,
              "A zero precipiton coefficient changed terrain.");
        for (const double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::infinity()}) {
            expectInvalid([&] { filter.SetSettings({invalid}); },
                          "Precipiton diffusion accepted an invalid coefficient.");
        }
        filter.SetSettings({std::numeric_limits<double>::max()});
        bool overflow = false;
        try {
            filter.Apply(*model.GetSnapshot(), elevations);
        } catch (const std::overflow_error&) {
            overflow = true;
        }
        check(overflow && elevations.size() == size,
              "Precipiton diffusion failed to report numeric overflow.");
}

void testThermalWeathering() {
    SphericalGrid model({1000, 0.0, 100});
    model.SetPlanetRadius(1200.0);
    const auto size = model.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(size, 100.0F);
    elevations[0] = -50.0F; // Fixed ocean sink.
    const auto& edges = model.GetSnapshot()->GetHalfEdges();
    std::vector<std::vector<std::size_t>> adjacency(size);
    for (const auto& edge : edges) {
        const auto first = static_cast<std::size_t>(edge.originVertex);
        const auto second = static_cast<std::size_t>(
            edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
        adjacency[first].push_back(second);
    }
    for (const auto neighbor : adjacency[1]) {
        elevations[1] = 1000.0F;
        elevations[neighbor] = std::max(elevations[neighbor], 100.0F);
    }

    ThermalWeatheringFilter filter;
    const ThermalWeatheringSettings settings{30.0};
    filter.SetSettings(settings);
    const auto verify = [&](const SphericalGridSnapshot& snapshot) {
        const auto& vertices = snapshot.GetVertices();
        const auto sinks = [&] {
            std::vector<bool> fixed(size, false);
            bool hasSink = false;
            for (std::size_t i = 0; i < size; ++i) {
                fixed[i] = elevations[i] <= 0.0F ||
                    (snapshot.HasSelection() && vertices[i].selectionWeight == 0.0F);
                hasSink |= fixed[i];
            }
            if (!hasSink) {
                const auto minimum = *std::min_element(elevations.begin(), elevations.end());
                for (std::size_t i = 0; i < size; ++i) fixed[i] = elevations[i] == minimum;
            }
            return fixed;
        }();
        std::vector<double> displacement(size, 0.0);
        const double reposeSlope = std::tan(settings.angleOfReposeDegrees * std::acos(-1.0) / 180.0);
        for (std::size_t first = 0; first < size; ++first) {
            for (const auto second : adjacency[first]) {
                if (first >= second || sinks[first] || sinks[second] ||
                    elevations[first] == elevations[second]) continue;
                const auto high = elevations[first] > elevations[second] ? first : second;
                const auto low = high == first ? second : first;
                const auto from = glm::normalize(glm::dvec3(vertices[high].position));
                const auto to = glm::normalize(glm::dvec3(vertices[low].position));
                const double distance = snapshot.GetOptions().planetRadius *
                    std::acos(std::clamp(glm::dot(from, to), -1.0, 1.0));
                const double excess = elevations[high] - elevations[low] - distance * reposeSlope;
                if (excess > 0.0) {
                    const double transfer = 0.5 * excess;
                    displacement[high] -= transfer;
                    displacement[low] += transfer;
                }
            }
        }
        const auto original = elevations;
        const auto result = filter.Apply(snapshot, elevations);
        bool lowered = false;
        bool raised = false;
        for (std::size_t i = 0; i < size; ++i) {
            double expected = elevations[i];
            if (!sinks[i]) {
                const double target = std::clamp(elevations[i] + displacement[i],
                    -32767.0, 32767.0);
                const double weight = snapshot.HasSelection() ? vertices[i].selectionWeight : 1.0;
                expected += (target - elevations[i]) * weight;
            }
            check(std::abs(result[i] - static_cast<float>(expected)) < 0.001F,
                  "Thermal weathering disagreed with pairwise repose transfer.");
            check(result[i] >= -32767.0F && result[i] <= 32767.0F,
                  "Thermal weathering exceeded supported elevation limits.");
            if (sinks[i]) {
                check(result[i] == elevations[i], "Thermal weathering changed a fixed sink.");
            }
            lowered |= result[i] < elevations[i];
            raised |= result[i] > elevations[i];
        }
        check(lowered && raised, "Thermal weathering fixture did not transfer material.");
        check(original == elevations, "Thermal weathering modified its input buffer.");
    };

    verify(*model.GetSnapshot());
    std::vector<float> weights(size, 1.0F);
    for (std::size_t i = 0; i < size; ++i) {
        if (i % 5 == 0) weights[i] = 0.0F;
        else if (i % 3 == 0) weights[i] = 0.5F;
    }
    model.SetSelectionWeights(weights);
    verify(*model.GetSnapshot());

    filter.SetSettings({89.0});
    check(filter.Apply(*model.GetSnapshot(), elevations) == elevations,
          "A high repose angle unexpectedly moved terrain.");
    for (const double invalid : {-1.0, 90.0, std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity()}) {
        expectInvalid([&] { filter.SetSettings({invalid}); },
                      "Thermal weathering accepted an invalid repose angle.");
    }
    const auto snapshot = model.GetSnapshot();
    expectInvalid([&] { filter.Apply(*snapshot, std::span<const float>(elevations).first(1)); },
                  "Thermal weathering accepted a mismatched elevation count.");
}

void testGlacialErosion() {
    SphericalGrid model({1000, 0.0, 100});
    model.SetPlanetRadius(900.0);
    const auto size = model.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(size);
    for (std::size_t i = 0; i < size; ++i) {
        elevations[i] = i % 19 == 0 ? -100.0F :
            300.0F + static_cast<float>((i * 193) % 2600);
    }
    GlacialErosionSettings settings;
    settings.equilibriumLineAltitude = 1200.0;
    settings.elaLatitudeGradient = 8.0;
    settings.accumulationRate = 0.015;
    settings.ablationRate = 0.025;
    settings.initialIceThickness = 20.0;
    settings.durationYears = 3.0;
    settings.glacialErodibility = 0.0002;
    settings.iceViscosityFactor = 0.003;
    settings.thicknessExponent = 1.0;
    settings.velocityExponent = 2.0;
    settings.troughHalfWidth = 1500.0;
    GlacialErosionFilter filter;
    filter.SetSettings(settings);

    const auto verify = [&](const SphericalGridSnapshot& snapshot) {
        const auto& vertices = snapshot.GetVertices();
        std::vector<bool> sinks(size, false);
        bool hasSink = false;
        for (std::size_t i = 0; i < size; ++i) {
            sinks[i] = elevations[i] <= 0.0F ||
                (snapshot.HasSelection() && vertices[i].selectionWeight == 0.0F);
            hasSink |= sinks[i];
        }
        if (!hasSink) {
            const auto minimum = *std::min_element(elevations.begin(), elevations.end());
            for (std::size_t i = 0; i < size; ++i) sinks[i] = elevations[i] == minimum;
        }
        struct TestNeighbor {
            std::size_t index;
            double distance;
            glm::dvec3 tangent;
        };
        std::vector<std::vector<TestNeighbor>> adjacency(size);
        const auto& edges = snapshot.GetHalfEdges();
        for (const auto& edge : edges) {
            const auto source = static_cast<std::size_t>(edge.originVertex);
            const auto target = static_cast<std::size_t>(
                edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
            const auto from = glm::normalize(glm::dvec3(vertices[source].position));
            const auto to = glm::normalize(glm::dvec3(vertices[target].position));
            const auto cosine = std::clamp(glm::dot(from, to), -1.0, 1.0);
            const double distance = snapshot.GetOptions().planetRadius * std::acos(cosine);
            const auto tangent = to - cosine * from;
            adjacency[source].push_back({target, distance, glm::normalize(tangent)});
        }
        std::vector<double> thickness(size, settings.initialIceThickness);
        std::vector<double> surface(size);
        for (std::size_t i = 0; i < size; ++i) {
            const auto position = glm::normalize(glm::dvec3(vertices[i].position));
            const double latitude = std::asin(std::clamp(position.z, -1.0, 1.0)) *
                (180.0 / std::acos(-1.0));
            const double ela = settings.equilibriumLineAltitude -
                settings.elaLatitudeGradient * std::abs(latitude);
            const double initialSurface = elevations[i] + thickness[i];
            const double balance = settings.accumulationRate *
                    std::max(0.0, initialSurface - ela) -
                settings.ablationRate * std::max(0.0, ela - initialSurface);
            thickness[i] = std::max(0.0, thickness[i] + settings.durationYears * balance);
            surface[i] = elevations[i] + thickness[i];
        }
        std::vector<double> rates(size, 0.0);
        std::vector<glm::dvec3> directions(size, glm::dvec3(0.0));
        for (std::size_t i = 0; i < size; ++i) {
            if (sinks[i] || thickness[i] <= 0.0) continue;
            double maxSlope = 0.0;
            double fallbackSlope = 0.0;
            glm::dvec3 flow(0.0), fallback(0.0);
            for (const auto& neighbor : adjacency[i]) {
                if (surface[i] <= surface[neighbor.index]) continue;
                const double slope = (surface[i] - surface[neighbor.index]) / neighbor.distance;
                flow += neighbor.tangent * slope;
                if (slope > fallbackSlope) {
                    fallbackSlope = slope;
                    fallback = neighbor.tangent;
                }
                maxSlope = std::max(maxSlope, slope);
            }
            if (maxSlope == 0.0) continue;
            const double flowLength = glm::length(flow);
            directions[i] = flowLength > 1.0e-12 ? flow / flowLength : fallback;
            const double velocity = settings.iceViscosityFactor * thickness[i] *
                maxSlope * maxSlope * maxSlope;
            rates[i] = settings.glacialErodibility * std::pow(thickness[i], settings.thicknessExponent) *
                std::pow(velocity, settings.velocityExponent);
        }
        std::vector<double> erosion(size, 0.0);
        for (std::size_t source = 0; source < size; ++source) {
            const double depth = rates[source] * settings.durationYears;
            if (depth <= 0.0) continue;
            std::vector<std::pair<std::size_t, double>> weights{{source, 1.0}};
            double sum = 1.0;
            for (const auto& neighbor : adjacency[source]) {
                if (sinks[neighbor.index]) continue;
                const double along = glm::dot(directions[source], neighbor.tangent);
                if (std::abs(along) > std::sqrt(0.5)) continue;
                const double perpendicular = neighbor.distance *
                    std::sqrt(std::max(0.0, 1.0 - along * along));
                const double ratio = perpendicular / settings.troughHalfWidth;
                const double weight = std::max(0.0, 1.0 - ratio * ratio);
                if (weight > 0.0) {
                    weights.emplace_back(neighbor.index, weight);
                    sum += weight;
                }
            }
            for (const auto& [target, weight] : weights) {
                erosion[target] += depth * weight / sum;
            }
        }
        const auto original = elevations;
        const auto result = filter.Apply(snapshot, elevations);
        bool changedByErosion = false;
        bool changedByLoad = false;
        for (std::size_t i = 0; i < size; ++i) {
            double expected = elevations[i];
            if (!sinks[i]) {
                const double target = std::clamp(elevations[i] - erosion[i] -
                    (917.0 / 3300.0) * thickness[i], -32767.0, 32767.0);
                const double weight = snapshot.HasSelection() ? vertices[i].selectionWeight : 1.0;
                expected += (target - elevations[i]) * weight;
            }
            check(std::abs(result[i] - static_cast<float>(expected)) < 0.002F,
                  "Glacial erosion disagreed with the mass-balance, Glen-law, trough, and load formulas.");
            check(result[i] >= -32767.0F && result[i] <= 32767.0F,
                  "Glacial erosion exceeded the supported elevation limits.");
            if (sinks[i]) {
                check(result[i] == elevations[i], "Glacial erosion changed a fixed sink.");
            }
            changedByLoad |= !sinks[i] && thickness[i] > 0.0;
            changedByErosion |= !sinks[i] && erosion[i] > 0.0;
        }
        check(changedByLoad && changedByErosion,
              "Glacial fixture did not exercise ice loading and bedrock erosion.");
        check(original == elevations, "Glacial erosion modified its input buffer.");
    };

    verify(*model.GetSnapshot());
    std::vector<float> selection(size, 1.0F);
    for (std::size_t i = 0; i < size; ++i) {
        selection[i] = i % 5 == 0 ? 0.0F : i % 3 == 0 ? 0.5F : 1.0F;
    }
    model.SetSelectionWeights(selection);
    verify(*model.GetSnapshot());

    auto loadOnly = settings;
    loadOnly.durationYears = 0.0;
    loadOnly.accumulationRate = 0.0;
    loadOnly.ablationRate = 0.0;
    loadOnly.glacialErodibility = 0.0;
    filter.SetSettings(loadOnly);
    const auto selectedSnapshot = model.GetSnapshot();
    const auto loaded = filter.Apply(*selectedSnapshot, elevations);
    for (std::size_t i = 0; i < size; ++i) {
        const double weight = selectedSnapshot->HasSelection()
            ? selectedSnapshot->GetVertices()[i].selectionWeight : 1.0;
        const double expected = elevations[i] - (917.0 / 3300.0) * loadOnly.initialIceThickness * weight;
        if (elevations[i] > 0.0F && weight > 0.0) {
            check(std::abs(loaded[i] - static_cast<float>(expected)) < 0.001F,
                  "A zero-duration glacial pass did not preserve initial ice loading.");
        } else {
            check(loaded[i] == elevations[i],
                  "A zero-duration glacial pass changed a fixed sink.");
        }
    }

    const auto defaults = GlacialErosionSettings{};
    const std::vector<double GlacialErosionSettings::*> nonnegativeParameters{
        &GlacialErosionSettings::elaLatitudeGradient,
        &GlacialErosionSettings::accumulationRate,
        &GlacialErosionSettings::ablationRate,
        &GlacialErosionSettings::initialIceThickness,
        &GlacialErosionSettings::durationYears,
        &GlacialErosionSettings::glacialErodibility,
        &GlacialErosionSettings::iceViscosityFactor,
        &GlacialErosionSettings::thicknessExponent,
        &GlacialErosionSettings::velocityExponent
    };
    for (const auto member : nonnegativeParameters) {
        auto invalid = defaults;
        invalid.*member = -1.0;
        expectInvalid([&] { filter.SetSettings(invalid); },
                      "Glacial erosion accepted a negative rate, thickness, duration, or exponent.");
        invalid.*member = std::numeric_limits<double>::quiet_NaN();
        expectInvalid([&] { filter.SetSettings(invalid); },
                      "Glacial erosion accepted a non-finite parameter.");
    }
    auto invalidWidth = defaults;
    invalidWidth.troughHalfWidth = 0.0;
    expectInvalid([&] { filter.SetSettings(invalidWidth); },
                  "Glacial erosion accepted a zero trough width.");
    filter.SetSettings({});
    const auto snapshot = model.GetSnapshot();
    expectInvalid([&] { filter.Apply(*snapshot, std::span<const float>(elevations).first(1)); },
                  "Glacial erosion accepted a mismatched elevation count.");
}

void testCratonicGrooving() {
    SphericalGrid model({1000, 0.0, 101});
    model.SetPlanetRadius(1200.0);
    const auto size = model.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(size);
    for (std::size_t i = 0; i < size; ++i) {
        elevations[i] = 100.0F + static_cast<float>((i * 17) % 70);
    }
    elevations[0] = -20.0F;
    CratonicGroovingSettings settings;
    settings.iceFlowDirection = {0.4, -0.3, 0.8};
    settings.erodibility = 0.7;
    settings.grooveCoefficient = 0.2;
    settings.anisotropy = 0.003;
    CratonicGroovingFilter filter;
    filter.SetSettings(settings);

    const auto snapshot = model.GetSnapshot();
    const auto& vertices = snapshot->GetVertices();
    const auto& edges = snapshot->GetHalfEdges();
    const auto direction = glm::normalize(settings.iceFlowDirection);
    std::vector<double> parallelDistance(size, 0.0);
    std::vector<std::size_t> counts(size, 0);
    for (const auto& edge : edges) {
        const auto source = static_cast<std::size_t>(edge.originVertex);
        const auto target = static_cast<std::size_t>(
            edges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
        const auto from = glm::normalize(glm::dvec3(vertices[source].position));
        const auto to = glm::normalize(glm::dvec3(vertices[target].position));
        const double dot = std::clamp(glm::dot(from, to), -1.0, 1.0);
        const double distance = snapshot->GetOptions().planetRadius * std::acos(dot);
        const auto tangent = to - dot * from;
        auto localFlow = direction - glm::dot(direction, from) * from;
        if (glm::length(localFlow) > 0.0) {
            localFlow = glm::normalize(localFlow);
            parallelDistance[source] += std::abs(
                glm::dot(glm::normalize(tangent) * distance, localFlow));
        }
        ++counts[source];
    }
    const auto result = filter.Apply(*snapshot, elevations);
    bool changed = false;
    for (std::size_t i = 0; i < size; ++i) {
        double expected = elevations[i];
        if (i != 0) {
            const double meanParallelDistance = parallelDistance[i] / counts[i];
            expected -= settings.grooveCoefficient * settings.erodibility *
                (1.0 + settings.anisotropy * meanParallelDistance);
        }
        check(std::abs(result[i] - expected) < 0.001,
              "Cratonic grooving disagreed with the directional incision formula.");
        check(result[i] >= -32767.0F && result[i] <= 32767.0F,
              "Cratonic grooving exceeded supported elevation limits.");
        changed |= result[i] != elevations[i];
    }
    check(result[0] == elevations[0] && changed,
          "Cratonic grooving changed a fixed sink or failed to incise terrain.");

    std::vector<float> weights(size, 1.0F);
    weights[1] = 0.0F;
    weights[2] = 0.25F;
    model.SetSelectionWeights(weights);
    const auto selected = filter.Apply(*model.GetSnapshot(), elevations);
    check(selected[1] == elevations[1],
          "Cratonic grooving changed an unselected vertex.");
    check(std::abs(selected[2] - elevations[2] -
                   0.25F * (result[2] - elevations[2])) < 0.001F,
          "Cratonic grooving did not honor selection feathering.");

    for (auto invalid : {CratonicGroovingSettings{glm::dvec3(0.0), 0.5, 1.0, 1.0},
                         CratonicGroovingSettings{{1.0, 0.0, 0.0}, 1.1, 1.0, 1.0},
                         CratonicGroovingSettings{{1.0, 0.0, 0.0}, 0.5, -1.0, 1.0}}) {
        expectInvalid([&] { filter.SetSettings(invalid); },
                      "Cratonic grooving accepted invalid parameters.");
    }
    expectInvalid([&] {
        filter.Apply(*model.GetSnapshot(), std::span<const float>(elevations).first(1));
    }, "Cratonic grooving accepted a mismatched elevation count.");
}

void testCratonicScourAndPlanation() {
    SphericalGrid model({1000, 0.0, 102});
    const auto size = model.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(size);
    for (std::size_t i = 0; i < size; ++i) {
        elevations[i] = 500.0F + static_cast<float>((i * 23) % 700);
    }
    elevations[0] = -10.0F;
    elevations[1] = 100.0F;

    CratonicScourSettings settings;
    settings.iceFlowDirection = {0.0, 0.0, 1.0};
    settings.erodibility = 0.6;
    settings.stossAbrasionCoefficient = 0.0;
    settings.pluckingRate = 0.0;
    settings.cratonElevation = 300.0;
    settings.planationAttenuation = 0.002;
    CratonicScourFilter filter;
    filter.SetSettings(settings);
    const auto result = filter.Apply(*model.GetSnapshot(), elevations);
    bool planed = false;
    for (std::size_t i = 0; i < size; ++i) {
        double expected = elevations[i];
        if (i != 0) {
            const double above = elevations[i] - settings.cratonElevation;
            expected = settings.cratonElevation + above /
                (1.0 + settings.planationAttenuation * std::max(0.0, above));
        }
        check(std::abs(result[i] - expected) < 0.001,
              "Cratonic planation disagreed with the specified attenuation formula.");
        check(result[i] >= -32767.0F && result[i] <= 32767.0F,
              "Cratonic scour exceeded supported elevation limits.");
        planed |= result[i] < elevations[i];
    }
    check(result[0] == elevations[0] && planed,
          "Cratonic planation changed a fixed sink or failed to truncate high terrain.");
    check(result[1] == elevations[1],
          "Cratonic planation moved terrain below its baseline.");

    settings.planationAttenuation = 0.0;
    settings.stossAbrasionCoefficient = 0.05;
    settings.pluckingRate = 0.2;
    filter.SetSettings(settings);
    const auto scoured = filter.Apply(*model.GetSnapshot(), elevations);
    bool scouredChangedTerrain = false;
    for (std::size_t i = 1; i < size; ++i) {
        scouredChangedTerrain |= scoured[i] != elevations[i];
    }
    check(scouredChangedTerrain && scoured[0] == elevations[0],
          "Cratonic stoss/lee scouring failed or changed a fixed sink.");

    std::vector<float> weights(size, 1.0F);
    weights[1] = 0.0F;
    weights[2] = 0.5F;
    model.SetSelectionWeights(weights);
    const auto selected = filter.Apply(*model.GetSnapshot(), elevations);
    check(selected[1] == elevations[1],
          "Cratonic scour changed an unselected vertex.");
    check(std::abs(selected[2] - elevations[2] -
                   0.5F * (scoured[2] - elevations[2])) < 0.001F,
          "Cratonic scour did not honor selection feathering.");

    auto invalid = settings;
    invalid.iceFlowDirection = glm::dvec3(0.0);
    expectInvalid([&] { filter.SetSettings(invalid); },
                  "Cratonic scour accepted a zero flow direction.");
    invalid = settings;
    invalid.planationAttenuation = -1.0;
    expectInvalid([&] { filter.SetSettings(invalid); },
                  "Cratonic scour accepted negative planation attenuation.");
    invalid = settings;
    invalid.cratonElevation = 32768.0;
    expectInvalid([&] { filter.SetSettings(invalid); },
                  "Cratonic scour accepted an unsupported craton elevation.");
    expectInvalid([&] {
        filter.Apply(*model.GetSnapshot(), std::span<const float>(elevations).first(1));
    }, "Cratonic scour accepted a mismatched elevation count.");
}

void testAbsoluteMagnitudeNoise() {
    SphericalGrid model({1000, 0.0, 99});
    const auto size = model.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(size, 0.0F);
    AbsoluteMagnitudeNoiseFilter filter;
    for (const float invalid : {-1.0F, 500.1F, std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity()}) {
        expectInvalid([&] { filter.SetAmplitudeFeet(invalid); },
                      "Noise filter accepted an invalid amplitude.");
    }
    for (const float amplitude : {0.0F, 100.0F, 500.0F}) {
        filter.SetAmplitudeFeet(amplitude);
        check(filter.GetAmplitudeFeet() == amplitude, "Noise amplitude setting was not retained.");
        const auto result = filter.Apply(*model.GetSnapshot(), elevations);
        float minimum = 0.0F;
        float maximum = 0.0F;
        for (const auto value : result) {
            check(std::abs(value) <= amplitude * 0.3048 + 1e-5,
                  "Noise exceeded its amplitude in feet converted to metres.");
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        if (amplitude > 0.0F) {
            check(minimum < -amplitude * 0.3048 * 0.8 && maximum > amplitude * 0.3048 * 0.8,
                  "Noise did not produce both signs over the requested amplitude range.");
        } else {
            check(result == elevations, "Zero-amplitude noise changed terrain.");
        }
    }
    std::vector<float> weights(size, 0.0F);
    for (std::size_t i = 0; i < size; ++i) {
        weights[i] = i % 3 == 0 ? 0.0F : i % 3 == 1 ? 0.25F : 1.0F;
    }
    model.SetSelectionWeights(weights);
    const auto result = filter.Apply(*model.GetSnapshot(), elevations);
    bool featheredVariation = false;
    for (std::size_t i = 0; i < size; ++i) {
        check(std::abs(result[i]) <= 152.4 * weights[i] + 1e-5,
              "Noise did not multiply its offset by selection feathering.");
        if (weights[i] == 0.25F && std::abs(result[i]) > 30.0F) {
            featheredVariation = true;
        }
        check(elevations[i] == 0.0F, "Noise modified its input elevation buffer.");
    }
    check(featheredVariation, "Noise fixture did not exercise fractional feathering.");
    model.SetSelectionWeights(std::vector<float>(size, 0.0F));
    check(filter.Apply(*model.GetSnapshot(), elevations) == elevations,
          "Noise modified an active empty selection.");
    model.SetSelectionWeights(weights, false);
    for (const float height : {-32767.0F, -1000.0F, 32767.0F}) {
        std::fill(elevations.begin(), elevations.end(), height);
        const auto clamped = filter.Apply(*model.GetSnapshot(), elevations);
        bool varied = false;
        for (const auto value : clamped) {
            check(value >= -32767.0F && value <= 32767.0F &&
                  std::abs(value - height) <= 152.402F,
                  "Noise exceeded the elevation limits or amplitude.");
            varied |= value != height;
        }
        check(varied, "Noise failed to affect unselected global or underwater terrain.");
    }
    const auto snapshot = model.GetSnapshot();
    expectInvalid([&] { filter.Apply(*snapshot, std::span<const float>(elevations).first(1)); },
                  "Noise accepted an incorrect elevation count.");
    for (const float invalid : {32768.0F, -32768.0F, std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity()}) {
        elevations[0] = invalid;
        expectInvalid([&] { filter.Apply(*snapshot, elevations); },
                      "Noise accepted invalid input elevations.");
    }
}

}

int main() {
    try {
        testFillOperationsAndSelectionWeights();
        testGlobalApplicationAndClamping();
        testValidationAndPluginRegistration();
        testSphericalFillBasins();
        testFillBasinsSelectionAndValidation();
        testInciseFlowFormulaAndSelection();
        testInciseFlowSinksAndParameterScaling();
        testInciseFlowValidation();
        testPrecipitonDiffusion();
        testThermalWeathering();
        testGlacialErosion();
        testCratonicGrooving();
        testCratonicScourAndPlanation();
        testAbsoluteMagnitudeNoise();
        std::cout << "Filter tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Filter test failure: " << error.what() << '\n';
        return 1;
    }
}
