#include "ElevationRaster.hpp"
#include "ColorMap.hpp"
#include <tiffio.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Function>
void expectFailure(Function function) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Expected raster operation to fail.");
}

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("oswald-raster-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    TemporaryDirectory() { std::filesystem::create_directory(path); }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path writePpm() const {
        const auto file = path / "sample.ppm";
        std::ofstream output(file, std::ios::binary);
        output << "P6\n4 2\n255\n";
        constexpr std::array<unsigned char, 12> row{
            0, 0, 0,       // black: L = 0
            255, 0, 0,     // red: L = 0.2126
            0, 255, 0,     // green: L = 0.7152
            255, 255, 255  // white: L = 1
        };
        output.write(reinterpret_cast<const char*>(row.data()), row.size());
        output.write(reinterpret_cast<const char*>(row.data()), row.size());
        output.close();
        check(static_cast<bool>(output), "Could not write PPM test fixture.");
        return file;
    }
};

void testLoadingAndSampling() {
    TemporaryDirectory temporary;
    const auto raster = ElevationRaster::Load(temporary.writePpm());
    check(raster.GetWidth() == 2048 && raster.GetHeight() == 1024,
          "Raster was not normalized to the standard 2:1 dimensions.");
    check(std::abs(raster.SampleLuminosity(0.125, 0.5) - 0.0F) < 0.002F,
          "Black sample luminosity is incorrect.");
    check(std::abs(raster.SampleLuminosity(0.375, 0.5) - 0.2126F) < 0.002F,
          "Red sample luminosity does not use Rec. 709 coefficients.");
    check(std::abs(raster.SampleLuminosity(0.625, 0.5) - 0.7152F) < 0.002F,
          "Green sample luminosity does not use Rec. 709 coefficients.");
    check(std::abs(raster.SampleLuminosity(0.875, 0.5) - 1.0F) < 0.002F,
          "White sample luminosity is incorrect.");
    check(std::abs(raster.SampleLuminosity(0.125, -2.0) -
                   raster.SampleLuminosity(0.125, 0.0)) < 1e-6F,
          "Latitude sampling did not clamp at the north/south image edge.");
    check(std::abs(raster.SampleLuminosity(0.0, 0.5) -
                   raster.SampleLuminosity(1.0, 0.5)) < 1e-6F,
          "Longitude seam is not periodic.");
    expectFailure([&] { raster.SampleLuminosity(std::numeric_limits<double>::quiet_NaN(), 0.5); });
    expectFailure([&] { ElevationRaster::Load(temporary.path / "missing.png"); });
    const auto invalid = temporary.path / "invalid.png";
    std::ofstream(invalid) << "not an image";
    expectFailure([&] { ElevationRaster::Load(invalid); });
    expectFailure([&] { ElevationRaster::Load({}); });
}

void testGridPopulationAndSnapshotLifetime() {
    TemporaryDirectory temporary;
    const auto raster = ElevationRaster::Load(temporary.writePpm());
    SphericalGrid grid({1000, 0.0, 42});
    const auto original = grid.GetSnapshot();
    const auto values = raster.SampleElevations(*original);
    check(values.size() == original->GetVertices().size(),
          "Elevation raster did not produce one value per grid vertex.");
    for (const float elevation : values) {
        check(std::isfinite(elevation) && elevation >= -32767.0F && elevation <= 32767.0F,
              "Raster elevation is outside the supported signed range.");
    }
    constexpr double pi = 3.14159265358979323846;
    for (const std::size_t index : std::array<std::size_t, 4>{0, 17, 499, 999}) {
        const auto& position = original->GetVertices()[index].position;
        const double latitude = std::asin(std::clamp(static_cast<double>(position.z), -1.0, 1.0));
        const double longitude = std::atan2(static_cast<double>(position.y),
                                            static_cast<double>(position.x));
        const float luminosity = raster.SampleLuminosity(longitude / (2.0 * pi) + 0.5,
                                                         0.5 - latitude / pi);
        const float expected = -32767.0F + luminosity * 65534.0F;
        check(std::abs(values[index] - expected) < 0.01F,
              "Raster was sampled at incorrect geographic coordinates.");
    }
    const auto indices = original->GetTriangleIndices();
    grid.SetElevations(values);
    const auto populated = grid.GetSnapshot();
    check(populated != original, "Elevation update did not publish an immutable snapshot.");
    check(populated->GetTriangleIndices() == indices,
          "Applying elevations changed the spherical triangulation.");
    check(original->GetVertices()[0].elevation == 0.0F,
          "Updating elevations mutated an existing snapshot.");
        check(std::any_of(populated->GetVertices().begin(), populated->GetVertices().end(),
                    [](const GridVertex& vertex) { return vertex.elevation != 0.0F; }),
            "New snapshot did not contain populated elevations.");

    const auto unchanged = grid.GetSnapshot();
    expectFailure([&] { grid.SetElevations(std::span<const float>(values).first(values.size() - 1)); });
    std::vector<float> invalid(values);
    invalid[3] = std::numeric_limits<float>::quiet_NaN();
    expectFailure([&] { grid.SetElevations(invalid); });
    invalid[3] = 32768.0F;
    expectFailure([&] { grid.SetElevations(invalid); });
    check(grid.GetSnapshot() == unchanged,
          "Rejected elevation data replaced the valid model snapshot.");
}

