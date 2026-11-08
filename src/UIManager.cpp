#include "UIManager.hpp"

#include <imgui.h>

#include <exception>
#include <filesystem>

UIManager::UIManager(const GridOptions& options) : options_(options) {}

bool UIManager::Render(SphericalFibonacciGrid& grid, GlobeRenderer& renderer) {
    bool requestExit = false;
    if (ImGui::IsKeyPressed(ImGuiKey_O) && ImGui::GetIO().KeyCtrl &&
        !ImGui::GetIO().WantTextInput) {
        rasterOpen_ = true;
        errorMessage_.clear();
    }
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Raster...", "Ctrl+O")) {
                rasterOpen_ = true;
                errorMessage_.clear();
            }
            if (ImGui::MenuItem("Settings...")) {
                settingsOpen_ = true;
                errorMessage_.clear();
            }
            ImGui::Separator();
            requestExit = ImGui::MenuItem("Exit");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (rasterOpen_) {
        ImGui::OpenPopup("Open Raster");
        rasterOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Open Raster", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Enter an equirectangular image path:");
        ImGui::SetNextItemWidth(480.0F);
        ImGui::InputText("##rasterPath", rasterPathInput_.data(), rasterPathInput_.size());
        if (!errorMessage_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.35F, 0.3F, 1.0F));
            ImGui::TextWrapped("%s", errorMessage_.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::Button("Open")) {
            try {
                const std::filesystem::path path(rasterPathInput_.data());
                auto loaded = ElevationRaster::Load(path);
                auto elevations = loaded.SampleElevations(*grid.GetSnapshot());
                grid.SetElevations(elevations);
                renderer.UpdateElevations(grid.GetSnapshot()->GetVertices());
                loadedRasterPath_ = path;
                raster_ = std::move(loaded);
                errorMessage_.clear();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& error) {
                errorMessage_ = error.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            errorMessage_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (raster_) {
        ImGui::SetNextWindowBgAlpha(0.75F);
        ImGui::Begin("Raster", nullptr, ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%s (%d x %d)", loadedRasterPath_.filename().string().c_str(),
                    raster_->GetWidth(), raster_->GetHeight());
        ImGui::End();
    }

    if (settingsOpen_) {
        ImGui::OpenPopup("Settings");
        settingsOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        int pointCount = static_cast<int>(options_.pointCount);
        float jitterStrength = static_cast<float>(options_.jitterStrength);
        if (ImGui::SliderInt("Point Count (N)", &pointCount, 1'000, 1'000'000)) {
            options_.pointCount = static_cast<std::size_t>(pointCount);
        }
        if (ImGui::SliderFloat("Jitter Strength (J)", &jitterStrength, 0.0F, 1.0F, "%.2f")) {
            options_.jitterStrength = jitterStrength;
        }
        if (!errorMessage_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.35F, 0.3F, 1.0F));
            ImGui::TextWrapped("%s", errorMessage_.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::Button("Apply")) {
            try {
                grid.GenerateGrid(options_);
                if (raster_) {
                    const auto elevations = raster_->SampleElevations(*grid.GetSnapshot());
                    grid.SetElevations(elevations);
                }
                renderer.UploadGrid(*grid.GetSnapshot());
                errorMessage_.clear();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& error) {
                errorMessage_ = error.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return requestExit;
}