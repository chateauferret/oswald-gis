#include "SphericalGrid.hpp"

#include <libqhull_r/qhull_ra.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

glm::dvec3 unitDirection(glm::dvec3 direction) {
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z)) {
        throw std::invalid_argument("Query direction must be finite and nonzero.");
    }
    const double scale = std::max({std::abs(direction.x), std::abs(direction.y),
                                   std::abs(direction.z)});
    if (scale == 0.0) {
        throw std::invalid_argument("Query direction must be finite and nonzero.");
    }
    direction /= scale;
    return direction / glm::length(direction);
}

void validateOptions(const GridOptions& options) {
    if (options.pointCount < 1000 || options.pointCount > 1'000'000) {
        throw std::invalid_argument("Point count must be between 1,000 and 1,000,000.");
    }
    if (options.relaxationIterations > 100) {
        throw std::invalid_argument("Relaxation iterations must be between 0 and 100.");
    }
    if (!std::isfinite(options.poissonVariability) ||
        options.poissonVariability < 0.0 || options.poissonVariability > 1.0) {
        throw std::invalid_argument("Poisson variability must be finite and between 0 and 1.");
    }
}

class QhullContext {
public:
    QhullContext() { qh_zero(&state, stderr); }
    ~QhullContext() {
        qh_freeqhull(&state, !qh_ALL);
        int remainingPieces = 0;
        int remainingBytes = 0;
        qh_memfreeshort(&state, &remainingPieces, &remainingBytes);
        if (remainingPieces != 0 || remainingBytes != 0) {
            std::fprintf(stderr, "Qhull cleanup left %d pieces (%d bytes).\n",
                         remainingPieces, remainingBytes);
        }
    }
    QhullContext(const QhullContext&) = delete;
    QhullContext& operator=(const QhullContext&) = delete;

    qhT state{};
};

std::uint64_t edgeKey(std::int32_t origin, std::int32_t destination) {
    return (static_cast<std::uint64_t>(origin) << 32) |
           static_cast<std::uint32_t>(destination);
}

void buildHalfEdges(const std::vector<GridVertex>& vertices,
                    const std::vector<std::uint32_t>& indices,
                    std::vector<HalfEdge>& edges) {
    if (vertices.size() < 4 || vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
        indices.size() != (vertices.size() * 2 - 4) * 3) {
        throw std::runtime_error("Spherical mesh has an invalid vertex or face count.");
    }
    edges.clear();
    edges.reserve(indices.size());
    std::unordered_map<std::uint64_t, std::int32_t> unmatched;
    unmatched.reserve(indices.size() / 2);
    std::vector<bool> used(vertices.size(), false);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto origin = indices[i];
        const auto destination = indices[(i / 3) * 3 + (i + 1) % 3];
        if (origin >= vertices.size() || destination >= vertices.size() || origin == destination) {
            throw std::runtime_error("Spherical mesh contains an invalid edge endpoint.");
        }
        used[origin] = true;
        edges.push_back({static_cast<std::int32_t>(origin), -1,
                         static_cast<std::int32_t>((i / 3) * 3 + (i + 1) % 3),
                         static_cast<std::int32_t>(i / 3)});
    }
    if (std::find(used.begin(), used.end(), false) != used.end()) {
        throw std::runtime_error("Spherical mesh contains an isolated vertex.");
    }
    for (std::size_t i = 0; i < edges.size(); ++i) {
        auto& edge = edges[i];
        const auto destination = edges[edge.nextEdge].originVertex;
        const auto reverse = unmatched.find(edgeKey(destination, edge.originVertex));
        if (reverse != unmatched.end()) {
            edge.twinEdge = reverse->second;
            edges[static_cast<std::size_t>(reverse->second)].twinEdge =
                static_cast<std::int32_t>(i);
            unmatched.erase(reverse);
        } else if (!unmatched.emplace(edgeKey(edge.originVertex, destination),
                                      static_cast<std::int32_t>(i)).second) {
            throw std::runtime_error("Spherical mesh contains duplicate directed edges.");
        }
    }
    if (!unmatched.empty()) {
        throw std::runtime_error("Spherical mesh has an open boundary.");
    }
}

struct RefinedPoint {
    glm::dvec3 position;
    float elevation;
    float selectionWeight;
};

RefinedPoint interpolateFacePoint(const std::vector<GridVertex>& vertices,
                                  const std::uint32_t* face) {
    const glm::dvec3 a = unitDirection(glm::dvec3(vertices[face[0]].position));
    const glm::dvec3 b = unitDirection(glm::dvec3(vertices[face[1]].position));
    const glm::dvec3 c = unitDirection(glm::dvec3(vertices[face[2]].position));
    const glm::dvec3 direction = unitDirection(a + b + c);
    const glm::dvec3 normal = glm::cross(b - a, c - a);
    const double denominator = glm::dot(normal, direction);
    if (std::abs(denominator) < 1e-14) {
        throw std::runtime_error("Cannot interpolate attributes across a degenerate spherical face.");
    }
    const glm::dvec3 point = direction * (glm::dot(normal, a) / denominator);
    const glm::dvec3 v0 = b - a;
    const glm::dvec3 v1 = c - a;
    const glm::dvec3 v2 = point - a;
    const double d00 = glm::dot(v0, v0);
    const double d01 = glm::dot(v0, v1);
    const double d11 = glm::dot(v1, v1);
    const double d20 = glm::dot(v2, v0);
    const double d21 = glm::dot(v2, v1);
    const double barycentricDenominator = d00 * d11 - d01 * d01;
    if (std::abs(barycentricDenominator) < 1e-20) {
        throw std::runtime_error("Cannot interpolate attributes across a degenerate spherical face.");
    }
    const double weightB = (d11 * d20 - d01 * d21) / barycentricDenominator;
    const double weightC = (d00 * d21 - d01 * d20) / barycentricDenominator;
    const double weightA = 1.0 - weightB - weightC;
    const auto value = [&](auto member) {
        return static_cast<float>(weightA * (vertices[face[0]].*member) +
                                  weightB * (vertices[face[1]].*member) +
                                  weightC * (vertices[face[2]].*member));
    };
    return {direction, value(&GridVertex::elevation),
            std::clamp(value(&GridVertex::selectionWeight), 0.0F, 1.0F)};
}