void testTiffImport() {
    TemporaryDirectory temporary;
    const auto reference = ElevationRaster::Load(temporary.writePpm());
    const auto path = temporary.path / "rgb.TIFF";
    for (const auto orientation : {ORIENTATION_TOPLEFT, ORIENTATION_TOPRIGHT,
                                   ORIENTATION_BOTRIGHT, ORIENTATION_BOTLEFT}) {
        {
            std::unique_ptr<TIFF, decltype(&TIFFClose)> image(
                TIFFOpen(path.string().c_str(), "w"), TIFFClose);
            check(static_cast<bool>(image), "Cannot create TIFF fixture.");
            TIFFSetField(image.get(), TIFFTAG_IMAGEWIDTH, 4);
            TIFFSetField(image.get(), TIFFTAG_IMAGELENGTH, 2);
            TIFFSetField(image.get(), TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(image.get(), TIFFTAG_SAMPLESPERPIXEL, 3);
            TIFFSetField(image.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
            TIFFSetField(image.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(image.get(), TIFFTAG_ORIENTATION, orientation);
            std::array<unsigned char, 12> row{0, 0, 0, 255, 0, 0, 0, 255, 0, 255, 255, 255};
            for (int y = 0; y < 2; ++y) {
                if (y == 1) row.fill(0);
                check(TIFFWriteScanline(image.get(), row.data(), y, 0) >= 0,
                      "Cannot write TIFF fixture.");
            }
        }
        const auto image = ElevationRaster::Load(path);
        for (const double u : {0.125, 0.375, 0.625, 0.875}) {
            const bool flipped = orientation == ORIENTATION_TOPRIGHT ||
                                 orientation == ORIENTATION_BOTRIGHT;
            const bool upsideDown = orientation == ORIENTATION_BOTLEFT ||
                                    orientation == ORIENTATION_BOTRIGHT;
            for (const double v : {0.25, 0.75}) {
                const bool blackRow = upsideDown ? v < 0.5 : v > 0.5;
                const float expected = blackRow ? 0.0F :
                    reference.SampleLuminosity(flipped ? 1.0 - u : u, 0.25);
                check(std::abs(image.SampleLuminosity(u, v) - expected) < 0.002F,
                      "TIFF RGB luminance or orientation is incorrect.");
            }
        }
    }
    for (const auto sampleFormat : {SAMPLEFORMAT_UINT, SAMPLEFORMAT_INT}) {
        {
            std::unique_ptr<TIFF, decltype(&TIFFClose)> image(
                TIFFOpen(path.string().c_str(), "w"), TIFFClose);
            check(static_cast<bool>(image), "Cannot create grayscale TIFF fixture.");
            TIFFSetField(image.get(), TIFFTAG_IMAGEWIDTH, 4);
            TIFFSetField(image.get(), TIFFTAG_IMAGELENGTH, 2);
            TIFFSetField(image.get(), TIFFTAG_BITSPERSAMPLE, 16);
            TIFFSetField(image.get(), TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(image.get(), TIFFTAG_SAMPLEFORMAT, sampleFormat);
            TIFFSetField(image.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE);
            TIFFSetField(image.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            std::array<std::uint16_t, 4> row{0, 65535, 0, 65535};
            for (int y = 0; y < 2; ++y) {
                check(TIFFWriteScanline(image.get(), row.data(), y, 0) >= 0,
                      "Cannot write grayscale TIFF fixture.");
            }
        }
        if (sampleFormat == SAMPLEFORMAT_INT) {
            expectFailure([&] { ElevationRaster::Load(path); });
        } else {
            const auto image = ElevationRaster::Load(path);
            check(image.SampleLuminosity(0.125, 0.5) > 0.999F &&
                  image.SampleLuminosity(0.375, 0.5) < 0.001F,
                  "TIFF import did not handle 16-bit grayscale white-is-zero images.");
        }
    }
    const auto invalid = temporary.path / "corrupt.tif";
    std::ofstream(invalid) << "not a TIFF";
    expectFailure([&] { ElevationRaster::Load(invalid); });
    expectFailure([&] { ElevationRaster::Load(temporary.path / "missing.tiff"); });
    std::filesystem::resize_file(path, 8);
    expectFailure([&] { ElevationRaster::Load(path); });
}

void testExport() {
    TemporaryDirectory temporary;
    SphericalGrid grid({1000, 0.0, 58});
    const auto snapshot = grid.GetSnapshot();
    std::vector<float> values;
    for (const auto& vertex : snapshot->GetVertices()) {
        values.push_back(vertex.position.z > 0 ?
            (vertex.position.y > 0 ? 30000.0F : 10000.0F) :
            (vertex.position.y > 0 ? -10000.0F : -32000.0F));
    }
    grid.SetElevations(values);
    auto& offset = grid.AddElevationLayer("Offset", "default.map", "grayscale.map");
    offset.SetBlendMode(LayerBlendMode::Add);
    grid.SetElevations(std::vector<float>(values.size(), 1000));
    grid.AddElevationLayer("Excluded upper layer", "default.map", "grayscale.map");
    grid.SetElevations(std::vector<float>(values.size(), -32767));
    grid.SetActiveLayer("Offset");
    for (const auto* extension : {".png", ".bmp", ".tga", ".tif", ".tiff", ".TIF", ".TIFF"}) {
        const auto path = temporary.path / (std::string("export") + extension);
        ElevationRaster::Export(path, grid, 64, 32);
        const auto image = ElevationRaster::Load(path);
        const auto checkSample = [&](double u, double v, float elevation) {
            const float expected = (elevation + 32767) / 65534;
            check(std::abs(image.SampleLuminosity(u, v) - expected) <= 1.0F / 255,
                  "Export orientation, signed mapping or cumulative elevation was incorrect.");
        };
        checkSample(0.25, 0.25, 11000);
        checkSample(0.75, 0.25, 31000);
        checkSample(0.25, 0.75, -31000);
        checkSample(0.75, 0.75, -9000);
        const auto paths = ElevationRaster::ExportPaths(path);
        if (paths.size() > 1) {
            check(paths.size() == 3 && paths[1].extension() == ".tfw" &&
                  paths[2] == path.string() + ".aux.xml", "Wrong TIFF sidecar paths.");
            std::ifstream world(paths[1]);
            for (double expected : {5.625, 0.0, 0.0, -5.625, -177.1875, 87.1875}) {
                double actual = 0;
                check(static_cast<bool>(world >> actual) && std::abs(actual - expected) < 1e-12,
                      "World file must reference pixel centres of the global north-up raster.");
            }
            std::string extra;
            check(!(world >> extra), "World file contains extra transform terms.");
            std::ifstream projection(paths[2]);
            const std::string xml((std::istreambuf_iterator<char>(projection)), {});
            check(xml.find("<PAMDataset>") != std::string::npos &&
                  xml.find("AUTHORITY[\"EPSG\",\"4326\"]") != std::string::npos &&
                  xml.find("dataAxisToSRSAxisMapping=\"2,1\"") != std::string::npos,
                  "TIFF sidecar must declare WGS84 with longitude-first raster coordinates.");
        }
    }
    const auto blockedPath = temporary.path / "blocked.tif";
    const auto blockedOutputs = ElevationRaster::ExportPaths(blockedPath);
    std::ofstream(blockedOutputs[0]) << "original image";
    std::ofstream(blockedOutputs[1]) << "original world file";
    std::filesystem::create_directory(blockedOutputs[2]);
    expectFailure([&] { ElevationRaster::Export(blockedPath, grid, 64, 32); });
    std::ifstream originalImage(blockedOutputs[0]);
    std::ifstream originalWorld(blockedOutputs[1]);
    check(std::string((std::istreambuf_iterator<char>(originalImage)), {}) == "original image" &&
          std::string((std::istreambuf_iterator<char>(originalWorld)), {}) == "original world file",
          "A sidecar staging failure replaced existing TIFF output.");
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path)) {
        check(!entry.path().filename().string().starts_with(".oswald-write-"),
              "Failed TIFF export leaked temporary staging files.");
    }
    grid.SetActiveLayer("Elevation");
    for (const auto* extension : {".png", ".tif"}) {
        for (const float elevation : {-32767.0F, 32767.0F}) {
            const auto path = temporary.path / (std::string("extrema") + extension);
            grid.SetElevations(std::vector<float>(values.size(), elevation));
            ElevationRaster::Export(path, grid, 16, 8);
            const auto image = ElevationRaster::Load(path);
            check(std::abs(image.SampleLuminosity(0.5, 0.5) -
                           (elevation < 0 ? 0.0F : 1.0F)) < 1e-5F,
                  "Grayscale export did not preserve the elevation endpoints.");
        }
    }
    const auto extremaPath = temporary.path / "extrema.png";
    const auto defaultPath = temporary.path / "default.png";
    ElevationRaster::Export(defaultPath, grid);
    std::ifstream png(defaultPath, std::ios::binary);
    std::array<unsigned char, 24> header{};
    png.read(reinterpret_cast<char*>(header.data()), header.size());
    check(png && header[16] == 0 && header[17] == 0 && header[18] == 8 && header[19] == 0 &&
          header[20] == 0 && header[21] == 0 && header[22] == 4 && header[23] == 0,
          "Default export dimensions were not 2048 by 1024.");
    expectFailure([&] { ElevationRaster::Export(temporary.path / "bad.jpg", grid, 64, 32); });
    expectFailure([&] { ElevationRaster::Export(extremaPath, grid, 64, 31); });
    expectFailure([&] { ElevationRaster::Export(extremaPath, grid, -2, -1); });
    expectFailure([&] {
        ElevationRaster::Export(temporary.path / "missing" / "bad.png", grid, 16, 8);
    });
    grid.DeactivateActiveLayer();
    expectFailure([&] { ElevationRaster::Export(extremaPath, grid, 16, 8); });
    check(ElevationRaster::Load(extremaPath).SampleLuminosity(0.5, 0.5) > 0.999F,
          "A rejected export damaged an existing file.");
}

void testTiffPrecision() {
    TemporaryDirectory temporary;
    SphericalGrid grid({1000, 0.0, 58});
    using SampleType = ElevationRaster::TiffSampleType;
    const auto path = temporary.path / "precision.tif";
    for (const auto type : {SampleType::Grayscale8, SampleType::Elevation16, SampleType::Elevation32}) {
        for (const float elevation : {-32767.0F, -1234.75F, -0.125F, 0.125F, 1234.75F, 32767.0F}) {
            grid.SetElevations(std::vector<float>(grid.GetSnapshot()->GetVertices().size(), elevation));
            ElevationRaster::Export(path, grid, 16, 8, type);
            {
                std::unique_ptr<TIFF, decltype(&TIFFClose)> image(
                    TIFFOpen(path.string().c_str(), "r"), TIFFClose);
                check(static_cast<bool>(image), "Cannot read precision TIFF.");
                std::uint16_t bits = 0, format = 0;
                TIFFGetField(image.get(), TIFFTAG_BITSPERSAMPLE, &bits);
                TIFFGetField(image.get(), TIFFTAG_SAMPLEFORMAT, &format);
                if (type == SampleType::Grayscale8) {
                    check(bits == 8 && format == SAMPLEFORMAT_UINT, "Wrong 8-bit TIFF sample type.");
                    std::array<unsigned char, 16> row{};
                    check(TIFFReadScanline(image.get(), row.data(), 0, 0) >= 0 &&
                          row[0] == std::lround((elevation + 32767.0F) / 65534.0F * 255),
                          "8-bit TIFF must retain the grayscale mapping.");
                } else if (type == SampleType::Elevation16) {
                    check(bits == 16 && format == SAMPLEFORMAT_INT, "Wrong Int16 TIFF sample type.");
                    std::array<std::int16_t, 16> row{};
                    check(TIFFReadScanline(image.get(), row.data(), 0, 0) >= 0 &&
                          row[0] == std::lround(elevation),
                          "Int16 TIFF must store direct rounded elevations, including negative values.");
                } else {
                    check(bits == 32 && format == SAMPLEFORMAT_IEEEFP, "Wrong Float32 TIFF sample type.");
                    std::array<float, 16> row{};
                    check(TIFFReadScanline(image.get(), row.data(), 0, 0) >= 0 && row[0] == elevation,
                          "Float32 TIFF must preserve fractional elevation bits.");
                }
            }
            const auto imported = ElevationRaster::Load(path).SampleElevations(*grid.GetSnapshot());
            if (type != SampleType::Grayscale8) {
                const float expected = type == SampleType::Elevation16 ? std::round(elevation) : elevation;
                check(std::all_of(imported.begin(), imported.end(),
                                 [expected](float value) { return value == expected; }),
                      "Elevation TIFF round trip lost precision or rescaled direct sample values.");
            }
        }
    }
    // Omitted TIFF sample type must choose lossless Float32 elevations.
    grid.SetElevations(std::vector<float>(grid.GetSnapshot()->GetVertices().size(), 0.125F));
    ElevationRaster::Export(path, grid, 16, 8);
    check(ElevationRaster::Load(path).SampleElevations(*grid.GetSnapshot()).front() == 0.125F,
          "Default TIFF export did not preserve fractional elevations.");
    expectFailure([&] {
        ElevationRaster::Export(path, grid, 16, 8, static_cast<SampleType>(99));
    });
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(), -32768.0F, 32768.0F}) {
        {
            std::unique_ptr<TIFF, decltype(&TIFFClose)> image(
                TIFFOpen(path.string().c_str(), "w"), TIFFClose);
            check(static_cast<bool>(image), "Cannot create invalid elevation fixture.");
            TIFFSetField(image.get(), TIFFTAG_IMAGEWIDTH, 2);
            TIFFSetField(image.get(), TIFFTAG_IMAGELENGTH, 1);
            TIFFSetField(image.get(), TIFFTAG_BITSPERSAMPLE, 32);
            TIFFSetField(image.get(), TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(image.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
            TIFFSetField(image.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(image.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            std::array<float, 2> row{0, invalid};
            check(TIFFWriteScanline(image.get(), row.data(), 0, 0) >= 0,
                  "Cannot write invalid elevation fixture.");
        }
        expectFailure([&] { ElevationRaster::Load(path); });
    }
}

void testImageExport() {
    TemporaryDirectory temporary;
    const auto mapPath = temporary.path / "image.map";
    std::ofstream(mapPath) << "-32767 FF0000\n0 00FF00\n32767 0000FF\n";
    const auto selectionPath = temporary.path / "selection.map";
    std::ofstream(selectionPath) << "-32767 FFFFFF\n32767 000000\n";
    const auto map = ColorMap::Load(mapPath);
    const auto selectionMap = ColorMap::Load(selectionPath);
    check(map.Sample(-40000) == glm::vec3(1, 0, 0) &&
          map.Sample(40000) == glm::vec3(0, 0, 1) &&
          map.Sample(0) == glm::vec3(0, 1, 0) &&
          glm::length(map.Sample(-16383.5F) - glm::vec3(0.5F, 0.5F, 0)) < 1e-6F,
          "Colour-map sampling must interpolate stops and clamp endpoints.");
    expectFailure([&] { map.Sample(std::numeric_limits<float>::quiet_NaN()); });

    SphericalGrid grid({1000, 0.0, 58});
    std::vector<float> elevations, weights;
    for (const auto& vertex : grid.GetSnapshot()->GetVertices()) {
        elevations.push_back(vertex.position.z > 0 ? 15000.0F : -20000.0F);
        weights.push_back(vertex.position.z > 0 ?
            (vertex.position.y > 0 ? 1.0F : 0.5F) :
            (vertex.position.y > 0 ? 0.25F : 0.0F));
    }
    grid.SetElevations(elevations);
    grid.SetSelectionWeights(weights, true);
    auto& offset = grid.AddElevationLayer("Offset", "image.map", "selection.map");
    offset.SetBlendMode(LayerBlendMode::Add);
    grid.SetElevations(std::vector<float>(weights.size(), 1000.0F));
    grid.AddElevationLayer("Excluded", "image.map", "selection.map");
    grid.SetElevations(std::vector<float>(weights.size(), 32767.0F));
    grid.SetActiveLayer("Offset");
    const auto snapshot = grid.GetSnapshot();
    constexpr int width = 64, height = 32;
    for (const auto* extension : {".png", ".bmp", ".tga", ".tif", ".TIFF"}) {
        for (bool highlight : {false, true}) {
            const auto path = temporary.path / (std::string("image") + extension);
            ElevationRaster::ExportImage(path, grid, map, highlight ? &selectionMap : nullptr,
                                         width, height);
            std::vector<unsigned char> pixels(width * height * 3);
            if (ElevationRaster::ExportPaths(path).size() == 3) {
                std::unique_ptr<TIFF, decltype(&TIFFClose)> image(
                    TIFFOpen(path.string().c_str(), "r"), TIFFClose);
                check(static_cast<bool>(image), "Cannot read RGB TIFF.");
                std::uint16_t bits = 0, format = 0, channels = 0, photometric = 0;
                TIFFGetField(image.get(), TIFFTAG_BITSPERSAMPLE, &bits);
                TIFFGetField(image.get(), TIFFTAG_SAMPLEFORMAT, &format);
                TIFFGetField(image.get(), TIFFTAG_SAMPLESPERPIXEL, &channels);
                TIFFGetField(image.get(), TIFFTAG_PHOTOMETRIC, &photometric);
                check(bits == 8 && format == SAMPLEFORMAT_UINT && channels == 3 &&
                      photometric == PHOTOMETRIC_RGB, "Image TIFF must have three RGB byte channels.");
                for (int y = 0; y < height; ++y) {
                    check(TIFFReadScanline(image.get(), pixels.data() + y * width * 3, y, 0) >= 0,
                          "Cannot read RGB TIFF pixels.");
                }
                for (const auto& output : ElevationRaster::ExportPaths(path)) {
                    check(std::filesystem::exists(output), "RGB TIFF is missing a sidecar.");
                }
            } else {
                int w = 0, h = 0, channels = 0;
                std::unique_ptr<unsigned char, decltype(&stbi_image_free)> image(
                    stbi_load(path.string().c_str(), &w, &h, &channels, 3), stbi_image_free);
                check(image && w == width && h == height && channels == 3,
                      "Image export must write full-size three-channel RGB.");
                std::copy_n(image.get(), pixels.size(), pixels.data());
            }
            constexpr double pi = 3.14159265358979323846;
            for (int y = 0; y < height; ++y) {
                const double latitude = pi * (0.5 - (y + 0.5) / height);
                for (int x = 0; x < width; ++x) {
                    const double longitude = 2 * pi * ((x + 0.5) / width - 0.5);
                    const auto site = snapshot->FindNearest(glm::dvec3(
                        std::cos(latitude) * std::cos(longitude),
                        std::cos(latitude) * std::sin(longitude), std::sin(latitude)));
                    const float value = elevations[site] + 1000;
                    auto expected = map.Sample(value);
                    if (highlight) {
                        expected = glm::mix(selectionMap.Sample(value), expected, weights[site]);
                    }
                    for (int channel = 0; channel < 3; ++channel) {
                        check(pixels[(y * width + x) * 3 + channel] ==
                              std::lround(expected[channel] * 255),
                              "RGB export changed orientation, cumulative data, colourmap or selection.");
                    }
                }
            }
        }
    }
    grid.SetSelectionWeights(std::vector<float>(weights.size(), 0), false);
    const auto unchanged = grid.GetSnapshot();
    const auto plain = temporary.path / "plain.png";
    const auto inactive = temporary.path / "inactive.png";
    ElevationRaster::ExportImage(plain, grid, map, nullptr, width, height);
    ElevationRaster::ExportImage(inactive, grid, map, &selectionMap, width, height);
    std::ifstream plainFile(plain, std::ios::binary), inactiveFile(inactive, std::ios::binary);
    check(std::string((std::istreambuf_iterator<char>(plainFile)), {}) ==
          std::string((std::istreambuf_iterator<char>(inactiveFile)), {}),
          "An inactive selection must not alter image colours.");
    expectFailure([&] { ElevationRaster::ExportImage(temporary.path / "bad.jpg", grid, map); });
    expectFailure([&] { ElevationRaster::ExportImage(plain, grid, map, nullptr, 64, 31); });
    check(grid.GetSnapshot() == unchanged, "Image export changed the model or selection.");
    grid.DeactivateActiveLayer();
    expectFailure([&] { ElevationRaster::ExportImage(plain, grid, map); });
}

} // namespace

int main() {
    try {
        testLoadingAndSampling();
        testGridPopulationAndSnapshotLifetime();
        testTiffImport();
        testExport();
        testTiffPrecision();
        testImageExport();
        std::cout << "Elevation raster tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Elevation raster test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
