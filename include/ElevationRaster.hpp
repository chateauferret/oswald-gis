#pragma once

#include "SphericalGrid.hpp"

#include <filesystem>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

class ColorMap;

struct RasterExportSamples {
    std::vector<float> elevations;
    std::vector<std::uint32_t> rgb;
};

class RasterComputeBackend {
public:
    virtual ~RasterComputeBackend() = default;
    virtual std::vector<float> SampleRaster(
        std::shared_ptr<const SphericalGridSnapshot> snapshot,
        std::span<const float> rasterSamples, int width, int height,
        bool elevationSamples, const std::filesystem::path& shaderDirectory) = 0;
    virtual RasterExportSamples SampleExport(
        std::shared_ptr<const SphericalGridSnapshot> snapshot,
        std::span<const float> elevations, std::span<const float> selectionWeights,
        int width, int height, const ColorMap* colorMap,
        const ColorMap* selectionColorMap,
        const std::filesystem::path& shaderDirectory) = 0;
};

class ElevationRaster {
public:
    enum class TiffSampleType { Grayscale8, Elevation16, Elevation32 };

    static ElevationRaster Load(const std::filesystem::path& path);
    static void Export(const std::filesystem::path& path, const SphericalGrid& grid,
                       int width = 2048, int height = 1024,
                       TiffSampleType tiffSampleType = TiffSampleType::Elevation32);
    static void Export(const std::filesystem::path& path, const SphericalGrid& grid,
                       int width, int height, TiffSampleType tiffSampleType,
                       RasterComputeBackend& compute,
                       const std::filesystem::path& shaderDirectory);
    static std::vector<std::filesystem::path> ExportPaths(const std::filesystem::path& path);
    static void ExportImage(const std::filesystem::path& path, const SphericalGrid& grid,
                            const ColorMap& colorMap, const ColorMap* selectionColorMap = nullptr,
                            int width = 2048, int height = 1024);
    static void ExportImage(const std::filesystem::path& path, const SphericalGrid& grid,
                            const ColorMap& colorMap, const ColorMap* selectionColorMap,
                            int width, int height, RasterComputeBackend& compute,
                            const std::filesystem::path& shaderDirectory);

    std::vector<float> SampleElevations(const SphericalGridSnapshot& grid) const;
    std::vector<float> SampleElevations(const SphericalGridSnapshot& grid,
                                       RasterComputeBackend& compute,
                                       const std::filesystem::path& shaderDirectory) const;
    float SampleLuminosity(double u, double v) const;

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }

private:
    static void ExportRaster(const std::filesystem::path& path, const SphericalGrid& grid,
                             int width, int height, TiffSampleType tiffSampleType,
                             const ColorMap* colorMap, const ColorMap* selectionColorMap,
                             RasterComputeBackend* compute = nullptr,
                             const std::filesystem::path& shaderDirectory = {});
    float SampleValue(double u, double v) const;
    int width_ = 0;
    int height_ = 0;
    bool elevationSamples_ = false;
    std::vector<float> samples_;
};