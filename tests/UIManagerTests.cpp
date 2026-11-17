#include "UIManager.hpp"
#include "BrushScriptFile.hpp"
#include "ProjectFile.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
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
        std::ofstream(path / "images" / "unsupported.xyz") << "not supported";
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
    SphericalGrid grid({1000, 0.0, 42});
    renderer.UploadGrid(*grid.GetSnapshot());
    UIManager ui(grid.GetSnapshot()->GetOptions(),
                 std::filesystem::path(OSWALD_RESOURCE_DIR) / "assets/colormaps");
    ui.SetBrushScriptDirectory(OSWALD_BRUSH_SCRIPT_DIR);
    const ImGuiSession imgui;
    const TemporaryDirectory temporary;
    auto frame = [&] {
        ImGui::NewFrame();
        check(!ui.Render(grid, renderer), "Dialog unexpectedly requested application exit.");
        ImGui::Render();
    };
    frame();
    const auto* layersPanel = ImGui::FindWindowByName("Layers");
    check(layersPanel != nullptr && layersPanel->Active,
          "The layer management panel was not rendered.");
    auto key = [&](ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true);
        frame();
        ImGui::GetIO().AddKeyEvent(key, false);
        frame();
    };
    auto shortcut = [&](ImGuiKey keyCode) {
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
        key(keyCode);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
    };
    auto dialogOpen = [] {
        return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                          ImGuiPopupFlags_AnyPopupLevel);
    };
    auto openDialog = [&] {
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
        key(ImGuiKey_O);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
        frame();
        check(dialogOpen(),
              "Ctrl+Shift+O did not open the raster file dialog.");
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

    const auto panelClick = [&](const char* label, int parameterIndex = -1) {
        frame();
        auto* panel = ImGui::FindWindowByName("###BrushParameters");
        check(panel != nullptr && panel->Active, "Brush panel is not visible.");
        ImGui::FocusWindow(panel);
        const auto seed = parameterIndex < 0 ? panel->IDStack.back() :
            ImHashData(&parameterIndex, sizeof(parameterIndex), panel->IDStack.back());
        const auto id = ImHashStr(label, 0, seed);
        for (float scroll = 0.0F;; scroll += panel->InnerRect.GetHeight() * 0.5F) {
            ImGui::SetScrollY(panel, std::min(scroll, panel->ScrollMax.y));
            frame();
            for (float y = panel->InnerRect.Min.y + 2.0F; y < panel->InnerRect.Max.y; y += 5.0F) {
                for (float x = panel->InnerRect.Min.x + 10.0F; x < panel->InnerRect.Max.x; x += 14.0F) {
                    ImGui::GetIO().AddMousePosEvent(x, y);
                    frame();
                    if (ImGui::GetCurrentContext()->HoveredId == id) {
                        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                        frame();
                        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                        frame();
                        frame();
                        return ImVec2(x, y);
                    }
                }
            }
            if (scroll >= panel->ScrollMax.y) {
                break;
            }
        }
        throw std::runtime_error(std::string("Brush item was not interactive: ") + label);
    };
    auto selectBrushCombo = [&](const char* label, int, int selectedItem,
                                const char* expectedItem) {
        auto* panel = ImGui::FindWindowByName("###BrushParameters");
        check(panel != nullptr && panel->Active, "Brush panel is not visible.");
        const bool parameter = std::string_view(label) == "Operation" ||
                               std::string_view(label) == "Blur distance unit";
        panelClick(parameter ? "###ScriptParameter" : label, parameter ? 0 : -1);
        frame();
        auto* popup = ImGui::FindWindowByName("##Combo_00");
        check(panel->Active && popup != nullptr && popup->Active &&
              !ImGui::GetCurrentContext()->OpenPopupStack.empty() &&
              !ImGui::GetCurrentContext()->OpenPopupStack.empty(),
              "Clicking a brush combo arrow did not keep its dropdown and panel visible.");

        ImGui::GetIO().AddMousePosEvent(popup->DC.CursorStartPos.x + 10.0F,
            popup->DC.CursorStartPos.y + selectedItem * ImGui::GetTextLineHeightWithSpacing() +
            ImGui::GetTextLineHeight() * 0.5F);
        frame();
        check(ImGui::GetCurrentContext()->HoveredId ==
                  (parameter ? popup->GetID(expectedItem) :
                               ImHashStr(expectedItem, 0, popup->GetID(selectedItem))),
              "Brush combo did not point to the expected option.");
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        check(!dialogOpen() && panel->Active,
              "Selecting a brush combo option did not close the dropdown.");
    };
    auto checkBrushSlider = [&](const char* label, int, bool enabled, float relativeX = 0.25F) {
        auto* panel = ImGui::FindWindowByName("###BrushParameters");
        check(panel != nullptr && panel->Active, "Brush panel is not visible.");
        const bool parameter = std::string_view(label) == "Elevation" ||
                               std::string_view(label) == "Intensity (%)";
        const auto point = panelClick(parameter ? "###ScriptParameter" : label, parameter ? 1 : -1);
        const int index = 1;
        const ImGuiID sliderId = parameter ? ImHashStr("###ScriptParameter", 0,
            ImHashData(&index, sizeof(index), panel->IDStack.back())) : panel->GetID(label);
        ImGui::GetIO().AddMousePosEvent(panel->DC.CursorStartPos.x + panel->ItemWidthDefault * relativeX,
                                      point.y);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        check((ImGui::GetCurrentContext()->ActiveId == sliderId) == enabled,
              enabled ? "Brush slider should be enabled." : "Brush slider should be disabled.");
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
    };
    auto click = [&](float x, float y) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
    };
    auto selectMenuItem = [&](int menu, int row, const char* label) {
        auto* bar = ImGui::FindWindowByName("##MainMenuBar");
        check(bar != nullptr && bar->Active, "Main menu bar was not rendered.");
        constexpr std::array names{"File", "Edit", "Select", "Brushes", "Filter", "Mesh"};
        float x = bar->DC.CursorStartPos.x + ImGui::GetStyle().ItemSpacing.x * 0.5F;
        for (int i = 0; i < menu; ++i) {
            x += ImGui::CalcTextSize(names[i]).x + ImGui::GetStyle().ItemSpacing.x * 2.0F;
        }
        click(x + ImGui::CalcTextSize(names[menu]).x * 0.5F,
              bar->Pos.y + bar->Size.y * 0.5F);
        frame();
        auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
        check(!popups.empty() && popups.back().Window != nullptr, "Menu did not open.");
        auto* popup = popups.back().Window;
        if (menu == 3) {
            for (float y = popup->InnerRect.Min.y + 1; y < popup->InnerRect.Max.y; y += 2) {
                ImGui::GetIO().AddMousePosEvent(popup->DC.CursorStartPos.x + 20.0F, y);
                frame();
                if (ImGui::GetCurrentContext()->HoveredId == popup->GetID(label)) {
                    break;
                }
            }
        } else {
            ImGui::GetIO().AddMousePosEvent(popup->DC.CursorStartPos.x + 20.0F,
                popup->DC.CursorStartPos.y + row * ImGui::GetTextLineHeightWithSpacing() +
                (menu == 0 && row >= 5 ? ImGui::GetStyle().ItemSpacing.y : 0.0F) +
                ImGui::GetTextLineHeight() * 0.5F);
            frame();
        }
        check(ImGui::GetCurrentContext()->HoveredId == popup->GetID(label),
              "Menu did not point to the expected item.");
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        frame();
    };

    frame();
    frame();
    frame();
    selectMenuItem(3, 0, "Elevation Brush");
    checkBrushSlider("Falloff (%)", 3, true);
    constexpr std::array operationNames{"Replace", "Minimum", "Maximum"};
    for (int operation = 0; operation <= 2; ++operation) {
        selectBrushCombo("Operation", 0, operation, operationNames[operation]);
        checkBrushSlider("Elevation", 4, true);
    }
    selectBrushCombo("Distance unit", 1, 1, "Miles");
    selectMenuItem(3, 1, "Raise/Lower Brush");
    checkBrushSlider("Intensity (%)", 4, true);
    selectBrushCombo("Operation", 0, 1, "Lower");
    checkBrushSlider("Intensity (%)", 4, true);
    selectMenuItem(3, 0, "Elevation Brush");
    checkBrushSlider("Elevation", 4, true);
    selectMenuItem(3, 0, "Gaussian Blur");
    const auto readParameterNumber = [&](int index) {
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
        panelClick("###ScriptParameter", index);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
        const auto* panel = ImGui::FindWindowByName("###BrushParameters");
        const auto id = ImHashStr("###ScriptParameter", 0,
            ImHashData(&index, sizeof(index), panel->IDStack.back()));
        const auto* input = ImGui::GetInputTextState(id);
        check(input != nullptr, "Script parameter did not enter numeric input mode.");
        const double value = std::stod(input->TextA.Data);
        key(ImGuiKey_Escape);
        return value;
    };
    check(std::abs(readParameterNumber(1) - 100.0) < 0.001,
          "Gaussian blur range did not use its TOML default.");
    selectBrushCombo("Blur distance unit", 0, 1, "Miles");
    check(std::abs(readParameterNumber(1) - 62.1371192) < 0.001,
          "Changing Gaussian blur units did not preserve the physical range.");
    selectMenuItem(3, 0, "Elevation Brush");
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
    const auto paintedSnapshot = grid.GetSnapshot();
    selectMenuItem(0, 6, "Settings...");
    auto* settings = ImGui::FindWindowByName("Settings");
    check(settings != nullptr && settings->Active, "Settings dialog did not open.");
    const float rowHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    auto focusSpeed = [&] {
        click(settings->DC.CursorStartPos.x + 30.0F,
              settings->DC.CursorStartPos.y + rowHeight +
              ImGui::GetFrameHeight() * 0.5F);
    };
    auto editSpeed = [&](const char* text) {
        focusSpeed();
        shortcut(ImGuiKey_A);
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
        key(ImGuiKey_Enter);
    };
    auto settingsButton = [&](bool apply) {
        frame();
        frame();
        const float applyWidth = ImGui::CalcTextSize("Apply").x +
                                 ImGui::GetStyle().FramePadding.x * 2.0F;
        const float x = apply ? applyWidth * 0.5F :
            applyWidth + ImGui::GetStyle().ItemSpacing.x +
            ImGui::CalcTextSize("Cancel").x * 0.5F + ImGui::GetStyle().FramePadding.x;
        click(settings->DC.CursorStartPos.x + x,
              settings->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * 0.5F);
    };
    focusSpeed();
    auto* speedInput = ImGui::GetInputTextState(settings->GetID("Raise/Lower speed (units/s)"));
    check(speedInput != nullptr && std::string_view(speedInput->TextA.Data) == "8192",
          "Settings did not expose the default Raise/Lower speed.");
    editSpeed("4096");
    settingsButton(true);
    check(!dialogOpen(), "Valid Raise/Lower speed did not apply.");
    check(grid.GetSnapshot() == paintedSnapshot, "Changing brush speed regenerated the terrain.");
    selectMenuItem(0, 6, "Settings...");
    focusSpeed();
    speedInput = ImGui::GetInputTextState(settings->GetID("Raise/Lower speed (units/s)"));
    check(speedInput != nullptr && std::string_view(speedInput->TextA.Data) == "4096",
          "Settings did not retain the configured Raise/Lower speed.");
    editSpeed("-1");
    settingsButton(true);
    check(dialogOpen() && grid.GetSnapshot() == paintedSnapshot,
          "Invalid Raise/Lower speed applied or changed the terrain.");
    settingsButton(false);
    check(!dialogOpen(), "Cancel did not close the Settings dialog.");
    selectMenuItem(0, 6, "Settings...");
    focusSpeed();
    speedInput = ImGui::GetInputTextState(settings->GetID("Raise/Lower speed (units/s)"));
    check(speedInput != nullptr && std::string_view(speedInput->TextA.Data) == "4096",
          "Invalid or cancelled speed edits changed the configured speed.");
    settingsButton(false);
    const auto target = grid.GetSnapshot()->FindNearest({0.0, -1.0, 0.0});
    const auto point = grid.GetSnapshot()->GetVertices()[target].position;
    const auto display = ImGui::GetIO().DisplaySize;
    const float horizontal = 1.15F * display.x / display.y;
    const ImVec2 targetScreen((point.x / horizontal + 1.0F) * display.x * 0.5F,
                              (1.0F - point.z / 1.15F) * display.y * 0.5F);
    selectBrushCombo("Operation", 0, 1, "Minimum");
    {
        auto* panel = ImGui::FindWindowByName("###BrushParameters");
        const auto point = panelClick("Outer radius");
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
        click(panel->DC.CursorStartPos.x + panel->ItemWidthDefault * 0.5F, point.y);
        key(ImGuiKey_A);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
        ImGui::GetIO().AddInputCharactersUTF8("1500");
        frame();
        key(ImGuiKey_Enter);
    }
    auto elevationStroke = [&](int heldFrames) {
        ImGui::GetIO().AddMousePosEvent(targetScreen.x, targetScreen.y);
        frame();
        ImGui::GetIO().DeltaTime = 0.5F;
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        for (int held = 0; held < heldFrames; ++held) {
            frame();
        }
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        ImGui::GetIO().DeltaTime = 1.0F / 60.0F;
        std::vector<float> elevations;
        for (const auto& vertex : grid.GetSnapshot()->GetVertices()) {
            elevations.push_back(vertex.elevation);
        }
        shortcut(ImGuiKey_Z);
        return elevations;
    };
    const auto clicked = elevationStroke(1);
    check(clicked[target] < 32767.0F, "Elevation brush click did not change the terrain.");
    check(elevationStroke(6) == clicked,
          "Holding the elevation brush still applied it more than once.");
    check(grid.GetSnapshot()->GetVertices()[target].elevation == 32767.0F,
          "Undo did not restore the elevation brush stroke.");
    selectMenuItem(3, 1, "Raise/Lower Brush");
    selectBrushCombo("Operation", 0, 1, "Lower");
    checkBrushSlider("Intensity (%)", 4, true, 0.999F);
    ImGui::GetIO().AddMousePosEvent(targetScreen.x, targetScreen.y);
    frame();
    ImGui::GetIO().DeltaTime = 0.5F;
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    ImGui::GetIO().DeltaTime = 1.0F / 60.0F;
    check(std::abs(grid.GetSnapshot()->GetVertices()[target].elevation - 30719.0F) < 0.01F,
          "Raise/Lower stroke did not use the speed configured in Settings.");
    shortcut(ImGuiKey_Z);
    check(grid.GetSnapshot()->GetVertices()[target].elevation == 32767.0F,
          "Undo did not restore the Raise/Lower stroke.");
    shortcut(ImGuiKey_Y);
    check(std::abs(grid.GetSnapshot()->GetVertices()[target].elevation - 30719.0F) < 0.01F,
          "Redo did not restore the Raise/Lower stroke.");
    shortcut(ImGuiKey_Z);
    shortcut(ImGuiKey_Z);
    check(grid.GetSnapshot()->GetVertices()[0].elevation == 0.0F,
          "Ctrl+Z did not undo the raster import.");
    shortcut(ImGuiKey_Y);
    check(grid.GetSnapshot()->GetVertices()[0].elevation == 32767.0F,
          "Ctrl+Y did not redo the raster import.");
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

    for (const int tool : {0, 1}) {
        selectMenuItem(2, tool, tool == 0 ? "Radius Selection" : "Polygon Selection");
        const auto restoreSelection = [&] {
            const std::vector<float> weights(grid.GetSnapshot()->GetVertices().size(), 0.25F);
            grid.SetSelectionWeights(weights);
            renderer.UpdateSelection(grid.GetSnapshot()->GetVertices(), true);
        };
        restoreSelection();
        const ImVec2 viewportBackground(1270.0F, 790.0F);
        for (const auto button : {ImGuiMouseButton_Middle, ImGuiMouseButton_Right}) {
            ImGui::GetIO().AddMousePosEvent(viewportBackground.x, viewportBackground.y);
            frame();
            ImGui::GetIO().AddMouseButtonEvent(button, true);
            frame();
            ImGui::GetIO().AddMouseButtonEvent(button, false);
            frame();
            check(grid.GetSnapshot()->HasSelection() &&
                  grid.GetSnapshot()->GetVertices()[0].selectionWeight == 0.25F,
                  "Middle or right click on the viewport background cleared selection.");
        }
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
        click(viewportBackground.x, viewportBackground.y);
        check(grid.GetSnapshot()->HasSelection() &&
              grid.GetSnapshot()->GetVertices()[0].selectionWeight == 0.25F,
              "Shift-click on the viewport background changed selection.");
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
        frame();
        click(viewportBackground.x, viewportBackground.y);
        check(!grid.GetSnapshot()->HasSelection(),
              "A viewport-background click did not clear the selection.");
        for (const auto& vertex : grid.GetSnapshot()->GetVertices()) {
            check(vertex.selectionWeight == 0.0F,
                  "A viewport-background click left nonzero selection weights.");
        }
        shortcut(ImGuiKey_Z);
        check(grid.GetSnapshot()->HasSelection() &&
              grid.GetSnapshot()->GetVertices()[0].selectionWeight == 0.25F,
              "Undo did not restore the selection cleared from the viewport.");
        shortcut(ImGuiKey_Y);
        check(!grid.GetSnapshot()->HasSelection(),
              "Redo did not clear the selection again.");
        check(ui.IsBrushActive(), "Clearing selection deactivated the Select tool.");

        for (const auto position : {ImVec2(1200.0F, 8.0F), ImVec2(-10.0F, 400.0F),
                                    ImVec2(1290.0F, 400.0F)}) {
            restoreSelection();
            const auto beforeClick = grid.GetSnapshot();
            click(position.x, position.y);
            check(grid.GetSnapshot() == beforeClick,
                  "A click on the menu bar or outside the viewport changed selection.");
        }

        restoreSelection();
        const auto beforeMenu = grid.GetSnapshot();
        selectMenuItem(2, 2, "By value...");
        check(dialogOpen() && grid.GetSnapshot() == beforeMenu,
              "Opening a menu item changed the selection.");
        key(ImGuiKey_Escape);
        check(!dialogOpen() && grid.GetSnapshot() == beforeMenu,
              "Cancelling a menu dialog changed the selection.");

        auto* panel = ImGui::FindWindowByName("###SelectionOptions");
        check(panel != nullptr && panel->Active, "Selection options panel is not visible.");
        const float height = ImGui::GetFrameHeight();
        const ImVec2 widgetPosition(
            panel->DC.CursorStartPos.x + panel->ItemWidthDefault * 0.25F,
            panel->DC.CursorStartPos.y +
                (tool == 0 ? 2.0F * (height + ImGui::GetStyle().ItemSpacing.y) : 0.0F) +
                height * 0.5F);
        ImGui::GetIO().AddMousePosEvent(widgetPosition.x, widgetPosition.y);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        if (tool == 0) {
            check(ImGui::GetCurrentContext()->ActiveId == panel->GetID("Falloff (%)"),
                  "Selection falloff slider was not activated.");
        }
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        check(grid.GetSnapshot() == beforeMenu,
              "Clicking the selection options panel changed the selection.");
        selectMenuItem(2, 3, "Clear Selection");
        check(!grid.GetSnapshot()->HasSelection(),
              "The Clear Selection menu command did not clear the selection.");
        shortcut(ImGuiKey_Z);
        check(grid.GetSnapshot()->HasSelection() &&
              grid.GetSnapshot()->GetVertices()[0].selectionWeight == 0.25F,
              "Undo did not restore the selection cleared by the menu command.");
    }

    for (const int tool : {0, 1}) {
        for (const bool additive : {false, true}) {
            selectMenuItem(2, tool, tool == 0 ? "Radius Selection" : "Polygon Selection");
            const std::vector<float> original(grid.GetSnapshot()->GetVertices().size(), 0.25F);
            grid.SetSelectionWeights(original);
            renderer.UpdateSelection(grid.GetSnapshot()->GetVertices(), true);
            const auto globeClick = [&](float x, float y) {
                for (int i = 0; i < 25; ++i) {
                    frame();
                }
                click(x, y);
            };
            ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, additive);
            globeClick(640.0F, 350.0F);
            ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
            frame();
            if (tool == 1) {
                globeClick(700.0F, 450.0F);
                selectMenuItem(2, 2, "By value...");
                key(ImGuiKey_Escape);
                globeClick(580.0F, 450.0F);
                key(ImGuiKey_Enter);
            }
            const auto selected = grid.GetSnapshot();
            bool added = false;
            bool outside = false;
            for (const auto& vertex : selected->GetVertices()) {
                added |= vertex.selectionWeight > 0.25F;
                outside |= vertex.selectionWeight == (additive ? 0.25F : 0.0F);
                if (additive) {
                    check(vertex.selectionWeight >= 0.25F,
                          "Shift selection reduced an existing selection weight.");
                }
            }
            check(added && outside, "Selection did not preserve the requested add/replace mode.");
            shortcut(ImGuiKey_Z);
            for (const auto& vertex : grid.GetSnapshot()->GetVertices()) {
                check(vertex.selectionWeight == 0.25F,
                      "Undo did not restore the selection before add/replace.");
            }
            shortcut(ImGuiKey_Y);
            const auto redone = grid.GetSnapshot();
            for (std::size_t i = 0; i < original.size(); ++i) {
                check(redone->GetVertices()[i].selectionWeight ==
                          selected->GetVertices()[i].selectionWeight,
                      "Redo did not restore the add/replace selection.");
            }
            if (tool == 0) {
                selectMenuItem(2, 0, "Radius Selection");
            }
        }
    }

    std::vector<float> valueElevations(grid.GetSnapshot()->GetVertices().size(), 0.0F);
    valueElevations[0] = -100.0F;
    valueElevations[1] = -75.0F;
    valueElevations[2] = 50.0F;
    valueElevations[3] = 100.0F;
    valueElevations[4] = 120.0F;
    grid.SetElevations(valueElevations);
    renderer.UpdateElevations(grid.GetSnapshot()->GetVertices());
    const auto beforeValueSelection = grid.GetSnapshot();
    const auto valueDialog = [&] {
        auto* dialog = ImGui::FindWindowByName("Select By Value");
        check(dialog != nullptr && dialog->Active, "By value dialog did not open.");
        return dialog;
    };
    const auto editValueBound = [&](int row, const char* text) {
        auto* dialog = valueDialog();
        click(dialog->DC.CursorStartPos.x + ImGui::CalcTextSize("Between").x +
                  ImGui::GetStyle().ItemSpacing.x + 30.0F,
              dialog->DC.CursorStartPos.y + row * rowHeight +
                  ImGui::GetFrameHeight() * 0.5F);
        shortcut(ImGuiKey_A);
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
        key(ImGuiKey_Enter);
    };
    const auto valueDialogButton = [&](bool ok) {
        frame();
        frame();
        auto* dialog = valueDialog();
        const float okWidth = ImGui::CalcTextSize("OK").x +
                              ImGui::GetStyle().FramePadding.x * 2.0F;
        const float x = ok ? okWidth * 0.5F :
            okWidth + ImGui::GetStyle().ItemSpacing.x +
                ImGui::CalcTextSize("Cancel").x * 0.5F + ImGui::GetStyle().FramePadding.x;
        click(dialog->DC.CursorStartPos.x + x,
              dialog->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * 0.5F);
    };
    selectMenuItem(2, 2, "By value...");
    editValueBound(0, "-100");
    editValueBound(1, "100");
    valueDialogButton(false);
    check(!dialogOpen() && grid.GetSnapshot() == beforeValueSelection,
          "Cancelling By value changed the model or left the dialog open.");
    selectMenuItem(2, 2, "By value...");
    editValueBound(0, "40000");
    valueDialogButton(true);
    check(dialogOpen() && grid.GetSnapshot() == beforeValueSelection,
          "Invalid value bounds changed selection or closed the dialog.");
    editValueBound(0, "100");
    editValueBound(1, "-100");
    valueDialogButton(true);
    check(!dialogOpen(), "OK did not close the By value dialog.");
    const auto byValue = grid.GetSnapshot();
    check(byValue->HasSelection() &&
          byValue->GetVertices()[0].selectionWeight == 0.0F &&
          byValue->GetVertices()[1].selectionWeight == 0.5F &&
          byValue->GetVertices()[2].selectionWeight == 1.0F &&
          byValue->GetVertices()[3].selectionWeight == 0.0F &&
          byValue->GetVertices()[4].selectionWeight == 0.0F,
          "By value dialog did not swap reversed bounds and apply the requested range.");
    shortcut(ImGuiKey_Z);
    check(grid.GetSnapshot()->HasSelection() == beforeValueSelection->HasSelection(),
          "Undo did not restore the previous selection active state.");
    for (std::size_t i = 0; i < valueElevations.size(); ++i) {
        check(grid.GetSnapshot()->GetVertices()[i].selectionWeight ==
                  beforeValueSelection->GetVertices()[i].selectionWeight &&
              grid.GetSnapshot()->GetVertices()[i].elevation == valueElevations[i],
              "Undo By value did not restore selection or changed elevation.");
    }
    shortcut(ImGuiKey_Y);
    for (std::size_t i = 0; i < valueElevations.size(); ++i) {
        check(grid.GetSnapshot()->GetVertices()[i].selectionWeight ==
                  byValue->GetVertices()[i].selectionWeight,
              "Redo did not restore By value selection.");
    }
    selectMenuItem(2, 2, "By value...");
    editValueBound(0, "-100");
    auto* betweenInput = ImGui::GetInputTextState(valueDialog()->GetID("##LowerBound"));
    check(betweenInput != nullptr, "Between label did not precede the lower-bound input.");
    valueDialogButton(false);

    const auto fillDialog = [&] {
        auto* dialog = ImGui::FindWindowByName("Fill Filter");
        check(dialog != nullptr && dialog->Active, "Fill filter dialog did not open.");
        return dialog;
    };
    const auto setFillValueToMaximum = [&] {
        auto* dialog = fillDialog();
        ImGui::GetIO().AddMousePosEvent(
            dialog->DC.CursorStartPos.x + dialog->ItemWidthDefault - 1.0F,
            dialog->DC.CursorStartPos.y + rowHeight + ImGui::GetFrameHeight() * 0.5F);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        check(ImGui::GetCurrentContext()->ActiveId == dialog->GetID("Fill value"),
              "Fill value is not controlled by an interactive slider.");
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
    };
    const auto fillDialogApply = [&] {
        frame();
        frame();
        auto* dialog = fillDialog();
        const float buttonWidth = ImGui::CalcTextSize("Apply").x +
                                  ImGui::GetStyle().FramePadding.x * 2.0F;
        click(dialog->DC.CursorStartPos.x + buttonWidth * 0.5F,
              dialog->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * 0.5F);
    };
    selectMenuItem(4, 0, "Fill...");
    setFillValueToMaximum();
    fillDialogApply();
    check(!dialogOpen(), "Valid Fill filter did not close its dialog.");
    const auto afterFill = grid.GetSnapshot();
    check(afterFill->GetVertices()[0].elevation == valueElevations[0] &&
          afterFill->GetVertices()[1].elevation == 16346.0F &&
          afterFill->GetVertices()[2].elevation == 32767.0F &&
          afterFill->GetVertices()[4].elevation == valueElevations[4],
          "Fill filter dialog did not blend using selection weights.");
    shortcut(ImGuiKey_Z);
    check(grid.GetSnapshot()->GetVertices()[1].elevation == valueElevations[1] &&
          grid.GetSnapshot()->GetVertices()[2].elevation == valueElevations[2],
          "Undo did not restore terrain after Fill filter.");
    shortcut(ImGuiKey_Y);
    check(grid.GetSnapshot()->GetVertices()[1].elevation == 16346.0F &&
          grid.GetSnapshot()->GetVertices()[2].elevation == 32767.0F,
          "Redo did not reapply the Fill filter.");

    const auto noiseDialog = [&] {
        auto* dialog = ImGui::FindWindowByName("Absolute Magnitude Noise");
        check(dialog != nullptr && dialog->Active, "Absolute Magnitude Noise dialog did not open.");
        return dialog;
    };
    const auto setNoiseAmplitude = [&](bool maximum) {
        auto* dialog = noiseDialog();
        click(dialog->DC.CursorStartPos.x + (maximum ? dialog->ItemWidthDefault - 1.0F : 1.0F),
              dialog->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5F);
    };
    const auto noiseDialogButton = [&](bool apply) {
        frame();
        frame();
        auto* dialog = noiseDialog();
        const float width = ImGui::CalcTextSize("Apply").x +
                            ImGui::GetStyle().FramePadding.x * 2.0F;
        const float x = apply ? width * 0.5F :
            width + ImGui::GetStyle().ItemSpacing.x +
                ImGui::CalcTextSize("Cancel").x * 0.5F + ImGui::GetStyle().FramePadding.x;
        click(dialog->DC.CursorStartPos.x + x,
              dialog->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * 0.5F);
    };
    const auto beforeNoiseDialog = grid.GetSnapshot();
    selectMenuItem(4, 2, "Absolute Magnitude Noise...");
    setNoiseAmplitude(true);
    noiseDialogButton(false);
    check(!dialogOpen() && grid.GetSnapshot() == beforeNoiseDialog,
          "Cancelling noise changed terrain or left the dialog open.");
    selectMenuItem(4, 2, "Absolute Magnitude Noise...");
    setNoiseAmplitude(false);
    noiseDialogButton(true);
    check(!dialogOpen() && grid.GetSnapshot() == beforeNoiseDialog,
          "Zero-amplitude noise changed terrain.");
    std::vector<float> noiseTerrain(grid.GetSnapshot()->GetVertices().size(), -1000.0F);
    std::vector<float> noiseWeights(noiseTerrain.size());
    for (std::size_t i = 0; i < noiseWeights.size(); ++i) {
        noiseWeights[i] = i % 3 == 0 ? 0.0F : i % 3 == 1 ? 0.25F : 1.0F;
    }
    for (const bool selected : {false, true}) {
        grid.SetElevations(noiseTerrain);
        grid.SetSelectionWeights(noiseWeights, selected);
        renderer.UploadGrid(*grid.GetSnapshot());
        const auto before = grid.GetSnapshot();
        selectMenuItem(4, 2, "Absolute Magnitude Noise...");
        check(grid.GetSnapshot() == before, "Opening noise applied random variation before Apply.");
        setNoiseAmplitude(true);
        noiseDialogButton(true);
        check(!dialogOpen(), "Noise Apply did not close the dialog.");
        const auto after = grid.GetSnapshot();
        bool positive = false;
        bool negative = false;
        for (std::size_t i = 0; i < noiseTerrain.size(); ++i) {
            const float delta = after->GetVertices()[i].elevation - noiseTerrain[i];
            const double weight = selected ? noiseWeights[i] : 1.0;
            check(std::abs(delta) <= 152.4 * weight + 0.0001,
                  "Noise dialog ignored its amplitude, feet conversion, or selection feathering.");
            positive |= delta > 0.0F;
            negative |= delta < 0.0F;
            check(after->GetVertices()[i].selectionWeight == before->GetVertices()[i].selectionWeight,
                  "Noise changed selection weights.");
        }
        check(positive && negative, "Noise did not apply signed variation.");
        shortcut(ImGuiKey_Z);
        for (std::size_t i = 0; i < noiseTerrain.size(); ++i) {
            check(grid.GetSnapshot()->GetVertices()[i].elevation == noiseTerrain[i],
                  "Undo did not restore terrain before noise.");
        }
        shortcut(ImGuiKey_Y);
        for (std::size_t i = 0; i < noiseTerrain.size(); ++i) {
            check(grid.GetSnapshot()->GetVertices()[i].elevation == after->GetVertices()[i].elevation,
                  "Redo regenerated noise instead of restoring the exact result.");
        }
        check(grid.GetSnapshot()->HasSelection() == selected, "Noise changed selection state.");
        renderer.Draw(256, 256);
        check(glGetError() == GL_NO_ERROR, "Rendering noise produced an OpenGL error.");
    }

    const auto selectErosionItem = [&](int row, const char* label) {
        selectMenuItem(4, 1, "Erosion");
        frame();
        auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
        check(popups.Size >= 2 && popups.back().Window != nullptr,
              "Filter Erosion did not open a submenu.");
        auto* menu = popups.back().Window;
        ImGui::GetIO().AddMousePosEvent(menu->DC.CursorStartPos.x + 30.0F,
            menu->DC.CursorStartPos.y + row * ImGui::GetTextLineHeightWithSpacing() +
            ImGui::GetTextLineHeight() * 0.5F);
        frame();
        check(ImGui::GetCurrentContext()->HoveredId == menu->GetID(label),
              "Erosion submenu did not contain the expected filter.");
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        frame();
    };
    const auto fillBasins = [&] { selectErosionItem(0, "Fill basins..."); };
    std::vector<float> basinTerrain(grid.GetSnapshot()->GetVertices().size(), -20.0F);
    const auto& basinEdges = grid.GetSnapshot()->GetHalfEdges();
    for (const auto& edge : basinEdges) {
        if (edge.originVertex == 0) {
            basinTerrain[static_cast<std::size_t>(
                basinEdges[static_cast<std::size_t>(edge.nextEdge)].originVertex)] = 100.0F;
        }
    }
    basinTerrain[0] = 1.0F;
    std::vector<float> basinSelection(basinTerrain.size(), 0.0F);
    for (const bool selected : {false, true}) {
        basinSelection[0] = selected ? 0.5F : 0.0F;
        grid.SetSelectionWeights(basinSelection, selected);
        grid.SetElevations(basinTerrain);
        renderer.UploadGrid(*grid.GetSnapshot());
        fillBasins();
        check(!dialogOpen(), "Fill basins should run immediately without a settings dialog.");
        const auto filled = grid.GetSnapshot();
        const float expected = selected ? 1.0F + ((100.0F + 1.0e-4F) - 1.0F) * 0.5F
                                        : 100.0F + 1.0e-4F;
        check(filled->GetVertices()[0].elevation == expected &&
              filled->GetVertices()[1].elevation == basinTerrain[1],
              "Fill basins menu did not fill the basin using the current selection.");
        shortcut(ImGuiKey_Z);
        for (std::size_t i = 0; i < basinTerrain.size(); ++i) {
            check(grid.GetSnapshot()->GetVertices()[i].elevation == basinTerrain[i],
                  "Undo did not restore the terrain before Fill basins.");
        }
        shortcut(ImGuiKey_Y);
        check(grid.GetSnapshot()->GetVertices()[0].elevation == expected &&
              grid.GetSnapshot()->HasSelection() == selected &&
              grid.GetSnapshot()->GetVertices()[0].selectionWeight == basinSelection[0],
              "Redo did not restore filled basins or changed the selection.");
        renderer.Draw(256, 256);
        check(glGetError() == GL_NO_ERROR, "Rendering filled basins produced an OpenGL error.");
    }
    std::fill(basinTerrain.begin(), basinTerrain.end(), 32767.0F);
    basinTerrain[0] = 1.0F;
    grid.SetElevations(basinTerrain);
    renderer.UploadGrid(*grid.GetSnapshot());
    fillBasins();
    const auto* basinError = ImGui::FindWindowByName("Fill Basins Error");
    check(dialogOpen() && basinError != nullptr && basinError->Active,
          "Fill basins did not display an error for unrepresentable drainage.");
    for (std::size_t i = 0; i < basinTerrain.size(); ++i) {
        check(grid.GetSnapshot()->GetVertices()[i].elevation == basinTerrain[i],
              "Failed Fill basins partially modified terrain.");
    }
    key(ImGuiKey_Escape);
    check(!dialogOpen(), "Fill basins error could not be dismissed.");

    const auto inciseDialog = [&] {
        auto* dialog = ImGui::FindWindowByName("Incise Flow");
        check(dialog != nullptr && dialog->Active, "Incise Flow parameter dialog did not open.");
        return dialog;
    };
    const auto editInciseParameter = [&](int row, const char* label, const char* text) {
        auto* dialog = inciseDialog();
        click(dialog->DC.CursorStartPos.x + 30.0F,
              dialog->DC.CursorStartPos.y + row * rowHeight + ImGui::GetFrameHeight() * 0.5F);
        check(ImGui::GetCurrentContext()->ActiveId == dialog->GetID(label),
              "Incise Flow parameter is not an editable numeric input.");
        shortcut(ImGuiKey_A);
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
        key(ImGuiKey_Enter);
    };
    const auto inciseDialogButton = [&](bool apply) {
        frame();
        frame();
        auto* dialog = inciseDialog();
        const float applyWidth = ImGui::CalcTextSize("Apply").x +
                                 ImGui::GetStyle().FramePadding.x * 2.0F;
        const float x = apply ? applyWidth * 0.5F :
            applyWidth + ImGui::GetStyle().ItemSpacing.x +
                ImGui::CalcTextSize("Cancel").x * 0.5F + ImGui::GetStyle().FramePadding.x;
        click(dialog->DC.CursorStartPos.x + x,
              dialog->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * 0.5F);
    };
    std::vector<float> incisionTerrain(basinTerrain.size(), -20.0F);
    for (std::size_t i = 0; i < incisionTerrain.size(); ++i) {
        if (i % 7 != 0) incisionTerrain[i] = 100.0F + static_cast<float>((i * 193) % 4000);
    }
    grid.SetElevations(incisionTerrain);
    renderer.UploadGrid(*grid.GetSnapshot());
    const auto beforeIncisionDialog = grid.GetSnapshot();
    selectErosionItem(1, "Incise flow...");
    editInciseParameter(0, "Incision rate (m/pass)", "20");
    inciseDialogButton(false);
    check(!dialogOpen() && grid.GetSnapshot() == beforeIncisionDialog,
          "Cancelling Incise Flow changed terrain or left the dialog open.");
    selectErosionItem(1, "Incise flow...");
    editInciseParameter(1, "Reference area (runoff)", "0");
    inciseDialogButton(true);
    check(dialogOpen() && grid.GetSnapshot() == beforeIncisionDialog,
          "Invalid Incise Flow parameters changed terrain or closed the dialog.");
    key(ImGuiKey_Escape);
    check(!dialogOpen(), "Escape did not cancel the incision dialog.");

    grid.SetPlanetRadius(3000.0);
    const InciseFlowSettings incisionSettings{8.0, 4.0, 0.6, 1.2, 2.0};
    InciseFlowFilter incisionFilter;
    incisionFilter.SetSettings(incisionSettings);
    std::vector<float> incisionWeights(incisionTerrain.size(), 0.0F);
    for (std::size_t i = 0; i < incisionWeights.size(); ++i) {
        incisionWeights[i] = i % 5 == 0 ? 0.0F : i % 3 == 0 ? 0.5F : 1.0F;
    }
    for (const bool selected : {false, true}) {
        grid.SetSelectionWeights(incisionWeights, selected);
        grid.SetElevations(incisionTerrain);
        renderer.UploadGrid(*grid.GetSnapshot());
        const auto before = grid.GetSnapshot();
        const auto expected = incisionFilter.Apply(*before, incisionTerrain);
        selectErosionItem(1, "Incise flow...");
        check(grid.GetSnapshot() == before, "Opening Incise Flow ran the filter before Apply.");
        if (!selected) {
            editInciseParameter(0, "Incision rate (m/pass)", "8");
            editInciseParameter(1, "Reference area (runoff)", "4");
            editInciseParameter(2, "Flow exponent (m)", "0.6");
            editInciseParameter(3, "Slope exponent (n)", "1.2");
            editInciseParameter(4, "Precipitation (per vertex)", "2");
        }
        inciseDialogButton(true);
        check(!dialogOpen(), "Valid Incise Flow did not close the dialog.");
        const auto checkIncision = [&](const std::vector<float>& values) {
            const auto snapshot = grid.GetSnapshot();
            for (std::size_t i = 0; i < values.size(); ++i) {
                check(snapshot->GetVertices()[i].elevation == values[i],
                      "Incise Flow dialog, undo or redo produced incorrect elevations.");
                check(snapshot->GetVertices()[i].selectionWeight == before->GetVertices()[i].selectionWeight,
                      "Incise Flow changed selection weights.");
            }
            check(snapshot->HasSelection() == selected, "Incise Flow changed selection state.");
        };
        checkIncision(expected);
        shortcut(ImGuiKey_Z);
        checkIncision(incisionTerrain);
        shortcut(ImGuiKey_Y);
        checkIncision(expected);
        renderer.Draw(256, 256);
        check(glGetError() == GL_NO_ERROR, "Rendering incised flow produced an OpenGL error.");
    }
    grid.SetPlanetRadius(0.001);
    const auto beforeOverflow = grid.GetSnapshot();
    selectErosionItem(1, "Incise flow...");
    editInciseParameter(0, "Incision rate (m/pass)", "1e308");
    inciseDialogButton(true);
    check(dialogOpen() && grid.GetSnapshot() == beforeOverflow,
          "Failed incision did not keep its dialog open and leave terrain unchanged.");
    key(ImGuiKey_Escape);
    check(!dialogOpen(), "Could not dismiss incision after numeric overflow.");
    grid.SetPlanetRadius(6371.0);

        std::vector<float> meshSelection(grid.GetSnapshot()->GetVertices().size(), 1.0F);
        std::vector<float> uniformTerrain(meshSelection.size(), 250.0F);
        grid.SetSelectionWeights(meshSelection, true);
        grid.SetElevations(uniformTerrain);
        renderer.UploadGrid(*grid.GetSnapshot());
        const auto beforeRefinement = grid.GetSnapshot()->GetVertices().size();
        selectMenuItem(5, 0, "Refine Selection");
        const auto afterRefinement = grid.GetSnapshot()->GetVertices().size();
        check(afterRefinement > beforeRefinement * 2,
            "Mesh menu did not refine the selected region.");
        shortcut(ImGuiKey_Z);
        check(grid.GetSnapshot()->GetVertices().size() == beforeRefinement,
            "Undo did not restore the mesh before refinement.");
        shortcut(ImGuiKey_Y);
            check(grid.GetSnapshot()->GetVertices().size() == afterRefinement,
            "Redo did not restore the refined mesh.");
        selectMenuItem(5, 1, "Coarsen Flat Selection");
                check(grid.GetSnapshot()->GetVertices().size() == beforeRefinement,
                    "Coarsening after refinement did not restore the prior sampling density.");
        shortcut(ImGuiKey_Z);
            check(grid.GetSnapshot()->GetVertices().size() == afterRefinement,
            "Undo did not restore the mesh before coarsening.");

    const auto selectExport = [&](bool image = false) {
        selectMenuItem(0, 3, "Export");
        frame();
        const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
        check(popups.Size >= 2 && popups.back().Window != nullptr,
              "File Export did not open a submenu.");
        auto* menu = popups.back().Window;
        const auto* label = image ? "Export Image..." : "Export data...";
        const float x = menu->DC.CursorStartPos.x + 20;
        const float y = menu->DC.CursorStartPos.y +
            (image ? ImGui::GetTextLineHeightWithSpacing() : 0) +
            ImGui::GetTextLineHeight() * 0.5F;
        ImGui::GetIO().AddMousePosEvent(x, y);
        frame();
        check(ImGui::GetCurrentContext()->HoveredId == menu->GetID(label),
              "Export submenu is missing the requested export action.");
        click(x, y);
        frame();
    };
    const auto clickDialogItem = [&](const char* windowName, const char* label) {
        frame();
        auto* dialog = ImGui::FindWindowByName(windowName);
        check(dialog && dialog->Active, "Expected project dialog is not active.");
        auto id = dialog->GetID(label);
        if (std::string_view(windowName) == "Planet" &&
            std::string_view(label) != "Apply" && std::string_view(label) != "Cancel") {
            const auto* tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(dialog->GetID("PlanetTabs"));
            check(tabs != nullptr, "Planet tabs were not rendered.");
            id = ImHashStr(label, 0, tabs->SelectedTabId);
        }
        const float x = dialog->DC.CursorStartPos.x + 12;
        const float firstY = dialog->DC.CursorStartPos.y;
        const float lastY = dialog->Pos.y + dialog->Size.y - 8;
        for (float y = firstY; y < lastY; y += 4) {
            ImGui::GetIO().AddMousePosEvent(x, y);
            frame();
            if (ImGui::GetCurrentContext()->HoveredId == id) {
                click(x, y);
                return;
            }
        }
        throw std::runtime_error(std::string("Project dialog item was not interactive: ") + label);
    };
    const auto replaceDialogText = [&](const char* window, const char* label, const char* text) {
        clickDialogItem(window, label);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
        key(ImGuiKey_A);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
        key(ImGuiKey_Tab);
    };
    const auto planetTab = [&](const char* label) {
        frame();
        auto* dialog = ImGui::FindWindowByName("Planet");
        check(dialog && dialog->Active, "Planet menu did not open its dialog.");
        const auto barId = dialog->GetID("PlanetTabs");
        const auto tabId = ImHashStr(label, 0, barId);
        const float y = dialog->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5F;
        for (float x = dialog->DC.CursorStartPos.x + 4; x < dialog->InnerRect.Max.x; x += 4) {
            ImGui::GetIO().AddMousePosEvent(x, y);
            frame();
            if (ImGui::GetCurrentContext()->HoveredId == tabId) {
                click(x, y);
                frame();
                return;
            }
        }
        throw std::runtime_error(std::string("Planet tab is not interactive: ") + label);
    };
    const auto beforePlanet = grid.GetSnapshot();
    selectMenuItem(0, 4, "Planet...");
    planetTab("Geometry");
    replaceDialogText("Planet", "Planet radius (km)", "0");
    clickDialogItem("Planet", "Apply");
    check(dialogOpen() && grid.GetSnapshot() == beforePlanet,
          "Invalid planet radius was accepted or changed terrain.");
    replaceDialogText("Planet", "Planet radius (km)", "3000");
    planetTab("Metadata");
    replaceDialogText("Planet", "Description", "Project description");
    replaceDialogText("Planet", "Author", "Project author");
    clickDialogItem("Planet", "Apply");
    check(grid.GetMetadata().author == "Project author" &&
          grid.GetMetadata().description == "Project description" &&
          grid.GetMetadata().savedAt.empty(),
          "Metadata dialog did not apply author/description or changed the save date.");
    check(grid.GetSnapshot()->GetOptions().planetRadius == 3000.0 &&
          grid.GetSnapshot()->GetTriangleIndices() == beforePlanet->GetTriangleIndices(),
          "Planet geometry did not apply radius without regenerating the grid.");
    for (std::size_t i = 0; i < beforePlanet->GetVertices().size(); ++i) {
        const auto& before = beforePlanet->GetVertices()[i];
        const auto& after = grid.GetSnapshot()->GetVertices()[i];
        check(before.position == after.position && before.elevation == after.elevation &&
              before.selectionWeight == after.selectionWeight,
              "Changing planet radius modified terrain or selection.");
    }
    shortcut(ImGuiKey_Z);
    check(grid.GetSnapshot()->GetOptions().planetRadius == 3000.0,
          "Undoing a mesh edit reverted the planet radius.");
    shortcut(ImGuiKey_Y);
    check(grid.GetSnapshot()->GetOptions().planetRadius == 3000.0,
          "Redoing a mesh edit reverted the planet radius.");
    selectMenuItem(0, 4, "Planet...");
    planetTab("Metadata");
    replaceDialogText("Planet", "Author", "Cancelled author");
    planetTab("Geometry");
    replaceDialogText("Planet", "Planet radius (km)", "2000");
    key(ImGuiKey_Escape);
    check(grid.GetMetadata().author == "Project author" &&
          grid.GetSnapshot()->GetOptions().planetRadius == 3000.0,
          "Cancelling metadata edits changed the project.");
    shortcut(ImGuiKey_S);
    check(ImGui::FindWindowByName("Project File")->Active,
          "Saving an untitled project did not request a filename.");
    replaceDialogText("Project File", "File name", "ui.oswald");
    clickDialogItem("Project File", "Save");
    check(!dialogOpen() && std::filesystem::exists(temporary.path / "ui.oswald"),
          "Save dialog failed to write the selected project.");
    const auto savedSnapshot = grid.GetSnapshot();
    auto saved = ProjectFile::Load(temporary.path / "ui.oswald");
    check(saved->GetMetadata().author == "Project author" &&
          saved->GetSnapshot()->GetOptions().planetRadius == 3000.0 &&
          !saved->GetMetadata().savedAt.empty(),
          "UI save failed to persist metadata and the save date.");
    grid.GetMetadata().author = "Updated author";
    shortcut(ImGuiKey_S);
    check(!dialogOpen() &&
          ProjectFile::Load(temporary.path / "ui.oswald")->GetMetadata().author == "Updated author",
          "Save did not reuse the existing project filename.");
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    shortcut(ImGuiKey_S);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    frame();
    clickDialogItem("Project File", "Save");
    check(ImGui::FindWindowByName("Overwrite File?") &&
          ImGui::FindWindowByName("Overwrite File?")->Active,
          "Save As did not ask before replacing an existing file.");
    key(ImGuiKey_Escape);
    check(ImGui::FindWindowByName("Project File")->Active,
          "Cancelling overwrite unexpectedly closed Save As.");
    replaceDialogText("Project File", "File name", "copy.oswald");
    clickDialogItem("Project File", "Save");
    check(std::filesystem::exists(temporary.path / "copy.oswald"), "Save As did not write a copy.");
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    shortcut(ImGuiKey_S);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    frame();
    clickDialogItem("Project File", "Save");
    clickDialogItem("Overwrite File?", "Replace");
    check(!dialogOpen() && ProjectFile::Load(temporary.path / "copy.oswald")->GetMetadata().author ==
          "Updated author", "Confirmed overwrite did not save and close both dialogs.");
    shortcut(ImGuiKey_O);
    replaceDialogText("Project File", "File name", "images/broken.png");
    clickDialogItem("Project File", "Open");
    check(dialogOpen() && grid.GetSnapshot() == savedSnapshot,
          "Opening an invalid project closed the dialog or changed the model.");
    replaceDialogText("Project File", "File name", "ui.oswald");
    clickDialogItem("Project File", "Open");
    check(!dialogOpen() && grid.GetMetadata().author == "Updated author" &&
          grid.GetSnapshot()->GetVertices().size() == savedSnapshot->GetVertices().size(),
          "Project Open did not restore the saved model and metadata.");
    const auto openedSnapshot = grid.GetSnapshot();
    shortcut(ImGuiKey_Z);
    check(grid.GetSnapshot() == openedSnapshot, "Project Open retained the previous undo history.");
    selectExport();
    replaceDialogText("Project File", "File name", "ui.png");
    replaceDialogText("Project File", "Width (pixels)", "64");
    clickDialogItem("Project File", "Export");
    check(!dialogOpen() && std::filesystem::exists(temporary.path / "ui.png") &&
          ElevationRaster::Load(temporary.path / "ui.png").GetWidth() == 2048,
          "Export dialog did not write a loadable image.");
    const auto tiff = temporary.path / "ui.TIFF";
    const auto tiffOutputs = ElevationRaster::ExportPaths(tiff);
    std::ofstream(tiffOutputs[1]) << "Existing world file";
    selectExport();
    replaceDialogText("Project File", "File name", "ui.TIFF");
    clickDialogItem("Project File", "Export");
    check(ImGui::FindWindowByName("Overwrite File?")->Active && !std::filesystem::exists(tiff),
          "TIFF export did not ask before replacing an existing sidecar alone.");
    key(ImGuiKey_Escape);
    check(ImGui::FindWindowByName("Project File")->Active && !std::filesystem::exists(tiff),
          "Cancelling TIFF overwrite wrote output or closed the export dialog.");
    clickDialogItem("Project File", "Export");
    clickDialogItem("Overwrite File?", "Replace");
    check(!dialogOpen() && ElevationRaster::Load(tiff).GetWidth() == 2048 &&
          std::filesystem::exists(tiffOutputs[1]) && std::filesystem::exists(tiffOutputs[2]),
          "Confirmed TIFF export did not write the image and both georeferencing sidecars.");
    selectExport();
    replaceDialogText("Project File", "File name", "no-extension");
    clickDialogItem("Project File", "Export");
    check(!dialogOpen() && std::filesystem::exists(temporary.path / "no-extension.tif") &&
          std::filesystem::exists(temporary.path / "no-extension.tif.aux.xml"),
          "TIFF filename editing did not retain the selected export format.");
    check(grid.GetSnapshot() == openedSnapshot, "TIFF export changed the project.");
    grid.SetElevations(std::vector<float>(grid.GetSnapshot()->GetVertices().size(), -1234.75F));
    const auto precisionSnapshot = grid.GetSnapshot();
    renderer.UploadGrid(*grid.GetSnapshot());
    for (int type = 0; type < 3; ++type) {
        selectExport();
        clickDialogItem("Project File", "TIFF samples");
        frame();
        ImGuiWindow* sampleCombo = nullptr;
        for (auto* window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active && std::string_view(window->Name).starts_with("##Combo_")) {
                sampleCombo = window;
                break;
            }
        }
        check(sampleCombo != nullptr, "TIFF sample selection did not open a combo.");
        click(sampleCombo->DC.CursorStartPos.x + 10,
              sampleCombo->DC.CursorStartPos.y + type * ImGui::GetTextLineHeightWithSpacing() +
              ImGui::GetTextLineHeight() * 0.5F);
        const auto name = std::string("sample-type-") + std::to_string(type) + ".tif";
        replaceDialogText("Project File", "File name", name.c_str());
        clickDialogItem("Project File", "Export");
        check(!dialogOpen(), "Choosing TIFF sample precision did not allow export.");
        const auto values = ElevationRaster::Load(temporary.path / name)
                                .SampleElevations(*grid.GetSnapshot());
        const float expected = type == 0 ?
            -32767.0F + std::round((-1234.75F + 32767.0F) / 65534.0F * 255) / 255 * 65534.0F :
            type == 1 ? -1235.0F : -1234.75F;
        check(std::abs(values.front() - expected) < 0.001F,
              "TIFF precision combo did not export the chosen sample representation.");
    }
    check(grid.GetSnapshot() == precisionSnapshot, "TIFF precision export changed the project.");
    grid.SetSelectionWeights(std::vector<float>(grid.GetSnapshot()->GetVertices().size(), 0.25F), true);
    const auto imageSnapshot = grid.GetSnapshot();
    renderer.UploadGrid(*imageSnapshot);
    const auto& activeLayer = grid.GetActiveLayer();
    const auto maps = std::filesystem::path(OSWALD_RESOURCE_DIR) / "assets/colormaps";
    const auto map = ColorMap::Load(maps / activeLayer.GetColorMap());
    const auto selectionMap = ColorMap::Load(maps / activeLayer.GetSelectionColorMap());
    const auto exportedImage = temporary.path / "map-image.png";
    const auto expectedImage = temporary.path / "expected-image.png";
    const auto checkExportedImage = [&](bool selection) {
        ElevationRaster::ExportImage(expectedImage, grid, map, selection ? &selectionMap : nullptr, 64, 32);
        std::ifstream actual(exportedImage, std::ios::binary);
        std::ifstream expected(expectedImage, std::ios::binary);
        check(actual && expected &&
              std::string((std::istreambuf_iterator<char>(actual)), {}) ==
              std::string((std::istreambuf_iterator<char>(expected)), {}),
              "Image export dialog did not use active colourmaps and the selection option.");
    };
    selectExport(true);
    replaceDialogText("Project File", "File name", "map-image.png");
    clickDialogItem("Project File", "Export");
    check(!dialogOpen(), "Export Image did not write and close its dialog.");
    checkExportedImage(false);
    selectExport(true);
    clickDialogItem("Project File", "Include selection highlighting");
    replaceDialogText("Project File", "File name", "map-image.png");
    clickDialogItem("Project File", "Export");
    check(ImGui::FindWindowByName("Overwrite File?")->Active,
          "Image export did not confirm replacement of an existing image.");
    clickDialogItem("Overwrite File?", "Replace");
    check(!dialogOpen(), "Confirmed image export did not close its dialogs.");
    checkExportedImage(true);
    selectExport();
    replaceDialogText("Project File", "File name", "data-after-image.tif");
    clickDialogItem("Project File", "Export");
    check(!dialogOpen() && ElevationRaster::Load(temporary.path / "data-after-image.tif")
              .SampleElevations(*imageSnapshot).front() == -1234.75F,
          "Image export changed the data export precision or output mode.");
    check(grid.GetSnapshot() == imageSnapshot, "Image export changed the project or selection.");
    selectMenuItem(0, 0, "Open");
    check(ImGui::FindWindowByName("Project File")->Active,
          "File menu Open did not open the project browser.");
    key(ImGuiKey_Escape);
    check(grid.GetSnapshot() == imageSnapshot, "Cancelling Open changed the project.");

    // Scripted brush library: load, save, overwrite, export and import.
    const auto library = temporary.path / "brushes";
    std::filesystem::create_directories(library);
    BrushScriptFile{"ridges", "Ridged relief", "ridged(x, y, z, 3, 2, 0.5, 5)"}
        .Save(library / "ridges.toml");
    BrushScriptFile{"imported", "From elsewhere", "simplex(x, y, z)\n* 0.5",
                    ScriptedBrushSettings::Output::Elevation}
        .Save(temporary.path / "outside.toml");
    ui.SetBrushScriptDirectory(library);
    selectMenuItem(3, 8, "Custom");
    ImGui::SetWindowPos("###BrushParameters", ImVec2(40.0F, 40.0F));
    ImGui::SetWindowSize("###BrushParameters", ImVec2(560.0F, 720.0F));
    const auto replacePanelText = [&](const char* label, const char* text, int parameterIndex = -1) {
        panelClick(label, parameterIndex);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
        key(ImGuiKey_A);
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
        if (std::string_view(label) != "##BrushScript") {
            key(ImGuiKey_Tab);
        }
    };
    panelClick("##ScriptLibrary");
    frame();
    frame();
    {
        const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
        check(!popups.empty() && popups.back().Window != nullptr,
              "Script library list did not open.");
        auto* list = popups.back().Window;
        click(list->DC.CursorStartPos.x + 10.0F,
              list->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * 0.5F);
    }
    panelClick("Load");
    replacePanelText("Name", "ridges copy");
    panelClick("Save to library");
    const auto copy = BrushScriptFile::Load(library / "ridges_copy.toml");
    check(copy.name == "ridges copy" && copy.description == "Ridged relief" &&
          copy.script == "ridged(x, y, z, 3, 2, 0.5, 5)" &&
          copy.output == ScriptedBrushSettings::Output::Normalized,
          "Loading from the library and saving under a new name lost the script.");
    replacePanelText("Description", "Changed");
    panelClick("Save to library");
    check(ImGui::FindWindowByName("Replace Brush Script?")->Active,
          "Saving over a library script did not ask for confirmation.");
    clickDialogItem("Replace Brush Script?", "Replace");
    check(BrushScriptFile::Load(library / "ridges_copy.toml").description == "Changed",
          "Confirmed library overwrite did not save.");

    panelClick("Export...");
    check(ImGui::FindWindowByName("Project File")->Active, "Export did not open a file browser.");
    replaceDialogText("Project File", "File name", "exported");
    clickDialogItem("Project File", "Export");
    check(!dialogOpen() && BrushScriptFile::Load(library / "exported.toml").name == "ridges copy",
          "Exporting a brush script did not write a .toml file.");

    panelClick("Import...");
    replaceDialogText("Project File", "File name", (temporary.path / "outside.toml").string().c_str());
    clickDialogItem("Project File", "Import");
    check(!dialogOpen(), "Importing a brush script did not close the browser.");
    panelClick("Save to library");
    const auto imported = BrushScriptFile::Load(library / "imported.toml");
    check(imported.description == "From elsewhere" && imported.script == "simplex(x, y, z)\n* 0.5" &&
          imported.output == ScriptedBrushSettings::Output::Elevation,
          "Imported brush script did not replace the editor contents and output mode.");

    const auto parameterized = BrushScriptFile::Parse(
        "name='parameterized'\noutput='elevation'\nscript=\"frequency + octaves + offset\"\n"
        "[[parameter]]\nname='frequency'\ntype='float'\ncontrol='slider'\n"
        "min=0.1\nmax=100\nscale='log'\ndefault=4\n"
        "[[parameter]]\nname='octaves'\ntype='integer'\ncontrol='spin'\n"
        "min=0\nmax=30\ndefault=5\n"
        "[[parameter]]\nname='mode'\ntype='choice'\nchoices=['one','two']\ndefault='one'\n"
        "[[parameter]]\nname='offset'\ntype='float'\ncontrol='text'\n"
        "min=-10\nmax=10\ndefault=0.25\n");
    const auto importScript = [&](const BrushScriptFile& file) {
        file.Save(temporary.path / "outside.toml");
        panelClick("Import...");
        replaceDialogText("Project File", "File name", (temporary.path / "outside.toml").string().c_str());
        clickDialogItem("Project File", "Import");
        check(!dialogOpen(), "Importing a parameterized script did not close the browser.");
    };
    importScript(parameterized);
    const auto sliderPoint = panelClick("###ScriptParameter", 0);
    auto* panel = ImGui::FindWindowByName("###BrushParameters");
    const float midpoint = panel->DC.CursorStartPos.x + panel->ItemWidthDefault * 0.5F;
    click(midpoint, sliderPoint.y);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    click(midpoint, sliderPoint.y);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    const int parameterIndex = 0;
    const auto sliderId = ImHashStr("###ScriptParameter", 0,
        ImHashData(&parameterIndex, sizeof(parameterIndex), panel->IDStack.back()));
    const auto* sliderInput = ImGui::GetInputTextState(sliderId);
    check(sliderInput != nullptr, "Parameterized slider did not allow numeric input.");
    const auto midpointValue = std::stod(sliderInput->TextA.Data);
    check(midpointValue > 2.8 && midpointValue < 3.6,
          "Logarithmic slider midpoint was not the geometric mean of its bounds.");
    key(ImGuiKey_Escape);
    panelClick("###ScriptParameter", 1);
    panelClick("###ScriptParameter", 2);
    {
        const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
        check(!popups.empty() && popups.back().Window != nullptr,
              "Choice parameter did not open a dropdown.");
        const auto* list = popups.back().Window;
        click(list->DC.CursorStartPos.x + 10.0F,
              list->DC.CursorStartPos.y + ImGui::GetTextLineHeightWithSpacing() +
                  ImGui::GetTextLineHeight() * 0.5F);
    }
    replacePanelText("###ScriptParameter", "8", 3);
    const char* editedScript = "mode == 'two' ? offset + octaves : frequency";
    replacePanelText("##BrushScript", editedScript);
    panelClick("Compile");
    replacePanelText("##BrushScript", "unknownParameter");
    panelClick("Compile");
    panelClick("Revert");
    panelClick("Save to library");
    auto savedParameters = BrushScriptFile::Load(library / "parameterized.toml");
    check(savedParameters.parameters == parameterized.parameters &&
          savedParameters.script == editedScript,
          "Editing/compiling/reverting the script lost definitions or changed file defaults.");

    auto replacement = BrushScriptFile::Parse(
        "name='parameterized'\nscript='replacement'\n"
        "[[parameter]]\nname='replacement'\ntype='integer'\ncontrol='slider'\n"
        "min=-2147483648\nmax=2147483647\ndefault=9\n"
        "[[parameter]]\nname='amount'\ntype='float'\ncontrol='spin'\nmin=0\nmax=1\ndefault=0.5\n");
    replacement.Save(library / "parameterized.toml");
    panelClick("Load");
    panelClick("###ScriptParameter", 0);
    panelClick("###ScriptParameter", 1);
    replacePanelText("Name", "reloaded");
    panelClick("Save to library");
    const auto reloaded = BrushScriptFile::Load(library / "reloaded.toml");
    check(reloaded.parameters == replacement.parameters && reloaded.script == replacement.script,
          "Reload did not replace custom controls using externally edited definitions.");

    auto failed = parameterized;
    failed.name = "failed";
    failed.script = "unknownParameter";
    importScript(failed);
    panelClick("Revert");
    panelClick("Save to library");
    const auto reverted = BrushScriptFile::Load(library / "failed.toml");
    check(reverted.parameters == reloaded.parameters && reverted.script == reloaded.script,
          "Reverting a failed load did not restore the active script and parameter definitions.");

    importScript(BrushScriptFile{"legacy", "", "1"});
    panelClick("Save to library");
    check(BrushScriptFile::Load(library / "legacy.toml").parameters.empty(),
          "Loading a legacy script did not remove the previous custom controls.");

    auto menuBrush = BrushScriptFile::Parse(
        "name='File brush'\napplication='elevation'\nscript='amount'\n"
        "[[parameter]]\nname='amount'\ntype='float'\ncontrol='slider'\nmin=0\nmax=100\ndefault=20");
    menuBrush.Save(library / "menu.toml");
    BrushScriptFile{"Broken expression", "", "unknownFunction(1)"}.Save(library / "bad_expression.toml");
    BrushScriptFile{"Custom", "", "999"}.Save(library / "reserved.toml");
    std::ofstream(library / "invalid.toml") << "name = ";
    selectMenuItem(3, 0, "Refresh brushes");
    selectMenuItem(3, 0, "File brush");
    check(ImGui::FindWindowByName("###BrushParameters")->Active,
          "A TOML-defined tool did not open its parameter panel.");
    panelClick("###ScriptParameter", 0);
    selectMenuItem(3, 0, "Custom");
    panelClick("Export...");
    replaceDialogText("Project File", "File name", (library / "custom_preserved.toml").string().c_str());
    clickDialogItem("Project File", "Export");
    const auto preserved = BrushScriptFile::Load(library / "custom_preserved.toml");
    check(preserved.name == "legacy" && preserved.script == "1" && preserved.parameters.empty(),
          "Refreshing the menu or using a preset replaced the Custom editor.");

    menuBrush.name = "Renamed file brush";
    menuBrush.Save(library / "menu.toml");
    selectMenuItem(3, 0, "Refresh brushes");
    selectMenuItem(3, 0, "Renamed file brush");
    const auto toolsMenuHas = [&](const char* label) {
        auto* bar = ImGui::FindWindowByName("##MainMenuBar");
        float x = bar->DC.CursorStartPos.x + ImGui::GetStyle().ItemSpacing.x * 0.5F;
        for (const auto* name : {"File", "Edit", "Select"}) {
            x += ImGui::CalcTextSize(name).x + ImGui::GetStyle().ItemSpacing.x * 2.0F;
        }
        click(x + ImGui::CalcTextSize("Brushes").x * 0.5F, bar->Pos.y + bar->Size.y * 0.5F);
        frame();
        auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
        bool found = false;
        for (float y = popup->InnerRect.Min.y + 1; y < popup->InnerRect.Max.y; y += 2) {
            ImGui::GetIO().AddMousePosEvent(popup->DC.CursorStartPos.x + 20, y);
            frame();
            if (ImGui::GetCurrentContext()->HoveredId == popup->GetID(label)) {
                found = true;
                break;
            }
        }
        ImGui::GetIO().AddMousePosEvent(1200, 750);
        ImGui::ClosePopupsOverWindow(nullptr, false);
        frame();
        return found;
    };
    check(!toolsMenuHas("File brush") && !toolsMenuHas("Broken expression") &&
          toolsMenuHas("Brush errors"),
          "Brushes retained a removed name, exposed a broken script, or hid library errors.");

    std::vector<float> original(grid.GetSnapshot()->GetVertices().size(), 100.0F);
    std::vector<float> feather(original.size());
    for (std::size_t i = 0; i < feather.size(); ++i) {
        feather[i] = static_cast<float>(i % 5) / 4.0F;
    }
    grid.SetElevations(original);
    grid.SetSelectionWeights(feather);
    renderer.UploadGrid(*grid.GetSnapshot());
    std::vector<std::vector<float>> otherLayers;
    for (const auto& layer : grid.GetLayers()) {
        if (layer.get() != &grid.GetElevationLayer()) {
            const auto values = layer->GetValues();
            otherLayers.emplace_back(values.begin(), values.end());
        }
    }
    // The library brush must use current controls, not its file defaults.
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    panelClick("###ScriptParameter", 0);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    key(ImGuiKey_A);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddInputCharactersUTF8("80");
    key(ImGuiKey_Enter);
    panelClick("Fill selection");
    const auto filled = grid.GetSnapshot();
    for (std::size_t i = 0; i < original.size(); ++i) {
        check(filled->GetVertices()[i].elevation == 100.0F - 20.0F * feather[i] &&
              filled->GetVertices()[i].selectionWeight == feather[i],
              "Library fill did not use current parameters or preserve selection feathering.");
    }
    std::size_t otherIndex = 0;
    for (const auto& layer : grid.GetLayers()) {
        if (layer.get() != &grid.GetElevationLayer()) {
            check(std::ranges::equal(layer->GetValues(), otherLayers[otherIndex++]),
                  "Selection fill modified an inactive layer.");
        }
    }
    shortcut(ImGuiKey_Z);
    for (std::size_t i = 0; i < original.size(); ++i) {
        check(grid.GetSnapshot()->GetVertices()[i].elevation == original[i],
              "Selection fill was not undone in one action.");
    }
    shortcut(ImGuiKey_Y);
    for (std::size_t i = 0; i < original.size(); ++i) {
        check(grid.GetSnapshot()->GetVertices()[i].elevation == filled->GetVertices()[i].elevation,
              "Redo did not restore the selection fill.");
    }

    selectMenuItem(3, 0, "Custom");
    auto customFill = BrushScriptFile::Parse(
        "name='Custom fill'\napplication='delta'\ncontinuous=true\nscript='rate * elapsed'");
    importScript(customFill);
    grid.SetSelectionWeights(feather, false);
    grid.SetElevations(original);
    renderer.UploadGrid(*grid.GetSnapshot());
    panelClick("Fill selection");
    for (const auto& vertex : grid.GetSnapshot()->GetVertices()) {
        check(vertex.elevation == 4196.0F,
              "Custom fill without selection did not apply one second over the whole layer.");
    }
    shortcut(ImGuiKey_Z);
    for (const auto& vertex : grid.GetSnapshot()->GetVertices()) {
        check(vertex.elevation == 100.0F, "Custom fill undo did not restore the layer.");
    }
    customFill.script = "sqrt(-1)";
    importScript(customFill);
    panelClick("Fill selection");
    for (const auto& vertex : grid.GetSnapshot()->GetVertices()) {
        check(vertex.elevation == 100.0F, "Failed Custom fill changed the layer.");
    }
    selectMenuItem(0, 4, "Planet...");
    planetTab("Geometry");
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    clickDialogItem("Planet", "Point Count (N)");
    key(ImGuiKey_A);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddInputCharactersUTF8("1100");
    frame();
    key(ImGuiKey_Enter);
    replaceDialogText("Planet", "Planet radius (km)", "4000");
    clickDialogItem("Planet", "Apply");
    check(!dialogOpen() && grid.GetSnapshot()->GetVertices().size() == 1100 &&
          grid.GetSnapshot()->GetOptions().pointCount == 1100 &&
          grid.GetSnapshot()->GetOptions().planetRadius == 4000.0,
          "Planet Geometry did not regenerate the requested point count with its shared radius.");
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