void insertHullPoint(std::vector<GridVertex>& vertices,
                     std::vector<std::uint32_t>& indices,
                     std::vector<HalfEdge>& edges,
                     std::vector<std::uint8_t>& refinementVertices,
                     const RefinedPoint& point) {
    const auto newIndex = static_cast<std::uint32_t>(vertices.size());
    const glm::vec3 storedPosition(point.position);
    vertices.push_back({storedPosition, point.elevation, point.selectionWeight});
    refinementVertices.push_back(1);
    const glm::dvec3 p = unitDirection(glm::dvec3(storedPosition));
    const std::size_t faceCount = indices.size() / 3;
    std::vector<bool> visible(faceCount, false);
    std::size_t visibleCount = 0;
    for (std::size_t face = 0; face < faceCount; ++face) {
        const glm::dvec3 a = unitDirection(glm::dvec3(vertices[indices[3 * face]].position));
        const glm::dvec3 b = unitDirection(glm::dvec3(vertices[indices[3 * face + 1]].position));
        const glm::dvec3 c = unitDirection(glm::dvec3(vertices[indices[3 * face + 2]].position));
        const glm::dvec3 normal = glm::cross(b - a, c - a);
        visible[face] = glm::dot(normal, p - a) > 1e-12 * glm::length(normal);
        visibleCount += visible[face] ? 1U : 0U;
    }
    if (visibleCount == 0 || visibleCount == faceCount) {
        vertices.pop_back();
        refinementVertices.pop_back();
        throw std::runtime_error("Incremental spherical insertion found no valid hull cavity.");
    }

    std::vector<std::uint32_t> nextIndices;
    nextIndices.reserve((vertices.size() * 2 - 4) * 3);
    for (std::size_t face = 0; face < faceCount; ++face) {
        if (!visible[face]) {
            nextIndices.insert(nextIndices.end(), indices.begin() + static_cast<std::ptrdiff_t>(3 * face),
                               indices.begin() + static_cast<std::ptrdiff_t>(3 * face + 3));
        }
    }
    std::size_t horizonCount = 0;
    for (std::size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
        const auto& edge = edges[edgeIndex];
        if (!visible[static_cast<std::size_t>(edge.faceIndex)] ||
            visible[static_cast<std::size_t>(edges[static_cast<std::size_t>(edge.twinEdge)].faceIndex)]) {
            continue;
        }
        const auto destination = edges[static_cast<std::size_t>(edge.nextEdge)].originVertex;
        const auto origin = edge.originVertex;
        const glm::dvec3 a = unitDirection(glm::dvec3(vertices[static_cast<std::size_t>(origin)].position));
        const glm::dvec3 b = unitDirection(glm::dvec3(vertices[static_cast<std::size_t>(destination)].position));
        if (glm::dot(glm::cross(b - a, p - a), a) <= 0.0) {
            vertices.pop_back();
            refinementVertices.pop_back();
            throw std::runtime_error("Incremental insertion produced an inward-facing cavity face.");
        }
        nextIndices.insert(nextIndices.end(), {static_cast<std::uint32_t>(origin),
                               static_cast<std::uint32_t>(destination), newIndex});
        ++horizonCount;
    }
    if (horizonCount < 3) {
        vertices.pop_back();
        refinementVertices.pop_back();
        throw std::runtime_error("Incremental spherical insertion produced an invalid horizon.");
    }
    indices = std::move(nextIndices);
    buildHalfEdges(vertices, indices, edges);
}

void triangulate(const std::vector<GridVertex>& vertices,
                 std::vector<HalfEdge>& edges,
                 std::vector<std::uint32_t>& indices) {
    std::vector<coordT> coordinates;
    coordinates.reserve(vertices.size() * 3);
    for (const auto& vertex : vertices) {
        // Re-normalize float storage in double precision so every point is on the hull.
        const auto position = unitDirection(glm::dvec3(vertex.position));
        coordinates.insert(coordinates.end(), {position.x, position.y, position.z});
    }

    QhullContext context;
    qhT* qh = &context.state;
    char flags[] = "qhull Qt";
    const int status = qh_new_qhull(qh, 3, static_cast<int>(vertices.size()),
                                   coordinates.data(), False, flags, nullptr, stderr);
    if (status != qh_ERRnone) {
        throw std::runtime_error("Spherical triangulation failed; Qhull error " +
                                 std::to_string(status) + " (see stderr).");
    }

    indices.reserve((vertices.size() * 2 - 4) * 3);
    facetT* facet;
    FORALLfacets {
        if (!facet->simplicial || qh_setsize(qh, facet->vertices) != 3 ||
            facet->offset >= 0.0) {
            throw std::runtime_error("Spherical hull must be triangular and enclose the origin.");
        }
        std::array<std::uint32_t, 3> triangle{};
        std::size_t count = 0;
        vertexT* vertex;
        vertexT** vertexp;
        FOREACHvertex_(facet->vertices) {
            const int index = qh_pointid(qh, vertex->point);
            if (index < 0 || static_cast<std::size_t>(index) >= vertices.size()) {
                throw std::runtime_error("Qhull returned an invalid vertex index.");
            }
            triangle[count++] = static_cast<std::uint32_t>(index);
        }
        const glm::dvec3 a(vertices[triangle[0]].position);
        const glm::dvec3 b(vertices[triangle[1]].position);
        const glm::dvec3 c(vertices[triangle[2]].position);
        const double orientation = glm::dot(glm::cross(b - a, c - a), a);
        if (orientation == 0.0) {
            throw std::runtime_error("Spherical triangulation contains a degenerate face.");
        }
        if (orientation < 0.0) {
            std::swap(triangle[1], triangle[2]);
        }
        indices.insert(indices.end(), triangle.begin(), triangle.end());
    }

    if (indices.size() != (vertices.size() * 2 - 4) * 3) {
        throw std::runtime_error("Spherical hull does not include every generated vertex.");
    }

    buildHalfEdges(vertices, indices, edges);
}

// One Lloyd step: move every site to the centroid of its spherical Voronoi cell.
void relaxOnce(std::vector<GridVertex>& vertices, const std::vector<std::uint32_t>& indices,
               const std::vector<bool>* movable = nullptr, double damping = 1.0) {
    std::vector<glm::dvec3> sums(vertices.size(), glm::dvec3(0.0));
    for (std::size_t face = 0; face + 2 < indices.size(); face += 3) {
        const std::array<std::uint32_t, 3> corner{indices[face], indices[face + 1], indices[face + 2]};
        std::array<glm::dvec3, 3> p{};
        for (std::size_t i = 0; i < 3; ++i) {
            p[i] = glm::dvec3(vertices[corner[i]].position);
        }
        glm::dvec3 normal = glm::cross(p[1] - p[0], p[2] - p[0]);
        if (glm::dot(normal, p[0] + p[1] + p[2]) < 0.0) {
            normal = -normal;
        }
        const double length = glm::length(normal);
        if (length == 0.0) {
            continue;
        }
        const glm::dvec3 circumcenter = normal / length;
        // The cell of each site inside this triangle is the quad (site, mid, circumcenter, mid).
        for (std::size_t i = 0; i < 3; ++i) {
            const glm::dvec3& a = p[i];
            const glm::dvec3 m1 = 0.5 * (a + p[(i + 1) % 3]);
            const glm::dvec3 m2 = 0.5 * (a + p[(i + 2) % 3]);
            const auto accumulate = [&](const glm::dvec3& b, const glm::dvec3& c) {
                // Signed area keeps obtuse triangles (circumcenter outside) correct.
                const double area = 0.5 * glm::dot(glm::cross(b - a, c - a), a);
                sums[corner[i]] += area * (a + b + c) / 3.0;
            };
            // Orient the fan consistently regardless of vertex winding.
            const double sign = glm::dot(glm::cross(m1 - a, m2 - a), a) >= 0.0 ? 1.0 : -1.0;
            if (sign > 0.0) {
                accumulate(m1, circumcenter);
                accumulate(circumcenter, m2);
            } else {
                accumulate(circumcenter, m1);
                accumulate(m2, circumcenter);
            }
        }
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if ((movable == nullptr || (*movable)[i]) && glm::dot(sums[i], sums[i]) > 0.0) {
            const auto target = unitDirection(sums[i]);
            const auto current = unitDirection(glm::dvec3(vertices[i].position));
            vertices[i].position = glm::vec3(unitDirection((1.0 - damping) * current +
                                                           damping * target));
        }
    }
}

} // namespace

