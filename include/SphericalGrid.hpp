#pragma once

#include "Layer.hpp"

#include <glm/glm.hpp>
#include <nanoflann.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

struct GridVertex {
    glm::vec3 position;
    float elevation = 0.0F;
    float selectionWeight = 0.0F;
};

struct HalfEdge {
    std::int32_t originVertex;
    std::int32_t twinEdge;
    std::int32_t nextEdge;
    std::int32_t faceIndex;
};

struct GridOptions {
    std::size_t pointCount = 10'000;
    // Higher values reduce the Poisson exclusion radius and allow more spacing variation.
    double poissonVariability = 0.5;
    std::uint32_t seed = 42;
    // Lloyd relaxation passes applied after Poisson sampling; they make cells more regular.
    std::uint32_t relaxationIterations = 0;
};

// Thread-safe progress and cancellation channel for long grid generations.
class GridGenerationControl {
public:
    void Cancel() { cancelled_.store(true); }
    bool IsCancelled() const { return cancelled_.load(); }
    float GetProgress() const { return progress_.load(); }
    void SetProgress(float progress) { progress_.store(progress); }

private:
    std::atomic<bool> cancelled_{false};
    std::atomic<float> progress_{0.0F};
};

class GridGenerationCancelled : public std::runtime_error {
public:
    GridGenerationCancelled() : std::runtime_error("Grid generation was cancelled.") {}
};

struct PointCloudAdaptor {
    const std::vector<glm::vec3>& points;

    std::size_t kdtree_get_point_count() const { return points.size(); }
    float kdtree_get_pt(std::size_t index, std::size_t dimension) const;

    template <class BoundingBox>
    bool kdtree_get_bbox(BoundingBox&) const { return false; }
};

using KDTree3D = nanoflann::KDTreeSingleIndexAdaptor<
    nanoflann::L2_Simple_Adaptor<float, PointCloudAdaptor>,
    PointCloudAdaptor, 3, std::size_t>;

struct SpatialIndex;

class SphericalGridSnapshot {
public:
    // Throws GridGenerationCancelled if the control is cancelled; cancellation is checked
    // between stages, so a Qhull triangulation already in progress runs to completion.
    explicit SphericalGridSnapshot(const GridOptions& options,
                                   GridGenerationControl* control = nullptr);
    SphericalGridSnapshot(const SphericalGridSnapshot& source,
                          std::span<const float> elevations);
    SphericalGridSnapshot(const SphericalGridSnapshot& source,
                          std::span<const float> elevations,
                          std::vector<float> selectionWeights,
                          bool selectionActive);
    SphericalGridSnapshot(const SphericalGridSnapshot&) = delete;
    SphericalGridSnapshot& operator=(const SphericalGridSnapshot&) = delete;
    SphericalGridSnapshot(SphericalGridSnapshot&&) = delete;
    SphericalGridSnapshot& operator=(SphericalGridSnapshot&&) = delete;

    const std::vector<GridVertex>& GetVertices() const { return vertices_; }
    const std::vector<HalfEdge>& GetHalfEdges() const { return halfEdges_; }
    const std::vector<std::uint32_t>& GetTriangleIndices() const { return triangleIndices_; }
    const GridOptions& GetOptions() const { return options_; }
    bool HasSelection() const { return selectionActive_; }

    std::size_t FindNearest(glm::dvec3 direction) const;
    std::vector<std::size_t> FindWithinRadius(
        glm::dvec3 direction, double arcRadius, double planetRadius = 6371.0) const;

    // Returns a new immutable snapshot, or nullptr if the selection contains no eligible faces.
    // Eligible face centers are selected and not much smaller than the selected region's
    // area-weighted median face. A tiny selection can instead seed incident faces.
    // A zero limit processes all eligible faces; otherwise the insertion count is capped.
    std::shared_ptr<const SphericalGridSnapshot> RefineSelectedRegion(
        std::size_t maxNewPoints = 0) const;
    // Removes eligible refinement-created sites, or coarsens flat selected interior sites.
    // A zero limit regenerates the interior at approximately half density when necessary;
    // a nonzero limit caps removals and uses a sparse subset of existing sites instead.
    std::shared_ptr<const SphericalGridSnapshot> CoarsenSelectedRegion(
        float maximumElevationDifference = 0.0F,
        std::size_t maxRemovedVertices = 0) const;

private:
    SphericalGridSnapshot(const GridOptions& options, std::vector<GridVertex> vertices,
                          std::vector<std::uint32_t> triangleIndices,
                          std::vector<std::uint8_t> refinementVertices,
                          bool selectionActive);

    GridOptions options_;
    std::vector<GridVertex> vertices_;
    std::vector<HalfEdge> halfEdges_;
    std::vector<std::uint32_t> triangleIndices_;
    std::vector<std::uint8_t> refinementVertices_;
    bool selectionActive_ = false;
    std::shared_ptr<const SpatialIndex> spatialIndex_;
};

class SphericalGrid {
public:
    explicit SphericalGrid(const GridOptions& options = {});
    void GenerateGrid(const GridOptions& options);
    void InstallSnapshot(std::shared_ptr<const SphericalGridSnapshot> snapshot);
    void SetElevations(std::span<const float> elevations);
    void SetSelectionWeights(std::span<const float> selectionWeights, bool active = true);
    std::shared_ptr<const SphericalGridSnapshot> GetSnapshot() const;
    const std::vector<std::unique_ptr<ElevationLayer>>& GetLayers() const { return layers_; }
    const ElevationLayer& GetElevationLayer() const { return *activeLayer_; }
    ElevationLayer& GetElevationLayer() { return *activeLayer_; }
    bool HasActiveLayer() const { return activeLayer_ != nullptr; }
    const Layer& GetActiveLayer() const;
    float GetActiveCompositedElevation(std::size_t vertexIndex,
                                      std::span<const float> activePreview = {}) const;
    void SetActiveLayer(std::string_view name);
    void DeactivateActiveLayer();
    void SetLayerVisible(std::string_view name, bool visible);
    void RenameLayer(std::string_view oldName, std::string newName);
    ElevationLayer& AddElevationLayer(std::string name, std::string colorMap,
                                     std::string selectionColorMap);
    void RemoveLayer(std::string_view name);
    void MoveLayer(std::string_view name, std::size_t newIndex);

private:
    std::atomic<std::shared_ptr<const SphericalGridSnapshot>> snapshot_;
    std::vector<std::unique_ptr<ElevationLayer>> layers_;
    ElevationLayer* activeLayer_ = nullptr;

    std::vector<float> ValuesForLayer(const ElevationLayer& layer,
                                      const SphericalGridSnapshot& target,
                                      const SphericalGridSnapshot& source) const;
};
