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
    std::array<std::size_t, 8> latitudeBands{};
    for (const auto& vertex : vertices) {
        const glm::dvec3 actual(vertex.position);
        check(std::abs(glm::length(actual) - 1.0) < 1e-6, "Vertex is not on unit sphere.");
        check(vertex.elevation == 0.0F, "Initial elevation is not zero.");
        const auto band = std::min<std::size_t>(7, static_cast<std::size_t>(
            (1.0 - (actual.z + 1.0) * 0.5) * latitudeBands.size()));
        ++latitudeBands[band];
    }
    const double expectedPerBand = static_cast<double>(options.pointCount) /
                                   static_cast<double>(latitudeBands.size());
    const double tolerance = options.pointCount >= 100'000 ? 0.04 : 0.35;
    for (const auto count : latitudeBands) {
        check(std::abs(static_cast<double>(count) - expectedPerBand) <=
                  expectedPerBand * tolerance,
              "Poisson grid is not approximately uniform by spherical area.");
    }

    if (vertices.size() <= 2000) {
        const double minimumSpacing = (0.78 - 0.16 * options.poissonVariability) *
                                      std::sqrt(4.0 * std::numbers::pi / options.pointCount);
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            for (std::size_t j = i + 1; j < vertices.size(); ++j) {
                const glm::dvec3 delta = glm::dvec3(vertices[i].position) -
                                         glm::dvec3(vertices[j].position);
                check(glm::dot(delta, delta) + 1e-10 >= minimumSpacing * minimumSpacing,
                      "Poisson grid contains points inside its minimum spacing.");
            }
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
    SphericalGrid grid({1000, 0.0, 1});
    const auto original = grid.GetSnapshot();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto count : {std::size_t{0}, std::size_t{999}, std::size_t{1'000'001}}) {
        expectInvalid([&] { grid.GenerateGrid({count, 0.0, 1}); });
    }
    for (const auto variability : {-1.0, 1.1, nan}) {
        expectInvalid([&] { grid.GenerateGrid({1000, variability, 1}); });
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

    SphericalGrid oddGrid({1001, 0.0, 42});
    const auto oddSnapshot = oddGrid.GetSnapshot();
        const auto& oddVertices = oddSnapshot->GetVertices();
        const glm::dvec3 boundaryCenter = glm::normalize(glm::dvec3(oddVertices[0].position));
        const glm::dvec3 boundaryPoint = glm::normalize(glm::dvec3(oddVertices[1].position));
        const double boundaryAngle = std::atan2(glm::length(glm::cross(boundaryCenter, boundaryPoint)),
                                  glm::dot(boundaryCenter, boundaryPoint));
        const auto boundaryHit = oddSnapshot->FindWithinRadius(boundaryCenter, boundaryAngle, 1.0);
        check(std::find(boundaryHit.begin(), boundaryHit.end(), 1) != boundaryHit.end(),
            "Inclusive spherical radius excluded a point exactly on its boundary.");
        const auto boundaryMiss = oddSnapshot->FindWithinRadius(boundaryCenter,
                                             boundaryAngle - 1e-10, 1.0);
        check(std::find(boundaryMiss.begin(), boundaryMiss.end(), 1) == boundaryMiss.end(),
            "Spherical radius included a point outside its boundary.");

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
            "Changing the seed did not change the Poisson sample.");
    grid.GenerateGrid({2000, 0.0, 99});
    check(grid.GetSnapshot()->GetVertices()[0].position != repeated->GetVertices()[0].position,
            "Changing the seed did not change the Poisson sample.");
}