float PointCloudAdaptor::kdtree_get_pt(std::size_t index, std::size_t dimension) const {
    return points[index][static_cast<glm::vec3::length_type>(dimension)];
}

struct SpatialIndex {
    explicit SpatialIndex(std::span<const GridVertex> vertices)
        : positions(), adaptor{positions},
          tree(3, adaptor, nanoflann::KDTreeSingleIndexAdaptorParams(
              10, nanoflann::KDTreeSingleIndexAdaptorFlags::SkipInitialBuildIndex)) {
        positions.reserve(vertices.size());
        for (const auto& vertex : vertices) {
            positions.push_back(vertex.position);
        }
        tree.buildIndex();
    }

    SpatialIndex(const SpatialIndex&) = delete;
    SpatialIndex& operator=(const SpatialIndex&) = delete;
    SpatialIndex(SpatialIndex&&) = delete;
    SpatialIndex& operator=(SpatialIndex&&) = delete;

    std::vector<glm::vec3> positions;
    PointCloudAdaptor adaptor;
    KDTree3D tree;
};

SphericalGridSnapshot::SphericalGridSnapshot(const GridOptions& options,
                                             GridGenerationControl* control)
    : options_(options) {
    validateOptions(options);
    const float stageCount = static_cast<float>(options.relaxationIterations) + 1.0F;
    const auto checkpoint = [&](float completedStages) {
        if (control == nullptr) {
            return;
        }
        if (control->IsCancelled()) {
            throw GridGenerationCancelled();
        }
        control->SetProgress(std::min(completedStages / stageCount, 1.0F));
    };
    checkpoint(0.0F);
    std::mt19937 random(options.seed);
    // Explicit mapping avoids implementation-specific uniform distribution behavior.
    const auto unit = [&random]() {
        return static_cast<double>(random()) / 4294967296.0;
    };
    vertices_.reserve(options.pointCount);
    const double spacing = (0.78 - 0.16 * options.poissonVariability) *
                           std::sqrt(4.0 * std::numbers::pi / options.pointCount);
    const auto cellCoordinate = [spacing](double value) {
        return static_cast<std::int64_t>(std::floor((value + 1.0) / spacing));
    };
    const auto cellKey = [&](const glm::dvec3& point) {
        const auto x = static_cast<std::uint64_t>(cellCoordinate(point.x));
        const auto y = static_cast<std::uint64_t>(cellCoordinate(point.y));
        const auto z = static_cast<std::uint64_t>(cellCoordinate(point.z));
        return (x << 42) | (y << 21) | z;
    };
    const std::size_t noPoint = std::numeric_limits<std::size_t>::max();
    std::unordered_map<std::uint64_t, std::size_t> buckets;
    buckets.reserve(options.pointCount);
    std::vector<std::size_t> nextInCell;
    nextInCell.reserve(options.pointCount);
    const std::size_t maxAttempts = options.pointCount * 80;
    std::size_t attempts = 0;
    while (vertices_.size() < options.pointCount && attempts < maxAttempts) {
        if (control != nullptr && (attempts & 4095U) == 0U) {
            if (control->IsCancelled()) {
                throw GridGenerationCancelled();
            }
            control->SetProgress(static_cast<float>(vertices_.size()) /
                                 static_cast<float>(options.pointCount) / stageCount);
        }
        ++attempts;
        const double z = 2.0 * unit() - 1.0;
        const double theta = 2.0 * std::numbers::pi * unit();
        const double radial = std::sqrt(std::max(0.0, 1.0 - z * z));
        const glm::dvec3 candidate(radial * std::cos(theta), radial * std::sin(theta), z);
        const auto x = cellCoordinate(candidate.x);
        const auto y = cellCoordinate(candidate.y);
        const auto zCell = cellCoordinate(candidate.z);
        bool tooClose = false;
        for (std::int64_t dx = -1; dx <= 1 && !tooClose; ++dx) {
            for (std::int64_t dy = -1; dy <= 1 && !tooClose; ++dy) {
                for (std::int64_t dz = -1; dz <= 1 && !tooClose; ++dz) {
                    const auto key = (static_cast<std::uint64_t>(x + dx) << 42) |
                                     (static_cast<std::uint64_t>(y + dy) << 21) |
                                     static_cast<std::uint64_t>(zCell + dz);
                    const auto bucket = buckets.find(key);
                    if (bucket == buckets.end()) {
                        continue;
                    }
                    for (auto index = bucket->second; index != noPoint;
                         index = nextInCell[index]) {
                        const glm::dvec3 existing(vertices_[index].position);
                        const auto delta = candidate - existing;
                        if (glm::dot(delta, delta) < spacing * spacing) {
                            tooClose = true;
                            break;
                        }
                    }
                }
            }
        }
        if (tooClose) {
            continue;
        }
        const auto index = vertices_.size();
        vertices_.push_back({glm::vec3(candidate), 0.0F});
        const auto key = cellKey(candidate);
        const auto bucket = buckets.find(key);
        nextInCell.push_back(bucket == buckets.end() ? noPoint : bucket->second);
        if (bucket == buckets.end()) {
            buckets.emplace(key, index);
        } else {
            bucket->second = index;
        }
    }
    if (vertices_.size() != options.pointCount) {
        throw std::runtime_error("Poisson sampling could place only " +
            std::to_string(vertices_.size()) + " of " + std::to_string(options.pointCount) +
            " requested points. Increase Poisson variability or lower the point count.");
    }
    checkpoint(1.0F);
    for (std::uint32_t pass = 0; pass < options.relaxationIterations; ++pass) {
        triangulate(vertices_, halfEdges_, triangleIndices_);
        relaxOnce(vertices_, triangleIndices_);
        halfEdges_.clear();
        triangleIndices_.clear();
        checkpoint(static_cast<float>(pass) + 2.0F);
    }
    triangulate(vertices_, halfEdges_, triangleIndices_);
    refinementVertices_.assign(vertices_.size(), 0);
    spatialIndex_ = std::make_shared<const SpatialIndex>(vertices_);
    checkpoint(stageCount);
}

