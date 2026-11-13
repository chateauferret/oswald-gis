#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "ElevationRaster.hpp"
#include "AtomicFile.hpp"
#include "ColorMap.hpp"
#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <locale>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int rasterWidth = 2048;
constexpr int rasterHeight = 1024;
constexpr double pi = 3.14159265358979323846;

bool isTiff(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension == ".tif" || extension == ".tiff";
}

using Tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

void registerGeoTiffFields(TIFF* image) {
    static char scaleName[] = "ModelPixelScaleTag";
    static char tiepointName[] = "ModelTiepointTag";
    static char keysName[] = "GeoKeyDirectoryTag";
    const TIFFFieldInfo fields[]{
        {33550, 3, 3, TIFF_DOUBLE, FIELD_CUSTOM, 1, 0, scaleName},
        {33922, 6, 6, TIFF_DOUBLE, FIELD_CUSTOM, 1, 0, tiepointName},
        {34735, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SHORT, FIELD_CUSTOM, 1, 1, keysName}};
    if (TIFFMergeFieldInfo(image, fields, 3) != 0) {
        throw std::runtime_error("Cannot register GeoTIFF fields.");
    }
}

float luminance(const unsigned char* pixel) {
    return (0.2126F * pixel[0] + 0.7152F * pixel[1] + 0.0722F * pixel[2]) / 255.0F;
}

std::vector<float> loadTiff(const std::filesystem::path& path, int& width, int& height,
                          bool& elevationSamples) {
    Tiff image(TIFFOpen(path.string().c_str(), "rh"), TIFFClose);
    if (!image) {
        throw std::runtime_error("Cannot open TIFF: " + path.string());
    }
    registerGeoTiffFields(image.get());
    if (!TIFFReadDirectory(image.get())) {
        throw std::runtime_error("Cannot read TIFF directory: " + path.string());
    }
    std::uint32_t w = 0, h = 0;
    if (!TIFFGetField(image.get(), TIFFTAG_IMAGEWIDTH, &w) ||
        !TIFFGetField(image.get(), TIFFTAG_IMAGELENGTH, &h) ||
        w == 0 || h == 0 || w > 32768 || h > 32768 ||
        std::uint64_t(w) * h > 134'217'728) {
        throw std::runtime_error("TIFF dimensions are invalid or exceed the import limit.");
    }
    std::uint16_t orientation = ORIENTATION_TOPLEFT;
    TIFFGetFieldDefaulted(image.get(), TIFFTAG_ORIENTATION, &orientation);
    if (orientation < ORIENTATION_TOPLEFT || orientation > ORIENTATION_BOTLEFT) {
        throw std::runtime_error("Rotated TIFF orientations are not supported.");
    }
    std::uint16_t sampleFormat = SAMPLEFORMAT_UINT;
    TIFFGetFieldDefaulted(image.get(), TIFFTAG_SAMPLEFORMAT, &sampleFormat);
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    if (sampleFormat == SAMPLEFORMAT_INT || sampleFormat == SAMPLEFORMAT_IEEEFP) {
        std::uint16_t bits = 0, channels = 0, photometric = 0;
        TIFFGetFieldDefaulted(image.get(), TIFFTAG_BITSPERSAMPLE, &bits);
        TIFFGetFieldDefaulted(image.get(), TIFFTAG_SAMPLESPERPIXEL, &channels);
        TIFFGetField(image.get(), TIFFTAG_PHOTOMETRIC, &photometric);
        if (channels != 1 || photometric != PHOTOMETRIC_MINISBLACK ||
            TIFFIsTiled(image.get()) ||
            (sampleFormat == SAMPLEFORMAT_INT ? bits != 16 : bits != 32)) {
            throw std::runtime_error(
                "Elevation TIFF requires stripped, single-band Int16 or Float32, black-is-zero samples.");
        }
        const auto rowBytes = TIFFScanlineSize(image.get());
        if (rowBytes < static_cast<tmsize_t>(w) * (bits / 8)) {
            throw std::runtime_error("Invalid elevation TIFF scanline size.");
        }
        std::vector<unsigned char> row(static_cast<std::size_t>(rowBytes));
        std::vector<float> result(static_cast<std::size_t>(w) * h);
        for (std::uint32_t y = 0; y < h; ++y) {
            if (TIFFReadScanline(image.get(), row.data(), y, 0) < 0) {
                throw std::runtime_error("Cannot decode elevation TIFF: " + path.string());
            }
            for (std::uint32_t x = 0; x < w; ++x) {
                float value;
                if (sampleFormat == SAMPLEFORMAT_INT) {
                    std::int16_t integer;
                    std::memcpy(&integer, row.data() + x * sizeof(integer), sizeof(integer));
                    value = integer;
                } else {
                    std::memcpy(&value, row.data() + x * sizeof(value), sizeof(value));
                }
                if (!std::isfinite(value) || value < -32767.0F || value > 32767.0F) {
                    throw std::runtime_error(
                        "TIFF elevation samples must be finite and in [-32767, 32767]; NoData is not supported.");
                }
                const auto destX = orientation == ORIENTATION_TOPRIGHT ||
                    orientation == ORIENTATION_BOTRIGHT ? w - 1 - x : x;
                const auto destY = orientation == ORIENTATION_BOTLEFT ||
                    orientation == ORIENTATION_BOTRIGHT ? h - 1 - y : y;
                result[static_cast<std::size_t>(destY) * w + destX] = value;
            }
        }
        elevationSamples = true;
        return result;
    }
    if (sampleFormat != SAMPLEFORMAT_UINT) {
        throw std::runtime_error("Unsupported TIFF sample format.");
    }
    std::vector<std::uint32_t> rgba(static_cast<std::size_t>(w) * h);
    if (!TIFFReadRGBAImageOriented(image.get(), w, h, rgba.data(), ORIENTATION_TOPLEFT, 1)) {
        throw std::runtime_error("Cannot decode TIFF: " + path.string());
    }
    std::vector<float> result(rgba.size());
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        const unsigned char rgb[]{
            static_cast<unsigned char>(TIFFGetR(rgba[i])),
            static_cast<unsigned char>(TIFFGetG(rgba[i])),
            static_cast<unsigned char>(TIFFGetB(rgba[i]))};
        result[i] = luminance(rgb);
    }
    return result;
}

