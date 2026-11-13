#include "GlobeRenderer.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
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
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
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
