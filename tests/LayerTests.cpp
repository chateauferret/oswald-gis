#include "Layer.hpp"
#include "SphericalGrid.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception = std::invalid_argument, typename Operation>
void expectFailure(Operation&& operation, const char* message) {
    try {
        operation();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void testLayerMetadataAndGridSynchronization() {
    static_assert(std::is_abstract_v<Layer>);
    static_assert(std::is_base_of_v<Layer, ElevationLayer>);

    SphericalGrid grid({1000, 0.0, 101});
    check(grid.GetLayers().size() == 1, "Grid did not create its elevation layer.");
    check(grid.GetActiveLayer().GetName() == "Elevation" &&
              grid.GetActiveLayer().GetType() == LayerType::Raster &&
              grid.GetActiveLayer().GetBlendMode() == LayerBlendMode::Overlay &&
              grid.GetActiveLayer().GetDataType() == LayerDataType::Float32 &&
              grid.GetActiveLayer().GetColorMap() == "default.map" &&
              grid.GetActiveLayer().GetSelectionColorMap() == "grayscale.map" &&
              !grid.GetActiveLayer().GetDescription().empty(),
          "Elevation layer metadata was not initialized.");
    grid.GetElevationLayer().SetBlendMode(LayerBlendMode::Multiply);
    check(grid.GetActiveLayer().GetBlendMode() == LayerBlendMode::Multiply &&
              grid.GetActiveLayer().IsVisible(),
          "Layer blend mode or default visibility metadata was not stored.");

    const auto count = grid.GetSnapshot()->GetVertices().size();
    std::vector<float> elevations(count, 123.5F);
    grid.SetElevations(elevations);
    check(grid.GetElevationLayer().GetValueCount() == count &&
              std::abs(grid.GetElevationLayer().GetValues()[0] - 123.5F) < 1e-5F &&
              grid.GetSnapshot()->GetVertices()[0].elevation == 123.5F,
          "Elevation layer values did not stay synchronized with the grid.");

    grid.GetElevationLayer().SetColorMap("relief.map");
    check(grid.GetActiveLayer().GetColorMap() == "relief.map",
          "Elevation layer metadata did not retain its colour map.");
    grid.SetActiveLayer("Elevation");
    std::vector<float> selection(count, 0.0F);
    selection[0] = 1.0F;
    grid.SetSelectionWeights(selection);
    check(grid.GetElevationLayer().GetValues()[0] == 123.5F &&
              grid.GetSnapshot()->GetVertices()[0].selectionWeight == 1.0F,
          "Selection data was stored in or changed the elevation layer.");

    expectFailure<std::out_of_range>([&] { grid.SetActiveLayer("Missing"); },
                                     "Grid accepted a layer name that does not exist.");

    grid.InstallSnapshot(std::make_shared<const SphericalGridSnapshot>(
        GridOptions{1000, 0.0, 102}));
    check(grid.GetElevationLayer().GetValueCount() == grid.GetSnapshot()->GetVertices().size() &&
              grid.GetElevationLayer().GetValues()[0] == 0.0F,
          "Installing a new mesh snapshot did not synchronize the elevation layer.");
    grid.SetLayerVisible("Elevation", false);
    check(!grid.HasActiveLayer() && !grid.GetLayers()[0]->IsVisible(),
          "Hiding the active layer did not deactivate it.");
    grid.SetActiveLayer("Elevation");
    check(grid.HasActiveLayer() && grid.GetLayers()[0]->IsVisible(),
          "Activating a hidden layer did not make it visible.");
}

void testLayerMetadataValidation() {
    RasterLayer raster("Temperature", "Surface temperature", "thermal.map", {1.0F, 2.0F});
    raster.SetName("Temperature anomaly");
    raster.SetDescription("Float raster values.");
    check(raster.GetName() == "Temperature anomaly" &&
              raster.GetValueCount() == 2 && raster.GetValues()[1] == 2.0F,
          "Raster layer metadata or values were not preserved.");
    expectFailure([&] { raster.SetName(""); }, "Layer accepted an empty name.");
    expectFailure([&] { raster.SetColorMap(""); }, "Layer accepted an empty colour map.");
}

void testLayerStackOperationsAndIndependentData() {
    SphericalGrid grid({1000, 0.0, 103});
    const auto count = grid.GetSnapshot()->GetVertices().size();
    std::vector<float> baseValues(count, 240.0F);
    grid.SetElevations(baseValues);
    std::vector<float> weights(count, 0.0F);
    weights[0] = 0.75F;
    grid.SetSelectionWeights(weights);

    auto& added = grid.AddElevationLayer("Temperature", "thermal.map", "grayscale.map");
    check(grid.GetLayers().size() == 2 && &grid.GetElevationLayer() == &added &&
              grid.GetLayers()[0].get() == &added &&
              grid.GetSnapshot()->GetVertices()[0].elevation == 0.0F,
          "New elevation layer was not inserted above and activated independently.");
    std::vector<float> temperature(count, 15.0F);
    grid.SetElevations(temperature);

    grid.SetActiveLayer("Elevation");
    check(grid.GetSnapshot()->GetVertices()[0].elevation == 240.0F &&
              grid.GetSnapshot()->GetVertices()[0].selectionWeight == 0.75F,
          "Switching active layers did not restore its raster and preserve selection.");
    grid.SetActiveLayer("Temperature");
    check(grid.GetSnapshot()->GetVertices()[0].elevation == 15.0F,
          "Switching back did not restore the second layer's values.");

    grid.RenameLayer("Temperature", "Climate");
    expectFailure([&] { grid.RenameLayer("Climate", "Elevation"); },
                  "Grid accepted a duplicate layer name.");
    grid.MoveLayer("Climate", 1);
    check(grid.GetLayers()[1]->GetName() == "Climate",
          "Grid did not reorder the requested layer.");
    grid.RemoveLayer("Climate");
    check(grid.GetLayers().size() == 1 &&
              grid.GetActiveLayer().GetName() == "Elevation" &&
              grid.GetSnapshot()->GetVertices()[0].elevation == 240.0F,
          "Removing the active layer did not select and display its neighbor.");
    expectFailure<std::logic_error>([&] { grid.RemoveLayer("Elevation"); },
                                    "Grid allowed removal of its final layer.");
}

void testElevationComposition() {
    check(BlendElevations(0, 0, LayerBlendMode::Add) == 0,
          "Adding two zero elevations did not preserve zero.");
    check(BlendElevations(1000, 200, LayerBlendMode::Add) == 1200 &&
          BlendElevations(1000, 200, LayerBlendMode::Subtract) == 800 &&
          BlendElevations(1000, 2, LayerBlendMode::Multiply) == 2000 &&
          BlendElevations(1000, 200, LayerBlendMode::Minimum) == 200 &&
          BlendElevations(1000, 200, LayerBlendMode::Maximum) == 1000 &&
          BlendElevations(1000, 200, LayerBlendMode::Overlay) == 200,
          "Blend modes did not operate on elevation values.");
    check(BlendElevations(32767, 1, LayerBlendMode::Add) == 32767 &&
          BlendElevations(-32767, 1, LayerBlendMode::Subtract) == -32767 &&
          BlendElevations(-32767, 2, LayerBlendMode::Multiply) == -32767,
          "Composited elevations exceeded the supported range.");

    SphericalGrid grid({1000, 0.0, 104});
    const auto count = grid.GetSnapshot()->GetVertices().size();
    grid.SetElevations(std::vector<float>(count, 1000));
    auto& offset = grid.AddElevationLayer("Offset", "default.map", "grayscale.map");
    offset.SetBlendMode(LayerBlendMode::Add);
    check(grid.GetActiveCompositedElevation(0) == 1000,
          "An active zero-valued Add layer did not include the lower elevation.");
    const std::vector<float> preview(count, 200);
    check(grid.GetActiveCompositedElevation(0, preview) == 1200 &&
          offset.GetValues()[0] == 0,
          "Composited hover preview changed stored layer data or ignored the preview.");
    grid.SetElevations(preview);
    auto& upper = grid.AddElevationLayer("Upper", "default.map", "grayscale.map");
    grid.SetElevations(std::vector<float>(count, 9000));
    grid.SetActiveLayer("Offset");
    check(grid.GetActiveCompositedElevation(0) == 1200,
          "Active-layer readout included a layer above it.");
    grid.SetLayerVisible("Elevation", false);
    check(grid.GetActiveCompositedElevation(0) == 200,
          "Hidden lower layers contributed to the active readout.");
    grid.SetLayerVisible("Elevation", true);
    offset.SetBlendMode(LayerBlendMode::Subtract);
    check(grid.GetActiveCompositedElevation(0) == 800,
          "Subtract did not subtract the upper elevation from the lower elevation.");
    grid.MoveLayer("Offset", 2);
    check(grid.GetActiveCompositedElevation(0) == 200,
          "Reordering did not change which layers contribute to the active readout.");
    grid.SetActiveLayer(upper.GetName());
    check(grid.GetActiveCompositedElevation(0) == 9000,
          "Overlay did not replace accumulated lower elevations.");
    expectFailure([&] { grid.GetActiveCompositedElevation(count); },
                  "Composited hover query accepted an invalid vertex.");
    grid.DeactivateActiveLayer();
    expectFailure<std::logic_error>([&] { grid.GetActiveCompositedElevation(0); },
                                    "Composited hover query accepted no active layer.");
}

}

int main() {
    try {
        testLayerMetadataAndGridSynchronization();
        testLayerMetadataValidation();
        testLayerStackOperationsAndIndependentData();
        testElevationComposition();
        std::cout << "Layer tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Layer test failure: " << error.what() << '\n';
        return 1;
    }
}
