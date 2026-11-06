#pragma once

#include <glm/glm.hpp>

#include <filesystem>
#include <span>
#include <vector>

class ColorMap {
public:
    static ColorMap Load(const std::filesystem::path& path);

    std::span<const glm::vec4> GetOceanStops() const { return ocean_; }
    std::span<const glm::vec4> GetLandStops() const { return land_; }

private:
    ColorMap(std::vector<glm::vec4> ocean, std::vector<glm::vec4> land);
    std::vector<glm::vec4> ocean_;
    std::vector<glm::vec4> land_;
};
