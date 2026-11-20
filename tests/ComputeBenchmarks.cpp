#include "BrushTool.hpp"
#include "ColorMap.hpp"
#include "ElevationRaster.hpp"
#include "Filter.hpp"
#include "ScriptedBrushTool.hpp"
#include "SelectionTool.hpp"
#include "SphericalGrid.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::array cases{
    "grid_generation", "lloyd_generation", "regrid", "refine", "coarsen",
    "precipiton", "thermal", "grooving", "scour", "glacial", "incise",
    "fill_basins", "fill", "fill_raise", "fill_minimum", "fill_maximum",
    "absolute_noise", "select_value", "select_radius", "select_clear", "select_invert",
    "select_polygon", "histogram", "export_data", "export_image", "import_raster",
    "simplex", "value_noise", "ridge_noise", "spectral_noise", "gaussian",
    "scripted", "clamp", "hover"};

std::size_t number(std::string_view text) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("Expected a non-negative integer.");
    }
    return result;
}

class TemporaryImage {
public:
    TemporaryImage() {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            directory_ = std::filesystem::temp_directory_path() /
                ("oswald-benchmark-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(directory_)) return;
        }
        throw std::runtime_error("Cannot create benchmark temporary directory.");
    }
    ~TemporaryImage() {
        std::error_code error;
        std::filesystem::remove(directory_ / "raster.png", error);
        std::filesystem::remove(directory_, error);
    }
    std::filesystem::path path() const { return directory_ / "raster.png"; }
private:
    std::filesystem::path directory_;
};

template <typename Function>
void measure(std::string_view name, std::size_t count, std::size_t iteration,
             const Function& function) {
    const auto start = std::chrono::steady_clock::now();
    const auto checksum = function();
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << name << ',' << count << ',' << iteration << ',' << elapsed << ','
              << checksum << '\n' << std::flush;
}