SphericalGridSnapshot::SphericalGridSnapshot(const SphericalGridSnapshot& source,
                                             std::span<const float> elevations)
    : options_(source.options_), vertices_(source.vertices_), halfEdges_(source.halfEdges_),
    triangleIndices_(source.triangleIndices_),
    refinementVertices_(source.refinementVertices_), selectionActive_(source.selectionActive_),
      spatialIndex_(source.spatialIndex_) {
    if (elevations.size() != vertices_.size()) {
        throw std::invalid_argument("Elevation count must match the grid vertex count.");
    }
    for (std::size_t i = 0; i < elevations.size(); ++i) {
        if (!std::isfinite(elevations[i]) || elevations[i] < -32767.0F ||
            elevations[i] > 32767.0F) {
            throw std::invalid_argument("Elevations must be finite and in the range [-32767, 32767].");
        }
        vertices_[i].elevation = elevations[i];
    }
}

SphericalGridSnapshot::SphericalGridSnapshot(const SphericalGridSnapshot& source,
                                             std::span<const float> elevations,
                                             std::vector<float> selectionWeights,
                                             bool selectionActive)
    : options_(source.options_), vertices_(source.vertices_), halfEdges_(source.halfEdges_),
    triangleIndices_(source.triangleIndices_),
    refinementVertices_(source.refinementVertices_), selectionActive_(selectionActive),
      spatialIndex_(source.spatialIndex_) {
    if (elevations.size() != vertices_.size()) {
        throw std::invalid_argument("Elevation count must match the grid vertex count.");
    }
    if (selectionWeights.size() != vertices_.size()) {
        throw std::invalid_argument("Selection weight count must match the grid vertex count.");
    }
    for (std::size_t i = 0; i < elevations.size(); ++i) {
        if (!std::isfinite(elevations[i]) || elevations[i] < -32767.0F || elevations[i] > 32767.0F) {
            throw std::invalid_argument("Elevations must be finite and in the range [-32767, 32767].");
        }
        if (!std::isfinite(selectionWeights[i]) ||
            selectionWeights[i] < 0.0F || selectionWeights[i] > 1.0F) {
            throw std::invalid_argument("Selection weights must be finite and in the range [0, 1].");
        }
        vertices_[i].elevation = elevations[i];
        vertices_[i].selectionWeight = selectionActive ? selectionWeights[i] : 0.0F;
    }
}

SphericalGridSnapshot::SphericalGridSnapshot(const GridOptions& options,
                                             std::vector<GridVertex> vertices,
                                             std::vector<std::uint32_t> triangleIndices,
                                                                                         std::vector<std::uint8_t> refinementVertices,
                                             bool selectionActive)
    : options_(options), vertices_(std::move(vertices)),
            triangleIndices_(std::move(triangleIndices)),
            refinementVertices_(std::move(refinementVertices)), selectionActive_(selectionActive) {
    buildHalfEdges(vertices_, triangleIndices_, halfEdges_);
    if (refinementVertices_.size() != vertices_.size()) {
        throw std::invalid_argument("Refinement provenance count must match the mesh vertex count.");
    }
    for (const auto& vertex : vertices_) {
        if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) ||
            !std::isfinite(vertex.position.z) || glm::dot(vertex.position, vertex.position) == 0.0F ||
            !std::isfinite(vertex.elevation) || vertex.elevation < -32767.0F ||
            vertex.elevation > 32767.0F || !std::isfinite(vertex.selectionWeight) ||
            vertex.selectionWeight < 0.0F || vertex.selectionWeight > 1.0F) {
            throw std::invalid_argument("Adaptive mesh attributes are outside their supported ranges.");
        }
    }
    spatialIndex_ = std::make_shared<const SpatialIndex>(vertices_);
}

std::shared_ptr<const SphericalGridSnapshot> SphericalGridSnapshot::RefineSelectedRegion(
    std::size_t maxNewPoints) const {
    if (maxNewPoints > 1'000'000) {
        throw std::invalid_argument("Refinement point limit cannot exceed 1,000,000.");
    }
    if (!selectionActive_) {
        return nullptr;
    }
    if (vertices_.size() >= 1'000'000) {
        throw std::runtime_error("The spherical mesh has reached its 1,000,000-vertex limit.");
    }
    struct CandidateFace {
        double area;
        RefinedPoint point;
    };
    std::vector<CandidateFace> selectedFaces;
    std::vector<CandidateFace> incidentFaces;
    selectedFaces.reserve(std::min(triangleIndices_.size() / 3,
                                   (maxNewPoints == 0 ? 4096 : maxNewPoints) * 8));
    for (std::size_t face = 0; face < triangleIndices_.size() / 3; ++face) {
        const auto a = triangleIndices_[3 * face];
        const auto b = triangleIndices_[3 * face + 1];
        const auto c = triangleIndices_[3 * face + 2];
        const float strongestWeight = std::max({vertices_[a].selectionWeight,
                            vertices_[b].selectionWeight,
                            vertices_[c].selectionWeight});
        if (strongestWeight < 0.5F) {
            continue;
        }
        const auto point = interpolateFacePoint(vertices_, triangleIndices_.data() + 3 * face);
        const auto pa = unitDirection(glm::dvec3(vertices_[a].position));
        const auto pb = unitDirection(glm::dvec3(vertices_[b].position));
        const auto pc = unitDirection(glm::dvec3(vertices_[c].position));
        const double area = 2.0 * std::atan2(std::abs(glm::dot(pa, glm::cross(pb, pc))),
            1.0 + glm::dot(pa, pb) + glm::dot(pb, pc) + glm::dot(pc, pa));
        CandidateFace candidate{area, point};
        if (point.selectionWeight >= 0.5F) {
            selectedFaces.push_back(candidate);
        } else {
            incidentFaces.push_back(candidate);
        }
    }
    // A one-vertex selection has no face center above the threshold, but must
    // still be usable as a seed for refinement.
    if (selectedFaces.empty()) {
        selectedFaces = std::move(incidentFaces);
    }
    if (selectedFaces.empty()) {
        return nullptr;
    }

    // Weight by area rather than face count: a dense seam contains many small
    // triangles but represents little of the selected surface. Refining those
    // along with the coarser interior would amplify the seam into a dense ring.
    std::vector<double> areas;
    areas.reserve(selectedFaces.size());
    double totalArea = 0.0;
    for (const auto& face : selectedFaces) {
        areas.push_back(face.area);
        totalArea += face.area;
    }
    std::sort(areas.begin(), areas.end());
    double cumulativeArea = 0.0;
    double medianArea = areas.back();
    for (const double area : areas) {
        cumulativeArea += area;
        if (cumulativeArea >= totalArea * 0.5) {
            medianArea = area;
            break;
        }
    }
    std::erase_if(selectedFaces, [medianArea](const CandidateFace& face) {
        return face.area < medianArea * 0.5;
    });

    const std::size_t insertCount = maxNewPoints == 0
                                        ? selectedFaces.size()
                                        : std::min(maxNewPoints, selectedFaces.size());
    if (insertCount > 1'000'000 - vertices_.size()) {
        throw std::runtime_error("Refinement would exceed the 1,000,000-vertex mesh limit. "
                                 "Select a smaller region or use a point limit.");
    }
    std::vector<RefinedPoint> points;
    points.reserve(insertCount);
    for (std::size_t i = 0; i < insertCount; ++i) {
        const std::size_t selectedIndex = ((2 * i + 1) * selectedFaces.size()) /
                                          (2 * insertCount);
        points.push_back(selectedFaces[selectedIndex].point);
    }

    auto vertices = vertices_;
    auto indices = triangleIndices_;
    auto edges = halfEdges_;
    auto refinementVertices = refinementVertices_;
    if (points.size() <= 32) {
        for (const auto& point : points) {
            insertHullPoint(vertices, indices, edges, refinementVertices, point);
        }
    } else {
        vertices.reserve(vertices.size() + points.size());
        refinementVertices.reserve(refinementVertices.size() + points.size());
        for (const auto& point : points) {
            vertices.push_back({glm::vec3(point.position), point.elevation,
                                point.selectionWeight});
            refinementVertices.push_back(1);
        }
        indices.clear();
        edges.clear();
        triangulate(vertices, edges, indices);
    }
    return std::shared_ptr<const SphericalGridSnapshot>(new SphericalGridSnapshot(
        options_, std::move(vertices), std::move(indices),
        std::move(refinementVertices), selectionActive_));
}

