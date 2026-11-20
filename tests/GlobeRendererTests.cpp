#include "ComputeContext.hpp"
#include "BrushScriptFile.hpp"
#include "ElevationRaster.hpp"
#include "Filter.hpp"
#include "GlobeRenderer.hpp"
#include "SelectionTool.hpp"
#include "ScriptedBrushTool.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
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
    throw std::runtime_error("Expected an explicit failure.");
}

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("oswald-view-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { std::filesystem::create_directory(path); }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) {
            std::cerr << "Test cleanup failed: " << error.message() << '\n';
        }
    }
    std::filesystem::path write(const char* name, std::string_view contents) const {
        const auto file = path / name;
        std::ofstream output(file);
        output << contents;
        output.close();
        check(static_cast<bool>(output), "Failed to write test fixture.");
        return file;
    }
};

const std::filesystem::path resources(OSWALD_RESOURCE_DIR);

void testColorMap() {
    const auto map = ColorMap::Load(resources / "assets/colormaps/default.map");
        check(map.GetStops().size() == 6,
          "Default map has incorrect stops.");
        check(map.GetStops()[0] == glm::vec4(-32767, .02F, .08F, .25F),
            "Default minimum-elevation colour changed.");
        check(map.GetStops()[3] == glm::vec4(13106.8F, .72F, .68F, .42F),
            "Default intermediate colour changed.");
    TemporaryDirectory temporary;
    const auto comments = ColorMap::Load(temporary.write("comments.map",
          " # comment\n-32767 1.0 0 0\n-100.5 0 1.0 0\n\n"
          "2500 0 0 1.0\n+32767 1.0 1 1 # endpoint\n"));
        check(comments.GetStops().size() == 4 && comments.GetStops()[1].x == -100.5F &&
            comments.GetStops()[2].x == 2500.0F && comments.GetStops()[3].w == 1.0F,
            "Signed elevations or comments parsed incorrectly.");
    expectFailure([&] { ColorMap::Load(temporary.path / "missing.map"); });
    const auto encodings = ColorMap::Load(temporary.write("encodings.map",
        "\t# standalone comment\r\n"
        "-32767\t0000FF# pure blue\r\n"
        "-100\v255\t128\f0 # decimal bytes\n"
        "0 0.0\t5e-1 1.0 # normalized floats\n"
        "100\tabcdef # lowercase hex\n"
        "200 808080 # digit-only hex is unambiguous\n"
        "300 0 1 0 # integer one means one byte\n"
        "+32767 FFFFFF # white\n"));
    check(encodings.GetStops()[0] == glm::vec4(-32767, 0, 0, 1) &&
          encodings.GetStops()[1] == glm::vec4(-100, 1, 128.0F / 255.0F, 0) &&
          encodings.GetStops()[2] == glm::vec4(0, 0, .5F, 1) &&
          encodings.GetStops()[3] == glm::vec4(100, 171.0F / 255.0F,
                                             205.0F / 255.0F, 239.0F / 255.0F) &&
          encodings.GetStops()[4] == glm::vec4(200, glm::vec3(128.0F / 255.0F)) &&
          encodings.GetStops()[5] == glm::vec4(300, 0, 1.0F / 255.0F, 0) &&
          encodings.GetStops()[6] == glm::vec4(32767, 1, 1, 1),
          "RGB encodings, whitespace or trailing comments parsed incorrectly.");
    for (const auto invalid : {
             "", "-32767 0 0 0\n", "0 0 0 0\n1 1 1 1\n",
             "[ocean]\n", "[land]\n", "[unknown]\n", "-32767 0 0 0 extra\n",
             "-32767 0 0\n", "-32767 2.0 0 0\n", "-32767 0 -0.1 0\n",
             "-32767 256 0 0\n", "-32767 -1 0 0\n", "-32767 1.0 255 0\n",
             "-32767 FF 00 00\n", "-32767 0xFF 0 0\n",
             "-32767 FFFFF\n", "-32767 FFFFFFF\n", "-32767 GG0000\n",
             "-32767 0x0000FF\n", "-32767 0000FF extra\n", "-32767\n",
             "-32767 0 0 1.1\n", "-32767 nan 0 0\n", "-32767 0 inf 0\n",
             "nan 0 0 0\n", "inf 0 0 0\n", "-32768 0 0 0\n", "32768 0 0 0\n",
             "-32767 0 0 0\n0 0 0 0\n0 1 1 1\n32767 1 1 1\n",
             "-32767 0 0 0\n100 0 0 0\n-100 1 1 1\n32767 1 1 1\n",
             "-32766 0 0 0\n32767 1 1 1\n",
             "-32767 0 0 0\n32766 1 1 1\n"}) {
        expectFailure([&] { ColorMap::Load(temporary.write("invalid.map", invalid)); });
    }
}

struct GlfwSession {
    GlfwSession() {
        glfwSetErrorCallback([](int code, const char* message) {
            std::cerr << "GLFW " << code << ": " << message << '\n';
        });
        if (!glfwInit()) {
            throw std::runtime_error("GPU test requires a working display and GLFW.");
        }
    }
    ~GlfwSession() { glfwTerminate(); }
};

std::array<unsigned char, 3> pixel(int x, int y) {
    std::array<unsigned char, 3> result{};
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, result.data());
    check(glGetError() == GL_NO_ERROR, "Pixel read failed.");
    return result;
}

std::array<unsigned char, 3> palettePixel(int width, int height) {
    return pixel(width / 2 + 6, height / 2 + 6);
}

void clear() {
    glClearColor(0.06F, 0.08F, 0.12F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

glm::vec3 expectedColor(float elevation) {
    const std::array stops{
        glm::vec4(-32767, .02F, .08F, .25F), glm::vec4(-1, .15F, .55F, .65F),
        glm::vec4(0, .18F, .48F, .18F), glm::vec4(13106.8F, .72F, .68F, .42F),
        glm::vec4(26213.6F, .45F, .38F, .30F), glm::vec4(32767, .95F, .95F, .98F)};
    const float t = glm::clamp(elevation, stops.front().x, stops.back().x);
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].x) {
            const auto& low = stops[i - 1];
            const auto& high = stops[i];
            return glm::mix(glm::vec3(low.y, low.z, low.w),
                            glm::vec3(high.y, high.z, high.w),
                            (t - low.x) / (high.x - low.x));
        }
    }
    throw std::logic_error("Expected colour lookup failed.");
}

