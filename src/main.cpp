#include <glad/gl.h>

#include <GLFW/glfw3.h>

#include "CameraController.hpp"
#include "GlobeRenderer.hpp"
#include "UIManager.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
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

CameraController* activeCamera = nullptr;
UIManager* activeUiManager = nullptr;
int navigationButton = -1;

void mouseButton(GLFWwindow* window, int button, int action, int) {
    if (activeCamera == nullptr ||
        (button != GLFW_MOUSE_BUTTON_LEFT && button != GLFW_MOUSE_BUTTON_MIDDLE)) {
        return;
    }
    if (action == GLFW_RELEASE && button != navigationButton) {
        return;
    }
    if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_LEFT &&
        (navigationButton == GLFW_MOUSE_BUTTON_MIDDLE ||
         (activeUiManager != nullptr && activeUiManager->IsBrushActive()))) {
        return;
    }
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    int width = 0;
    int height = 0;
    glfwGetWindowSize(window, &width, &height);
    const bool captured = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
    activeCamera->HandleMouseButton(action == GLFW_PRESS, x, y, glfwGetTime(), width, height,
                                    captured, button == GLFW_MOUSE_BUTTON_LEFT);
    navigationButton = action == GLFW_PRESS && !captured ? button : -1;
}

void cursorPosition(GLFWwindow* window, double x, double y) {
    if (activeCamera == nullptr || navigationButton == -1) {
        return;
    }
    const bool down = glfwGetMouseButton(window, navigationButton) == GLFW_PRESS;
    const bool captured = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
    activeCamera->HandleCursorPosition(x, y, down, captured);
}

void scroll(GLFWwindow*, double, double yOffset) {
    if (activeCamera == nullptr) {
        return;
    }
    const bool captured = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
    activeCamera->HandleScroll(yOffset, captured);
}

struct ImGuiSession {
    explicit ImGuiSession(GLFWwindow* window) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
            ImGui::DestroyContext();
            throw std::runtime_error("Failed to initialize Dear ImGui GLFW backend.");
        }
        if (!ImGui_ImplOpenGL3_Init("#version 450 core")) {
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            throw std::runtime_error("Failed to initialize Dear ImGui OpenGL backend.");
        }
    }
    ~ImGuiSession() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
};

} // namespace

int runApplication() {
    GridOptions gridOptions;
    gridOptions.relaxationIterations = 5;
    SphericalGrid grid(gridOptions);
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
    CameraController camera;
    activeCamera = &camera;
    glfwSetMouseButtonCallback(window.get(), mouseButton);
    glfwSetCursorPosCallback(window.get(), cursorPosition);
    glfwSetScrollCallback(window.get(), scroll);
    const ImGuiSession imgui(window.get());

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window.get(), &framebufferWidth, &framebufferHeight);
    resizeFramebuffer(window.get(), framebufferWidth, framebufferHeight);

    const std::filesystem::path resources(OSWALD_RESOURCE_DIR);
    GlobeRenderer renderer(resources / "shaders");
    renderer.UploadGrid(*snapshot);
    renderer.SetColorMap(ColorMap::Load(resources / "assets/colormaps/default.map"));
    renderer.SetSelectionColorMap(ColorMap::Load(resources / "assets/colormaps/grayscale.map"));
    UIManager ui(snapshot->GetOptions(), resources / "assets/colormaps");
    activeUiManager = &ui;
    ui.SetCameraController(&camera);
    double previousTime = glfwGetTime();

    while (glfwWindowShouldClose(window.get()) == GLFW_FALSE) {
        const double currentTime = glfwGetTime();
        const float deltaSeconds = static_cast<float>(std::max(0.0, currentTime - previousTime));
        previousTime = currentTime;
        camera.Update(deltaSeconds);
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (ui.Render(grid, renderer, camera.GetModelMatrix(), camera.GetZoom())) {
            glfwSetWindowShouldClose(window.get(), GLFW_TRUE);
        }

        glfwGetFramebufferSize(window.get(), &framebufferWidth, &framebufferHeight);
        glClearColor(0.06F, 0.08F, 0.12F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.Draw(framebufferWidth, framebufferHeight, camera.GetModelMatrix(), camera.GetZoom());
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window.get());
    }

    activeCamera = nullptr;
    activeUiManager = nullptr;
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
