#pragma once

#include "BrushTool.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <span>
#include <vector>

struct SelectionSettings {
    BrushDistanceUnit distanceUnit = BrushDistanceUnit::Kilometres;
    double radius = 500.0;
    double falloffPercent = 25.0;
};

class RadiusSelectionTool {
public:
    std::vector<std::size_t> Apply(const SphericalGridSnapshot& grid, glm::dvec3 center,
                                   std::vector<float>& selectionWeights,
                                   const SelectionSettings& settings) const;
};

std::vector<float> SelectSphericalPolygon(const SphericalGridSnapshot& grid,
                                          std::span<const glm::dvec3> polygon);

std::vector<float> SelectByValue(const SphericalGridSnapshot& grid,
                                 double lowerBound, double upperBound,
                                 double featherPercent);
