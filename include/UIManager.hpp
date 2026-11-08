#pragma once

#include "GlobeRenderer.hpp"
#include "ElevationRaster.hpp"
#include "RasterFileBrowser.hpp"

#include <filesystem>
#include <optional>
#include <string>

class SphericalFibonacciGrid;

class UIManager {
public:
    explicit UIManager(const GridOptions& options);

    bool Render(SphericalFibonacciGrid& grid, GlobeRenderer& renderer);

private:
    void NavigateRasterDirectory(const std::filesystem::path& directory);

    GridOptions options_;
    bool settingsOpen_ = false;
    bool rasterOpen_ = false;
    RasterFileBrowser rasterBrowser_;
    std::filesystem::path selectedRasterPath_;
    bool selectedRasterIsDirectory_ = false;
    std::optional<ElevationRaster> raster_;
    std::filesystem::path loadedRasterPath_;
    std::string errorMessage_;
};