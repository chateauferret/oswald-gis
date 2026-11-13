#pragma once

#include "SphericalGrid.hpp"

#include <filesystem>
#include <vector>

class ColorMap;

class ElevationRaster {
public:
    enum class TiffSampleType { Grayscale8, Elevation16, Elevation32 };

    static ElevationRaster Load(const std::filesystem::path& path);
    static void Export(const std::filesystem::path& path, const SphericalGrid& grid,
                       int width = 2048, int height = 1024,
                       TiffSampleType tiffSampleType = TiffSampleType::Elevation32);
    static std::vector<std::filesystem::path> ExportPaths(const std::filesystem::path& path);
    static void ExportImage(const std::filesystem::path& path, const SphericalGrid& grid,
                            const ColorMap& colorMap, const ColorMap* selectionColorMap = nullptr,
                            int width = 2048, int height = 1024);

    std::vector<float> SampleElevations(const SphericalGridSnapshot& grid) const;
    float SampleLuminosity(double u, double v) const;

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }

private:
    static void ExportRaster(const std::filesystem::path& path, const SphericalGrid& grid,
                             int width, int height, TiffSampleType tiffSampleType,
                             const ColorMap* colorMap, const ColorMap* selectionColorMap);
    float SampleValue(double u, double v) const;
    int width_ = 0;
    int height_ = 0;
    bool elevationSamples_ = false;
    std::vector<float> samples_;
};