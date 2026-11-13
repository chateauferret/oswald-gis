#include "ElevationRaster.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

} // namespace

int main() {
    try {
        testLoadingAndSampling();
        testGridPopulationAndSnapshotLifetime();
        std::cout << "Elevation raster tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Elevation raster test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
