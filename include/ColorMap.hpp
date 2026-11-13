#pragma once

#include <glm/glm.hpp>

#include <filesystem>
#include <span>
#include <vector>

class ColorMap {
public:
    static ColorMap Load(const std::filesystem::path& path);

    // Each stop contains elevation (nominally feet), followed by normalized RGB.
    std::span<const glm::vec4> GetStops() const { return stops_; }
    glm::vec3 Sample(float elevation) const;

private:
    explicit ColorMap(std::vector<glm::vec4> stops);
    std::vector<glm::vec4> stops_;
};
