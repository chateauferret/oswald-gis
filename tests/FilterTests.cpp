#include "Filter.hpp"

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
    SphericalFibonacciGrid model({1000, 0.0, 91});
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
    SphericalFibonacciGrid model({1000, 0.0, 92});
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
    SphericalFibonacciGrid model({1000, 0.0, 93});
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
    check(manager.GetFilters().size() == 1 &&
          manager.GetFilter("Fill").GetName() == "Fill",
          "Filter manager did not register or return the Fill plugin.");
    expectInvalid([&] { manager.RegisterFilter(std::make_unique<FillFilter>()); },
                  "Filter manager accepted a duplicate filter name.");
    expectInvalid([&] { manager.RegisterFilter(nullptr); },
                  "Filter manager accepted an empty plugin.");
}

}

int main() {
    try {
        testFillOperationsAndSelectionWeights();
        testGlobalApplicationAndClamping();
        testValidationAndPluginRegistration();
        std::cout << "Filter tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Filter test failure: " << error.what() << '\n';
        return 1;
    }
}
