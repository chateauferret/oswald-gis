#include "SphericalGrid.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Function>
void expectInvalid(Function function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Expected std::invalid_argument.");
}

void verifyTopology(const SphericalGridSnapshot& snapshot) {
    const auto& vertices = snapshot.GetVertices();
    const auto& edges = snapshot.GetHalfEdges();
    const auto& indices = snapshot.GetTriangleIndices();
    check(indices.size() == (2 * vertices.size() - 4) * 3, "Incorrect face count.");
    check(edges.size() == indices.size(), "Incorrect half-edge count.");
    check(vertices.size() + indices.size() / 3 == edges.size() / 2 + 2,
          "Euler characteristic is not 2.");
    std::vector<std::size_t> degree(vertices.size(), 0);
    std::vector<std::int32_t> firstEdge(vertices.size(), -1);
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const auto& edge = edges[i];
        check(edge.originVertex >= 0 &&
              static_cast<std::size_t>(edge.originVertex) < vertices.size(),
              "Invalid edge origin.");
        check(edge.nextEdge >= 0 &&
              static_cast<std::size_t>(edge.nextEdge) < edges.size(),
              "Invalid next edge.");
        check(edge.twinEdge >= 0 &&
              static_cast<std::size_t>(edge.twinEdge) < edges.size(),
              "Invalid twin edge.");
        check(edge.faceIndex == static_cast<std::int32_t>(i / 3), "Invalid face index.");
        check(indices[i] == static_cast<std::uint32_t>(edge.originVertex),
              "Render indices disagree with half-edges.");
        const auto& next = edges[edge.nextEdge];
        const auto& third = edges[next.nextEdge];
        const auto& twin = edges[edge.twinEdge];
        check(next.faceIndex == edge.faceIndex && third.faceIndex == edge.faceIndex &&
              third.nextEdge == static_cast<std::int32_t>(i), "Invalid triangle cycle.");
        check(twin.twinEdge == static_cast<std::int32_t>(i), "Twins are not reciprocal.");
        check(twin.originVertex == next.originVertex &&
              edges[twin.nextEdge].originVertex == edge.originVertex,
              "Twin endpoints are not reversed.");
        ++degree[edge.originVertex];
        firstEdge[edge.originVertex] = static_cast<std::int32_t>(i);
        if (i % 3 == 0) {
            const glm::dvec3 a(vertices[edge.originVertex].position);
            const glm::dvec3 b(vertices[next.originVertex].position);
            const glm::dvec3 c(vertices[third.originVertex].position);
            check(glm::dot(glm::cross(b - a, c - a), a) > 0.0,
                  "Triangle is degenerate or inward-facing.");
        }
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        check(degree[i] >= 3, "Vertex missing from closed topology.");
        auto edgeIndex = firstEdge[i];
        for (std::size_t step = 0; step < degree[i]; ++step) {
            check(edges[edgeIndex].originVertex == static_cast<std::int32_t>(i),
                  "Vertex fan traversal changed origin.");
            edgeIndex = edges[edges[edgeIndex].twinEdge].nextEdge;
            check(step + 1 == degree[i] || edgeIndex != firstEdge[i],
                  "Vertex fan closed before visiting all incident faces.");
        }
        check(edgeIndex == firstEdge[i], "Vertex fan is not closed.");
    }
}

void verifyGeometry(const SphericalGridSnapshot& snapshot) {
    const auto& options = snapshot.GetOptions();
    const auto& vertices = snapshot.GetVertices();
    check(vertices.size() == options.pointCount, "Incorrect vertex count.");
    const double goldenRatio = (1.0 + std::sqrt(5.0)) / 2.0;
    const double scale = options.jitterStrength *
                         std::sqrt(std::numbers::pi / options.pointCount);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const glm::dvec3 actual(vertices[i].position);
        check(std::abs(glm::length(actual) - 1.0) < 1e-7, "Vertex is not on unit sphere.");
        check(vertices[i].elevation == 0.0F, "Initial elevation is not zero.");
        const double z = 1.0 - (2.0 * i + 1.0) / options.pointCount;
        const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double theta = 2.0 * std::numbers::pi * i / goldenRatio;
        const glm::dvec3 expected(radius * std::cos(theta), radius * std::sin(theta), z);
        if (options.jitterStrength == 0.0) {
            check(glm::length(actual - expected) < 1e-7, "Fibonacci formula mismatch.");
        } else {
            const double angle = std::atan2(glm::length(glm::cross(expected, actual)),
                                            glm::dot(expected, actual));
            check(angle <= std::asin(scale) + 1e-7, "Jitter exceeded its specified bound.");
        }
    }
}

