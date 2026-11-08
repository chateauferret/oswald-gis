#include "ColorMap.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

ColorMap::ColorMap(std::vector<glm::vec4> stops) : stops_(std::move(stops)) {}

ColorMap ColorMap::Load(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open colour map: " + path.string());
    }
    std::vector<glm::vec4> stops;
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
        const auto elevation = token;
        std::array<std::string, 3> rgb;
        std::size_t colorFields = 0;
        while (fields >> token) {
            if (colorFields == rgb.size()) {
                fail("Expected elevation followed by RRGGBB or three RGB components.");
            }
            rgb[colorFields++] = token;
        }
        glm::vec4 stop{};
        if (colorFields != 1 && colorFields != 3) {
            fail("Expected elevation followed by RRGGBB or three RGB components.");
        }
        const auto parseFloat = [&](const std::string& value) {
            std::istringstream number(value);
            float result = 0.0F;
            if (!(number >> result) || !number.eof() || !std::isfinite(result)) {
                fail("Expected a finite decimal number.");
            }
            return result;
        };
        stop.x = parseFloat(elevation);
        if (!std::isfinite(stop.x) || stop.x < -32767.0F || stop.x > 32767.0F) {
            fail("Elevations must be finite and in [-32767, +32767].");
        }
        if (colorFields == 1) {
            const auto& value = rgb[0];
            unsigned int color = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), color, 16);
            if (value.size() != 6 ||
                value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
                result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
                fail("Hex colour must contain exactly six digits: RRGGBB.");
            }
            stop.y = static_cast<float>((color >> 16) & 255) / 255.0F;
            stop.z = static_cast<float>((color >> 8) & 255) / 255.0F;
            stop.w = static_cast<float>(color & 255) / 255.0F;
        } else {
            bool floating = false;
            for (const auto& value : rgb) {
                floating |= value.find_first_of(".eE") != std::string::npos;
            }
            for (std::size_t i = 0; i < rgb.size(); ++i) {
                float component = 0.0F;
                if (floating) {
                    component = parseFloat(rgb[i]);
                    if (component < 0.0F || component > 1.0F) {
                        fail("Floating RGB components must be in [0, 1].");
                    }
                } else {
                    const auto& value = rgb[i];
                    unsigned int byte = 0;
                    const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                                        byte, 10);
                    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
                        byte > 255) {
                        fail("Decimal RGB bytes must be integers in [0, 255].");
                    }
                    component = static_cast<float>(byte) / 255.0F;
                }
                stop[static_cast<glm::vec4::length_type>(i + 1)] = component;
            }
        }
        if (!stops.empty() && stop.x <= stops.back().x) {
            fail("Stop elevations must be strictly increasing.");
        }
        stops.push_back(stop);
    }
    if (input.bad()) {
        throw std::runtime_error("Failed to read colour map: " + path.string());
    }
    if (stops.size() < 2 || stops.front().x != -32767.0F || stops.back().x != 32767.0F) {
        fail("The map needs at least two stops with endpoints -32767 and +32767.");
    }
    return ColorMap(std::move(stops));
}
