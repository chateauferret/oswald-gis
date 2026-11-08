#include "UIManager.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct GlfwSession {
    GlfwSession() {
        glfwSetErrorCallback([](int, const char* message) {
            std::cerr << "GLFW: " << message << '\n';
        });
        check(glfwInit(), "UI test requires a working display and GLFW.");
    }
    ~GlfwSession() { glfwTerminate(); }
};

struct ImGuiSession {
    ImGuiSession() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 800.0F);
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~ImGuiSession() { ImGui::DestroyContext(); }
};

struct TemporaryDirectory {
    std::filesystem::path original = std::filesystem::current_path();
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("oswald-dialog-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    TemporaryDirectory() {
        std::filesystem::create_directories(path / "images");
        std::ofstream output(path / "images" / "white raster##1.PPM", std::ios::binary);
        output << "P6\n2 1\n255\n";
        for (int i = 0; i < 6; ++i) {
            output.put(static_cast<char>(255));
        }
        output.close();
        check(static_cast<bool>(output), "Could not write dialog raster fixture.");
        std::ofstream(path / "images" / "broken.png") << "not an image";
        std::ofstream(path / "images" / "unsupported.tiff") << "not supported";
        std::filesystem::current_path(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::current_path(original, error);
        if (error) {
            std::cerr << "Dialog test working directory cleanup failed: " << error.message() << '\n';
            return;
        }
        std::filesystem::remove_all(path, error);
        if (error) {
            std::cerr << "Dialog test cleanup failed: " << error.message() << '\n';
        }
    }
};

void testDialog() {
    const GlfwSession glfw;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    const std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(256, 256, "Oswald dialog test", nullptr, nullptr), glfwDestroyWindow);
    check(window != nullptr, "Cannot create UI test OpenGL context.");
    glfwMakeContextCurrent(window.get());
    check(gladLoadGL(glfwGetProcAddress) != 0 && GLAD_GL_VERSION_4_5,
          "UI test requires OpenGL 4.5.");
    GlobeRenderer renderer(std::filesystem::path(OSWALD_RESOURCE_DIR) / "shaders");
    SphericalFibonacciGrid grid({1000, 0.0, 42});
    renderer.UploadGrid(*grid.GetSnapshot());
    UIManager ui(grid.GetSnapshot()->GetOptions());
    const ImGuiSession imgui;
    const TemporaryDirectory temporary;
    auto frame = [&] {
        ImGui::NewFrame();
        check(!ui.Render(grid, renderer), "Dialog unexpectedly requested application exit.");
        ImGui::Render();
    };
    auto key = [&](ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true);
        frame();
        ImGui::GetIO().AddKeyEvent(key, false);
        frame();
    };
    auto dialogOpen = [] {
        return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                          ImGuiPopupFlags_AnyPopupLevel);
    };
    auto openDialog = [&] {
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
        key(ImGuiKey_O);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
        check(dialogOpen(),
              "Ctrl+O did not open the file dialog.");
    };
    auto selectRow = [&](int row) {
        ImGuiWindow* listing = nullptr;
        for (auto* candidate : ImGui::GetCurrentContext()->Windows) {
            if (candidate->Active &&
                std::string_view(candidate->Name).find("/RasterFiles_") != std::string_view::npos) {
                listing = candidate;
                break;
            }
        }
        check(listing != nullptr, "File dialog did not render its directory listing.");
        ImGui::GetIO().AddMousePosEvent(listing->DC.CursorStartPos.x + 30.0F,
            listing->DC.CursorStartPos.y + row * ImGui::GetTextLineHeightWithSpacing() +
            ImGui::GetTextLineHeight() * 0.5F);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
    };

    frame();
    openDialog();
    selectRow(0);
    key(ImGuiKey_Enter);
    check(dialogOpen(),
          "Opening a folder closed the file dialog.");
    selectRow(0);
    key(ImGuiKey_Enter);
    check(dialogOpen(),
          "Invalid image closed the dialog instead of allowing retry.");
    check(grid.GetSnapshot()->GetVertices()[0].elevation == 0.0F,
          "Invalid image changed the grid.");
    selectRow(1);
    key(ImGuiKey_Enter);
    check(!dialogOpen(),
          "Selecting a compatible raster did not close the dialog.");
    check(grid.GetSnapshot()->GetVertices()[0].elevation == 32767.0F,
          "Selected raster did not update grid elevations.");
    openDialog();
    for (int i = 0; i < 30; ++i) {
        frame();
    }
    selectRow(1);
    check(dialogOpen(), "Single-click opened a raster instead of selecting it.");
    selectRow(1);
    check(!dialogOpen(), "Double-click did not open the selected raster.");
    openDialog();
    key(ImGuiKey_Escape);
    check(!dialogOpen(),
          "Escape did not cancel the dialog.");
    check(grid.GetSnapshot()->GetVertices()[0].elevation == 32767.0F,
          "Cancel changed the loaded raster.");
}

} // namespace

int main() {
    try {
        testDialog();
        std::cout << "Raster file dialog tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Raster file dialog test failed: " << error.what() << '\n';
        return 1;
    }
}
