#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "ElevationRaster.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int rasterWidth = 2048;
constexpr int rasterHeight = 1024;
constexpr double pi = 3.14159265358979323846;

float luminance(const unsigned char* pixel) {
    return (0.2126F * pixel[0] + 0.7152F * pixel[1] + 0.0722F * pixel[2]) / 255.0F;
}

float sampleClamped(const std::vector<float>& pixels, int width, int height, double x, double y) {
    x = std::clamp(x, 0.0, static_cast<double>(width - 1));
    y = std::clamp(y, 0.0, static_cast<double>(height - 1));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float tx = static_cast<float>(x - x0);
    const float ty = static_cast<float>(y - y0);
    const auto at = [&](int px, int py) {
        return pixels[static_cast<std::size_t>(py) * width + px];
    };
    return std::lerp(std::lerp(at(x0, y0), at(x1, y0), tx),
                     std::lerp(at(x0, y1), at(x1, y1), tx), ty);
}

} // namespace

ElevationRaster ElevationRaster::Load(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::invalid_argument("Raster path must not be empty.");
    }
    int sourceWidth = 0;
    int sourceHeight = 0;
    int sourceChannels = 0;
    using Image = std::unique_ptr<unsigned char, decltype(&stbi_image_free)>;
    Image image(stbi_load(path.string().c_str(), &sourceWidth, &sourceHeight,
                          &sourceChannels, STBI_rgb), stbi_image_free);
    if (!image) {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error("Cannot load raster '" + path.string() + "': " +
                                 (reason != nullptr ? reason : "unknown image error"));
    }
    if (sourceWidth <= 0 || sourceHeight <= 0 ||
        static_cast<std::size_t>(sourceWidth) >
            std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(sourceHeight)) {
        throw std::runtime_error("Raster dimensions are invalid or too large.");
    }

    const std::size_t sourcePixels = static_cast<std::size_t>(sourceWidth) * sourceHeight;
    std::vector<float> sourceLuminosity(sourcePixels);
    for (std::size_t i = 0; i < sourcePixels; ++i) {
        sourceLuminosity[i] = luminance(image.get() + i * 3);
    }

    ElevationRaster raster;
    raster.width_ = rasterWidth;
    raster.height_ = rasterHeight;
    raster.luminosity_.resize(static_cast<std::size_t>(rasterWidth) * rasterHeight);
    for (int y = 0; y < rasterHeight; ++y) {
        const double sourceY = (static_cast<double>(y) + 0.5) * sourceHeight / rasterHeight - 0.5;
        for (int x = 0; x < rasterWidth; ++x) {
            const double sourceX = (static_cast<double>(x) + 0.5) * sourceWidth / rasterWidth - 0.5;
            raster.luminosity_[static_cast<std::size_t>(y) * rasterWidth + x] =
                sampleClamped(sourceLuminosity, sourceWidth, sourceHeight, sourceX, sourceY);
        }
    }
    return raster;
}

float ElevationRaster::SampleLuminosity(double u, double v) const {
    if (width_ <= 0 || height_ <= 0 || luminosity_.size() !=
        static_cast<std::size_t>(width_) * height_) {
        throw std::logic_error("Raster has not been initialized.");
    }
    if (!std::isfinite(u) || !std::isfinite(v)) {
        throw std::invalid_argument("Raster coordinates must be finite.");
    }

    u -= std::floor(u);
    v = std::clamp(v, 0.0, 1.0);
    const double x = u * width_ - 0.5;
    const double y = v * height_ - 0.5;
    const auto x0Raw = static_cast<long long>(std::floor(x));
    const auto y0Raw = static_cast<long long>(std::floor(y));
    const int x0 = static_cast<int>((x0Raw % width_ + width_) % width_);
    const int x1 = (x0 + 1) % width_;
    const int y0 = static_cast<int>(std::clamp(y0Raw, 0LL, static_cast<long long>(height_ - 1)));
    const int y1 = static_cast<int>(std::clamp(y0Raw + 1, 0LL, static_cast<long long>(height_ - 1)));
    const float tx = static_cast<float>(x - std::floor(x));
    const float ty = static_cast<float>(y - std::floor(y));
    const auto at = [&](int px, int py) {
        return luminosity_[static_cast<std::size_t>(py) * width_ + px];
    };
    return std::clamp(std::lerp(std::lerp(at(x0, y0), at(x1, y0), tx),
                                std::lerp(at(x0, y1), at(x1, y1), tx), ty), 0.0F, 1.0F);
}

std::vector<float> ElevationRaster::SampleElevations(const SphericalGridSnapshot& grid) const {
    std::vector<float> elevations;
    elevations.reserve(grid.GetVertices().size());
    for (const auto& vertex : grid.GetVertices()) {
        const double z = std::clamp(static_cast<double>(vertex.position.z), -1.0, 1.0);
        const double latitude = std::asin(z);
        const double longitude = std::atan2(static_cast<double>(vertex.position.y),
                                            static_cast<double>(vertex.position.x));
        const double u = longitude / (2.0 * pi) + 0.5;
        const double v = 0.5 - latitude / pi;
        const float luminosity = SampleLuminosity(u, v);
        elevations.push_back(-32767.0F + luminosity * 65534.0F);
    }
    return elevations;
}
