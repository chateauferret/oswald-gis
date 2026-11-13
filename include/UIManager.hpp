#pragma once

#include "GlobeRenderer.hpp"
#include "ElevationRaster.hpp"
#include "RasterFileBrowser.hpp"
#include "BrushTool.hpp"
#include "Filter.hpp"
#include "SelectionTool.hpp"
#include "UndoStack.hpp"
#include "CameraController.hpp"

#include <filesystem>
#include <array>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class SphericalGrid;
class RasterImportCommand;

class UIManager {
public:
    explicit UIManager(const GridOptions& options,
                       std::filesystem::path colorMapDirectory = {});
    ~UIManager();
    UIManager(const UIManager&) = delete;
    UIManager& operator=(const UIManager&) = delete;

    // Grids with more points than this are generated on a worker thread with a progress dialog.
    static constexpr std::size_t backgroundGenerationThreshold = 25'000;

    bool Render(SphericalGrid& grid, GlobeRenderer& renderer,
                const glm::mat4& modelMatrix = glm::mat4(1.0F), float zoom = 1.0F);
    bool IsBrushActive() const { return brushTools_.IsActive() || selectionMode_ != SelectionMode::None; }
    void SetCameraController(CameraController* camera) { camera_ = camera; }
    CameraController* GetCameraController() const { return camera_; }

private:
    friend class RasterImportCommand;
    friend class BrushStrokeCommand;

    struct Stroke {
        std::vector<float> original;
        std::vector<float> working;
        std::vector<float> scratch;
    };

    enum class SelectionMode { None, Radius, Polygon };

    struct SelectionStroke {
        std::vector<float> before;
        std::vector<float> working;
        std::vector<GridVertex> renderVertices;
        bool wasActive = false;
    };

    using GridFuture = std::future<std::shared_ptr<const SphericalGridSnapshot>>;

    void StartGridGeneration(SphericalGrid& grid, GlobeRenderer& renderer);
    void PollGridGeneration(SphericalGrid& grid, GlobeRenderer& renderer);
    void InstallGeneratedGrid(SphericalGrid& grid, GlobeRenderer& renderer,
                              std::shared_ptr<const SphericalGridSnapshot> snapshot);
    void RenderGridProgressDialog();
    void NavigateRasterDirectory(const std::filesystem::path& directory);
    void RenderBrushPanel();
    void RenderSelectionPanel();
    void RenderValueSelectionDialog(SphericalGrid& grid, GlobeRenderer& renderer);
    void RenderFillFilterDialog(SphericalGrid& grid, GlobeRenderer& renderer);
    void RenderLayerPanel(SphericalGrid& grid, GlobeRenderer& renderer);
    void ApplyActiveLayerColorMaps(SphericalGrid& grid, GlobeRenderer& renderer);
    const ColorMap& GetColorMap(const std::string& name);
    void ProcessBrushInput(SphericalGrid& grid, GlobeRenderer& renderer,
                           const glm::mat4& modelMatrix, float zoom);
    void ProcessSelectionInput(SphericalGrid& grid, GlobeRenderer& renderer,
                               const glm::mat4& modelMatrix, float zoom);
    void SetSelectionMode(SelectionMode mode);
    void FinishPolygonSelection(SphericalGrid& grid, GlobeRenderer& renderer);

    GridOptions options_;
    GridOptions previousOptions_;
    GridFuture gridFuture_;
    std::unique_ptr<GridGenerationControl> gridControl_;
    bool gridProgressOpen_ = false;
    bool gridCancelRequested_ = false;
    UndoStack undoStack_;
    BrushToolManager brushTools_;
    FilterManager filters_;
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
    bool fillFilterOpen_ = false;
    FillFilterSettings pendingFillSettings_;
    std::string fillFilterError_;
    bool settingsOpen_ = false;
    double pendingRaiseLowerRate_ = BrushSettings{}.raiseLowerRate;
    CameraController* camera_ = nullptr;
    bool viewportWireframe_ = false;
    bool viewportGraticule_ = true;
    bool rasterOpen_ = false;
    RasterFileBrowser rasterBrowser_;
    std::filesystem::path selectedRasterPath_;
    bool selectedRasterIsDirectory_ = false;
    std::shared_ptr<const ElevationRaster> raster_;
    std::filesystem::path loadedRasterPath_;
    std::filesystem::path colorMapDirectory_;
    std::unordered_map<std::string, ColorMap> colorMapCache_;
    std::string colorMapName_ = "default.map";
    std::string selectionColorMapName_ = "grayscale.map";
    std::string legendError_;
    bool legendErrorOpen_ = false;
    bool layerDialogOpen_ = false;
    bool deleteLayerDialogOpen_ = false;
    std::array<char, 128> newLayerName_{};
    std::string newLayerColorMap_ = "default.map";
    std::string newLayerSelectionColorMap_ = "grayscale.map";
    std::string pendingLayerDeletion_;
    ElevationLayer* nameEditLayer_ = nullptr;
    std::array<char, 128> layerNameBuffer_{};
    std::string layerError_;
    std::string meshError_;
    bool meshErrorOpen_ = false;
    glm::vec3 graticuleColor_{0.75F};
    glm::vec3 highlightedGraticuleColor_{1.0F};
    glm::vec3 wireframeColor_{1.0F, 1.0F, 0.0F};
    glm::vec3 wireframeSelectedColor_{0.5F, 0.5F, 0.0F};
    glm::vec3 pendingWireframeColor_{1.0F, 1.0F, 0.0F};
    glm::vec3 pendingWireframeSelectedColor_{0.5F, 0.5F, 0.0F};
    glm::vec3 pendingGraticuleColor_{0.75F};
    glm::vec3 pendingHighlightedGraticuleColor_{1.0F};
    std::string errorMessage_;
};