#include "UIManager.hpp"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

namespace {

constexpr double earthRadiusKilometres = 6371.0;
constexpr double earthRadiusMiles = 3958.7613;

glm::mat4 brushMvp(const ImVec2& displaySize, const glm::mat4& modelMatrix, float zoom) {
    const float aspect = displaySize.x > 0.0F && displaySize.y > 0.0F
                             ? displaySize.x / displaySize.y : 1.0F;
    const float horizontal = 1.15F * std::max(aspect, 1.0F) / zoom;
    const float vertical = 1.15F * std::max(1.0F / aspect, 1.0F) / zoom;
    const auto projection = glm::ortho(-horizontal, horizontal, -vertical, vertical, 0.1F, 10.0F);
    const auto view = glm::lookAt(glm::vec3(0.0F, -3.0F, 0.0F), glm::vec3(0.0F),
                                  glm::vec3(0.0F, 0.0F, 1.0F));
    return projection * view * modelMatrix;
}

std::optional<glm::dvec3> pickSphere(const ImVec2& mouse, const ImVec2& displaySize,
                                    const glm::mat4& mvp) {
    if (displaySize.x <= 0.0F || displaySize.y <= 0.0F ||
        mouse.x < 0.0F || mouse.y < 0.0F || mouse.x >= displaySize.x || mouse.y >= displaySize.y) {
        return std::nullopt;
    }
    const float ndcX = 2.0F * mouse.x / displaySize.x - 1.0F;
    const float ndcY = 1.0F - 2.0F * mouse.y / displaySize.y;
    const auto inverse = glm::inverse(mvp);
    glm::vec4 nearPoint = inverse * glm::vec4(ndcX, ndcY, -1.0F, 1.0F);
    glm::vec4 farPoint = inverse * glm::vec4(ndcX, ndcY, 1.0F, 1.0F);
    if (nearPoint.w == 0.0F || farPoint.w == 0.0F) {
        return std::nullopt;
    }
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    const glm::dvec3 origin(nearPoint);
    const glm::dvec3 direction = glm::dvec3(farPoint) - origin;
    const double a = glm::dot(direction, direction);
    const double b = glm::dot(origin, direction);
    const double c = glm::dot(origin, origin) - 1.0;
    const double discriminant = b * b - a * c;
    if (a <= 0.0 || discriminant < 0.0) {
        return std::nullopt;
    }
    const double root = std::sqrt(discriminant);
    double t = (-b - root) / a;
    if (t < 0.0 || t > 1.0) {
        t = (-b + root) / a;
    }
    if (t < 0.0 || t > 1.0) {
        return std::nullopt;
    }
    return glm::normalize(origin + t * direction);
}

bool projectPoint(glm::dvec3 point, const glm::mat4& modelMatrix, const glm::mat4& mvp,
                  const ImVec2& displaySize, ImVec2& screenPoint, bool& visible) {
    const glm::vec4 clip = mvp * glm::vec4(glm::vec3(point), 1.0F);
    if (clip.w <= 0.0F) {
        return false;
    }
    const glm::vec3 worldNormal = glm::normalize(glm::vec3(modelMatrix *
                                                glm::vec4(glm::vec3(point), 0.0F)));
    const glm::vec3 worldPoint = glm::vec3(modelMatrix * glm::vec4(glm::vec3(point), 1.0F));
    visible = glm::dot(worldNormal, glm::vec3(0.0F, -3.0F, 0.0F) - worldPoint) > 0.0F;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    screenPoint = ImVec2((ndc.x + 1.0F) * 0.5F * displaySize.x,
                         (1.0F - ndc.y) * 0.5F * displaySize.y);
    return true;
}

void drawBrushRing(glm::dvec3 center, double radius, double planetRadius,
                   const glm::mat4& modelMatrix, const glm::mat4& mvp,
                   const ImVec2& displaySize, ImU32 color) {
    const double angle = std::min(radius / planetRadius, std::numbers::pi);
    const glm::dvec3 reference = std::abs(center.z) < 0.8 ? glm::dvec3(0.0, 0.0, 1.0)
                                                            : glm::dvec3(0.0, 1.0, 0.0);
    const glm::dvec3 tangent = glm::normalize(glm::cross(reference, center));
    const glm::dvec3 bitangent = glm::normalize(glm::cross(center, tangent));
    constexpr int segments = 128;
    std::array<ImVec2, segments + 1> points{};
    std::array<bool, segments + 1> onVisibleHemisphere{};
    std::array<bool, segments + 1> projected{};
    for (int i = 0; i <= segments; ++i) {
        const double theta = 2.0 * std::numbers::pi * i / segments;
        const auto direction = std::cos(theta) * tangent + std::sin(theta) * bitangent;
        const glm::dvec3 point = std::cos(angle) * center + std::sin(angle) * direction;
        projected[static_cast<std::size_t>(i)] = projectPoint(
            point, modelMatrix, mvp, displaySize, points[static_cast<std::size_t>(i)],
            onVisibleHemisphere[static_cast<std::size_t>(i)]);
    }
    auto* drawList = ImGui::GetForegroundDrawList();
    for (int i = 0; i < segments; ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (projected[index] && projected[index + 1] && onVisibleHemisphere[index] &&
            onVisibleHemisphere[index + 1]) {
            drawList->AddLine(points[index], points[index + 1], color, 1.5F);
        }
    }
}

} // namespace

