#include "UIManager.hpp"
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

    auto selectBrushCombo = [&](const char* label, int row, int selectedItem,
                                const char* expectedItem) {
        auto* panel = ImGui::FindWindowByName("###BrushParameters");
        check(panel != nullptr && panel->Active, "Brush panel is not visible.");
        const float height = ImGui::GetFrameHeight();
        const auto start = panel->DC.CursorStartPos;
        const ImGuiID popupId = ImHashStr("##ComboPopup", 0, panel->GetID(label));
        ImGui::GetIO().AddMousePosEvent(start.x + panel->ItemWidthDefault - height * 0.5F,
            start.y + row * (height + ImGui::GetStyle().ItemSpacing.y) + height * 0.5F);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        frame();
        auto* popup = ImGui::FindWindowByName("##Combo_00");
        check(panel->Active && popup != nullptr && popup->Active &&
              !ImGui::GetCurrentContext()->OpenPopupStack.empty() &&
              ImGui::GetCurrentContext()->OpenPopupStack.back().PopupId == popupId,
              "Clicking a brush combo arrow did not keep its dropdown and panel visible.");

        ImGui::GetIO().AddMousePosEvent(popup->DC.CursorStartPos.x + 10.0F,
            popup->DC.CursorStartPos.y + selectedItem * ImGui::GetTextLineHeightWithSpacing() +
            ImGui::GetTextLineHeight() * 0.5F);
        frame();
        check(ImGui::GetCurrentContext()->HoveredId ==
                  ImHashStr(expectedItem, 0, popup->GetID(selectedItem)),
              "Brush combo did not point to the expected option.");
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        check(!dialogOpen() && panel->Active,
              "Selecting a brush combo option did not close the dropdown.");
    };
    auto checkBrushSlider = [&](const char* label, int row, bool enabled, float relativeX = 0.25F) {
        auto* panel = ImGui::FindWindowByName("###BrushParameters");
        check(panel != nullptr && panel->Active, "Brush panel is not visible.");
        const float height = ImGui::GetFrameHeight();
        const auto start = panel->DC.CursorStartPos;
        const ImGuiID sliderId = panel->GetID(label);
        ImGui::GetIO().AddMousePosEvent(start.x + panel->ItemWidthDefault * relativeX,
            start.y + row * (height + ImGui::GetStyle().ItemSpacing.y) + height * 0.5F);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        check((ImGui::GetCurrentContext()->ActiveId == sliderId) == enabled,
              enabled ? "Brush slider should be enabled." : "Brush slider should be disabled.");
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
    };
    auto sliderGrabPosition = [&](int row) -> std::optional<float> {
        frame();
        auto* panel = ImGui::FindWindowByName("###BrushParameters");
        check(panel != nullptr && panel->Active, "Brush panel is not visible.");
        const float height = ImGui::GetFrameHeight();
        const float top = panel->DC.CursorStartPos.y +
                          row * (height + ImGui::GetStyle().ItemSpacing.y);
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_SliderGrab);
        float left = std::numeric_limits<float>::max();
        float right = std::numeric_limits<float>::lowest();
        for (const auto& vertex : panel->DrawList->VtxBuffer) {
            if (vertex.col == color && vertex.pos.y >= top && vertex.pos.y <= top + height) {
                left = std::min(left, vertex.pos.x);
                right = std::max(right, vertex.pos.x);
            }
        }
        if (left > right) {
            return std::nullopt;
        }
        return (left + right) * 0.5F;
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
        constexpr std::array names{"File", "Edit", "Select", "Tools", "Filter", "Mesh"};
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
        ImGui::GetIO().AddMousePosEvent(popup->DC.CursorStartPos.x + 20.0F,
            popup->DC.CursorStartPos.y + row * ImGui::GetTextLineHeightWithSpacing() +
            (menu == 0 && row >= 5 ? ImGui::GetStyle().ItemSpacing.y : 0.0F) +
            ImGui::GetTextLineHeight() * 0.5F);
        frame();
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
    checkBrushSlider("Falloff (%)", 3, true);
    constexpr std::array operationNames{"Replace", "Minimum", "Maximum"};
    for (int operation = 0; operation <= 2; ++operation) {
        selectBrushCombo("Operation", 0, operation, operationNames[operation]);
        checkBrushSlider("Elevation", 4, true);
        check(!sliderGrabPosition(5).has_value(),
              "Elevation brush still shows an intensity slider.");
    }
    selectBrushCombo("Distance unit", 1, 1, "Miles");
    selectMenuItem(3, 1, "Raise/Lower Brush");
    checkBrushSlider("Intensity (%)", 4, true);
    check(!sliderGrabPosition(5).has_value(), "Raise/Lower brush still has a value slider.");
    selectBrushCombo("Operation", 0, 1, "Lower");
    checkBrushSlider("Intensity (%)", 4, true);
    selectMenuItem(3, 0, "Elevation Brush");
    checkBrushSlider("Elevation", 4, true);
    check(!sliderGrabPosition(5).has_value(),
          "Switching tools restored an intensity slider to the elevation brush.");
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
              settings->DC.CursorStartPos.y + 2.0F * rowHeight +
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
        const float height = ImGui::GetFrameHeight();
        const auto start = panel->DC.CursorStartPos;
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
        click(start.x + panel->ItemWidthDefault * 0.5F,
              start.y + 2.0F * (height + ImGui::GetStyle().ItemSpacing.y) + height * 0.5F);
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
        const auto id = dialog->GetID(label);
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
    selectMenuItem(0, 4, "Metadata...");
    auto* metadataDialog = ImGui::FindWindowByName("Project Metadata");
    check(metadataDialog && metadataDialog->Active, "Metadata menu did not open its dialog.");
    click(metadataDialog->DC.CursorStartPos.x + 20, metadataDialog->DC.CursorStartPos.y + 20);
    ImGui::GetIO().AddInputCharactersUTF8("Project description");
    frame();
    replaceDialogText("Project Metadata", "Author", "Project author");
    clickDialogItem("Project Metadata", "Apply");
    check(grid.GetMetadata().author == "Project author" &&
          grid.GetMetadata().description == "Project description" &&
          grid.GetMetadata().savedAt.empty(),
          "Metadata dialog did not apply author/description or changed the save date.");
    selectMenuItem(0, 4, "Metadata...");
    replaceDialogText("Project Metadata", "Author", "Cancelled author");
    key(ImGuiKey_Escape);
    check(grid.GetMetadata().author == "Project author",
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
