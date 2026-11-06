#include "ColorMap.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

ColorMap::ColorMap(std::vector<glm::vec4> ocean, std::vector<glm::vec4> land)
    : ocean_(std::move(ocean)), land_(std::move(land)) {}

ColorMap ColorMap::Load(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open colour map: " + path.string());
    }
    std::vector<glm::vec4> ocean;
    std::vector<glm::vec4> land;
    std::vector<glm::vec4>* current = nullptr;
    bool oceanSeen = false;
    bool landSeen = false;
    std::size_t lineNumber = 0;
    const auto fail = [&](const std::string& reason) {
        throw std::runtime_error("Invalid colour map " + path.string() + ":" +
                                 std::to_string(lineNumber) + ": " + reason);
    };
    std::string line;
    while (std::getline(input, line)) {
        ++lineNumber;
        line = line.substr(0, line.find('#'));
        std::istringstream fields(line);
        std::string token;
        if (!(fields >> token)) {
            continue;
        }
        if (token == "[ocean]" || token == "[land]") {
            const bool isOcean = token == "[ocean]";
            bool& seen = isOcean ? oceanSeen : landSeen;
            if (seen || (fields >> token)) {
                fail("Sections must appear once, without trailing fields.");
            }
            seen = true;
            current = isOcean ? &ocean : &land;
            continue;
        }
        if (current == nullptr) {
            fail("A stop must follow [ocean] or [land].");
        }
        fields.clear();
        fields.str(line);
        glm::vec4 stop{};
        if (!(fields >> stop.x >> stop.y >> stop.z >> stop.w) || (fields >> token)) {
            fail("Expected exactly four numbers: position red green blue.");
        }
        for (glm::vec4::length_type i = 0; i < 4; ++i) {
            if (!std::isfinite(stop[i]) || stop[i] < 0.0F || stop[i] > 1.0F) {
                fail("Positions and RGB components must be finite and in [0, 1].");
            }
        }
        if (!current->empty() && stop.x <= current->back().x) {
            fail("Stop positions must be strictly increasing.");
        }
        current->push_back(stop);
    }
    if (input.bad()) {
        throw std::runtime_error("Failed to read colour map: " + path.string());
    }
    for (const auto* stops : {&ocean, &land}) {
        if (stops->size() < 2 || stops->front().x != 0.0F || stops->back().x != 1.0F) {
            fail("Both sections need at least two stops with endpoints 0 and 1.");
        }
    }
    return ColorMap(std::move(ocean), std::move(land));
}