void writeTiff(const std::filesystem::path& path, int width, int height,
               const std::vector<float>& pixels, ElevationRaster::TiffSampleType sampleType,
               const std::vector<unsigned char>& rgb) {
    Tiff image(TIFFOpen(path.string().c_str(), "w"), TIFFClose);
    if (!image) {
        throw std::runtime_error("Cannot create TIFF output.");
    }
    const bool color = !rgb.empty();
    const bool grayscale = color || sampleType == ElevationRaster::TiffSampleType::Grayscale8;
    const bool integer = sampleType == ElevationRaster::TiffSampleType::Elevation16;
    if (!TIFFSetField(image.get(), TIFFTAG_IMAGEWIDTH, width) ||
        !TIFFSetField(image.get(), TIFFTAG_IMAGELENGTH, height) ||
        !TIFFSetField(image.get(), TIFFTAG_SAMPLESPERPIXEL, color ? 3 : 1) ||
        !TIFFSetField(image.get(), TIFFTAG_BITSPERSAMPLE, grayscale ? 8 : integer ? 16 : 32) ||
        !TIFFSetField(image.get(), TIFFTAG_SAMPLEFORMAT,
                      grayscale ? SAMPLEFORMAT_UINT : integer ? SAMPLEFORMAT_INT : SAMPLEFORMAT_IEEEFP) ||
        !TIFFSetField(image.get(), TIFFTAG_PHOTOMETRIC, color ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK) ||
        !TIFFSetField(image.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) ||
        !TIFFSetField(image.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) ||
        !TIFFSetField(image.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW) ||
        !TIFFSetField(image.get(), TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(image.get(), 0))) {
        throw std::runtime_error("Cannot configure TIFF output.");
    }
    // GeoTIFF 1.0: pixel-is-area, geographic WGS84; retain sidecars for other readers.
    registerGeoTiffFields(image.get());
    const double scale[]{360.0 / width, 180.0 / height, 0};
    const double tiepoint[]{0, 0, 0, -180, 90, 0};
    const std::uint16_t keys[]{
        1, 1, 0, 3,
        1024, 0, 1, 2,
        1025, 0, 1, 1,
        2048, 0, 1, 4326};
    if (!TIFFSetField(image.get(), 33550, scale) ||
        !TIFFSetField(image.get(), 33922, tiepoint) ||
        !TIFFSetField(image.get(), 34735, 16, keys)) {
        throw std::runtime_error("Cannot write GeoTIFF georeferencing.");
    }
    std::vector<unsigned char> grayscaleRow(static_cast<std::size_t>(width) * (color ? 3 : 1));
    std::vector<std::int16_t> integerRow(width);
    std::vector<float> floatRow(width);
    for (int y = 0; y < height; ++y) {
        if (color) {
            std::copy_n(rgb.data() + static_cast<std::size_t>(y) * width * 3,
                        grayscaleRow.size(), grayscaleRow.data());
        } else {
            for (int x = 0; x < width; ++x) {
                const float value = pixels[static_cast<std::size_t>(y) * width + x];
                if (grayscale) {
                    grayscaleRow[x] = static_cast<unsigned char>(
                        std::lround((value + 32767.0F) / 65534.0F * 255));
                } else if (integer) {
                    integerRow[x] = static_cast<std::int16_t>(std::lround(value));
                } else {
                    floatRow[x] = value;
                }
            }
        }
        void* row = grayscale ? static_cast<void*>(grayscaleRow.data()) :
            integer ? static_cast<void*>(integerRow.data()) : static_cast<void*>(floatRow.data());
        if (TIFFWriteScanline(image.get(), row, static_cast<std::uint32_t>(y), 0) < 0) {
            throw std::runtime_error("Failed to write TIFF pixels.");
        }
    }
    if (!TIFFWriteDirectory(image.get()) || !TIFFFlush(image.get())) {
        throw std::runtime_error("Failed to finish TIFF output.");
    }
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

std::vector<std::filesystem::path> ElevationRaster::ExportPaths(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> paths{path};
    if (isTiff(path)) {
        auto world = path;
        paths.push_back(world.replace_extension(".tfw"));
        paths.emplace_back(path.string() + ".aux.xml");
    }
    return paths;
}

void ElevationRaster::Export(const std::filesystem::path& path, const SphericalGrid& grid,
                              int width, int height, TiffSampleType tiffSampleType) {
    ExportRaster(path, grid, width, height, tiffSampleType, nullptr, nullptr);
}

void ElevationRaster::ExportImage(const std::filesystem::path& path, const SphericalGrid& grid,
                                  const ColorMap& colorMap, const ColorMap* selectionColorMap,
                                  int width, int height) {
    ExportRaster(path, grid, width, height, TiffSampleType::Grayscale8, &colorMap, selectionColorMap);
}

void ElevationRaster::ExportRaster(const std::filesystem::path& path, const SphericalGrid& grid,
                                   int width, int height, TiffSampleType tiffSampleType,
                                   const ColorMap* colorMap, const ColorMap* selectionColorMap) {
    if (!grid.HasActiveLayer()) {
        throw std::logic_error("Activate an elevation layer before exporting.");
    }
    if (width < 2 || width > 8192 || height < 1 || height > 4096 || width != height * 2) {
        throw std::invalid_argument("Export dimensions must be 2:1, up to 8192 by 4096.");
    }
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (extension != ".png" && extension != ".bmp" && extension != ".tga" && !isTiff(path)) {
        throw std::invalid_argument("Export supports PNG, BMP, TGA and TIFF files.");
    }
    if (isTiff(path) && tiffSampleType != TiffSampleType::Grayscale8 &&
        tiffSampleType != TiffSampleType::Elevation16 && tiffSampleType != TiffSampleType::Elevation32) {
        throw std::invalid_argument("Unsupported TIFF sample type.");
    }
    const auto snapshot = grid.GetSnapshot();
    std::vector<float> elevations(snapshot->GetVertices().size());
    for (std::size_t i = 0; i < elevations.size(); ++i) {
        elevations[i] = grid.GetActiveCompositedElevation(i);
    }
    std::vector<glm::vec3> colors;
    if (colorMap != nullptr) {
        colors.reserve(elevations.size());
        for (std::size_t i = 0; i < elevations.size(); ++i) {
            auto color = colorMap->Sample(elevations[i]);
            if (selectionColorMap != nullptr && snapshot->HasSelection()) {
                color = glm::mix(selectionColorMap->Sample(elevations[i]), color,
                                 snapshot->GetVertices()[i].selectionWeight);
            }
            colors.push_back(color);
        }
    }
    std::vector<float> pixels(static_cast<std::size_t>(width) * height);
    std::vector<unsigned char> rgb(colorMap == nullptr ? 0 : pixels.size() * 3);
    for (int y = 0; y < height; ++y) {
        const double latitude = pi * (0.5 - (y + 0.5) / height);
        const double radial = std::cos(latitude);
        for (int x = 0; x < width; ++x) {
            const double longitude = 2.0 * pi * ((x + 0.5) / width - 0.5);
            const glm::dvec3 point(radial * std::cos(longitude),
                                   radial * std::sin(longitude), std::sin(latitude));
            const auto site = snapshot->FindNearest(point);
            const auto pixel = static_cast<std::size_t>(y) * width + x;
            pixels[pixel] = elevations[site];
            if (colorMap != nullptr) {
                for (int channel = 0; channel < 3; ++channel) {
                    rgb[pixel * 3 + channel] = static_cast<unsigned char>(
                        std::lround(std::clamp(colors[site][channel], 0.0F, 1.0F) * 255));
                }
            }
        }
    }
    AtomicFile file(path);
    if (isTiff(path)) {
        const auto paths = ExportPaths(path);
        AtomicFile worldFile(paths[1]);
        AtomicFile projectionFile(paths[2]);
        writeTiff(file.GetPath(), width, height, pixels, tiffSampleType, rgb);
        std::ofstream world(worldFile.GetPath());
        world.exceptions(std::ios::failbit | std::ios::badbit);
        world.imbue(std::locale::classic());
        world << std::setprecision(17) << 360.0 / width << "\n0\n0\n" << -180.0 / height
              << '\n' << -180.0 + 180.0 / width << '\n' << 90.0 - 90.0 / height << '\n';
        world.close();
        std::ofstream projection(projectionFile.GetPath());
        projection.exceptions(std::ios::failbit | std::ios::badbit);
        projection << "<PAMDataset>\n  <SRS dataAxisToSRSAxisMapping=\"2,1\">"
                      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,"
                      "298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\","
                      "0.0174532925199433],AUTHORITY[\"EPSG\",\"4326\"]]"
                      "</SRS>\n</PAMDataset>\n";
        projection.close();
        worldFile.Commit();
        projectionFile.Commit();
        file.Commit();
        return;
    }
    std::vector<unsigned char> grayscale;
    if (colorMap == nullptr) {
        grayscale.resize(pixels.size());
        std::transform(pixels.begin(), pixels.end(), grayscale.begin(), [](float elevation) {
            return static_cast<unsigned char>(std::lround((elevation + 32767.0F) / 65534.0F * 255));
        });
    }
    const auto& image = colorMap == nullptr ? grayscale : rgb;
    const int channels = colorMap == nullptr ? 1 : 3;
    std::ofstream output(file.GetPath(), std::ios::binary);
    if (!output) {
        throw std::runtime_error("Cannot create the exported image.");
    }
    const auto write = [](void* context, void* data, int size) {
        auto& stream = *static_cast<std::ofstream*>(context);
        stream.write(static_cast<const char*>(data), size);
    };
    int result = 0;
    if (extension == ".png") {
        result = stbi_write_png_to_func(write, &output, width, height, channels, image.data(), width * channels);
    } else if (extension == ".bmp") {
        result = stbi_write_bmp_to_func(write, &output, width, height, channels, image.data());
    } else {
        result = stbi_write_tga_to_func(write, &output, width, height, channels, image.data());
    }
    output.close();
    if (!result || !output) {
        throw std::runtime_error("Failed to write the exported image.");
    }
    file.Commit();
}

ElevationRaster ElevationRaster::Load(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::invalid_argument("Raster path must not be empty.");
    }
    int sourceWidth = 0;
    int sourceHeight = 0;
    std::vector<float> sourceLuminosity;
    bool elevationSamples = false;
    if (isTiff(path)) {
        sourceLuminosity = loadTiff(path, sourceWidth, sourceHeight, elevationSamples);
    } else {
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
        sourceLuminosity.resize(sourcePixels);
        for (std::size_t i = 0; i < sourcePixels; ++i) {
            sourceLuminosity[i] = luminance(image.get() + i * 3);
        }
    }

    ElevationRaster raster;
    raster.width_ = rasterWidth;
    raster.height_ = rasterHeight;
    raster.elevationSamples_ = elevationSamples;
    raster.samples_.resize(static_cast<std::size_t>(rasterWidth) * rasterHeight);
    for (int y = 0; y < rasterHeight; ++y) {
        const double sourceY = (static_cast<double>(y) + 0.5) * sourceHeight / rasterHeight - 0.5;
        for (int x = 0; x < rasterWidth; ++x) {
            const double sourceX = (static_cast<double>(x) + 0.5) * sourceWidth / rasterWidth - 0.5;
            raster.samples_[static_cast<std::size_t>(y) * rasterWidth + x] =
                sampleClamped(sourceLuminosity, sourceWidth, sourceHeight, sourceX, sourceY);
        }
    }
    return raster;
}

float ElevationRaster::SampleLuminosity(double u, double v) const {
    const float value = SampleValue(u, v);
    return elevationSamples_ ? (value + 32767.0F) / 65534.0F : value;
}

float ElevationRaster::SampleValue(double u, double v) const {
    if (width_ <= 0 || height_ <= 0 || samples_.size() !=
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
        return samples_[static_cast<std::size_t>(py) * width_ + px];
    };
    return std::clamp(std::lerp(std::lerp(at(x0, y0), at(x1, y0), tx),
                                std::lerp(at(x0, y1), at(x1, y1), tx), ty),
                      elevationSamples_ ? -32767.0F : 0.0F, elevationSamples_ ? 32767.0F : 1.0F);
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
        const float value = SampleValue(u, v);
        elevations.push_back(elevationSamples_ ? value : -32767.0F + value * 65534.0F);
    }
    return elevations;
}