std::shared_ptr<const SphericalGridSnapshot> SphericalGridSnapshot::CoarsenSelectedRegion(
    float maximumElevationDifference, std::size_t maxRemovedVertices) const {
    if (!std::isfinite(maximumElevationDifference) || maximumElevationDifference < 0.0F ||
        maximumElevationDifference > 65534.0F) {
        throw std::invalid_argument("Coarsening elevation tolerance must be finite and in [0, 65534].");
    }
    if (maxRemovedVertices > 1'000'000) {
        throw std::invalid_argument("Coarsening vertex limit cannot exceed 1,000,000.");
    }
    if (!selectionActive_ || vertices_.size() <= 4) {
        return nullptr;
    }
    const std::size_t removalLimit = maxRemovedVertices == 0
                                         ? vertices_.size() - 4
                                         : std::min(maxRemovedVertices, vertices_.size() - 4);
    std::vector<std::int32_t> firstEdges(vertices_.size(), -1);
    for (std::size_t edge = 0; edge < halfEdges_.size(); ++edge) {
        const auto origin = static_cast<std::size_t>(halfEdges_[edge].originVertex);
        if (firstEdges[origin] < 0) {
            firstEdges[origin] = static_cast<std::int32_t>(edge);
        }
    }
    std::vector<std::size_t> refinementCandidates;
    std::vector<std::size_t> generalCandidates;
    std::vector<bool> relaxable(vertices_.size(), false);
    for (std::size_t vertexIndex = 0; vertexIndex < vertices_.size(); ++vertexIndex) {
        float minimum = vertices_[vertexIndex].elevation;
        float maximum = minimum;
        if (firstEdges[vertexIndex] < 0) {
            continue;
        }
        auto edgeIndex = firstEdges[vertexIndex];
        const auto firstIndex = edgeIndex;
        bool neighborSelected = false;
        bool neighborUnselected = false;
        do {
            const auto neighbor = static_cast<std::size_t>(
                halfEdges_[static_cast<std::size_t>(halfEdges_[static_cast<std::size_t>(edgeIndex)].nextEdge)]
                    .originVertex);
            minimum = std::min(minimum, vertices_[neighbor].elevation);
            maximum = std::max(maximum, vertices_[neighbor].elevation);
            neighborSelected |= vertices_[neighbor].selectionWeight >= 0.5F;
            neighborUnselected |= vertices_[neighbor].selectionWeight < 0.5F;
            edgeIndex = halfEdges_[static_cast<std::size_t>(
                halfEdges_[static_cast<std::size_t>(edgeIndex)].twinEdge)].nextEdge;
        } while (edgeIndex != firstIndex);
        if (maximum - minimum > maximumElevationDifference) {
            continue;
        }
        const bool selected = vertices_[vertexIndex].selectionWeight >= 0.5F;
        relaxable[vertexIndex] = selected && !neighborUnselected;
        if (refinementVertices_[vertexIndex] != 0 && (selected || neighborSelected)) {
            refinementCandidates.push_back(vertexIndex);
        } else if (selected && !neighborUnselected) {
            generalCandidates.push_back(vertexIndex);
        }
    }

    std::vector<std::size_t> removable;
    bool regenerateSelectedRegion = false;
    std::size_t regeneratedPointCount = 0;
    if (!refinementCandidates.empty()) {
        const auto count = std::min(refinementCandidates.size(), removalLimit);
        removable.insert(removable.end(), refinementCandidates.begin(),
                         refinementCandidates.begin() + static_cast<std::ptrdiff_t>(count));
    } else if (!generalCandidates.empty()) {
        if (maxRemovedVertices == 0) {
            // Replace the selected interior instead of retaining a biased subset of its
            // original sites. New samples are area-distributed and Poisson-spaced.
            removable = generalCandidates;
            regeneratedPointCount = std::max<std::size_t>(4, generalCandidates.size() / 2);
            regenerateSelectedRegion = true;
        } else {
            const std::size_t target = std::min(removalLimit, maxRemovedVertices);
            const auto priority = [](std::size_t index) {
                std::uint64_t value = static_cast<std::uint64_t>(index) + 0x9e3779b97f4a7c15ULL;
                value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
                value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
                return value ^ (value >> 31);
            };
            std::sort(generalCandidates.begin(), generalCandidates.end(), [&](std::size_t a,
                                                                              std::size_t b) {
                const auto priorityA = priority(a);
                const auto priorityB = priority(b);
                return priorityA == priorityB ? a < b : priorityA < priorityB;
            });
            std::vector<bool> removed(vertices_.size(), false);
            for (const auto vertexIndex : generalCandidates) {
                if (removable.size() >= target) {
                    break;
                }
                auto edgeIndex = firstEdges[vertexIndex];
                const auto firstIndex = edgeIndex;
                std::size_t removedNeighbors = 0;
                std::size_t degree = 0;
                do {
                    const auto neighbor = static_cast<std::size_t>(
                        halfEdges_[static_cast<std::size_t>(
                            halfEdges_[static_cast<std::size_t>(edgeIndex)].nextEdge)].originVertex);
                    removedNeighbors += removed[neighbor] ? 1U : 0U;
                    ++degree;
                    edgeIndex = halfEdges_[static_cast<std::size_t>(
                        halfEdges_[static_cast<std::size_t>(edgeIndex)].twinEdge)].nextEdge;
                } while (edgeIndex != firstIndex);
                const std::size_t localRemovalLimit = std::max<std::size_t>(1, (degree + 2) / 3);
                if (removedNeighbors < localRemovalLimit) {
                    removed[vertexIndex] = true;
                    removable.push_back(vertexIndex);
                }
            }
        }
    }
    if (removable.empty()) {
        return nullptr;
    }

    std::vector<bool> removed(vertices_.size(), false);
    for (const auto index : removable) {
        removed[index] = true;
    }
    std::vector<GridVertex> vertices;
    std::vector<std::uint8_t> refinementVertices;
    std::vector<bool> retainedRelaxable;
    vertices.reserve(vertices_.size() - removable.size());
    refinementVertices.reserve(vertices_.size() - removable.size());
    retainedRelaxable.reserve(vertices_.size() - removable.size());
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        if (!removed[i]) {
            vertices.push_back(vertices_[i]);
            refinementVertices.push_back(refinementVertices_[i]);
            retainedRelaxable.push_back(relaxable[i]);
        }
    }

    if (regenerateSelectedRegion) {
        struct SelectedFace {
            std::array<std::uint32_t, 3> indices;
            double cumulativeArea;
            bool interior;
        };
        std::vector<SelectedFace> selectedFaces;
        double selectedArea = 0.0;
        for (std::size_t face = 0; face < triangleIndices_.size() / 3; ++face) {
            const std::array<std::uint32_t, 3> corners{
                triangleIndices_[3 * face], triangleIndices_[3 * face + 1],
                triangleIndices_[3 * face + 2]};
            // Sample only faces wholly inside the selected region. Sampling the
            // partially selected boundary faces biases replacements toward the seam.
            const bool interior = vertices_[corners[0]].selectionWeight >= 0.5F &&
                                  vertices_[corners[1]].selectionWeight >= 0.5F &&
                                  vertices_[corners[2]].selectionWeight >= 0.5F;
            if (!interior) {
                continue;
            }
            const glm::dvec3 a = unitDirection(glm::dvec3(vertices_[corners[0]].position));
            const glm::dvec3 b = unitDirection(glm::dvec3(vertices_[corners[1]].position));
            const glm::dvec3 c = unitDirection(glm::dvec3(vertices_[corners[2]].position));
            const double area = 2.0 * std::atan2(
                std::abs(glm::dot(a, glm::cross(b, c))),
                1.0 + glm::dot(a, b) + glm::dot(b, c) + glm::dot(c, a));
            if (area > 0.0 && std::isfinite(area)) {
                selectedArea += area;
                selectedFaces.push_back({corners, selectedArea, interior});
            }
        }
        regeneratedPointCount = std::min(regeneratedPointCount,
            1'000'000 - vertices.size());
        if (selectedFaces.empty() || regeneratedPointCount == 0) {
            return nullptr;
        }

        const double spacing = 0.7 * std::sqrt(selectedArea /
                                                static_cast<double>(regeneratedPointCount));
        const double cellWidth = spacing;
        struct SpatialEntry {
            glm::dvec3 position;
            bool fixed;
        };
        const auto cellCoordinate = [cellWidth](double value) {
            return static_cast<std::int64_t>(std::floor((value + 1.0) / cellWidth));
        };
        const auto cellKey = [&](const glm::dvec3& point) {
            const auto x = static_cast<std::uint64_t>(cellCoordinate(point.x));
            const auto y = static_cast<std::uint64_t>(cellCoordinate(point.y));
            const auto z = static_cast<std::uint64_t>(cellCoordinate(point.z));
            return (x << 42) | (y << 21) | z;
        };
        std::unordered_map<std::uint64_t, std::vector<SpatialEntry>> buckets;
        buckets.reserve(vertices.size() + regeneratedPointCount);
        const auto addSpatialPoint = [&](const glm::dvec3& point, bool fixed) {
            buckets[cellKey(point)].push_back({point, fixed});
        };
        for (const auto& vertex : vertices) {
            addSpatialPoint(unitDirection(glm::dvec3(vertex.position)), true);
        }

        std::mt19937 random(options_.seed ^ static_cast<std::uint32_t>(vertices_.size()) ^
                            0xa511e9b3U);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::vector<GridVertex> generated;
        generated.reserve(regeneratedPointCount);
        std::vector<bool> generatedRelaxable;
        generatedRelaxable.reserve(regeneratedPointCount);
        const std::size_t attemptLimit = regeneratedPointCount * 80;
        for (std::size_t attempt = 0; attempt < attemptLimit &&
                                       generated.size() < regeneratedPointCount; ++attempt) {
            const double areaSample = unit(random) * selectedArea;
            const auto faceIt = std::lower_bound(selectedFaces.begin(), selectedFaces.end(), areaSample,
                [](const SelectedFace& face, double value) { return face.cumulativeArea < value; });
            const auto& face = (faceIt == selectedFaces.end() ? selectedFaces.back() : *faceIt);
            const double root = std::sqrt(unit(random));
            const double second = unit(random);
            const double weightA = 1.0 - root;
            const double weightB = root * (1.0 - second);
            const double weightC = root * second;
            const auto& a = vertices_[face.indices[0]];
            const auto& b = vertices_[face.indices[1]];
            const auto& c = vertices_[face.indices[2]];
            const glm::dvec3 direction = unitDirection(
                weightA * glm::dvec3(a.position) + weightB * glm::dvec3(b.position) +
                weightC * glm::dvec3(c.position));
            const float selectionWeight = static_cast<float>(
                weightA * a.selectionWeight + weightB * b.selectionWeight +
                weightC * c.selectionWeight);
            if (selectionWeight < 0.5F) {
                continue;
            }

            const auto x = cellCoordinate(direction.x);
            const auto y = cellCoordinate(direction.y);
            const auto z = cellCoordinate(direction.z);
            bool tooClose = false;
            for (std::int64_t dx = -1; dx <= 1 && !tooClose; ++dx) {
                for (std::int64_t dy = -1; dy <= 1 && !tooClose; ++dy) {
                    for (std::int64_t dz = -1; dz <= 1 && !tooClose; ++dz) {
                        const auto key = (static_cast<std::uint64_t>(x + dx) << 42) |
                                         (static_cast<std::uint64_t>(y + dy) << 21) |
                                         static_cast<std::uint64_t>(z + dz);
                        const auto bucket = buckets.find(key);
                        if (bucket == buckets.end()) {
                            continue;
                        }
                        for (const auto& entry : bucket->second) {
                            // Keep a wider buffer around the preserved seam than the
                            // interior Poisson spacing; otherwise replacements pack against
                            // the fixed ring and create a visible density ridge.
                            const double minimumDistance = entry.fixed ? spacing * 1.35 : spacing;
                            const auto delta = direction - entry.position;
                            if (glm::dot(delta, delta) < minimumDistance * minimumDistance) {
                                tooClose = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (tooClose) {
                continue;
            }
            const float elevation = static_cast<float>(weightA * a.elevation +
                                                        weightB * b.elevation +
                                                        weightC * c.elevation);
            generated.push_back({glm::vec3(direction), elevation, selectionWeight});
            generatedRelaxable.push_back(face.interior);
            addSpatialPoint(direction, false);
        }
        if (generated.size() < regeneratedPointCount / 2) {
            throw std::runtime_error("Could not place enough evenly spaced points in the selected region. "
                                     "Expand the selection or use a coarser target density.");
        }
        for (std::size_t i = 0; i < generated.size(); ++i) {
            const auto& vertex = generated[i];
            vertices.push_back(vertex);
            refinementVertices.push_back(0);
            retainedRelaxable.push_back(generatedRelaxable[i]);
        }
    }

    std::vector<HalfEdge> edges;
    std::vector<std::uint32_t> indices;
    triangulate(vertices, edges, indices);
    if (regenerateSelectedRegion || refinementCandidates.empty()) {
        // A couple of damped Lloyd steps even out the coarsened interior while
        // leaving its selection boundary and all outside vertices anchored.
        for (int pass = 0; pass < 2; ++pass) {
            relaxOnce(vertices, indices, &retainedRelaxable, 0.5);
            edges.clear();
            indices.clear();
            triangulate(vertices, edges, indices);
        }
    }
    return std::shared_ptr<const SphericalGridSnapshot>(new SphericalGridSnapshot(
        options_, std::move(vertices), std::move(indices),
        std::move(refinementVertices), selectionActive_));
}

std::size_t SphericalGridSnapshot::FindNearest(glm::dvec3 direction) const {
    const glm::vec3 query(unitDirection(direction));
    std::size_t index = 0;
    float squaredDistance = 0.0F;
    if (spatialIndex_->tree.knnSearch(&query.x, 1, &index, &squaredDistance) != 1) {
        throw std::runtime_error("Spatial index failed to return a nearest vertex.");
    }
    return index;
}

std::vector<std::size_t> SphericalGridSnapshot::FindWithinRadius(
    glm::dvec3 direction, double arcRadius, double planetRadius) const {
    const auto center = unitDirection(direction);
    if (!std::isfinite(arcRadius) || arcRadius < 0.0 ||
        !std::isfinite(planetRadius) || planetRadius <= 0.0) {
        throw std::invalid_argument("Arc radius must be finite and nonnegative; planet radius must be finite and positive.");
    }
    const double angle = std::min(arcRadius / planetRadius, std::numbers::pi);
    const double chord = 2.0 * std::sin(angle / 2.0);
    const glm::vec3 query(center);
    std::vector<nanoflann::ResultItem<std::size_t, float>> candidates;
    // Pad float k-d tree distances, then enforce the inclusive arc boundary in doubles.
    spatialIndex_->tree.radiusSearch(&query.x, static_cast<float>(chord * chord) +
                       16.0F * std::numeric_limits<float>::epsilon(), candidates);
    std::vector<std::size_t> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const auto point = unitDirection(glm::dvec3(vertices_[candidate.first].position));
        const double distance = std::atan2(glm::length(glm::cross(center, point)),
                                          glm::dot(center, point));
        if (distance <= angle) {
            result.push_back(candidate.first);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

SphericalGrid::SphericalGrid(const GridOptions& options) {
    auto elevationLayer = std::make_unique<ElevationLayer>();
    activeLayer_ = elevationLayer.get();
    layers_.push_back(std::move(elevationLayer));
    GenerateGrid(options);
}

void SphericalGrid::GenerateGrid(const GridOptions& options) {
    auto snapshot = std::make_shared<const SphericalGridSnapshot>(options);
    const std::vector<float> elevations(snapshot->GetVertices().size(), 0.0F);
    for (auto& layer : layers_) {
        layer->SetElevations(elevations);
    }
    snapshot_.store(std::move(snapshot));
}

void SphericalGrid::InstallSnapshot(std::shared_ptr<const SphericalGridSnapshot> snapshot) {
    if (!snapshot) {
        throw std::invalid_argument("Cannot install a null grid snapshot.");
    }
    const auto previous = GetSnapshot();
    for (auto& layer : layers_) {
        if (layer.get() == activeLayer_) {
            std::vector<float> elevations;
            elevations.reserve(snapshot->GetVertices().size());
            for (const auto& vertex : snapshot->GetVertices()) {
                elevations.push_back(vertex.elevation);
            }
            layer->SetElevations(elevations);
        } else {
            layer->SetElevations(ValuesForLayer(*layer, *snapshot, *previous));
        }
    }
    snapshot_.store(std::move(snapshot));
}

void SphericalGrid::SetElevations(std::span<const float> elevations) {
    if (activeLayer_ == nullptr) {
        throw std::logic_error("Cannot edit raster data without an active layer.");
    }
    const auto current = GetSnapshot();
    auto snapshot = std::make_shared<const SphericalGridSnapshot>(*current, elevations);
    activeLayer_->SetElevations(elevations);
    snapshot_.store(std::move(snapshot));
}

void SphericalGrid::SetSelectionWeights(std::span<const float> selectionWeights,
                                                 bool active) {
    const auto current = GetSnapshot();
    const auto currentVertices = current->GetVertices();
    std::vector<float> elevations;
    elevations.reserve(currentVertices.size());
    if (activeLayer_ != nullptr) {
        const auto activeValues = activeLayer_->GetValues();
        elevations.assign(activeValues.begin(), activeValues.end());
    } else {
        for (const auto& vertex : currentVertices) {
            elevations.push_back(vertex.elevation);
        }
    }
    snapshot_.store(std::make_shared<const SphericalGridSnapshot>(
        *current, elevations, std::vector<float>(selectionWeights.begin(), selectionWeights.end()),
        active));
}

std::shared_ptr<const SphericalGridSnapshot> SphericalGrid::GetSnapshot() const {
    return snapshot_.load();
}

const Layer& SphericalGrid::GetActiveLayer() const {
    if (activeLayer_ == nullptr) {
        throw std::logic_error("There is no active layer.");
    }
    return *activeLayer_;
}

float SphericalGrid::GetActiveCompositedElevation(
    std::size_t vertexIndex, std::span<const float> activePreview) const {
    if (activeLayer_ == nullptr) {
        throw std::logic_error("There is no active layer.");
    }
    if (vertexIndex >= activeLayer_->GetValueCount() ||
        (!activePreview.empty() && activePreview.size() != activeLayer_->GetValueCount())) {
        throw std::invalid_argument("Composited elevation query must match the grid vertices.");
    }
    float result = 0.0F;
    bool hasBase = false;
    for (auto layer = layers_.rbegin(); layer != layers_.rend(); ++layer) {
        if (!(*layer)->IsVisible()) {
            continue;
        }
        const float value = layer->get() == activeLayer_ && !activePreview.empty()
            ? activePreview[vertexIndex] : (*layer)->GetValues()[vertexIndex];
        result = BlendElevations(result, value,
            hasBase ? (*layer)->GetBlendMode() : LayerBlendMode::Overlay);
        hasBase = true;
        if (layer->get() == activeLayer_) {
            return result;
        }
    }
    throw std::logic_error("The active layer must be visible.");
}

void SphericalGrid::SetActiveLayer(std::string_view name) {
    const auto found = std::find_if(layers_.begin(), layers_.end(), [&](const auto& layer) {
        return layer->GetName() == name;
    });
    if (found == layers_.end()) {
        throw std::out_of_range("Layer does not exist: " + std::string(name));
    }
    if (activeLayer_ == found->get()) {
        (*found)->SetVisible(true);
        return;
    }
    const auto current = GetSnapshot();
    auto snapshot = std::make_shared<const SphericalGridSnapshot>(
        *current, (*found)->GetValues());
    activeLayer_ = found->get();
    activeLayer_->SetVisible(true);
    snapshot_.store(std::move(snapshot));
}

void SphericalGrid::DeactivateActiveLayer() {
    activeLayer_ = nullptr;
}

void SphericalGrid::SetLayerVisible(std::string_view name, bool visible) {
    const auto found = std::find_if(layers_.begin(), layers_.end(), [&](const auto& layer) {
        return layer->GetName() == name;
    });
    if (found == layers_.end()) {
        throw std::out_of_range("Layer does not exist: " + std::string(name));
    }
    (*found)->SetVisible(visible);
    if (!visible && found->get() == activeLayer_) {
        activeLayer_ = nullptr;
    }
}

void SphericalGrid::RenameLayer(std::string_view oldName, std::string newName) {
    const auto found = std::find_if(layers_.begin(), layers_.end(), [&](const auto& layer) {
        return layer->GetName() == oldName;
    });
    if (found == layers_.end()) {
        throw std::out_of_range("Layer does not exist: " + std::string(oldName));
    }
    if (std::any_of(layers_.begin(), layers_.end(), [&](const auto& layer) {
            return layer.get() != found->get() && layer->GetName() == newName;
        })) {
        throw std::invalid_argument("Layer names must be unique.");
    }
    (*found)->SetName(std::move(newName));
}

ElevationLayer& SphericalGrid::AddElevationLayer(std::string name, std::string colorMap,
                                                  std::string selectionColorMap) {
    if (name.empty()) {
        throw std::invalid_argument("Layer names must not be empty.");
    }
    if (std::any_of(layers_.begin(), layers_.end(), [&](const auto& layer) {
            return layer->GetName() == name;
        })) {
        throw std::invalid_argument("Layer names must be unique.");
    }
    const auto vertexCount = GetSnapshot()->GetVertices().size();
    auto values = std::vector<float>(vertexCount, 0.0F);
    auto layer = std::make_unique<ElevationLayer>(std::move(name), std::move(colorMap),
                                                   std::move(selectionColorMap),
                                                   std::move(values));
    auto* result = layer.get();
    const auto current = GetSnapshot();
    auto snapshot = std::make_shared<const SphericalGridSnapshot>(
        *current, layer->GetValues());
    const auto active = std::find_if(layers_.begin(), layers_.end(), [&](const auto& current) {
        return current.get() == activeLayer_;
    });
    const auto insertionIndex = active == layers_.end()
                                    ? std::size_t{0}
                                    : static_cast<std::size_t>(
                                          std::distance(layers_.begin(), active));
    layers_.reserve(layers_.size() + 1);
    activeLayer_ = result;
    layers_.insert(layers_.begin() + static_cast<std::ptrdiff_t>(insertionIndex),
                   std::move(layer));
    snapshot_.store(std::move(snapshot));
    return *result;
}

void SphericalGrid::RemoveLayer(std::string_view name) {
    const auto found = std::find_if(layers_.begin(), layers_.end(), [&](const auto& layer) {
        return layer->GetName() == name;
    });
    if (found == layers_.end()) {
        throw std::out_of_range("Layer does not exist: " + std::string(name));
    }
    if (layers_.size() == 1) {
        throw std::logic_error("The last data layer cannot be removed.");
    }
    const bool removingActive = found->get() == activeLayer_;
    const auto index = static_cast<std::size_t>(std::distance(layers_.begin(), found));
    ElevationLayer* nextActive = nullptr;
    std::shared_ptr<const SphericalGridSnapshot> snapshot;
    if (removingActive) {
        nextActive = layers_[std::min(index + 1, layers_.size() - 1)].get();
        if (nextActive == found->get()) {
            nextActive = layers_[index == 0 ? 1 : index - 1].get();
        }
        snapshot = std::make_shared<const SphericalGridSnapshot>(
            *GetSnapshot(), nextActive->GetValues());
    }
    layers_.erase(found);
    if (removingActive) {
        activeLayer_ = nextActive;
        snapshot_.store(std::move(snapshot));
    }
}

void SphericalGrid::MoveLayer(std::string_view name, std::size_t newIndex) {
    const auto found = std::find_if(layers_.begin(), layers_.end(), [&](const auto& layer) {
        return layer->GetName() == name;
    });
    if (found == layers_.end()) {
        throw std::out_of_range("Layer does not exist: " + std::string(name));
    }
    if (newIndex >= layers_.size()) {
        throw std::out_of_range("Layer position is out of range.");
    }
    const auto oldIndex = static_cast<std::size_t>(std::distance(layers_.begin(), found));
    if (oldIndex == newIndex) {
        return;
    }
    auto layer = std::move(*found);
    layers_.erase(found);
    layers_.insert(layers_.begin() + static_cast<std::ptrdiff_t>(newIndex),
                   std::move(layer));
}

std::vector<float> SphericalGrid::ValuesForLayer(
    const ElevationLayer& layer, const SphericalGridSnapshot& target,
    const SphericalGridSnapshot& source) const {
    if (layer.GetValueCount() != source.GetVertices().size()) {
        throw std::logic_error("Layer values do not match the source mesh.");
    }
    std::vector<float> values;
    values.reserve(target.GetVertices().size());
    const auto layerValues = layer.GetValues();
    for (const auto& vertex : target.GetVertices()) {
        const auto nearest = source.FindNearest(glm::dvec3(vertex.position));
        values.push_back(layerValues[nearest]);
    }
    return values;
}