void verifyAdaptiveRefinementAndCoarsening() {
    SphericalGrid grid({1000, 0.0, 314});
    const auto original = grid.GetSnapshot();
    std::vector<float> elevations(original->GetVertices().size(), 1234.0F);
    grid.SetElevations(elevations);
    std::vector<float> selection(original->GetVertices().size(), 1.0F);
    grid.SetSelectionWeights(selection, true);
    const auto selected = grid.GetSnapshot();

    const auto refined = selected->RefineSelectedRegion(12);
    check(refined != nullptr && refined->GetVertices().size() == selected->GetVertices().size() + 12,
        "Selected-region refinement did not insert the requested bounded point count.");
    verifyTopology(*refined);
    const auto verifyHull = [](const SphericalGridSnapshot& snapshot) {
        const auto& meshVertices = snapshot.GetVertices();
        const auto& meshIndices = snapshot.GetTriangleIndices();
        for (std::size_t face = 0; face < meshIndices.size(); face += 3) {
            const auto a = glm::normalize(glm::dvec3(meshVertices[meshIndices[face]].position));
            const auto b = glm::normalize(glm::dvec3(meshVertices[meshIndices[face + 1]].position));
            const auto c = glm::normalize(glm::dvec3(meshVertices[meshIndices[face + 2]].position));
            const auto normal = glm::normalize(glm::cross(b - a, c - a));
            for (const auto& vertex : meshVertices) {
                check(glm::dot(normal, glm::normalize(glm::dvec3(vertex.position)) - a) <= 1e-9,
                      "Adaptive mesh contains a face that is not on its convex spherical hull.");
            }
        }
    };
    verifyHull(*refined);
    const auto fullyRefined = selected->RefineSelectedRegion();
    check(fullyRefined != nullptr &&
          fullyRefined->GetVertices().size() > selected->GetVertices().size() * 2 &&
          fullyRefined->GetVertices().size() <= selected->GetVertices().size() +
              selected->GetTriangleIndices().size() / 3,
          "Default refinement did not substantially increase the selected density.");
    verifyTopology(*fullyRefined);
    verifyHull(*fullyRefined);
    const auto conservativeCoarsening = selected->CoarsenSelectedRegion();
    check(conservativeCoarsening != nullptr &&
          conservativeCoarsening->GetVertices().size() >= 450 &&
          conservativeCoarsening->GetVertices().size() <= 550,
          "Coarsening an unrefined uniform grid did not regenerate at approximately half density.");
    verifyTopology(*conservativeCoarsening);
    verifyHull(*conservativeCoarsening);
    std::array<std::size_t, 8> pointsPerLatitudeBand{};
    for (const auto& vertex : conservativeCoarsening->GetVertices()) {
        const auto band = std::min<std::size_t>(7, static_cast<std::size_t>(
            (1.0 - (static_cast<double>(vertex.position.z) + 1.0) * 0.5) * 8.0));
        ++pointsPerLatitudeBand[band];
    }
    check(std::all_of(pointsPerLatitudeBand.begin(), pointsPerLatitudeBand.end(),
                      [](std::size_t count) { return count > 20; }),
          "Regenerated coarsening left whole latitude bands undersampled.");

        std::vector<float> regionalSelection(selected->GetVertices().size(), 0.0F);
        std::vector<float> taggedElevations(selected->GetVertices().size());
        const auto& originalVertices = selected->GetVertices();
        for (std::size_t i = 0; i < originalVertices.size(); ++i) {
            regionalSelection[i] = originalVertices[i].position.z > 0.55F ? 1.0F : 0.0F;
            taggedElevations[i] = static_cast<float>(i) / 10000.0F;
        }
        grid.SetElevations(taggedElevations);
        grid.SetSelectionWeights(regionalSelection, true);
        const auto regionalBefore = grid.GetSnapshot();
        const auto regionalAfter = regionalBefore->CoarsenSelectedRegion(1.0F);
        check(regionalAfter != nullptr, "Regional coarsening did not change a selected flat region.");
        verifyTopology(*regionalAfter);
        bool generatedSelectedPoints = false;
        std::vector<glm::dvec3> fixedBoundary;
        for (std::size_t i = 0; i < regionalBefore->GetVertices().size(); ++i) {
            const auto& vertex = regionalBefore->GetVertices()[i];
            bool boundary = false;
            for (const auto& edge : regionalBefore->GetHalfEdges()) {
                if (edge.originVertex != static_cast<std::int32_t>(i)) {
                    continue;
                }
                const auto neighbor = static_cast<std::size_t>(
                    regionalBefore->GetHalfEdges()[static_cast<std::size_t>(edge.nextEdge)].originVertex);
                boundary |= regionalBefore->GetVertices()[neighbor].selectionWeight < 0.5F;
            }
            if (vertex.selectionWeight < 0.5F || boundary) {
                fixedBoundary.emplace_back(glm::normalize(glm::dvec3(vertex.position)));
            }
        }
        for (const auto& afterVertex : regionalAfter->GetVertices()) {
            const auto original = std::find_if(regionalBefore->GetVertices().begin(),
                                               regionalBefore->GetVertices().end(),
                [&](const GridVertex& vertex) { return vertex.position == afterVertex.position; });
            if (original == regionalBefore->GetVertices().end()) {
                check(afterVertex.selectionWeight == 1.0F,
                      "Regeneration inserted a point on or outside the selected-region seam.");
                const auto point = glm::normalize(glm::dvec3(afterVertex.position));
                for (const auto& fixed : fixedBoundary) {
                    const double distance = std::atan2(glm::length(glm::cross(point, fixed)),
                                                      glm::dot(point, fixed));
                    check(distance > 0.04,
                          "Regenerated point is packed too tightly against the fixed selection boundary.");
                }
                generatedSelectedPoints = true;
            }
        }
        for (std::size_t i = 0; i < regionalBefore->GetVertices().size(); ++i) {
            bool boundary = false;
            for (const auto& edge : regionalBefore->GetHalfEdges()) {
                if (edge.originVertex != static_cast<std::int32_t>(i)) {
                    continue;
                }
                const auto neighbor = static_cast<std::size_t>(
                    regionalBefore->GetHalfEdges()[static_cast<std::size_t>(edge.nextEdge)].originVertex);
                boundary |= regionalBefore->GetVertices()[neighbor].selectionWeight < 0.5F;
            }
            if (regionalBefore->GetVertices()[i].selectionWeight < 0.5F || boundary) {
                check(std::any_of(regionalAfter->GetVertices().begin(), regionalAfter->GetVertices().end(),
                    [&](const GridVertex& vertex) { return vertex.position == regionalBefore->GetVertices()[i].position; }),
                    "Selected-region regeneration moved or removed its fixed exterior/boundary ring.");
            }
        }
        check(generatedSelectedPoints, "Selected-region coarsening did not generate replacement samples.");
    check(refined->HasSelection(), "Refinement discarded the active selection state.");
    for (std::size_t i = 0; i < selected->GetVertices().size(); ++i) {
      check(refined->GetVertices()[i].position == selected->GetVertices()[i].position &&
          refined->GetVertices()[i].elevation == 1234.0F &&
          refined->GetVertices()[i].selectionWeight == 1.0F,
          "Refinement changed an existing vertex or its attributes.");
    }
    for (std::size_t i = selected->GetVertices().size(); i < refined->GetVertices().size(); ++i) {
      check(std::abs(glm::length(glm::dvec3(refined->GetVertices()[i].position)) - 1.0) < 1e-7 &&
          refined->GetVertices()[i].elevation == 1234.0F &&
          refined->GetVertices()[i].selectionWeight == 1.0F,
          "Newly refined vertices were not projected or attribute-interpolated correctly.");
    }
    check(selected->GetVertices().size() == 1000,
        "Refinement mutated the original immutable snapshot.");

        const auto limitedCoarsening = refined->CoarsenSelectedRegion(0.0F, 8);
        check(limitedCoarsening != nullptr &&
            limitedCoarsening->GetVertices().size() == refined->GetVertices().size() - 8,
            "Uniform selected-region coarsening did not honor its explicit removal limit.");
        verifyTopology(*limitedCoarsening);
        const auto coarsened = refined->CoarsenSelectedRegion();
        check(coarsened != nullptr && coarsened->GetVertices().size() == selected->GetVertices().size(),
            "Coarsening did not remove refinement-created vertices and restore the prior grid density.");
    verifyTopology(*coarsened);
    verifyHull(*coarsened);
    check(coarsened->HasSelection(), "Coarsening discarded the active selection state.");
        for (std::size_t i = 0; i < coarsened->GetVertices().size(); ++i) {
          check(coarsened->GetVertices()[i].position == selected->GetVertices()[i].position &&
              coarsened->GetVertices()[i].elevation == selected->GetVertices()[i].elevation &&
              coarsened->GetVertices()[i].selectionWeight == selected->GetVertices()[i].selectionWeight,
              "Refine-then-coarsen did not preserve the original sites and attributes.");
    }
    check(refined->GetVertices().size() == 1012,
        "Coarsening mutated the prior immutable snapshot.");

    std::vector<float> noSelection(selected->GetVertices().size(), 0.0F);
    grid.SetSelectionWeights(noSelection, false);
    const auto unselected = grid.GetSnapshot();
    check(!unselected->RefineSelectedRegion() && !unselected->CoarsenSelectedRegion(),
        "Adaptive mesh edits ran without an active selection.");
        std::vector<float> sparseSelection(unselected->GetVertices().size(), 0.0F);
        sparseSelection[0] = 1.0F;
        grid.SetSelectionWeights(sparseSelection, true);
        const auto sparseRefinement = grid.GetSnapshot()->RefineSelectedRegion(1);
        check(sparseRefinement != nullptr &&
                    sparseRefinement->GetVertices().size() == unselected->GetVertices().size() + 1,
                    "A localized single-vertex selection did not seed regional refinement.");
    expectInvalid([&] { selected->RefineSelectedRegion(1'000'001); });
    expectInvalid([&] { selected->CoarsenSelectedRegion(-1.0F); });
}