class RasterImportCommand final : public UndoableCommand {
public:
    RasterImportCommand(UIManager& ui, SphericalFibonacciGrid& grid, GlobeRenderer& renderer,
                        std::vector<float> previousElevations,
                        std::vector<float> importedElevations,
                        std::shared_ptr<const ElevationRaster> previousRaster,
                        std::filesystem::path previousPath,
                        std::shared_ptr<const ElevationRaster> importedRaster,
                        std::filesystem::path importedPath)
        : ui_(ui), grid_(grid), renderer_(renderer),
          previousElevations_(std::move(previousElevations)),
          importedElevations_(std::move(importedElevations)),
          previousRaster_(std::move(previousRaster)), previousPath_(std::move(previousPath)),
          importedRaster_(std::move(importedRaster)), importedPath_(std::move(importedPath)) {}

    void redo() override { Apply(importedElevations_, importedRaster_, importedPath_); }
    void undo() override { Apply(previousElevations_, previousRaster_, previousPath_); }

private:
    void Apply(const std::vector<float>& elevations,
               const std::shared_ptr<const ElevationRaster>& raster,
               const std::filesystem::path& path) {
        grid_.SetElevations(elevations);
        renderer_.UpdateElevations(grid_.GetSnapshot()->GetVertices());
        ui_.raster_ = raster;
        ui_.loadedRasterPath_ = path;
    }

    UIManager& ui_;
    SphericalFibonacciGrid& grid_;
    GlobeRenderer& renderer_;
    std::vector<float> previousElevations_;
    std::vector<float> importedElevations_;
    std::shared_ptr<const ElevationRaster> previousRaster_;
    std::filesystem::path previousPath_;
    std::shared_ptr<const ElevationRaster> importedRaster_;
    std::filesystem::path importedPath_;
};

class BrushStrokeCommand final : public UndoableCommand {
public:
    BrushStrokeCommand(SphericalFibonacciGrid& grid, GlobeRenderer& renderer,
                       std::vector<float> before, std::vector<float> after)
        : grid_(grid), renderer_(renderer), before_(std::move(before)), after_(std::move(after)) {}

    void redo() override { Apply(after_); }
    void undo() override { Apply(before_); }

private:
    void Apply(const std::vector<float>& elevations) {
        grid_.SetElevations(elevations);
        renderer_.UpdateElevations(grid_.GetSnapshot()->GetVertices());
    }

    SphericalFibonacciGrid& grid_;
    GlobeRenderer& renderer_;
    std::vector<float> before_;
    std::vector<float> after_;
};

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

void UIManager::RenderBrushPanel() {
    if (!brushTools_.IsActive()) {
        return;
    }
    bool open = true;
    if (ImGui::Begin("Elevation Brush", &open)) {
        auto& settings = brushTools_.GetSettings();
        constexpr std::array operationNames{"Replace", "Raise", "Lower", "Minimum", "Maximum"};
        int operation = static_cast<int>(settings.operation);
        if (ImGui::Combo("Operation", &operation, operationNames.data(),
                         static_cast<int>(operationNames.size()))) {
            settings.operation = static_cast<BrushOperation>(operation);
        }

        int unit = static_cast<int>(settings.distanceUnit);
        constexpr std::array unitNames{"Kilometres", "Miles"};
        if (ImGui::Combo("Distance unit", &unit, unitNames.data(),
                         static_cast<int>(unitNames.size()))) {
            if (unit != static_cast<int>(settings.distanceUnit)) {
                settings.radius *= unit == static_cast<int>(BrushDistanceUnit::Miles)
                                       ? 0.621371192 : 1.609344;
                settings.distanceUnit = static_cast<BrushDistanceUnit>(unit);
            }
        }
        float radius = static_cast<float>(settings.radius);
        const float maximumRadius = settings.distanceUnit == BrushDistanceUnit::Kilometres
                                        ? 20020.0F : 12500.0F;
        if (ImGui::DragFloat("Inner radius", &radius, 10.0F, 0.1F,
                             maximumRadius, "%.1f")) {
            settings.radius = std::clamp(static_cast<double>(radius), 0.1,
                                         static_cast<double>(maximumRadius));
        }
        float falloff = static_cast<float>(settings.falloffPercent);
        if (ImGui::SliderFloat("Falloff (%)", &falloff, 0.0F, 100.0F, "%.0f%%")) {
            settings.falloffPercent = falloff;
        }
        float intensity = static_cast<float>(settings.intensityPercent);
        if (ImGui::SliderFloat("Intensity (%)", &intensity, 0.0F, 100.0F, "%.0f%%")) {
            settings.intensityPercent = intensity;
        }
        ImGui::SliderFloat("Elevation / amount", &settings.value, -32767.0F, 32767.0F,
                           "%.0f");
        ImGui::TextWrapped("Left-drag over the globe to paint. Falloff feathers outward from the inner radius.");
    }
    ImGui::End();
    if (!open) {
        brushTools_.SetActive(false);
    }
}