void verifyQueries(const SphericalGridSnapshot& snapshot) {
    const auto& vertices = snapshot.GetVertices();
    std::mt19937 random(123);
    std::uniform_real_distribution<double> coordinate(-1.0, 1.0);
    for (int trial = 0; trial < 60; ++trial) {
        const auto x = coordinate(random);
        const auto y = coordinate(random);
        const auto z = coordinate(random);
        const glm::dvec3 center = glm::normalize(glm::dvec3(x, y, z));
        const glm::vec3 query(center);
        float bestDistance = std::numeric_limits<float>::infinity();
        std::size_t bestIndex = 0;
        const double angle = (trial % 10) * std::numbers::pi / 9.0;
        std::vector<std::size_t> expectedRadius;
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const auto delta = vertices[i].position - query;
            const float distance = glm::dot(delta, delta);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = i;
            }
            const auto point = glm::normalize(glm::dvec3(vertices[i].position));
            const double arc = std::atan2(glm::length(glm::cross(center, point)),
                                          glm::dot(center, point));
            if (arc <= angle) {
                expectedRadius.push_back(i);
            }
        }
        check(snapshot.FindNearest(center * 5.0) == bestIndex,
              "Nearest query disagrees with brute force.");
        check(snapshot.FindWithinRadius(center, angle * 6371.0) == expectedRadius,
              "Arc radius query disagrees with brute force.");
        check(snapshot.FindWithinRadius(center, angle * 100.0, 100.0) == expectedRadius,
              "Custom planet radius query disagrees with brute force.");
    }
    for (std::size_t i = 0; i < vertices.size(); i += 37) {
        check(snapshot.FindNearest(glm::dvec3(vertices[i].position)) == i,
              "Exact point nearest query mismatch.");
    }
    check(snapshot.FindWithinRadius({0, 0, 1}, 0.0).empty(), "Zero radius must not select nearby points.");
    check(snapshot.FindWithinRadius({0, 0, 1}, 1e10).size() == vertices.size(),
          "Radius beyond half circumference must select the entire globe.");

    // All points must lie inside every outward hull plane (spherical Delaunay condition).
    const auto& indices = snapshot.GetTriangleIndices();
    for (std::size_t face = 0; face < indices.size(); face += 3) {
        const auto a = glm::normalize(glm::dvec3(vertices[indices[face]].position));
        const auto b = glm::normalize(glm::dvec3(vertices[indices[face + 1]].position));
        const auto c = glm::normalize(glm::dvec3(vertices[indices[face + 2]].position));
        const auto normal = glm::normalize(glm::cross(b - a, c - a));
        for (const auto& vertex : vertices) {
            check(glm::dot(normal, glm::normalize(glm::dvec3(vertex.position)) - a) <= 1e-10,
                  "Triangle violates convex hull / spherical Delaunay condition.");
        }
    }
}

