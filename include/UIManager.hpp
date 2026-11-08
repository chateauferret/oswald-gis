#pragma once

#include "GlobeRenderer.hpp"
#include "ElevationRaster.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

class SphericalFibonacciGrid;

class UIManager {
public:
    explicit UIManager(const GridOptions& options);

    bool Render(SphericalFibonacciGrid& grid, GlobeRenderer& renderer);

private:
    GridOptions options_;
    bool settingsOpen_ = false;
    bool rasterOpen_ = false;
    std::array<char, 4096> rasterPathInput_{};
    std::optional<ElevationRaster> raster_;
    std::filesystem::path loadedRasterPath_;
    std::string errorMessage_;
};