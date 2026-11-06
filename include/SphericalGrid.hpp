#pragma once

#include <glm/glm.hpp>
#include <nanoflann.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct GridVertex {
    glm::vec3 position;
    float elevation = 0.0F;
};

struct HalfEdge {
    std::int32_t originVertex;
    std::int32_t twinEdge;
    std::int32_t nextEdge;
    std::int32_t faceIndex;
};

struct GridOptions {
    std::size_t pointCount = 10'000;
    double jitterStrength = 0.15;
    std::uint32_t seed = 42;
};

struct PointCloudAdaptor {
    const std::vector<GridVertex>& points;

    std::size_t kdtree_get_point_count() const { return points.size(); }
    float kdtree_get_pt(std::size_t index, std::size_t dimension) const;

    template <class BoundingBox>
    bool kdtree_get_bbox(BoundingBox&) const { return false; }
};

using KDTree3D = nanoflann::KDTreeSingleIndexAdaptor<
    nanoflann::L2_Simple_Adaptor<float, PointCloudAdaptor>,
    PointCloudAdaptor, 3, std::size_t>;

class SphericalGridSnapshot {
public:
    explicit SphericalGridSnapshot(const GridOptions& options);
    SphericalGridSnapshot(const SphericalGridSnapshot&) = delete;
    SphericalGridSnapshot& operator=(const SphericalGridSnapshot&) = delete;
    SphericalGridSnapshot(SphericalGridSnapshot&&) = delete;
    SphericalGridSnapshot& operator=(SphericalGridSnapshot&&) = delete;

    const std::vector<GridVertex>& GetVertices() const { return vertices_; }
    const std::vector<HalfEdge>& GetHalfEdges() const { return halfEdges_; }
    const std::vector<std::uint32_t>& GetTriangleIndices() const { return triangleIndices_; }
    const GridOptions& GetOptions() const { return options_; }

    std::size_t FindNearest(glm::dvec3 direction) const;
    std::vector<std::size_t> FindWithinRadius(
        glm::dvec3 direction, double arcRadius, double planetRadius = 6371.0) const;

private:
    GridOptions options_;
    std::vector<GridVertex> vertices_;
    std::vector<HalfEdge> halfEdges_;
    std::vector<std::uint32_t> triangleIndices_;
    PointCloudAdaptor adaptor_;
    KDTree3D tree_;
};

class SphericalFibonacciGrid {
public:
    explicit SphericalFibonacciGrid(const GridOptions& options = {});
    void GenerateGrid(const GridOptions& options);
    std::shared_ptr<const SphericalGridSnapshot> GetSnapshot() const;

private:
    std::atomic<std::shared_ptr<const SphericalGridSnapshot>> snapshot_;
};