void verifyValidationAndRegeneration() {
    SphericalFibonacciGrid grid({1000, 0.0, 1});
    const auto original = grid.GetSnapshot();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto count : {std::size_t{0}, std::size_t{999}, std::size_t{1'000'001}}) {
        expectInvalid([&] { grid.GenerateGrid({count, 0.0, 1}); });
    }
    for (const auto jitter : {-1.0, 1.1, nan}) {
        expectInvalid([&] { grid.GenerateGrid({1000, jitter, 1}); });
    }
    check(grid.GetSnapshot() == original, "Failed regeneration replaced the valid snapshot.");
    expectInvalid([&] { original->FindNearest({0, 0, 0}); });
    expectInvalid([&] { original->FindNearest({nan, 0, 0}); });
    expectInvalid([&] { original->FindWithinRadius({0, 0, 0}, 1); });
    expectInvalid([&] { original->FindWithinRadius({0, 0, 1}, -1); });
    expectInvalid([&] { original->FindWithinRadius({0, 0, 1}, nan); });
    expectInvalid([&] { original->FindWithinRadius({0, 0, 1}, 1, 0); });
    expectInvalid([&] { original->FindWithinRadius({0, 0, 1}, 1, nan); });
    expectInvalid([&] { original->FindNearest({std::numeric_limits<double>::infinity(), 0, 0}); });
    check(original->FindNearest({1e300, 0, 0}) == original->FindNearest({1, 0, 0}),
          "Large finite query direction was not normalized safely.");
    check(original->FindNearest({1e-300, 0, 0}) == original->FindNearest({1, 0, 0}),
          "Small finite query direction was not normalized safely.");

    SphericalFibonacciGrid oddGrid({1001, 0.0, 42});
    const auto oddSnapshot = oddGrid.GetSnapshot();
    const auto hemisphere = oddSnapshot->FindWithinRadius({0, 0, 1}, std::numbers::pi / 2.0, 1.0);
    check(std::find(hemisphere.begin(), hemisphere.end(), 500) != hemisphere.end(),
          "Inclusive hemisphere boundary excluded the equator.");
    check(oddSnapshot->FindWithinRadius({0, 0, 1}, std::numbers::pi / 2.0 - 1e-12, 1.0).size() == 500,
          "Radius boundary included an outside vertex.");

    std::atomic<bool> stop{false};
    std::atomic<bool> readerFailed{false};
    std::atomic<std::size_t> readCount{0};
    std::jthread reader([&] {
        try {
            while (!stop.load()) {
                const auto snapshot = grid.GetSnapshot();
                const auto index = snapshot->FindNearest({0, 0, 1});
                check(index < snapshot->GetVertices().size(), "Reader saw invalid snapshot.");
                ++readCount;
            }
        } catch (const std::exception&) {
            readerFailed = true;
        }
    });
    for (std::uint32_t seed = 1; seed < 5; ++seed) {
        grid.GenerateGrid({2000, 1.0, seed});
    }
    stop = true;
    reader.join();
    check(!readerFailed && readCount > 0, "Concurrent reader failed.");
    check(original->GetVertices().size() == 1000, "Old snapshot was invalidated.");
    check(original->FindNearest(glm::dvec3(original->GetVertices()[10].position)) == 10,
          "Old snapshot index was invalidated.");

    const auto snapshot = grid.GetSnapshot();
    grid.GenerateGrid(snapshot->GetOptions());
    const auto repeated = grid.GetSnapshot();
    check(snapshot->GetTriangleIndices() == repeated->GetTriangleIndices(),
          "Triangulation is not reproducible.");
    for (std::size_t i = 0; i < snapshot->GetVertices().size(); ++i) {
        check(snapshot->GetVertices()[i].position == repeated->GetVertices()[i].position,
              "Seeded generation is not reproducible.");
    }
    grid.GenerateGrid({2000, 1.0, 99});
    check(grid.GetSnapshot()->GetVertices()[0].position != repeated->GetVertices()[0].position,
          "Changing the seed did not change jitter.");
    grid.GenerateGrid({2000, 0.0, 99});
    check(grid.GetSnapshot()->GetVertices()[0].position != repeated->GetVertices()[0].position,
          "Jitter did not change points.");
    const auto unjittered = grid.GetSnapshot();
    grid.GenerateGrid({2000, 0.0, 1});
    for (std::size_t i = 0; i < 2000; ++i) {
        check(unjittered->GetVertices()[i].position == grid.GetSnapshot()->GetVertices()[i].position,
              "Unjittered generation depends on the seed.");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--scale") {
            const auto start = std::chrono::steady_clock::now();
            SphericalFibonacciGrid grid({1'000'000, 1.0, 42});
            const auto snapshot = grid.GetSnapshot();
            verifyGeometry(*snapshot);
            verifyTopology(*snapshot);
            check(snapshot->FindNearest(glm::dvec3(snapshot->GetVertices()[12345].position)) == 12345,
                  "Million-point nearest query failed.");
            std::cout << "Million-point generation and validation: "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                      << " seconds.\n";
        } else {
            for (const auto jitter : {0.0, 0.15, 1.0}) {
                SphericalFibonacciGrid grid({1000, jitter, 42});
                const auto snapshot = grid.GetSnapshot();
                verifyGeometry(*snapshot);
                verifyTopology(*snapshot);
                verifyQueries(*snapshot);
            }
            verifyValidationAndRegeneration();
        }
        std::cout << "Spherical grid tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Spherical grid test failed: " << error.what() << '\n';
        return 1;
    }
}