void verifyCoarsenThenRefineDensity() {
    SphericalGrid grid({4000, 0.0, 314});
    std::vector<float> selection;
    for (const auto& vertex : grid.GetSnapshot()->GetVertices()) {
        selection.push_back(vertex.position.z > 0.2F ? 1.0F : 0.0F);
    }
    grid.SetSelectionWeights(selection, true);
    const auto coarse = grid.GetSnapshot()->CoarsenSelectedRegion();
    check(coarse != nullptr, "Regional coarsening failed.");
    const auto refined = coarse->RefineSelectedRegion();
    check(refined != nullptr, "Refinement after regional coarsening failed.");
    verifyTopology(*coarse);
    verifyTopology(*refined);

    std::size_t borderCount = 0;
    std::size_t interiorCount = 0;
    for (const auto& vertex : refined->GetVertices()) {
        borderCount += vertex.position.z >= 0.2F && vertex.position.z < 0.27F;
        interiorCount += vertex.position.z >= 0.6F;
    }
    // Both bands have known spherical area, proportional to their width in z.
    if (static_cast<double>(borderCount) * (0.4 / 0.07) > static_cast<double>(interiorCount) * 1.3) {
        throw std::runtime_error("Refinement produced a dense border: " +
                                 std::to_string(borderCount) + " border vs " +
                                 std::to_string(interiorCount) + " interior points.");
    }
    std::size_t borderInserted = 0;
    std::size_t interiorInserted = 0;
    for (std::size_t i = coarse->GetVertices().size(); i < refined->GetVertices().size(); ++i) {
        const auto z = refined->GetVertices()[i].position.z;
        borderInserted += z >= 0.2F && z < 0.27F;
        interiorInserted += z >= 0.6F;
    }
    if (static_cast<double>(borderInserted) * (0.4 / 0.07) > static_cast<double>(interiorInserted) * 1.3) {
        throw std::runtime_error("Refinement concentrated insertions at the border: " +
                                 std::to_string(borderInserted) + " border vs " +
                                 std::to_string(interiorInserted) + " interior points.");
    }
    for (std::size_t i = coarse->GetVertices().size(); i < refined->GetVertices().size(); ++i) {
        check(refined->GetVertices()[i].selectionWeight >= 0.5F,
              "Refinement inserted points outside the selected region.");
    }
    const auto restored = refined->CoarsenSelectedRegion();
    check(restored != nullptr && restored->GetVertices().size() == coarse->GetVertices().size(),
          "Coarsening did not reverse refinement of the coarsened region.");
    verifyTopology(*restored);
    for (std::size_t i = 0; i < coarse->GetVertices().size(); ++i) {
        check(restored->GetVertices()[i].position == coarse->GetVertices()[i].position &&
              restored->GetVertices()[i].elevation == coarse->GetVertices()[i].elevation &&
              restored->GetVertices()[i].selectionWeight == coarse->GetVertices()[i].selectionWeight,
              "Refinement and coarsening changed an existing coarse site.");
    }
}

