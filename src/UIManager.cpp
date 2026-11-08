#include "UIManager.hpp"

#include <imgui.h>

#include <algorithm>
#include <exception>
#include <filesystem>

UIManager::UIManager(const GridOptions& options) : options_(options) {}

void UIManager::NavigateRasterDirectory(const std::filesystem::path& directory) {
    try {
        rasterBrowser_.Navigate(directory.empty() ? std::filesystem::current_path() : directory);
        selectedRasterPath_.clear();
        selectedRasterIsDirectory_ = false;
        errorMessage_.clear();
    } catch (const std::exception& error) {
        errorMessage_ = error.what();
    }
}

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
        NavigateRasterDirectory(rasterBrowser_.GetDirectory());
        ImGui::OpenPopup("Open Raster");
        rasterOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(640.0F, 480.0F), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal("Open Raster")) {
        const auto& directory = rasterBrowser_.GetDirectory();
        ImGui::TextWrapped("Folder: %s", directory.string().c_str());
        ImGui::BeginDisabled(directory.empty() || directory == directory.root_path());
        if (ImGui::Button("Up")) {
            NavigateRasterDirectory(directory.parent_path());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            NavigateRasterDirectory(directory);
        }
        ImGui::TextWrapped("Supported images: PNG, JPEG, BMP, TGA, PSD, GIF, HDR, PIC, "
                           "binary PPM/PGM/PNM. Select an equirectangular raster.");

        bool openSelection = false;
        std::filesystem::path nextDirectory;
        const auto selectionText = "Selected: " + selectedRasterPath_.filename().string();
        const float footerWidth = ImGui::GetContentRegionAvail().x;
        float footerHeight = ImGui::CalcTextSize(selectionText.c_str(), nullptr, false,
                                                footerWidth).y +
                             ImGui::GetFrameHeightWithSpacing() +
                             3.0F * ImGui::GetStyle().ItemSpacing.y;
        if (!errorMessage_.empty()) {
            footerHeight += ImGui::CalcTextSize(errorMessage_.c_str(), nullptr, false,
                                               footerWidth).y +
                            ImGui::GetStyle().ItemSpacing.y;
        }
        const float listHeight = std::max(80.0F, ImGui::GetContentRegionAvail().y - footerHeight);
        if (ImGui::BeginChild("RasterFiles", ImVec2(0.0F, listHeight), true)) {
            if (rasterBrowser_.GetEntries().empty()) {
                ImGui::TextUnformatted("No folders or supported raster files.");
            }
            for (const auto& entry : rasterBrowser_.GetEntries()) {
                const auto name = entry.path.filename().string();
                ImGui::PushID(&entry);
                const auto textPosition = ImGui::GetCursorScreenPos();
                const bool selected = entry.path == selectedRasterPath_;
                if (ImGui::Selectable("##entry", selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedRasterPath_ = entry.path;
                    selectedRasterIsDirectory_ = entry.isDirectory;
                    errorMessage_.clear();
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (entry.isDirectory) {
                            nextDirectory = entry.path;
                        } else {
                            openSelection = true;
                        }
                    }
                }
                // Draw filenames literally, including ImGui's special "##" sequences.
                const auto label = (entry.isDirectory ? "[Folder] " : "") + name;
                ImGui::GetWindowDrawList()->AddText(textPosition,
                    ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (!nextDirectory.empty()) {
            NavigateRasterDirectory(nextDirectory);
        }
        ImGui::TextWrapped("Selected: %s", selectedRasterPath_.filename().string().c_str());
        if (!errorMessage_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.35F, 0.3F, 1.0F));
            ImGui::TextWrapped("%s", errorMessage_.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::BeginDisabled(selectedRasterPath_.empty());
        openSelection |= ImGui::Button(selectedRasterIsDirectory_ ? "Open Folder" : "Open");
        openSelection |= !selectedRasterPath_.empty() && ImGui::IsKeyPressed(ImGuiKey_Enter);
        ImGui::EndDisabled();
        if (openSelection && selectedRasterIsDirectory_) {
            NavigateRasterDirectory(selectedRasterPath_);
        } else if (openSelection) {
            try {
                const auto path = selectedRasterPath_;
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
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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