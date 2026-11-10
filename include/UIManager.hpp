#pragma once

#include "GlobeRenderer.hpp"
#include "ElevationRaster.hpp"
#include "RasterFileBrowser.hpp"
#include "BrushTool.hpp"
#include "UndoStack.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class SphericalFibonacciGrid;
class RasterImportCommand;

class UIManager {
public:
    explicit UIManager(const GridOptions& options);

    bool Render(SphericalFibonacciGrid& grid, GlobeRenderer& renderer,
                const glm::mat4& modelMatrix = glm::mat4(1.0F), float zoom = 1.0F);
    bool IsBrushActive() const { return brushTools_.IsActive(); }

private:
    friend class RasterImportCommand;
    friend class BrushStrokeCommand;

    struct Stroke {
        std::vector<float> original;
        std::vector<float> working;
        std::vector<float> scratch;
        std::vector<GridVertex> renderVertices;
    };

    void NavigateRasterDirectory(const std::filesystem::path& directory);
    void RenderBrushPanel();
    void ProcessBrushInput(SphericalFibonacciGrid& grid, GlobeRenderer& renderer,
                           const glm::mat4& modelMatrix, float zoom);

    GridOptions options_;
    UndoStack undoStack_;
    BrushToolManager brushTools_;
    std::optional<Stroke> stroke_;
    bool settingsOpen_ = false;
    double pendingRaiseLowerRate_ = BrushSettings{}.raiseLowerRate;
    bool rasterOpen_ = false;
    RasterFileBrowser rasterBrowser_;
    std::filesystem::path selectedRasterPath_;
    bool selectedRasterIsDirectory_ = false;
    std::shared_ptr<const ElevationRaster> raster_;
    std::filesystem::path loadedRasterPath_;
    std::string errorMessage_;
};