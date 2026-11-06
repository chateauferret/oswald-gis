#include <glad/gl.h>

#include <GLFW/glfw3.h>

#include "GlobeRenderer.hpp"

#include <exception>
#include <iostream>
#include <memory>

namespace {

void reportGlfwError(int, const char* description) {
    std::cerr << "GLFW error: " << description << '\n';
}

void resizeFramebuffer(GLFWwindow*, int width, int height) {
    glViewport(0, 0, width, height);
}

struct GlfwSession {
    ~GlfwSession() { glfwTerminate(); }
};

} // namespace

int runApplication() {
    SphericalFibonacciGrid grid;
    const auto snapshot = grid.GetSnapshot();
    std::cout << "Generated " << snapshot->GetVertices().size() << " vertices and "
              << snapshot->GetTriangleIndices().size() / 3 << " spherical faces.\n"
              << std::flush;

    glfwSetErrorCallback(reportGlfwError);
    if (glfwInit() != GLFW_TRUE) {
        std::cerr << "Failed to initialize GLFW.\n";
        return 1;
    }
    const GlfwSession session;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    const std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(1280, 800, "Oswald GIS", nullptr, nullptr), glfwDestroyWindow);
    if (window == nullptr) {
        std::cerr << "Failed to create an OpenGL 4.5 window.\n";
        return 1;
    }

    glfwMakeContextCurrent(window.get());
    glfwSwapInterval(1);

    if (gladLoadGL(glfwGetProcAddress) == 0) {
        std::cerr << "Failed to load OpenGL functions with GLAD.\n";
        return 1;
    }
    if (!GLAD_GL_VERSION_4_5) {
        std::cerr << "OpenGL 4.5 is not available.\n";
        return 1;
    }

    glfwSetFramebufferSizeCallback(window.get(), resizeFramebuffer);
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window.get(), &framebufferWidth, &framebufferHeight);
    resizeFramebuffer(window.get(), framebufferWidth, framebufferHeight);

    const std::filesystem::path resources(OSWALD_RESOURCE_DIR);
    GlobeRenderer renderer(resources / "shaders");
    renderer.UploadGrid(*snapshot);
    renderer.SetColorMap(ColorMap::Load(resources / "assets/colormaps/default.map"));

    while (glfwWindowShouldClose(window.get()) == GLFW_FALSE) {
        glfwGetFramebufferSize(window.get(), &framebufferWidth, &framebufferHeight);
        glClearColor(0.06F, 0.08F, 0.12F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.Draw(framebufferWidth, framebufferHeight);

        glfwSwapBuffers(window.get());
        glfwPollEvents();
    }

    return 0;
}

int main() {
    try {
        return runApplication();
    } catch (const std::exception& error) {
        std::cerr << "Oswald GIS startup failed: " << error.what() << '\n';
        return 1;
    }
}