void UIManager::ProcessBrushInput(SphericalFibonacciGrid& grid, GlobeRenderer& renderer,
                                  const glm::mat4& modelMatrix, float zoom) {
    auto& io = ImGui::GetIO();
    const auto mvp = brushMvp(io.DisplaySize, modelMatrix, zoom);
    const bool captured = io.WantCaptureMouse;
    if (stroke_ && (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || !brushTools_.IsActive())) {
        auto before = std::move(stroke_->original);
        auto after = std::move(stroke_->working);
        stroke_.reset();
        if (before != after) {
            undoStack_.Execute(std::make_unique<BrushStrokeCommand>(
                grid, renderer, std::move(before), std::move(after)));
        }
    }

    std::optional<glm::dvec3> cursorCenter;
    if (brushTools_.IsActive() && !captured) {
        cursorCenter = pickSphere(io.MousePos, io.DisplaySize, mvp);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && cursorCenter) {
            Stroke stroke;
            const auto snapshot = grid.GetSnapshot();
            stroke.original.reserve(snapshot->GetVertices().size());
            stroke.renderVertices = snapshot->GetVertices();
            for (const auto& vertex : stroke.renderVertices) {
                stroke.original.push_back(vertex.elevation);
            }
            stroke.working = stroke.original;
            stroke_ = std::move(stroke);
        }
    }

    if (stroke_ && brushTools_.IsActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !captured && cursorCenter) {
        const auto snapshot = grid.GetSnapshot();
        brushTools_.GetSelectedTool().Apply(*snapshot, *cursorCenter, stroke_->working,
                                            brushTools_.GetSettings());
        for (std::size_t i = 0; i < stroke_->working.size(); ++i) {
            stroke_->renderVertices[i].elevation = stroke_->working[i];
        }
        renderer.UpdateElevations(stroke_->renderVertices);
    }

    if (brushTools_.IsActive() && cursorCenter && !captured) {
        const auto& settings = brushTools_.GetSettings();
        const double planetRadius = settings.distanceUnit == BrushDistanceUnit::Kilometres
                                        ? earthRadiusKilometres : earthRadiusMiles;
        const double innerRadius = settings.radius;
        const double outerRadius = innerRadius * (1.0 + settings.falloffPercent / 100.0);
        constexpr ImU32 white = IM_COL32(255, 255, 255, 230);
        drawBrushRing(*cursorCenter, outerRadius, planetRadius, modelMatrix, mvp,
                      io.DisplaySize, white);
        drawBrushRing(*cursorCenter, innerRadius, planetRadius, modelMatrix, mvp,
                      io.DisplaySize, white);
    }
}

bool UIManager::Render(SphericalFibonacciGrid& grid, GlobeRenderer& renderer,
                       const glm::mat4& modelMatrix, float zoom) {
    bool requestExit = false;
    const auto undo = [&] { undoStack_.Undo(); };
    const auto redo = [&] { undoStack_.Redo(); };
    if (ImGui::IsKeyPressed(ImGuiKey_O) && ImGui::GetIO().KeyCtrl &&
        !ImGui::GetIO().WantTextInput) {
        rasterOpen_ = true;
        errorMessage_.clear();
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
            undo();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
            redo();
        }
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
        if (ImGui::BeginMenu("Edit")) {
            ImGui::BeginDisabled(!undoStack_.CanUndo());
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
                undo();
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!undoStack_.CanRedo());
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) {
                redo();
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            bool brushActive = brushTools_.IsActive();
            if (ImGui::MenuItem("Elevation Brush", nullptr, &brushActive)) {
                brushTools_.SetActive(brushActive);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (rasterOpen_) {
        NavigateRasterDirectory(rasterBrowser_.GetDirectory());
        ImGui::OpenPopup("Open Raster");
        rasterOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(640.0F, 320.0F), ImGuiCond_FirstUseEver);
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
                auto loaded = std::make_shared<ElevationRaster>(ElevationRaster::Load(path));
                auto elevations = loaded->SampleElevations(*grid.GetSnapshot());
                std::vector<float> previousElevations;
                const auto& previousVertices = grid.GetSnapshot()->GetVertices();
                previousElevations.reserve(previousVertices.size());
                for (const auto& vertex : previousVertices) {
                    previousElevations.push_back(vertex.elevation);
                }
                undoStack_.Execute(std::make_unique<RasterImportCommand>(
                    *this, grid, renderer, std::move(previousElevations),
                    std::move(elevations), raster_, loadedRasterPath_, std::move(loaded), path));
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
                undoStack_.Clear();
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
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                      ImGuiPopupFlags_AnyPopupLevel)) {
        RenderBrushPanel();
    }
    ProcessBrushInput(grid, renderer, modelMatrix, zoom);
    return requestExit;
}