void testComputeInfrastructure(const std::filesystem::path& shaderDirectory,
                               const std::filesystem::path& temporaryDirectory) {
    GridOptions options;
    options.pointCount = GridOptions::minimumSupportedPointCount;
    SphericalGrid grid(options);
    const auto snapshot = grid.GetSnapshot();

    ComputeContext compute;
    auto& mesh = compute.MeshFor(snapshot);
    check(&mesh == &compute.MeshFor(snapshot),
          "Compute context did not reuse the cached GPU mesh.");
    check(mesh.VertexCount() == snapshot->GetVertices().size() &&
          mesh.DirectedEdgeCount() == snapshot->GetHalfEdges().size(),
          "GPU mesh dimensions did not match the grid snapshot.");

    ScriptedBrushTool scripted("GLSL test");
    ScriptParameter scale;
    scale.name = "scale";
    scale.minimum = 0.0;
    scale.maximum = 2.0;
    scale.defaultValue = 1.0;
    scripted.SetScript("x * scale", {scale}, {1.5});
    scripted.SetGpuScript("return x * param_scale;");
    scripted.GetSettings().application = BrushApplication::Elevation;
    scripted.GetSettings().output = ScriptedBrushSettings::Output::Elevation;
    scripted.GetSettings().seed = 42;
    BrushSettings scriptBrush;
    scriptBrush.coverage = BrushCoverage::Selection;
    scriptBrush.raiseLowerRate = 100.0;
    std::vector<float> cpuScriptResult(snapshot->GetVertices().size(), 0.0F);
    std::vector<float> gpuScriptResult = cpuScriptResult;
    scripted.Apply(*snapshot, {0.0, 0.0, 1.0}, cpuScriptResult, scriptBrush, 0.0);
    scripted.ApplyGpu(compute, *snapshot, {0.0, 0.0, 1.0}, gpuScriptResult,
                      scriptBrush, 0.0);
    check(cpuScriptResult.size() == gpuScriptResult.size(),
          "GPU brush body returned an incorrect vertex count.");
    for (std::size_t i = 0; i < cpuScriptResult.size(); ++i) {
        check(std::abs(cpuScriptResult[i] - gpuScriptResult[i]) <= 1.0e-4F,
              "GPU brush body inputs or parameter values differed from exprtk.");
    }

    scripted.SetGpuScript(
        "return fbm(x, y, z, param_scale, 2.0, 0.5, 4) + "
        "valuefbm(x, y, z, 2.0, 2.0, 0.5, 3) + "
        "billow(x, y, z, 1.0, 2.0, 0.5, 2) + "
        "ridged(x, y, z, 1.0, 2.0, 0.5, 2) + valuenoise(x, y, z) + simplex(x, y, z);");
    std::fill(gpuScriptResult.begin(), gpuScriptResult.end(), 0.0F);
    scripted.ApplyGpu(compute, *snapshot, {0.0, 0.0, 1.0}, gpuScriptResult,
                      scriptBrush, 0.0);
    check(std::all_of(gpuScriptResult.begin(), gpuScriptResult.end(),
                      [](float value) { return std::isfinite(value); }),
          "GLSL noise functions returned non-finite results.");
    scripted.SetGpuScript(
        "return spectral(x,y,z,4.0,2.0,16.0) + "
        "warp_x(x,y,z,4.0,1.25,2.0,0.5,3) + "
        "warp_y(x,y,z,4.0,1.25,2.0,0.5,3) + "
        "warp_z(x,y,z,4.0,1.25,2.0,0.5,3);");
    std::fill(gpuScriptResult.begin(), gpuScriptResult.end(), 0.0F);
    scripted.ApplyGpu(compute, *snapshot, {0.0, 0.0, 1.0}, gpuScriptResult,
                      scriptBrush, 0.0);
    check(std::all_of(gpuScriptResult.begin(), gpuScriptResult.end(),
                      [](float value) { return std::isfinite(value); }),
          "GLSL spectral or domain-warp functions returned non-finite results.");

    const auto brushLibrary = BrushScriptLibrary::Scan(OSWALD_BRUSH_SCRIPT_DIR);
    check(brushLibrary.errors.empty() && brushLibrary.entries.size() == 10,
          "Every shipped brush must load successfully.");
    for (const auto& entry : brushLibrary.entries) {
        check(entry.file.gpuScript.has_value(),
              ("Brush has no gpu_script: " + entry.file.name).c_str());
        ScriptedBrushTool brush(entry.file.name);
        brush.SetScript(entry.file.script, entry.file.parameters);
        brush.SetGpuScript(entry.file.gpuScript);
        brush.GetSettings().application = entry.file.application;
        brush.GetSettings().output = entry.file.output.value_or(
            ScriptedBrushSettings::Output::Normalized);
        brush.GetSettings().rangeBetween = -1000.0F;
        brush.GetSettings().rangeAnd = 1000.0F;
        auto parameters = brush.GetParameters();
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            if (entry.file.name == "Gaussian Blur" && parameters[i].name == "Range") {
                brush.SetParameterValue(i, 5000.0);
            } else if (entry.file.name == "Gaussian Blur" &&
                       parameters[i].name == "Sigma") {
                brush.SetParameterValue(i, 2000.0);
            }
        }
        std::vector<float> cpuValues(snapshot->GetVertices().size());
        for (std::size_t i = 0; i < cpuValues.size(); ++i) {
            cpuValues[i] = static_cast<float>(static_cast<int>(i % 37) - 18) * 100.0F;
        }
        std::vector<float> gpuValues = cpuValues;
        brush.Apply(*snapshot, {0.0, 0.0, 1.0}, cpuValues, scriptBrush, 1.0);
        brush.ApplyGpu(compute, *snapshot, {0.0, 0.0, 1.0}, gpuValues,
                       scriptBrush, 1.0);
        check(gpuValues.size() == snapshot->GetVertices().size() &&
              std::all_of(gpuValues.begin(), gpuValues.end(),
                          [](float value) { return std::isfinite(value); }),
              ("GPU brush body failed: " + entry.file.name).c_str());
        if (entry.file.application != BrushApplication::Noise ||
            entry.file.name == "Gaussian Blur") {
            for (std::size_t i = 0; i < cpuValues.size(); ++i) {
                check(std::abs(cpuValues[i] - gpuValues[i]) <= 0.1F,
                      ("GPU brush body differed from CPU script: " + entry.file.name).c_str());
            }
        }
    }

    std::vector<std::uint32_t> offsets(mesh.VertexCount() + 1);
    std::vector<std::uint32_t> neighbors(mesh.DirectedEdgeCount());
    std::vector<float> distances(mesh.DirectedEdgeCount());
    std::vector<double> preciseDistances(mesh.DirectedEdgeCount());
    glGetNamedBufferSubData(mesh.NeighborOffsets(), 0,
        static_cast<GLsizeiptr>(offsets.size() * sizeof(offsets[0])), offsets.data());
    glGetNamedBufferSubData(mesh.Neighbors(), 0,
        static_cast<GLsizeiptr>(neighbors.size() * sizeof(neighbors[0])), neighbors.data());
    glGetNamedBufferSubData(mesh.EdgeDistancesKm(), 0,
        static_cast<GLsizeiptr>(distances.size() * sizeof(distances[0])), distances.data());
    glGetNamedBufferSubData(mesh.PreciseEdgeDistancesKm(), 0,
        static_cast<GLsizeiptr>(preciseDistances.size() * sizeof(preciseDistances[0])),
        preciseDistances.data());
    check(glGetError() == GL_NO_ERROR, "GPU mesh readback failed.");
    check(offsets.front() == 0 && offsets.back() == neighbors.size(),
          "GPU mesh CSR offsets did not cover all directed edges.");

    const double radiusKm = snapshot->GetOptions().planetRadius;
    for (std::size_t vertex = 0; vertex < mesh.VertexCount(); ++vertex) {
        const auto from = glm::normalize(glm::dvec3(snapshot->GetVertices()[vertex].position));
        for (std::size_t edge = offsets[vertex]; edge < offsets[vertex + 1]; ++edge) {
            check(neighbors[edge] < mesh.VertexCount() && std::isfinite(distances[edge]),
                  "GPU mesh contains an invalid neighbor or edge distance.");
            const auto to = glm::normalize(glm::dvec3(
                snapshot->GetVertices()[neighbors[edge]].position));
            const double expected = radiusKm * std::acos(
                std::clamp(glm::dot(from, to), -1.0, 1.0));
            check(std::abs(static_cast<double>(distances[edge]) - expected) < 1e-3,
                  "GPU edge distance exceeded the CPU precision tolerance.");
            check(std::abs(preciseDistances[edge] - expected) < 1e-10,
                  "GPU glacial edge distance exceeded the CPU double-precision tolerance.");
        }
    }

    const auto shader = temporaryDirectory / "compute_test.comp";
    {
        std::ofstream output(shader);
        output << "#version 450 core\n"
                  "layout(local_size_x = 256) in;\n"
                  "layout(std430, binding = 0) readonly buffer Source { float source[]; };\n"
                  "layout(std430, binding = 1) writeonly buffer Target { float target[]; };\n"
                  "layout(std430, binding = 2) buffer Error { uint code; uint index; };\n"
                  "uniform uint count;\n"
                  "void main() {\n"
                  "  uint i = gl_GlobalInvocationID.x;\n"
                  "  if (i >= count) return;\n"
                  "  float value = source[i];\n"
                  "  if (isnan(value) || isinf(value)) {\n"
                  "    if (atomicCompSwap(code, 0u, 1u) == 0u) index = i;\n"
                  "    return;\n"
                  "  }\n"
                  "  target[i] = value + 0.125;\n"
                  "}\n";
        output.close();
        check(static_cast<bool>(output), "Failed to write compute test shader.");
    }
    const GLuint program = compute.LoadProgram(shader);
    constexpr std::size_t count = 513;
    const GLuint sourceBuffer = compute.Scratch(count, 0);
    const GLuint targetBuffer = compute.Scratch(count, 1);
    std::vector<float> source(count);
    for (std::size_t i = 0; i < count; ++i) {
        source[i] = static_cast<float>(i) / 7.0F;
    }
    glNamedBufferSubData(sourceBuffer, 0,
        static_cast<GLsizeiptr>(source.size() * sizeof(source[0])), source.data());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sourceBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, targetBuffer);
    glProgramUniform1ui(program, glGetUniformLocation(program, "count"),
                        static_cast<GLuint>(count));
    compute.ClearErrors();
    compute.Dispatch1D(program, count);
    check(!compute.CheckErrors(), "Valid compute dispatch unexpectedly set an error flag.");
    const auto result = compute.Readback(targetBuffer, count);
    for (std::size_t i = 0; i < count; ++i) {
        check(std::abs(result[i] - (source[i] + 0.125F)) <= 1e-3F,
              "GPU compute output exceeded the CPU comparison tolerance.");
    }

    source[17] = std::numeric_limits<float>::infinity();
    glNamedBufferSubData(sourceBuffer, 0,
        static_cast<GLsizeiptr>(source.size() * sizeof(source[0])), source.data());
    compute.ClearErrors();
    compute.Dispatch1D(program, count);
    const auto error = compute.CheckErrors();
    check(error && error->code == ComputeError::Code::NonFinite && error->index == 17,
          "GPU compute error flag did not report the non-finite value and vertex.");
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ComputeContext::errorBufferBinding, 0);

    std::vector<float> elevations(mesh.VertexCount());
    for (std::size_t i = 0; i < elevations.size(); ++i) {
        const auto position = snapshot->GetVertices()[i].position;
        elevations[i] = 1000.0F + 700.0F * position.x + 250.0F * position.y;
    }
    const auto terrain = std::make_shared<SphericalGridSnapshot>(*snapshot, elevations);
    auto verifyFilter = [&](const Filter& filter, const char* name) {
        const auto verifySnapshot = [&](const auto& testSnapshot) {
            const auto expected = filter.Apply(*testSnapshot, elevations);
            const auto actual = compute.ApplyFilter(
                filter, testSnapshot, elevations, shaderDirectory);
            check(actual.size() == expected.size(),
                  "GPU filter returned an incorrect result size.");
            float maximumDifference = 0.0F;
            std::size_t maximumDifferenceIndex = 0;
            for (std::size_t i = 0; i < expected.size(); ++i) {
                const float difference = std::abs(actual[i] - expected[i]);
                if (difference > maximumDifference) {
                    maximumDifference = difference;
                    maximumDifferenceIndex = i;
                }
            }
            check(maximumDifference <= 1e-3F,
                  (std::string("GPU filter exceeded its comparison tolerance: ") + name +
                   " difference=" + std::to_string(maximumDifference) +
                   " vertex=" + std::to_string(maximumDifferenceIndex) +
                   " expected=" + std::to_string(expected[maximumDifferenceIndex]) +
                   " actual=" + std::to_string(actual[maximumDifferenceIndex]) +
                   " selected=" + std::to_string(testSnapshot->HasSelection())).c_str());
        };
        verifySnapshot(terrain);
        std::vector<float> selectionWeights(elevations.size());
        for (std::size_t i = 0; i < selectionWeights.size(); ++i) {
            selectionWeights[i] = static_cast<float>(i % 5) * 0.25F;
        }
        const auto selectedTerrain = std::make_shared<SphericalGridSnapshot>(
            *terrain, elevations, selectionWeights, true);
        verifySnapshot(selectedTerrain);
    };

    FillFilter fill;
    fill.SetSettings({FillOperation::Raise, 23.5F});
    verifyFilter(fill, "Fill");
    PrecipitonFilter precipiton;
    precipiton.SetSettings({0.025});
    verifyFilter(precipiton, "Precipiton");
    ThermalWeatheringFilter thermal;
    thermal.SetSettings({15.0});
    verifyFilter(thermal, "Thermal weathering");
    CratonicGroovingFilter grooving;
    grooving.SetSettings({glm::dvec3(0.3, 0.7, -0.2), 0.35, 0.8, 0.02});
    verifyFilter(grooving, "Cratonic grooving");
    CratonicScourFilter scour;
    scour.SetSettings({glm::dvec3(-0.2, 0.8, 0.4), 0.3, 0.015, 0.08, 150.0, 0.002});
    verifyFilter(scour, "Cratonic scour");
    GlacialErosionFilter glacial;
    GlacialErosionSettings glacialSettings;
    glacialSettings.equilibriumLineAltitude = 1200.0;
    glacialSettings.elaLatitudeGradient = 8.0;
    glacialSettings.accumulationRate = 0.015;
    glacialSettings.ablationRate = 0.025;
    glacialSettings.initialIceThickness = 20.0;
    glacialSettings.durationYears = 3.0;
    glacialSettings.glacialErodibility = 0.0002;
    glacialSettings.iceViscosityFactor = 0.003;
    glacialSettings.thicknessExponent = 1.0;
    glacialSettings.velocityExponent = 2.0;
    glacialSettings.troughHalfWidth = 1500.0;
    glacial.SetSettings(glacialSettings);
    auto glacialStaticLoad = glacialSettings;
    glacialStaticLoad.durationYears = 0.0;
    glacialStaticLoad.glacialErodibility = 0.0;
    glacial.SetSettings(glacialStaticLoad);
    verifyFilter(glacial, "Glacial static load");
    auto glacialLoadOnly = glacialSettings;
    glacialLoadOnly.glacialErodibility = 0.0;
    glacial.SetSettings(glacialLoadOnly);
    verifyFilter(glacial, "Glacial loading");
    auto glacialFractional = glacialSettings;
    glacialFractional.thicknessExponent = 1.3;
    glacialFractional.velocityExponent = 1.7;
    glacial.SetSettings(glacialFractional);
    verifyFilter(glacial, "Glacial fractional exponents");
    glacial.SetSettings(glacialSettings);
    verifyFilter(glacial, "Glacial erosion");
    AbsoluteMagnitudeNoiseFilter noise;
    noise.SetAmplitudeFeet(500.0F);
    const auto noisyElevations = compute.ApplyFilter(noise, terrain, elevations, shaderDirectory);
    bool noiseChangedElevation = false;
    check(noisyElevations.size() == elevations.size(),
          "GPU absolute-magnitude noise returned an incorrect result size.");
    for (std::size_t i = 0; i < noisyElevations.size(); ++i) {
        const float value = noisyElevations[i];
        check(std::isfinite(value) && value >= -32767.0F && value <= 32767.0F &&
              std::abs(value - elevations[i]) <= 152.4F + 1e-3F,
              "GPU absolute-magnitude noise exceeded its amplitude or elevation range.");
        noiseChangedElevation |= value != elevations[i];
    }
    check(noiseChangedElevation, "GPU absolute-magnitude noise did not alter any elevation.");
    auto invalidElevations = elevations;
    invalidElevations[23] = std::numeric_limits<float>::quiet_NaN();
    expectFailure([&] {
        compute.ApplyFilter(fill, terrain, invalidElevations, shaderDirectory);
    });

    const auto weights = std::vector<float>(elevations.size(), 0.0F);
    const auto selectedByValue = compute.ApplySelection(
        SelectionComputeOperation::ByValue, terrain, weights,
        500.0, 1200.0, 35.0, shaderDirectory);
    const auto expectedSelection = SelectByValue(*terrain, 500.0, 1200.0, 35.0);
    for (std::size_t i = 0; i < selectedByValue.size(); ++i) {
        check(std::abs(selectedByValue[i] - expectedSelection[i]) <= 1e-3F,
              "GPU value selection exceeded the 1e-3 tolerance.");
    }
    auto activeTerrain = std::make_shared<SphericalGridSnapshot>(
        *terrain, elevations, std::vector<float>(elevations.size(), 0.25F), true);
    const auto activeWeights = std::vector<float>(elevations.size(), 0.25F);
    const auto inverted = compute.ApplySelection(
        SelectionComputeOperation::Invert, activeTerrain, activeWeights,
        0.0, 0.0, 0.0, shaderDirectory);
    const auto cleared = compute.ApplySelection(
        SelectionComputeOperation::Clear, activeTerrain, activeWeights,
        0.0, 0.0, 0.0, shaderDirectory);
    check(std::all_of(inverted.begin(), inverted.end(),
                      [](float value) { return value == 0.75F; }) &&
          std::all_of(cleared.begin(), cleared.end(),
                      [](float value) { return value == 0.0F; }),
          "GPU invert or clear selection returned an incorrect result.");
    const auto invertedInactive = compute.ApplySelection(
        SelectionComputeOperation::Invert, terrain, weights,
        0.0, 0.0, 0.0, shaderDirectory);
    check(std::all_of(invertedInactive.begin(), invertedInactive.end(),
                      [](float value) { return value == 1.0F; }),
          "GPU invert did not select all sites when no selection was active.");
    SelectionSettings radiusSettings;
    radiusSettings.radius = 1400.0;
    radiusSettings.falloffPercent = 30.0;
    std::vector<float> expectedRadiusWeights(elevations.size(), 0.1F);
    auto actualRadiusWeights = expectedRadiusWeights;
    RadiusSelectionTool radiusTool;
    radiusTool.Apply(*terrain, {1.0, 0.0, 0.0}, expectedRadiusWeights, radiusSettings);
    actualRadiusWeights = compute.ApplyRadiusSelection(
        terrain, actualRadiusWeights, {1.0, 0.0, 0.0}, radiusSettings.radius,
        radiusSettings.falloffPercent, terrain->GetOptions().planetRadius, shaderDirectory);
    for (std::size_t i = 0; i < actualRadiusWeights.size(); ++i) {
        check(std::abs(actualRadiusWeights[i] - expectedRadiusWeights[i]) <= 1e-3F,
              "GPU radius selection exceeded the 1e-3 tolerance.");
    }
    radiusSettings.falloffPercent = 0.0;
    std::vector<float> hardRadiusExpected(elevations.size(), 0.0F);
    std::vector<float> hardRadiusActual(elevations.size(), 0.0F);
    radiusTool.Apply(*terrain, {1.0, 0.0, 0.0}, hardRadiusExpected, radiusSettings);
    hardRadiusActual = compute.ApplyRadiusSelection(
        terrain, hardRadiusActual, {1.0, 0.0, 0.0}, radiusSettings.radius,
        radiusSettings.falloffPercent, terrain->GetOptions().planetRadius, shaderDirectory);
    check(hardRadiusActual == hardRadiusExpected,
          "GPU radius selection with no falloff included points outside its radius.");

    const std::array polygon{
        glm::dvec3(1.0, -0.4, -0.4), glm::dvec3(1.0, 0.4, -0.4),
        glm::dvec3(1.0, 0.4, 0.4), glm::dvec3(1.0, -0.4, 0.4)};
    const auto actualPolygon = compute.ApplyPolygonSelection(terrain, polygon, shaderDirectory);
    const auto expectedPolygon = SelectSphericalPolygon(*terrain, polygon);
    check(actualPolygon == expectedPolygon,
          "GPU polygon selection differs from the CPU winding reference.");
    const auto histogram = compute.Histogram(elevations, shaderDirectory);
    std::array<float, 32> expectedHistogram{};
    for (const float value : elevations) {
        const auto bin = static_cast<std::size_t>(std::clamp(
            (value + 32767.0F) / 65534.0F * 32.0F, 0.0F, 31.0F));
        expectedHistogram[bin] += 1.0F;
    }
    check(histogram == expectedHistogram,
          "GPU histogram bins differ from the CPU reference.");

    const auto rasterPath = temporaryDirectory / "gpu_sampling.ppm";
    {
        std::ofstream output(rasterPath, std::ios::binary);
        output << "P6\n4 2\n255\n";
        const std::array<unsigned char, 24> pixels{
            0, 0, 0, 255, 0, 0, 0, 255, 0, 255, 255, 255,
            255, 255, 255, 0, 255, 0, 255, 0, 0, 0, 0, 0};
        output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        output.close();
        check(static_cast<bool>(output), "Failed to write GPU raster sampling fixture.");
    }
    const auto raster = ElevationRaster::Load(rasterPath);
    const auto cpuRasterSamples = raster.SampleElevations(*terrain);
    const auto gpuRasterSamples = raster.SampleElevations(
        *terrain, compute, shaderDirectory);
    check(cpuRasterSamples.size() == gpuRasterSamples.size(),
          "GPU raster sampling returned an incorrect result size.");
    float maximumRasterDifference = 0.0F;
    std::size_t maximumRasterIndex = 0;
    for (std::size_t i = 0; i < cpuRasterSamples.size(); ++i) {
        const float difference = std::abs(cpuRasterSamples[i] - gpuRasterSamples[i]);
        if (difference > maximumRasterDifference) {
            maximumRasterDifference = difference;
            maximumRasterIndex = i;
        }
    }
    check(maximumRasterDifference <= 0.05F,
          ("GPU raster bilinear sampling exceeded its 0.05 m tolerance: " +
           std::to_string(maximumRasterDifference) + " at " +
           std::to_string(maximumRasterIndex) + " cpu=" +
           std::to_string(cpuRasterSamples[maximumRasterIndex]) + " gpu=" +
           std::to_string(gpuRasterSamples[maximumRasterIndex])).c_str());

    std::vector<float> exportSelection(elevations.size());
    for (std::size_t i = 0; i < exportSelection.size(); ++i) {
        exportSelection[i] = static_cast<float>(i % 5) * 0.25F;
    }
    const auto exportSnapshot = std::make_shared<SphericalGridSnapshot>(
        *terrain, elevations, exportSelection, true);
    const auto colorMap = ColorMap::Load(resources / "assets/colormaps/default.map");
    const auto selectionMap = ColorMap::Load(resources / "assets/colormaps/default.map");
    constexpr int exportWidth = 128;
    constexpr int exportHeight = 64;
    const auto gpuExport = compute.SampleExport(exportSnapshot, elevations, exportSelection,
        exportWidth, exportHeight, &colorMap, &selectionMap, shaderDirectory);
    check(gpuExport.elevations.size() == static_cast<std::size_t>(exportWidth * exportHeight) &&
          gpuExport.rgb.size() == gpuExport.elevations.size(),
          "GPU export returned incorrect elevation or RGB dimensions.");
    for (int y = 0; y < exportHeight; ++y) {
        const double latitude = std::acos(-1.0) *
            (0.5 - (y + 0.5) / exportHeight);
        const double radial = std::cos(latitude);
        for (int x = 0; x < exportWidth; ++x) {
            const double longitude = 2.0 * std::acos(-1.0) *
                ((x + 0.5) / exportWidth - 0.5);
            const glm::dvec3 point(radial * std::cos(longitude),
                                   radial * std::sin(longitude), std::sin(latitude));
            const auto site = exportSnapshot->FindNearest(point);
            const auto pixelIndex = static_cast<std::size_t>(y) * exportWidth + x;
            check(gpuExport.elevations[pixelIndex] == elevations[site],
                  "GPU nearest-site export differed from the CPU reference.");
            auto color = glm::mix(selectionMap.Sample(elevations[site]),
                                  colorMap.Sample(elevations[site]),
                                  exportSelection[site]);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto expected = static_cast<unsigned int>(
                    std::lround(std::clamp(color[static_cast<glm::length_t>(channel)],
                                           0.0F, 1.0F) * 255));
                const auto actual = (gpuExport.rgb[pixelIndex] >>
                    (16 - static_cast<unsigned int>(channel) * 8)) & 0xFFu;
                check(actual == expected,
                      "GPU export color map differed from the CPU reference.");
            }
        }
    }
}