template <typename T>
double sample(const std::vector<T>& values) {
    return values.empty() ? 0.0 : static_cast<double>(values[values.size() / 2]);
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::size_t count = 1'000'000;
        std::size_t iterations = 1;
        std::size_t rasterWidth = 2048;
        std::vector<std::string> names;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument == "--list") {
                for (const auto* item : cases) std::cout << item << '\n';
                return 0;
            }
            if (i + 1 == argc) throw std::invalid_argument("Missing option value.");
            const std::string_view value(argv[++i]);
            if (argument == "--vertices") count = number(value);
            else if (argument == "--iterations") iterations = number(value);
            else if (argument == "--raster-width") rasterWidth = number(value);
            else if (argument == "--case") names.emplace_back(value);
            else throw std::invalid_argument("Unknown option: " + std::string(argument));
        }
        if (count < GridOptions::minimumSupportedPointCount ||
            count > GridOptions::maximumPointCount || iterations == 0 ||
            rasterWidth < 2 || rasterWidth > 8192 || rasterWidth % 2 != 0 ||
            names.empty() || std::any_of(names.begin(), names.end(), [](const auto& name) {
                return std::find(cases.begin(), cases.end(), name) == cases.end();
            })) {
            throw std::invalid_argument(
                "Use --case NAME --vertices 1000..10000000 --iterations N "
                "--raster-width EVEN_2..8192; see --list.");
        }
        const GridOptions options{count, 0.5, 42};
        std::cout << "case,vertices,iteration,milliseconds,checksum\n";
        for (const auto& name : names) {
            if (name != "grid_generation" && name != "lloyd_generation") continue;
            auto generation = options;
            generation.relaxationIterations = name == "lloyd_generation" ? 1 : 0;
            for (std::size_t i = 0; i < iterations; ++i) {
                measure(name, count, i, [&] {
                    SphericalGrid grid(generation);
                    return static_cast<double>(grid.GetSnapshot()->GetHalfEdges().size());
                });
            }
        }

        if (std::all_of(names.begin(), names.end(), [](const auto& name) {
                return name == "grid_generation" || name == "lloyd_generation";
            })) return 0;

        SphericalGrid grid(options);
        std::vector<float> elevations(count);
        for (std::size_t i = 0; i < count; ++i) {
            elevations[i] = i % 19 == 0 ? -100.0F :
                100.0F + static_cast<float>((i * 193) % 4000);
        }
        grid.SetElevations(elevations);
        const auto snapshot = grid.GetSnapshot();
        for (const auto& name : names) {
            if (name == "grid_generation" || name == "lloyd_generation") continue;
            BrushSettings brushSettings;
            brushSettings.coverage = BrushCoverage::Selection;
            const glm::dvec3 center(0, 0, 1);
            TemporaryImage temporary;
            std::unique_ptr<ElevationRaster> raster;
            if (name == "import_raster") {
                ElevationRaster::Export(temporary.path(), grid, static_cast<int>(rasterWidth),
                                        static_cast<int>(rasterWidth / 2));
                raster = std::make_unique<ElevationRaster>(ElevationRaster::Load(temporary.path()));
            }
            std::unique_ptr<ColorMap> colorMap;
            if (name == "export_image") {
                colorMap = std::make_unique<ColorMap>(
                    ColorMap::Load(OSWALD_BENCHMARK_COLORMAP));
            }

            FilterManager filters;
            auto& fill = dynamic_cast<FillFilter&>(filters.GetFilter("Fill"));
            if (name == "fill_raise") fill.SetSettings({FillOperation::Raise, 100.0F});
            if (name == "fill_minimum") fill.SetSettings({FillOperation::Minimum, 500.0F});
            if (name == "fill_maximum") fill.SetSettings({FillOperation::Maximum, 500.0F});
            auto& glacial = dynamic_cast<GlacialErosionFilter&>(filters.GetFilter("Glacial erosion"));
            auto glacialSettings = glacial.GetSettings();
            glacialSettings.initialIceThickness = 100.0;
            glacial.SetSettings(glacialSettings);
            SimplexNoiseBrushTool simplex;
            ValueNoiseBrushTool valueNoise;
            RidgeNoiseBrushTool ridge;
            PowerLawNoiseBrushTool spectral;
            GaussianBlurBrushTool gaussian;
            ClampBrushTool clamp;
            clamp.GetSettings().minimum = 0.0F;
            clamp.GetSettings().maximum = 2000.0F;
            ScriptedBrushTool scripted;
            const std::array<glm::dvec3, 4> polygon{
                glm::normalize(glm::dvec3(-1, -1, 1)), glm::normalize(glm::dvec3(1, -1, 1)),
                glm::normalize(glm::dvec3(1, 1, 1)), glm::normalize(glm::dvec3(-1, 1, 1))};

            for (std::size_t i = 0; i < iterations; ++i) {
                std::vector<float> values = elevations;
                measure(name, count, i, [&]() -> double {
                    if (name == "precipiton") return sample(filters.GetFilter("Precipiton diffusion").Apply(*snapshot, elevations));
                    if (name == "thermal") return sample(filters.GetFilter("Thermal weathering").Apply(*snapshot, elevations));
                    if (name == "grooving") return sample(filters.GetFilter("Cratonic directional grooving").Apply(*snapshot, elevations));
                    if (name == "scour") return sample(filters.GetFilter("Cratonic scour and planation").Apply(*snapshot, elevations));
                    if (name == "glacial") return sample(filters.GetFilter("Glacial erosion").Apply(*snapshot, elevations));
                    if (name == "incise") return sample(filters.GetFilter("Incise flow").Apply(*snapshot, elevations));
                    if (name == "fill_basins") return sample(filters.GetFilter("Fill basins").Apply(*snapshot, elevations));
                    if (name == "fill" || name == "fill_raise" || name == "fill_minimum" ||
                        name == "fill_maximum") return sample(fill.Apply(*snapshot, elevations));
                    if (name == "absolute_noise") return sample(filters.GetFilter("Absolute Magnitude Noise").Apply(*snapshot, elevations));
                    if (name == "select_value") return sample(SelectByValue(*snapshot, 0, 2000, 20));
                    if (name == "select_polygon") return sample(SelectSphericalPolygon(*snapshot, polygon));
                    if (name == "select_radius") {
                        std::vector<float> weights(count, 0.0F);
                        return static_cast<double>(RadiusSelectionTool().Apply(*snapshot, center, weights, {}).size());
                    }
                    if (name == "select_clear" || name == "select_invert") {
                        std::vector<float> weights(count, 0.5F);
                        if (name == "select_clear") {
                            std::fill(weights.begin(), weights.end(), 0.0F);
                        } else {
                            std::transform(weights.begin(), weights.end(), weights.begin(),
                                           [](float weight) { return 1.0F - weight; });
                        }
                        return sample(weights);
                    }
                    if (name == "histogram") {
                        std::array<float, 32> bins{};
                        for (const auto value : grid.GetElevationLayer().GetValues()) {
                            const auto bin = static_cast<std::size_t>(std::clamp(
                                (value + 32767.0F) / 65534.0F * static_cast<float>(bins.size()),
                                0.0F, static_cast<float>(bins.size() - 1)));
                            bins[bin] += 1.0F;
                        }
                        float peak = 0.0F;
                        for (auto& bin : bins) {
                            bin /= static_cast<float>(count);
                            peak = std::max(peak, bin);
                        }
                        return peak;
                    }
                    if (name == "export_data") {
                        ElevationRaster::Export(temporary.path(), grid, static_cast<int>(rasterWidth),
                                                static_cast<int>(rasterWidth / 2));
                        return static_cast<double>(std::filesystem::file_size(temporary.path()));
                    }
                    if (name == "export_image") {
                        ElevationRaster::ExportImage(temporary.path(), grid, *colorMap, nullptr,
                                                     static_cast<int>(rasterWidth),
                                                     static_cast<int>(rasterWidth / 2));
                        return static_cast<double>(std::filesystem::file_size(temporary.path()));
                    }
                    if (name == "import_raster") return sample(raster->SampleElevations(*snapshot));
                    if (name == "regrid") {
                        auto other = options;
                        ++other.seed;
                        auto result = grid.PrepareResampledGrid(other);
                        static_cast<void>(result);
                        return static_cast<double>(count);
                    }
                    if (name == "refine" || name == "coarsen") {
                        std::vector<float> weights(count, 1.0F);
                        SphericalGridSnapshot selected(*snapshot, elevations, std::move(weights), true);
                        const auto changed = name == "refine" ? selected.RefineSelectedRegion(64) :
                            selected.CoarsenSelectedRegion(5000.0F, 64);
                        return changed ? static_cast<double>(changed->GetVertices().size()) : 0.0;
                    }
                    if (name == "hover") return grid.GetActiveCompositedElevation(count / 2);
                    const BrushTool* brush = nullptr;
                    if (name == "simplex") brush = &simplex;
                    else if (name == "value_noise") brush = &valueNoise;
                    else if (name == "ridge_noise") brush = &ridge;
                    else if (name == "spectral_noise") brush = &spectral;
                    else if (name == "gaussian") brush = &gaussian;
                    else if (name == "scripted") brush = &scripted;
                    else if (name == "clamp") brush = &clamp;
                    if (brush == nullptr) throw std::logic_error("Unimplemented benchmark case.");
                    return static_cast<double>(brush->Apply(*snapshot, center, values, brushSettings, 1.0).size());
                });
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
