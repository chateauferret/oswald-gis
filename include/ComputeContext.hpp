#pragma once

#include "SphericalGrid.hpp"
#include "ElevationRaster.hpp"
#include "BrushScriptComputeBackend.hpp"

#include <glad/gl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Filter;
class ColorMap;

enum class ComputeBackendPreference : int {
    Auto,
    Cpu,
    Gpu
};

struct ComputeError {
    enum class Code : std::uint32_t {
        NonFinite = 1,
        Overflow = 2,
        OutOfRange = 3
    };

    Code code;
    std::uint32_t index;
};

enum class SelectionComputeOperation : int {
    ByValue,
    Clear,
    Invert
};

class GpuMesh {
public:
    GpuMesh() = default;
    ~GpuMesh();
    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;
    GpuMesh(GpuMesh&& source) noexcept;
    GpuMesh& operator=(GpuMesh&& source) noexcept;

    GLuint Positions() const { return positions_; }
    GLuint NeighborOffsets() const { return neighborOffsets_; }
    GLuint Neighbors() const { return neighbors_; }
    GLuint EdgeDistancesKm() const { return edgeDistancesKm_; }
    GLuint PreciseEdgeDistancesKm() const { return preciseEdgeDistancesKm_; }
    std::size_t VertexCount() const { return vertexCount_; }
    std::size_t DirectedEdgeCount() const { return directedEdgeCount_; }

private:
    friend class ComputeContext;
    GpuMesh(const SphericalGridSnapshot& snapshot);
    void Release() noexcept;

    GLuint positions_ = 0;
    GLuint neighborOffsets_ = 0;
    GLuint neighbors_ = 0;
    GLuint edgeDistancesKm_ = 0;
    GLuint preciseEdgeDistancesKm_ = 0;
    std::size_t vertexCount_ = 0;
    std::size_t directedEdgeCount_ = 0;
};

class ComputeContext : public RasterComputeBackend, public BrushScriptComputeBackend {
public:
    static constexpr GLuint errorBufferBinding = 2;

    ComputeContext();
    // Must be destroyed while the creating OpenGL context is current.
    ~ComputeContext();
    ComputeContext(const ComputeContext&) = delete;
    ComputeContext& operator=(const ComputeContext&) = delete;

    GpuMesh& MeshFor(std::shared_ptr<const SphericalGridSnapshot> snapshot);
    GLuint Scratch(std::size_t floatCount, std::size_t slot = 0);
    GLuint LoadProgram(const std::filesystem::path& shaderPath);
    GLuint LoadProgram(std::string_view source, std::string cacheKey);
    void Dispatch1D(GLuint program, std::size_t count);
    std::vector<float> Readback(GLuint buffer, std::size_t floatCount) const;
    void ClearErrors();
    std::optional<ComputeError> CheckErrors();
    std::vector<float> EvaluateBrushScript(
        std::string_view gpuScript, std::span<const GpuScriptInput> inputs,
        std::span<const GpuScriptParameter> parameters, std::uint64_t seed,
        std::span<const GpuScriptTerrainVertex> vertices,
        std::span<const float> elevations, double planetRadiusKm) override;
    std::vector<float> ApplyFilter(const Filter& filter,
                                   std::shared_ptr<const SphericalGridSnapshot> snapshot,
                                   std::span<const float> elevations,
                                   const std::filesystem::path& shaderDirectory);
    std::vector<float> ApplySelection(SelectionComputeOperation operation,
                                      std::shared_ptr<const SphericalGridSnapshot> snapshot,
                                      std::span<const float> currentWeights,
                                      double lowerBound, double upperBound,
                                      double featherPercent,
                                      const std::filesystem::path& shaderDirectory);
    std::vector<float> ApplyRadiusSelection(
        std::shared_ptr<const SphericalGridSnapshot> snapshot,
        std::span<const float> currentWeights, glm::dvec3 center, double radius,
        double falloffPercent, double planetRadius,
        const std::filesystem::path& shaderDirectory);
    std::vector<float> ApplyPolygonSelection(
        std::shared_ptr<const SphericalGridSnapshot> snapshot,
        std::span<const glm::dvec3> polygon,
        const std::filesystem::path& shaderDirectory);
    std::array<float, 32> Histogram(std::span<const float> elevations,
                                    const std::filesystem::path& shaderDirectory);
    std::vector<float> SampleRaster(
        std::shared_ptr<const SphericalGridSnapshot> snapshot,
        std::span<const float> rasterSamples, int width, int height,
        bool elevationSamples, const std::filesystem::path& shaderDirectory) override;
    RasterExportSamples SampleExport(
        std::shared_ptr<const SphericalGridSnapshot> snapshot,
        std::span<const float> elevations, std::span<const float> selectionWeights,
        int width, int height, const ColorMap* colorMap,
        const ColorMap* selectionColorMap,
        const std::filesystem::path& shaderDirectory) override;
    GLuint ErrorBuffer() const { return errorBuffer_; }

private:
    struct CachedMesh {
        std::weak_ptr<const SphericalGridSnapshot> snapshot;
        GpuMesh mesh;
    };

    GLuint errorBuffer_ = 0;
    std::unordered_map<const SphericalGridSnapshot*, CachedMesh> meshes_;
    std::map<std::pair<std::size_t, std::size_t>, GLuint> scratchBuffers_;
    std::vector<GLuint> programs_;
    std::unordered_map<std::string, GLuint> programsByPath_;
};