// Coefficient of variation of all edge lengths; lower means more uniform cells.
double edgeLengthSpread(const SphericalGridSnapshot& snapshot) {
    const auto& vertices = snapshot.GetVertices();
    const auto& edges = snapshot.GetHalfEdges();
    std::vector<double> lengths;
    lengths.reserve(edges.size());
    for (const auto& edge : edges) {
        const auto& from = vertices[static_cast<std::size_t>(edge.originVertex)];
        const auto& to = vertices[static_cast<std::size_t>(edges[edge.nextEdge].originVertex)];
        lengths.push_back(glm::length(glm::dvec3(from.position) - glm::dvec3(to.position)));
    }
    double mean = 0.0;
    for (const auto value : lengths) {
        mean += value;
    }
    mean /= lengths.size();
    double variance = 0.0;
    for (const auto value : lengths) {
        variance += (value - mean) * (value - mean);
    }
    return std::sqrt(variance / lengths.size()) / mean;
}

void verifyRelaxation() {
    GridOptions plain{2000, 0.0, 7};
    GridOptions relaxed = plain;
    relaxed.relaxationIterations = 5;
    SphericalGrid first(plain);
    SphericalGrid second(relaxed);
    SphericalGrid repeat(relaxed);
    verifyTopology(*second.GetSnapshot());
    for (const auto& vertex : second.GetSnapshot()->GetVertices()) {
        check(std::abs(glm::length(glm::dvec3(vertex.position)) - 1.0) < 1e-6,
              "Relaxed vertex is not on the unit sphere.");
    }
    check(edgeLengthSpread(*second.GetSnapshot()) < edgeLengthSpread(*first.GetSnapshot()),
          "Lloyd relaxation did not make the point spacing more uniform.");
    for (std::size_t i = 0; i < 2000; ++i) {
        check(second.GetSnapshot()->GetVertices()[i].position ==
                  repeat.GetSnapshot()->GetVertices()[i].position,
              "Relaxed generation is not reproducible.");
    }
    relaxed.relaxationIterations = 101;
    expectInvalid([&] { SphericalGrid invalid(relaxed); });
    relaxed.relaxationIterations = 3;
    relaxed.poissonVariability = 0.5;
    SphericalGrid variableSpacing(relaxed);
    verifyTopology(*variableSpacing.GetSnapshot());
}

