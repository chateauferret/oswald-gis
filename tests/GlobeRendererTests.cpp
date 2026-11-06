#include "GlobeRenderer.hpp"

#include <GLFW/glfw3.h>

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
    check(map.GetOceanStops().size() == 2 && map.GetLandStops().size() == 4,
          "Default map has incorrect stops.");
    check(map.GetOceanStops()[0] == glm::vec4(0, .02F, .08F, .25F),
          "Default deep ocean colour changed.");
    check(map.GetLandStops()[1] == glm::vec4(.4F, .72F, .68F, .42F),
          "Default land colour changed.");
    TemporaryDirectory temporary;
    const auto comments = ColorMap::Load(temporary.write("comments.map",
        " # comment\n[land] # land first\n0 1 0 0\n1 0 1 0\n\n"
        "[ocean]\n0 0 0 1\n1 1 1 1 # endpoint\n"));
    check(comments.GetLandStops()[0].y == 1.0F && comments.GetOceanStops()[0].w == 1.0F,
          "Sections or comments parsed incorrectly.");
    expectFailure([&] { ColorMap::Load(temporary.path / "missing.map"); });
    for (const auto invalid : {
             "", "[land]\n0 0 0 0\n1 1 1 1\n", "0 0 0 0\n",
             "[ocean] junk\n", "[unknown]\n", "[ocean]\n0 0 0 0 extra\n",
             "[ocean]\n0 0 0\n", "[ocean]\n0 2 0 0\n",
             "[ocean]\n0 nan 0 0\n", "[ocean]\n0 inf 0 0\n",
             "[ocean]\n0 0 0 0\n0 1 1 1\n",
             "[ocean]\n1 0 0 0\n0 1 1 1\n",
             "[ocean]\n0.1 0 0 0\n1 1 1 1\n[land]\n0 0 0 0\n1 1 1 1\n",
             "[ocean]\n0 0 0 0\n0.9 1 1 1\n[land]\n0 0 0 0\n1 1 1 1\n",
             "[ocean]\n0 0 0 0\n1 1 1 1\n[ocean]\n",
             "[ocean]\n0 0 0 0\n1 1 1 1\n[land]\n0 0 0 0\n[land]\n"}) {
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

void clear() {
    glClearColor(0.06F, 0.08F, 0.12F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

glm::vec3 expectedColor(float elevation) {
    if (elevation < 0) {
        return glm::mix(glm::vec3(.02F, .08F, .25F), glm::vec3(.15F, .55F, .65F),
                        glm::clamp((elevation + 32767.0F) / 32767.0F, 0.0F, 1.0F));
    }
    const float t = glm::clamp(elevation / 32767.0F, 0.0F, 1.0F);
    if (t < .4F) {
        return glm::mix(glm::vec3(.18F, .48F, .18F), glm::vec3(.72F, .68F, .42F), t / .4F);
    }
    if (t < .8F) {
        return glm::mix(glm::vec3(.72F, .68F, .42F), glm::vec3(.45F, .38F, .30F), (t - .4F) / .4F);
    }
    return glm::mix(glm::vec3(.45F, .38F, .30F), glm::vec3(.95F, .95F, .98F), (t - .8F) / .2F);
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
    SphericalFibonacciGrid grid({1000, 0.0, 42});
    const auto snapshot = grid.GetSnapshot();
    renderer.SetColorMap(ColorMap::Load(resources / "assets/colormaps/default.map"));
    renderer.UploadGrid(*snapshot);
    clear();
    renderer.Draw(width, height);
    const auto center = pixel(width / 2, height / 2);
    check(center[1] > center[0] * 2 && center[1] > center[2] * 2,
          "Zero-elevation globe is not coastal green.");
    check(std::abs(static_cast<int>(center[1]) - 97) <= 4,
          "Depth, winding, or lighting did not show the front-facing hemisphere.");
    const auto corner = pixel(0, 0);
    check(std::abs(static_cast<int>(corner[0]) - 15) <= 1 &&
          std::abs(static_cast<int>(corner[1]) - 20) <= 1 &&
          std::abs(static_cast<int>(corner[2]) - 31) <= 1,
          "Globe overwrote background outside its silhouette.");

    // Constant-elevation meshes isolate the colour map from elevation interpolation.
    auto vertices = snapshot->GetVertices();
    for (const float elevation : {-40000.0F, -32767.0F, -16383.5F, -.01F, 0.0F,
                                  6553.4F, 13106.8F, 19660.2F, 26213.6F, 32767.0F, 40000.0F}) {
        for (auto& vertex : vertices) {
            vertex.elevation = elevation;
        }
        renderer.UploadMesh(vertices, snapshot->GetTriangleIndices());
        clear();
        renderer.Draw(width, height);
        const auto actual = pixel(width / 2, height / 2);
        const auto expected = expectedColor(elevation);
        const float lighting = static_cast<float>(center[1]) / (.48F * 255.0F);
        for (int channel = 0; channel < 3; ++channel) {
            check(std::abs(actual[channel] - expected[channel] * lighting * 255.0F) <= 2.0F,
                  "GPU colour does not match original piecewise hypsometric mapping.");
        }
    }

    const auto alternate = ColorMap::Load(temporary.write("alternate.map",
        "[ocean]\n0 0 0 1\n0.25 0 0 1\n1 0 0 1\n"
        "[land]\n0 1 0 0\n0.1 1 0 0\n0.9 1 0 0\n1 1 0 0\n"));
    renderer.SetColorMap(alternate);
    clear();
    renderer.Draw(width, height);
    const auto red = pixel(width / 2, height / 2);
    check(red[0] > 100 && red[1] == 0 && red[2] == 0,
          "Replacing palette did not update rendered land colour.");
    for (auto& vertex : vertices) {
        vertex.elevation = -100.0F;
    }
    renderer.UploadMesh(vertices, snapshot->GetTriangleIndices());
    clear();
    renderer.Draw(width, height);
    const auto blue = pixel(width / 2, height / 2);
    check(blue[2] > 100 && blue[0] == 0 && blue[1] == 0,
          "Replacing palette did not update rendered ocean colour.");
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
        "[ocean]\n0 0 0 1\n1 0 1 1\n"
        "[land]\n0 1 0 0\n0.1 0 1 0\n0.9 0 0 1\n1 1 1 1\n"));
    renderer.SetColorMap(irregular);
    for (auto& vertex : vertices) {
        vertex.elevation = .05F * 32767.0F;
    }
    renderer.UploadMesh(vertices, snapshot->GetTriangleIndices());
    clear();
    renderer.Draw(width, height);
    const auto interpolated = pixel(width / 2, height / 2);
    check(interpolated[0] > 50 && std::abs(static_cast<int>(interpolated[0]) -
          static_cast<int>(interpolated[1])) <= 1 && interpolated[2] == 0,
          "Shader ignored irregular external stop positions.");

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
