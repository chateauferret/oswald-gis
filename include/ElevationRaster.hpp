#pragma once

#include "SphericalGrid.hpp"

#include <filesystem>
#include <vector>

class ElevationRaster {
public:
    static ElevationRaster Load(const std::filesystem::path& path);

    std::vector<float> SampleElevations(const SphericalGridSnapshot& grid) const;
    float SampleLuminosity(double u, double v) const;

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<float> luminosity_;
};