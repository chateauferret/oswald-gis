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
    if (!std::isfinite(options.jitterStrength) ||
        options.jitterStrength < 0.0 || options.jitterStrength > 1.0) {
        throw std::invalid_argument("Jitter strength must be finite and between 0 and 1.");
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

    edges.reserve(indices.size());
    std::unordered_map<std::uint64_t, std::int32_t> unmatched;
    unmatched.reserve(indices.size() / 2);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto faceStart = static_cast<std::int32_t>((i / 3) * 3);
        edges.push_back({static_cast<std::int32_t>(indices[i]), -1,
                         faceStart + static_cast<std::int32_t>((i + 1) % 3),
                         static_cast<std::int32_t>(i / 3)});
    }
    for (std::size_t i = 0; i < edges.size(); ++i) {
        auto& edge = edges[i];
        const auto destination = edges[edge.nextEdge].originVertex;
        const auto reverse = unmatched.find(edgeKey(destination, edge.originVertex));
        if (reverse != unmatched.end()) {
            edge.twinEdge = reverse->second;
            edges[reverse->second].twinEdge = static_cast<std::int32_t>(i);
            unmatched.erase(reverse);
        } else if (!unmatched.emplace(edgeKey(edge.originVertex, destination),
                                      static_cast<std::int32_t>(i)).second) {
            throw std::runtime_error("Spherical hull contains duplicate directed edges.");
        }
    }
    if (!unmatched.empty()) {
        throw std::runtime_error("Spherical hull has an open boundary.");
    }
}

} // namespace

float PointCloudAdaptor::kdtree_get_pt(std::size_t index, std::size_t dimension) const {
    return points[index].position[static_cast<glm::vec3::length_type>(dimension)];
}

SphericalGridSnapshot::SphericalGridSnapshot(const GridOptions& options)
    : options_(options), adaptor_{vertices_},
      tree_(3, adaptor_, nanoflann::KDTreeSingleIndexAdaptorParams(
          10, nanoflann::KDTreeSingleIndexAdaptorFlags::SkipInitialBuildIndex)) {
    validateOptions(options);
    vertices_.reserve(options.pointCount);
    std::mt19937 random(options.seed);
    // Explicit conversion avoids implementation-specific uniform_real_distribution mappings.
    const auto sample = [&random]() {
        return 2.0 * (static_cast<double>(random()) / 4294967296.0) - 1.0;
    };
    const double goldenRatio = (1.0 + std::sqrt(5.0)) / 2.0;
    const double jitterScale = options.jitterStrength *
                              std::sqrt(std::numbers::pi / options.pointCount);
    for (std::size_t i = 0; i < options.pointCount; ++i) {
        const double z = 1.0 - (2.0 * i + 1.0) / options.pointCount;
        const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double theta = 2.0 * std::numbers::pi * i / goldenRatio;
        glm::dvec3 position(radius * std::cos(theta), radius * std::sin(theta), z);
        if (options.jitterStrength > 0.0) {
            glm::dvec3 displacement;
            do {
                displacement.x = sample();
                displacement.y = sample();
                displacement.z = sample();
            } while (glm::dot(displacement, displacement) > 1.0);
            position = unitDirection(position + jitterScale * displacement);
        }
        vertices_.push_back({glm::vec3(position), 0.0F});
    }
    triangulate(vertices_, halfEdges_, triangleIndices_);
    tree_.buildIndex();
}

SphericalGridSnapshot::SphericalGridSnapshot(const SphericalGridSnapshot& source,
                                             std::span<const float> elevations)
    : options_(source.options_), vertices_(source.vertices_), halfEdges_(source.halfEdges_),
      triangleIndices_(source.triangleIndices_), selectionActive_(source.selectionActive_),
      adaptor_{vertices_},
      tree_(3, adaptor_, nanoflann::KDTreeSingleIndexAdaptorParams(
          10, nanoflann::KDTreeSingleIndexAdaptorFlags::SkipInitialBuildIndex)) {
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
    tree_.buildIndex();
}

SphericalGridSnapshot::SphericalGridSnapshot(const SphericalGridSnapshot& source,
                                             std::span<const float> elevations,
                                             std::vector<float> selectionWeights,
                                             bool selectionActive)
    : options_(source.options_), vertices_(source.vertices_), halfEdges_(source.halfEdges_),
      triangleIndices_(source.triangleIndices_), selectionActive_(selectionActive),
      adaptor_{vertices_},
      tree_(3, adaptor_, nanoflann::KDTreeSingleIndexAdaptorParams(
          10, nanoflann::KDTreeSingleIndexAdaptorFlags::SkipInitialBuildIndex)) {
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
    tree_.buildIndex();
}

std::size_t SphericalGridSnapshot::FindNearest(glm::dvec3 direction) const {
    const glm::vec3 query(unitDirection(direction));
    std::size_t index = 0;
    float squaredDistance = 0.0F;
    if (tree_.knnSearch(&query.x, 1, &index, &squaredDistance) != 1) {
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
    tree_.radiusSearch(&query.x, static_cast<float>(chord * chord) +
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

SphericalFibonacciGrid::SphericalFibonacciGrid(const GridOptions& options) {
    GenerateGrid(options);
}

void SphericalFibonacciGrid::GenerateGrid(const GridOptions& options) {
    snapshot_.store(std::make_shared<const SphericalGridSnapshot>(options));
}

void SphericalFibonacciGrid::SetElevations(std::span<const float> elevations) {
    const auto current = GetSnapshot();
    snapshot_.store(std::make_shared<const SphericalGridSnapshot>(*current, elevations));
}

void SphericalFibonacciGrid::SetSelectionWeights(std::span<const float> selectionWeights,
                                                 bool active) {
    const auto current = GetSnapshot();
    std::vector<float> elevations;
    elevations.reserve(current->GetVertices().size());
    for (const auto& vertex : current->GetVertices()) {
        elevations.push_back(vertex.elevation);
    }
    snapshot_.store(std::make_shared<const SphericalGridSnapshot>(
        *current, elevations, std::vector<float>(selectionWeights.begin(), selectionWeights.end()),
        active));
}

std::shared_ptr<const SphericalGridSnapshot> SphericalFibonacciGrid::GetSnapshot() const {
    return snapshot_.load();
}
