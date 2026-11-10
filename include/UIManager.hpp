#pragma once

#include "GlobeRenderer.hpp"
#include "ElevationRaster.hpp"
#include "RasterFileBrowser.hpp"
#include "BrushTool.hpp"
#include "SelectionTool.hpp"
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
    bool IsBrushActive() const { return brushTools_.IsActive() || selectionMode_ != SelectionMode::None; }

private:
    friend class RasterImportCommand;
    friend class BrushStrokeCommand;

    struct Stroke {
        std::vector<float> original;
        std::vector<float> working;
        std::vector<float> scratch;
        std::vector<GridVertex> renderVertices;
    };

    enum class SelectionMode { None, Radius, Polygon };

    struct SelectionStroke {
        std::vector<float> before;
        std::vector<float> working;
        std::vector<GridVertex> renderVertices;
        bool wasActive = false;
    };

    void NavigateRasterDirectory(const std::filesystem::path& directory);
    void RenderBrushPanel();
    void RenderSelectionPanel();
    void RenderValueSelectionDialog(SphericalFibonacciGrid& grid, GlobeRenderer& renderer);
    void ProcessBrushInput(SphericalFibonacciGrid& grid, GlobeRenderer& renderer,
                           const glm::mat4& modelMatrix, float zoom);
    void ProcessSelectionInput(SphericalFibonacciGrid& grid, GlobeRenderer& renderer,
                               const glm::mat4& modelMatrix, float zoom);
    void SetSelectionMode(SelectionMode mode);
    void FinishPolygonSelection(SphericalFibonacciGrid& grid, GlobeRenderer& renderer);

    GridOptions options_;
    UndoStack undoStack_;
    BrushToolManager brushTools_;
    std::optional<Stroke> stroke_;
    SelectionMode selectionMode_ = SelectionMode::None;
    SelectionSettings selectionSettings_;
    RadiusSelectionTool radiusSelectionTool_;
    std::optional<SelectionStroke> selectionStroke_;
    std::vector<glm::dvec3> polygonPoints_;
    bool polygonAdditive_ = false;
    bool valueSelectionOpen_ = false;
    double selectionLowerBound_ = -32767.0;
    double selectionUpperBound_ = 32767.0;
    float selectionFeatherPercent_ = 50.0F;
    std::string valueSelectionError_;
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