void verifyCancellation() {
    GridGenerationControl progress;
    GridOptions options{2000, 0.1, 3};
    options.relaxationIterations = 2;
    const SphericalGridSnapshot completed(options, &progress);
    check(progress.GetProgress() == 1.0F, "Completed generation did not report full progress.");
    check(completed.GetVertices().size() == 2000, "Generation with a control produced a wrong grid.");

    GridGenerationControl cancelled;
    cancelled.Cancel();
    bool threw = false;
    try {
        const SphericalGridSnapshot snapshot(options, &cancelled);
    } catch (const GridGenerationCancelled&) {
        threw = true;
    }
    check(threw, "Cancelled generation did not throw.");

    SphericalGrid grid({1000, 0.0, 1});
    const auto before = grid.GetSnapshot();
    check(grid.GetSnapshot() == before, "Snapshot changed unexpectedly.");
    auto replacement = std::make_shared<const SphericalGridSnapshot>(options);
    grid.InstallSnapshot(replacement);
    check(grid.GetSnapshot() == replacement, "Installed snapshot was not published.");
    expectInvalid([&] { grid.InstallSnapshot(nullptr); });
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--scale") {
            const auto start = std::chrono::steady_clock::now();
            SphericalGrid grid({1'000'000, 0.5, 42});
            const auto snapshot = grid.GetSnapshot();
            verifyGeometry(*snapshot);
            verifyTopology(*snapshot);
            check(snapshot->FindNearest(glm::dvec3(snapshot->GetVertices()[12345].position)) == 12345,
                  "Million-point nearest query failed.");
            std::cout << "Million-point generation and validation: "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                      << " seconds.\n";
        } else {
            for (const auto variability : {0.0, 0.15, 1.0}) {
                SphericalGrid grid({1000, variability, 42});
                const auto snapshot = grid.GetSnapshot();
                verifyGeometry(*snapshot);
                verifyTopology(*snapshot);
                verifyQueries(*snapshot);
            }
            verifyValidationAndRegeneration();
            verifyAdaptiveRefinementAndCoarsening();
            verifyCoarsenThenRefineDensity();
            verifyRelaxation();
            verifyCancellation();
        }
        std::cout << "Spherical grid tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Spherical grid test failed: " << error.what() << '\n';
        return 1;
    }
}