void testGpu() {
    const GlfwSession session;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    const std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(256, 256, "Oswald renderer test", nullptr, nullptr), glfwDestroyWindow);
    check(window != nullptr, "Cannot create GPU test context.");
    glfwMakeContextCurrent(window.get());
    check(gladLoadGL(glfwGetProcAddress) != 0 && GLAD_GL_VERSION_4_5,
          "GPU test requires OpenGL 4.5.");
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window.get(), &width, &height);

    TemporaryDirectory temporary;
    expectFailure([&] { GlobeRenderer missing(temporary.path); });
    temporary.write("globe.vert", "#version 450 core\ninvalid shader");
    temporary.write("globe.frag", "#version 450 core\ninvalid shader");
    expectFailure([&] { GlobeRenderer invalid(temporary.path); });
    std::filesystem::copy_file(resources / "shaders/globe.vert", temporary.path / "globe.vert",
                               std::filesystem::copy_options::overwrite_existing);
    expectFailure([&] { GlobeRenderer invalidFragment(temporary.path); });
    temporary.write("globe.frag",
        "#version 450 core\nin vec4 vElevation;\nout vec4 color;\n"
        "void main() { color = vElevation; }\n");
    expectFailure([&] { GlobeRenderer invalidLink(temporary.path); });
    check(glGetError() == GL_NO_ERROR, "Shader failure leaked OpenGL errors.");

    testComputeInfrastructure(resources / "shaders/compute", temporary.path);

    GlobeRenderer renderer(resources / "shaders");
    expectFailure([&] { renderer.Draw(width, height); });
    expectFailure([&] { renderer.Draw(-1, height); });
    renderer.Draw(0, 0);
    SphericalGrid grid({1000, 0.0, 42});
    const auto snapshot = grid.GetSnapshot();
    renderer.SetColorMap(ColorMap::Load(resources / "assets/colormaps/default.map"));
    renderer.UploadGrid(*snapshot);
    clear();
    renderer.Draw(width, height);
    const auto center = palettePixel(width, height);
    check(center[1] > center[0] * 2 && center[1] > center[2] * 2,
          "Zero-elevation globe is not coastal green.");
        check(std::abs(static_cast<int>(center[1]) - .48F * 255.0F) <= 1,
            "Ambient illumination did not preserve the coastal green colour.");
    const auto corner = pixel(0, 0);
    check(std::abs(static_cast<int>(corner[0]) - 15) <= 1 &&
          std::abs(static_cast<int>(corner[1]) - 20) <= 1 &&
          std::abs(static_cast<int>(corner[2]) - 31) <= 1,
          "Globe overwrote background outside its silhouette.");

    const auto grayscaleMap =
        ColorMap::Load(resources / "assets/colormaps/grayscale.map");
    const auto defaultMap = ColorMap::Load(resources / "assets/colormaps/default.map");
    const auto vertexCount = snapshot->GetVertices().size();
    ElevationLayer baseLayer("Base", "default.map", "grayscale.map",
                             std::vector<float>(vertexCount, 0.0F));
    ElevationLayer topLayer("Top", "grayscale.map", "grayscale.map",
                            std::vector<float>(vertexCount, -32767.0F));
    ElevationLayer invalidLayer("Invalid", "grayscale.map", "grayscale.map", {0.0F});
    expectFailure([&] {
        renderer.SetLayerStack({
            GlobeRenderLayer(invalidLayer, grayscaleMap, grayscaleMap)
        });
    });
    renderer.SetGraticuleEnabled(false);
    renderer.SetLayerStack({
        GlobeRenderLayer(topLayer, grayscaleMap, grayscaleMap),
        GlobeRenderLayer(baseLayer, defaultMap, grayscaleMap)
    });
    clear();
    renderer.Draw(width, height);
    const auto overlayPixel = palettePixel(width, height);
    check(overlayPixel[0] < 10 && overlayPixel[1] < 10 && overlayPixel[2] < 10,
          "Visible overlay layer was not drawn above the base layer.");
    const std::vector<float> previewValues(vertexCount, 32767.0F);
    renderer.UpdateLayerElevations(topLayer, previewValues);
    clear();
    renderer.Draw(width, height);
    const auto previewPixel = palettePixel(width, height);
    check(previewPixel[0] > 40 &&
          std::abs(static_cast<int>(previewPixel[0]) - previewPixel[1]) <= 2 &&
          std::abs(static_cast<int>(previewPixel[1]) - previewPixel[2]) <= 2,
          "Active-layer elevation preview was overwritten by another layer during drawing.");
    const std::array<std::size_t, 1> sparseUpdate{0};
    renderer.UpdateLayerElevations(topLayer, topLayer.GetValues(), sparseUpdate);
    expectFailure([&] {
        const std::array<std::size_t, 1> invalidUpdate{vertexCount};
        renderer.UpdateLayerElevations(topLayer, topLayer.GetValues(), invalidUpdate);
    });
    const std::vector<float> topLayerValues(vertexCount, -32767.0F);
    renderer.UpdateLayerElevations(topLayer, topLayerValues);
    topLayer.SetBlendMode(LayerBlendMode::Multiply);
    clear();
    renderer.Draw(width, height);
    const auto multiplyPixel = palettePixel(width, height);
    for (const auto channel : multiplyPixel) {
        check(std::abs(channel - 127.5F) <= 2,
              "Multiply did not map the product of elevations (zero) to grayscale.");
    }
    topLayer.SetVisible(false);
    clear();
    renderer.Draw(width, height);
    const auto hiddenPixel = palettePixel(width, height);
    check(hiddenPixel[1] > hiddenPixel[0] * 2 && hiddenPixel[1] > hiddenPixel[2] * 2,
          "Hidden layer remained visible or obscured the base layer.");
    topLayer.SetVisible(true);
    renderer.SetLayerStack({
        GlobeRenderLayer(topLayer, defaultMap, grayscaleMap),
        GlobeRenderLayer(baseLayer, grayscaleMap, grayscaleMap)
    });
    const auto checkElevationPixel = [&](float elevation) {
        clear();
        renderer.Draw(width, height);
        const auto actual = palettePixel(width, height);
        const auto expected = expectedColor(elevation);
        for (int channel = 0; channel < 3; ++channel) {
            check(std::abs(actual[channel] - expected[channel] * 255.0F) <= 2,
                  "Layer composition did not colour the combined elevation.");
        }
    };
    renderer.UpdateLayerElevations(topLayer, std::vector<float>(vertexCount, 0));
    topLayer.SetBlendMode(LayerBlendMode::Add);
    checkElevationPixel(0);
    checkElevationPixel(0);
    renderer.UpdateLayerElevations(baseLayer, std::vector<float>(vertexCount, 1000));
    checkElevationPixel(1000);
    renderer.UpdateLayerElevations(topLayer, std::vector<float>(vertexCount, 10000));
    for (const auto& [mode, elevation] : std::array{
             std::pair{LayerBlendMode::Overlay, 10000.0F},
             std::pair{LayerBlendMode::Add, 11000.0F},
             std::pair{LayerBlendMode::Subtract, -9000.0F},
             std::pair{LayerBlendMode::Multiply, 32767.0F},
             std::pair{LayerBlendMode::Minimum, 1000.0F},
             std::pair{LayerBlendMode::Maximum, 10000.0F}}) {
        topLayer.SetBlendMode(mode);
        checkElevationPixel(elevation);
    }
    renderer.SetLayerStack({
        GlobeRenderLayer(baseLayer, defaultMap, grayscaleMap),
        GlobeRenderLayer(topLayer, grayscaleMap, grayscaleMap)
    });
    baseLayer.SetBlendMode(LayerBlendMode::Subtract);
    checkElevationPixel(9000);
    baseLayer.SetVisible(false);
    topLayer.SetVisible(false);
    clear();
    renderer.Draw(width, height);
    check(palettePixel(width, height) == corner,
          "A stack with all layers hidden still rendered a globe.");
    renderer.ClearLayerStack();
    renderer.UpdateElevations(snapshot->GetVertices());
    renderer.SetGraticuleEnabled(true);

    clear();
    renderer.Draw(width, height);
    const auto graticuleCrossing = pixel(width / 2, height / 2);
    check(graticuleCrossing[0] > center[0] + 50 &&
          graticuleCrossing[1] > center[1] + 10,
          "Equator and Prime Meridian are not highlighted over the base palette.");
    renderer.SetGraticuleColors(glm::vec3(0.75F), glm::vec3(1.0F));
    renderer.SetGraticuleColors(glm::vec3(0.25F, 0.5F, 0.75F),
                                glm::vec3(0.8F, 0.6F, 0.4F));
    expectFailure([&] {
        renderer.SetGraticuleColors(glm::vec3(1.1F), glm::vec3(1.0F));
    });
    expectFailure([&] {
        renderer.SetWireframeColors(glm::vec3(-0.1F), glm::vec3(0.5F));
    });
    const auto countWireframePixels = [&] {
        int count = 0;
        for (int y = 0; y < height; y += 4) {
            for (int x = 0; x < width; x += 4) {
                const auto p = pixel(x, y);
                count += p[0] > 200 && p[1] > 200 && p[2] < 60 ? 1 : 0;
            }
        }
        return count;
    };
    clear();
    renderer.Draw(width, height);
    const int withoutWireframe = countWireframePixels();
    renderer.SetWireframeEnabled(true);
    clear();
    renderer.Draw(width, height);
    check(countWireframePixels() > withoutWireframe + 50,
          "Voronoi wireframe did not draw in the default yellow.");
    check(pixel(width / 2, height / 2)[1] > 100,
          "Wireframe replaced the elevation layer instead of overlaying it.");
    renderer.SetWireframeEnabled(false);
    for (const float zoom : {0.4F, 1.0F, 4.0F}) {
        clear();
        renderer.Draw(width, height, glm::mat4(1.0F), zoom);
    }

    auto selectionVertices = snapshot->GetVertices();
    renderer.UpdateSelection(selectionVertices, true);
    expectFailure([&] { renderer.Draw(width, height); });
    renderer.SetSelectionColorMap(ColorMap::Load(resources / "assets/colormaps/grayscale.map"));
    for (const float elevation : {-32767.0F, -16383.5F, 0.0F, 16383.5F, 32767.0F}) {
        for (auto& vertex : selectionVertices) {
            vertex.elevation = elevation;
        }
        renderer.UpdateElevations(selectionVertices);
        for (const float weight : {0.0F, 0.25F, 0.5F, 1.0F}) {
            for (auto& vertex : selectionVertices) {
                vertex.selectionWeight = weight;
            }
            for (const bool active : {true, false}) {
                renderer.UpdateSelection(selectionVertices, active);
                clear();
                renderer.Draw(width, height);
                const auto actual = palettePixel(width, height);
                const glm::vec3 grayscale((elevation + 32767.0F) / 65534.0F);
                const auto expected = active
                    ? glm::mix(grayscale, expectedColor(elevation), weight)
                    : expectedColor(elevation);
                for (int channel = 0; channel < 3; ++channel) {
                    check(std::abs(actual[channel] - expected[channel] * 255.0F) <= 2.0F,
                          "Selection did not blend the two elevation palettes correctly.");
                }
            }
        }
    }

    grid.SetSelectionWeights(std::vector<float>(selectionVertices.size(), 0.0F), true);
    renderer.UploadGrid(*grid.GetSnapshot());
    clear();
    renderer.Draw(width, height);
    const auto emptySelection = palettePixel(width, height);
    for (const auto channel : emptySelection) {
        check(std::abs(channel - 127.5F) <= 1.0F,
              "Uploading an active all-zero selection did not use grayscale.");
    }
    renderer.UploadGrid(*snapshot);
    clear();
    renderer.Draw(width, height);
    check(palettePixel(width, height) == center,
          "Uploading an inactive selection did not restore the default palette.");

    // Constant-elevation meshes isolate the colour map from elevation interpolation.
    auto vertices = snapshot->GetVertices();
    auto elevationUpdate = vertices;
    for (auto& vertex : elevationUpdate) {
        vertex.elevation = 32767.0F;
    }
    renderer.UpdateElevations(elevationUpdate);
    clear();
    renderer.Draw(width, height);
    const auto updatedPixel = palettePixel(width, height);
    const auto updatedColor = expectedColor(32767.0F);
    for (int channel = 0; channel < 3; ++channel) {
        check(std::abs(updatedPixel[channel] - updatedColor[channel] * 255.0F) <= 1.0F,
              "Elevation-only GPU subdata update did not change the rendered globe.");
    }
    renderer.UpdateElevations(snapshot->GetVertices());
    expectFailure([&] {
        renderer.UpdateElevations(std::span<const GridVertex>(elevationUpdate).first(
            elevationUpdate.size() - 1));
    });
    elevationUpdate[0].elevation = std::numeric_limits<float>::quiet_NaN();
    expectFailure([&] { renderer.UpdateElevations(elevationUpdate); });

    for (const float elevation : {-40000.0F, -32767.0F, -16383.5F, -1.0F, -.5F, -.01F, 0.0F,
                                  6553.4F, 13106.8F, 19660.2F, 26213.6F, 32767.0F, 40000.0F}) {
        for (auto& vertex : vertices) {
            vertex.elevation = elevation;
        }
        renderer.UploadMesh(vertices, snapshot->GetTriangleIndices());
        clear();
        renderer.Draw(width, height);
        const auto actual = palettePixel(width, height);
        const auto expected = expectedColor(elevation);
        for (int channel = 0; channel < 3; ++channel) {
            check(std::abs(actual[channel] - expected[channel] * 255.0F) <= 2.0F,
                  "GPU colour does not match the continuous elevation mapping.");
        }
    }

    const auto alternate = ColorMap::Load(temporary.write("alternate.map",
        "-32767 0000FF\n1000 0 0 255\n3000 1.0 0 0\n32767 FF0000\n"));
    renderer.SetColorMap(alternate);
    clear();
    renderer.Draw(width, height);
    const auto red = palettePixel(width, height);
    check(red[0] > 100 && red[1] == 0 && red[2] == 0,
          "Replacing palette did not update the maximum-elevation colour.");
    for (const float elevation : {-100.0F, 0.0F, 100.0F, 1000.0F, 2000.0F, 3000.0F}) {
        for (auto& vertex : vertices) {
            vertex.elevation = elevation;
        }
        renderer.UploadMesh(vertices, snapshot->GetTriangleIndices());
        clear();
        renderer.Draw(width, height);
        const auto actual = palettePixel(width, height);
        const float blend = glm::clamp((elevation - 1000.0F) / 2000.0F, 0.0F, 1.0F);
        check(std::abs(actual[0] - blend * 255.0F) <= 1.0F && actual[1] == 0 &&
              std::abs(actual[2] - (1.0F - blend) * 255.0F) <= 1.0F,
              "Palette transition was incorrectly tied to zero elevation.");
    }
    expectFailure([&] { renderer.UploadMesh({}, {}); });
    const std::array<std::uint32_t, 3> badIndices{0, 1, 1000};
    expectFailure([&] { renderer.UploadMesh(vertices, badIndices); });
    const auto validVertex = vertices[0];
    vertices[0].elevation = std::numeric_limits<float>::quiet_NaN();
    expectFailure([&] { renderer.UploadMesh(vertices, snapshot->GetTriangleIndices()); });
    vertices[0] = validVertex;
    vertices[0].position = glm::vec3(0);
    expectFailure([&] { renderer.UploadMesh(vertices, snapshot->GetTriangleIndices()); });
    vertices[0] = validVertex;
    expectFailure([&] { renderer.UploadMesh(vertices, std::span(badIndices).first(2)); });

    const auto irregular = ColorMap::Load(temporary.write("irregular.map",
        "-32767 0000FF\n-2000 FF0000\n2000 00FF00\n29000 0000FF\n32767 FFFFFF\n"));
    renderer.SetColorMap(irregular);
    for (auto& vertex : vertices) {
        vertex.elevation = 0.0F;
    }
    renderer.UploadMesh(vertices, snapshot->GetTriangleIndices());
    clear();
    renderer.Draw(width, height);
    const auto interpolated = palettePixel(width, height);
    check(interpolated[0] > 50 && std::abs(static_cast<int>(interpolated[0]) -
          static_cast<int>(interpolated[1])) <= 1 && interpolated[2] == 0,
          "Shader ignored irregular stops or failed to interpolate across zero.");

    renderer.SetColorMap(ColorMap::Load(resources / "assets/colormaps/default.map"));
    renderer.UploadGrid(*snapshot);
    // A sphere's projected diameter must be equal in pixels in both aspect ratios.
    for (const auto dimensions : {std::array<int, 2>{256, 128}, std::array<int, 2>{128, 256}}) {
        glfwSetWindowSize(window.get(), dimensions[0], dimensions[1]);
        glfwPollEvents();
        glfwGetFramebufferSize(window.get(), &width, &height);
        clear();
        renderer.Draw(width, height);
        int horizontalPixels = 0;
        int verticalPixels = 0;
        for (int x = 0; x < width; ++x) {
            if (pixel(x, height / 2) != pixel(0, 0)) {
                ++horizontalPixels;
            }
        }
        for (int y = 0; y < height; ++y) {
            if (pixel(width / 2, y) != pixel(0, 0)) {
                ++verticalPixels;
            }
        }
        check(horizontalPixels > 0 && std::abs(horizontalPixels - verticalPixels) <= 2,
              "Orthographic viewport distorts the sphere on resize.");
    }

        glfwSetWindowSize(window.get(), 256, 128);
        glfwPollEvents();
        glfwGetFramebufferSize(window.get(), &width, &height);
        renderer.SetGraticuleEnabled(false);
        renderer.SetEquirectangular(true);
        clear();
        renderer.Draw(width, height);
        const auto mapCenter = pixel(width / 2, height / 2);
        const auto mapLeft = pixel(4, height / 2);
        const auto mapRight = pixel(width - 5, height / 2);
        check(mapCenter[1] > mapCenter[0] * 2 && mapCenter[1] > mapCenter[2] * 2 &&
              mapLeft[1] > mapLeft[0] * 2 && mapRight[1] > mapRight[2] * 2,
              "Equirectangular projection did not render a continuous, horizontally wrapped map.");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--gpu") {
            testGpu();
        } else {
            testColorMap();
        }
        std::cout << "View tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "View test failed: " << error.what() << '\n';
        return 1;
    }
}
