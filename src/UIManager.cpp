#include "UIManager.hpp"
#include "ProjectFile.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <memory>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace {


float clipUiValue(float value, float minimum, float maximum, float fallback) {
    if (!std::isfinite(value)) {
        value = fallback;
    }
    return std::clamp(value, minimum, maximum);
}

void showRangeTooltip(float minimum, float maximum) {
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Valid range: %.2f to %.2f", minimum, maximum);
    }
}

void showRangeTooltip(int minimum, int maximum) {
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Valid range: %d to %d", minimum, maximum);
    }
}

glm::mat4 brushMvp(const ImVec2& displaySize, const glm::mat4& modelMatrix, float zoom,
                   ViewProjection viewProjection) {
    const float aspect = displaySize.x > 0.0F && displaySize.y > 0.0F
                             ? displaySize.x / displaySize.y : 1.0F;
    const float horizontal = viewProjection == ViewProjection::Equirectangular
        ? std::max(glm::pi<float>(), glm::pi<float>() * aspect / 2.0F) / zoom
        : 1.15F * std::max(aspect, 1.0F) / zoom;
    const float vertical = viewProjection == ViewProjection::Equirectangular
        ? horizontal / aspect
        : 1.15F * std::max(1.0F / aspect, 1.0F) / zoom;
    const auto projection = glm::ortho(-horizontal, horizontal, -vertical, vertical, 0.1F, 10.0F);
    if (viewProjection == ViewProjection::Equirectangular) {
        return projection * modelMatrix;
    }
    const auto view = glm::lookAt(glm::vec3(0.0F, -3.0F, 0.0F), glm::vec3(0.0F),
                                  glm::vec3(0.0F, 0.0F, 1.0F));
    return projection * view * modelMatrix;
}

bool isInsideViewport(const ImVec2& mouse, const ImVec2& displaySize) {
    return displaySize.x > 0.0F && displaySize.y > 0.0F &&
           mouse.x >= 0.0F && mouse.y >= 0.0F &&
           mouse.x < displaySize.x && mouse.y < displaySize.y;
}

std::optional<glm::dvec3> pickSphere(const ImVec2& mouse, const ImVec2& displaySize,
                                    const glm::mat4& mvp) {
    if (!isInsideViewport(mouse, displaySize)) {
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

std::optional<glm::dvec3> pickView(const ImVec2& mouse, const ImVec2& displaySize,
                                  const glm::mat4& mvp, ViewProjection projection) {
    if (projection == ViewProjection::Orthographic) {
        return pickSphere(mouse, displaySize, mvp);
    }
    if (!isInsideViewport(mouse, displaySize)) {
        return std::nullopt;
    }
    const float ndcX = 2.0F * mouse.x / displaySize.x - 1.0F;
    const float ndcY = 1.0F - 2.0F * mouse.y / displaySize.y;
    glm::vec4 point = glm::inverse(mvp) * glm::vec4(ndcX, ndcY, 0.0F, 1.0F);
    if (std::abs(point.w) <= std::numeric_limits<float>::epsilon()) {
        return std::nullopt;
    }
    point /= point.w;
    const double longitude = std::remainder(static_cast<double>(point.x), 2.0 * std::numbers::pi);
    const double latitude = static_cast<double>(point.y);
    if (latitude < -std::numbers::pi / 2.0 || latitude > std::numbers::pi / 2.0) {
        return std::nullopt;
    }
    const double radius = std::cos(latitude);
    return glm::dvec3(radius * std::sin(longitude), -radius * std::cos(longitude),
                      std::sin(latitude));
}

bool projectPoint(glm::dvec3 point, const glm::mat4& modelMatrix, const glm::mat4& mvp,
                  const ImVec2& displaySize, ImVec2& screenPoint, bool& visible,
                  ViewProjection projection, double mapCenterLongitude) {
    glm::vec3 position(point);
    if (projection == ViewProjection::Equirectangular) {
        double longitude = std::atan2(point.x, -point.y);
        longitude += 2.0 * std::numbers::pi *
                     std::round((mapCenterLongitude - longitude) / (2.0 * std::numbers::pi));
        position = glm::vec3(static_cast<float>(longitude),
                             static_cast<float>(std::asin(std::clamp(point.z, -1.0, 1.0))), 0.0F);
    }
    const glm::vec4 clip = mvp * glm::vec4(position, 1.0F);
    if (clip.w <= 0.0F) {
        return false;
    }
    if (projection == ViewProjection::Equirectangular) {
        visible = true;
    } else {
        const glm::vec3 worldNormal = glm::normalize(glm::vec3(modelMatrix *
                                                    glm::vec4(glm::vec3(point), 0.0F)));
        const glm::vec3 worldPoint = glm::vec3(modelMatrix * glm::vec4(glm::vec3(point), 1.0F));
        visible = glm::dot(worldNormal, glm::vec3(0.0F, -3.0F, 0.0F) - worldPoint) > 0.0F;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    screenPoint = ImVec2((ndc.x + 1.0F) * 0.5F * displaySize.x,
                         (1.0F - ndc.y) * 0.5F * displaySize.y);
    return true;
}

void drawBrushRing(glm::dvec3 center, double radius, double planetRadius,
                   const glm::mat4& modelMatrix, const glm::mat4& mvp,
                   const ImVec2& displaySize, ImU32 color, ViewProjection projection,
                   double mapCenterLongitude) {
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
            onVisibleHemisphere[static_cast<std::size_t>(i)], projection, mapCenterLongitude);
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

std::optional<glm::dvec3> inspectHoverPoint(const ImVec2& mouse,
                                          const ImVec2& displaySize,
                                          const glm::mat4& modelMatrix,
                                          float zoom, ViewProjection projection) {
    const auto mvp = brushMvp(displaySize, modelMatrix, zoom, projection);
    return pickView(mouse, displaySize, mvp, projection);
}

} // namespace

class RasterImportCommand final : public UndoableCommand {
public:
    RasterImportCommand(UIManager& ui, SphericalGrid& grid, GlobeRenderer& renderer,
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
        renderer_.UpdateLayerElevations(grid_.GetElevationLayer(), elevations);
        ui_.raster_ = raster;
        ui_.loadedRasterPath_ = path;
    }

    UIManager& ui_;
    SphericalGrid& grid_;
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
    BrushStrokeCommand(SphericalGrid& grid, GlobeRenderer& renderer,
                       std::vector<float> before, std::vector<float> after)
        : grid_(grid), renderer_(renderer), before_(std::move(before)), after_(std::move(after)) {}

    void redo() override { Apply(after_); }
    void undo() override { Apply(before_); }

private:
    void Apply(const std::vector<float>& elevations) {
        grid_.SetElevations(elevations);
        renderer_.UpdateLayerElevations(grid_.GetElevationLayer(), elevations);
    }

    SphericalGrid& grid_;
    GlobeRenderer& renderer_;
    std::vector<float> before_;
    std::vector<float> after_;
};

class TerrainFilterCommand final : public UndoableCommand {
public:
    TerrainFilterCommand(SphericalGrid& grid, GlobeRenderer& renderer,
                         std::vector<float> before, std::vector<float> after)
        : grid_(grid), renderer_(renderer), before_(std::move(before)),
          after_(std::move(after)) {}

    void redo() override { Apply(after_); }
    void undo() override { Apply(before_); }

private:
    void Apply(const std::vector<float>& elevations) {
        grid_.SetElevations(elevations);
        renderer_.UpdateLayerElevations(grid_.GetElevationLayer(), elevations);
    }

    SphericalGrid& grid_;
    GlobeRenderer& renderer_;
    std::vector<float> before_;
    std::vector<float> after_;
};

class SelectionCommand final : public UndoableCommand {
public:
    SelectionCommand(SphericalGrid& grid, GlobeRenderer& renderer,
                     std::vector<float> before, bool beforeActive,
                     std::vector<float> after, bool afterActive)
        : grid_(grid), renderer_(renderer), before_(std::move(before)),
          beforeActive_(beforeActive), after_(std::move(after)),
          afterActive_(afterActive) {}

    void redo() override { Apply(after_, afterActive_); }
    void undo() override { Apply(before_, beforeActive_); }

private:
    void Apply(const std::vector<float>& weights, bool active) {
        grid_.SetSelectionWeights(weights, active);
        renderer_.UpdateSelection(grid_.GetSnapshot()->GetVertices(), active);
    }

    SphericalGrid& grid_;
    GlobeRenderer& renderer_;
    std::vector<float> before_;
    bool beforeActive_;
    std::vector<float> after_;
    bool afterActive_;
};

class AdaptiveMeshCommand final : public UndoableCommand {
public:
    AdaptiveMeshCommand(SphericalGrid& grid, GlobeRenderer& renderer,
                        std::shared_ptr<const SphericalGridSnapshot> before,
                        std::shared_ptr<const SphericalGridSnapshot> after)
        : grid_(grid), renderer_(renderer), before_(std::move(before)), after_(std::move(after)) {}

    void redo() override { Apply(after_); }
    void undo() override { Apply(before_); }

private:
    void Apply(const std::shared_ptr<const SphericalGridSnapshot>& snapshot) {
        const double radius = grid_.GetSnapshot()->GetOptions().planetRadius;
        grid_.InstallSnapshot(snapshot);
        grid_.SetPlanetRadius(radius);
        renderer_.UploadGrid(*grid_.GetSnapshot());
    }

    SphericalGrid& grid_;
    GlobeRenderer& renderer_;
    std::shared_ptr<const SphericalGridSnapshot> before_;
    std::shared_ptr<const SphericalGridSnapshot> after_;
};

std::vector<float> selectionWeightsOf(const SphericalGridSnapshot& snapshot) {
    std::vector<float> weights;
    weights.reserve(snapshot.GetVertices().size());
    for (const auto& vertex : snapshot.GetVertices()) {
        weights.push_back(vertex.selectionWeight);
    }
    return weights;
}

UIManager::UIManager(const GridOptions& options, std::filesystem::path colorMapDirectory)
    : options_(options), previousOptions_(options),
      colorMapDirectory_(std::move(colorMapDirectory)) {
    auto scripted = std::make_unique<ScriptedBrushTool>();
    scriptedTool_ = scripted.get();
    brushTools_.RegisterTool(std::move(scripted));
    brushTools_.SetActive(false);
}

void UIManager::SetBrushScriptDirectory(std::filesystem::path directory) {
    if (stroke_) {
        throw std::logic_error("Finish the current stroke before changing the brush library.");
    }
    brushScriptDirectory_ = std::move(directory);
    RescanScriptLibrary();
}

UIManager::~UIManager() {
    if (gridControl_) {
        gridControl_->Cancel();
    }
    if (gridFuture_.valid()) {
        gridFuture_.wait();
    }
}

namespace {

std::shared_ptr<const SphericalGridSnapshot> buildGridSnapshot(
    const GridOptions& options, const std::shared_ptr<const ElevationRaster>& raster,
    GridGenerationControl* control) {
    auto generated = std::make_shared<const SphericalGridSnapshot>(options, control);
    if (!raster) {
        return generated;
    }
    if (control != nullptr && control->IsCancelled()) {
        throw GridGenerationCancelled();
    }
    const auto elevations = raster->SampleElevations(*generated);
    return std::make_shared<const SphericalGridSnapshot>(*generated, elevations);
}

} // namespace

void UIManager::InstallGeneratedGrid(SphericalGrid& grid, GlobeRenderer& renderer,
                                     std::shared_ptr<const SphericalGridSnapshot> snapshot) {
    grid.InstallSnapshot(std::move(snapshot));
    undoStack_.Clear();
    stroke_.reset();
    selectionStroke_.reset();
    polygonPoints_.clear();
    renderer.UploadGrid(*grid.GetSnapshot());
    previousOptions_ = options_;
}

void UIManager::StartGridGeneration(SphericalGrid& grid, GlobeRenderer& renderer) {
    const GridOptions requested = options_;
    previousOptions_ = grid.GetSnapshot()->GetOptions();
    if (requested.pointCount <= backgroundGenerationThreshold) {
        try {
            InstallGeneratedGrid(grid, renderer, buildGridSnapshot(requested, raster_, nullptr));
        } catch (...) {
            options_ = previousOptions_;
            throw;
        }
        return;
    }
    gridControl_ = std::make_unique<GridGenerationControl>();
    gridCancelRequested_ = false;
    gridProgressOpen_ = true;
    gridFuture_ = std::async(std::launch::async,
                             [requested, raster = raster_, control = gridControl_.get()] {
                                 return buildGridSnapshot(requested, raster, control);
                             });
}

void UIManager::PollGridGeneration(SphericalGrid& grid, GlobeRenderer& renderer) {
    if (!gridFuture_.valid() ||
        gridFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    try {
        InstallGeneratedGrid(grid, renderer, gridFuture_.get());
    } catch (const GridGenerationCancelled&) {
        options_ = previousOptions_;
    } catch (const std::exception& error) {
        options_ = previousOptions_;
        legendError_ = std::string("Grid generation failed: ") + error.what();
        legendErrorOpen_ = true;
    }
    gridFuture_ = {};
    gridControl_.reset();
}

void UIManager::RenderGridProgressDialog() {
    if (gridProgressOpen_) {
        ImGui::OpenPopup("Generating Grid");
        gridProgressOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Generating Grid", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoMove)) {
        return;
    }
    if (!gridFuture_.valid()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const float progress = gridControl_ ? gridControl_->GetProgress() : 0.0F;
    ImGui::Text("Generating %zu points...", options_.pointCount);
    ImGui::ProgressBar(progress, ImVec2(320.0F, 0.0F));
    ImGui::BeginDisabled(gridCancelRequested_);
    if (ImGui::Button("Cancel") && gridControl_) {
        gridControl_->Cancel();
        gridCancelRequested_ = true;
    }
    ImGui::EndDisabled();
    if (gridCancelRequested_) {
        ImGui::SameLine();
        ImGui::TextUnformatted("Cancelling after the current stage; the previous grid is kept.");
    }
    ImGui::EndPopup();
}

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

namespace {

std::vector<std::string> scriptDiagnostics(const ScriptError& error) {
    std::vector<std::string> messages;
    for (const auto& diagnostic : error.Diagnostics()) {
        messages.push_back(
            diagnostic.line == 0 ? diagnostic.message
                : "Line " + std::to_string(diagnostic.line) + ", column " +
                      std::to_string(diagnostic.column) + ": " + diagnostic.message);
    }
    if (messages.empty()) {
        messages.emplace_back(error.what());
    }
    return messages;
}

} // namespace

void UIManager::RescanScriptLibrary() {
    // Registry replacement invalidates tool references; never replace it during a stroke.
    if (stroke_) {
        scriptLibraryMessage_ = "Finish the current stroke before refreshing brushes.";
        scriptLibraryMessageIsError_ = true;
        return;
    }
    const auto selectedPath = selectedLibraryScript_ >= 0 &&
            selectedLibraryScript_ < static_cast<int>(scriptLibrary_.entries.size()) ?
        scriptLibrary_.entries[static_cast<std::size_t>(selectedLibraryScript_)].path :
        std::filesystem::path{};
    scriptLibrary_ = brushScriptDirectory_.empty() ? BrushScriptLibrary{} :
        BrushScriptLibrary::Scan(brushScriptDirectory_);
    brushLibraryErrors_ = scriptLibrary_.errors;
    std::vector<std::unique_ptr<BrushTool>> tools;
    for (const auto& entry : scriptLibrary_.entries) {
        try {
            if (entry.file.name == "Custom" ||
                std::any_of(tools.begin(), tools.end(), [&](const auto& tool) {
                    return tool->GetName() == entry.file.name;
                })) {
                throw std::invalid_argument("Duplicate or reserved brush name '" + entry.file.name + "'.");
            }
            auto tool = std::make_unique<ScriptedBrushTool>(entry.file.name);
            tool->SetScript(entry.file.script, entry.file.parameters);
            tool->GetSettings().output = entry.file.output.value_or(ScriptedBrushSettings::Output::Normalized);
            tool->GetSettings().application = entry.file.application;
            tool->GetSettings().continuous = entry.file.continuous;
            tools.push_back(std::move(tool));
        } catch (const std::exception& error) {
            brushLibraryErrors_.push_back(entry.path.string() + ": " + error.what());
        }
    }
    // Preserve the Custom editor/tool and its compiled state across rescans.
    brushTools_.ReplaceTools(std::move(tools), "Custom");
    selectedLibraryScript_ = -1;
    for (std::size_t index = 0; index < scriptLibrary_.entries.size(); ++index) {
        if (scriptLibrary_.entries[index].path == selectedPath) {
            selectedLibraryScript_ = static_cast<int>(index);
        }
    }
}

void UIManager::LoadScriptIntoEditor(const BrushScriptFile& file) {
    scriptName_ = file.name;
    scriptDescription_ = file.description;
    scriptDraft_ = file.script;
    scriptApplication_ = file.application;
    scriptContinuous_ = file.continuous;
    scriptParameters_ = file.parameters;
    scriptParameterValues_.clear();
    for (const auto& parameter : scriptParameters_) {
        scriptParameterValues_.push_back(parameter.defaultValue);
    }
    scriptParameterError_.clear();
    scriptParametersInstalled_ = false;
    scriptErrors_.clear();
    if (scriptedTool_ == nullptr) {
        return;
    }
    if (file.output) {
        scriptedTool_->GetSettings().output = *file.output;
    }
    // A script that fails to compile stays in the editor with its diagnostics; strokes keep
    // using the last compiled script.
    try {
        scriptedTool_->SetScript(file.script, scriptParameters_, scriptParameterValues_);
        scriptedTool_->GetSettings().application = scriptApplication_;
        scriptedTool_->GetSettings().continuous = scriptContinuous_;
        scriptParametersInstalled_ = true;
        brushError_.clear();
    } catch (const ScriptError& error) {
        scriptErrors_ = scriptDiagnostics(error);
    }
}

BrushScriptFile UIManager::CurrentScriptFile() const {
    std::string script = scriptDraft_ ? *scriptDraft_ :
        scriptedTool_ != nullptr ? scriptedTool_->GetScript() : std::string{};
    std::optional<ScriptedBrushSettings::Output> output;
    if (scriptedTool_ != nullptr) {
        output = scriptedTool_->GetSettings().output;
    }
    return {scriptName_, scriptDescription_, std::move(script), output, scriptParameters_,
            scriptApplication_, scriptContinuous_};
}

void UIManager::SaveScriptToLibrary(const std::filesystem::path& path) {
    try {
        std::filesystem::create_directories(path.parent_path());
        CurrentScriptFile().Save(path);
        RescanScriptLibrary();
        for (std::size_t index = 0; index < scriptLibrary_.entries.size(); ++index) {
            if (scriptLibrary_.entries[index].path == path) {
                selectedLibraryScript_ = static_cast<int>(index);
            }
        }
        scriptLibraryMessage_ = "Saved " + path.filename().string() + ".";
        scriptLibraryMessageIsError_ = false;
    } catch (const std::exception& error) {
        scriptLibraryMessage_ = error.what();
        scriptLibraryMessageIsError_ = true;
    }
}

void UIManager::RenderScriptLibrary() {
    ImGui::SeparatorText("Script library");
    const bool hasLibrary = !brushScriptDirectory_.empty();
    const auto& entries = scriptLibrary_.entries;
    const bool validSelection = selectedLibraryScript_ >= 0 &&
        selectedLibraryScript_ < static_cast<int>(entries.size());
    const char* preview = validSelection ?
        entries[static_cast<std::size_t>(selectedLibraryScript_)].file.name.c_str() :
        entries.empty() ? "(no scripts)" : "(choose a script)";
    ImGui::BeginDisabled(!hasLibrary || entries.empty());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5F);
    if (ImGui::BeginCombo("##ScriptLibrary", preview)) {
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            ImGui::PushID(static_cast<int>(index));
            const bool selected = static_cast<int>(index) == selectedLibraryScript_;
            if (ImGui::Selectable(entry.file.name.c_str(), selected,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedLibraryScript_ = static_cast<int>(index);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    try {
                        const auto file = BrushScriptFile::Load(entry.path);
                        LoadScriptIntoEditor(file);
                        scriptLibraryMessage_ = "Loaded " + file.name + ".";
                        scriptLibraryMessageIsError_ = false;
                    } catch (const std::exception& error) {
                        scriptLibraryMessage_ = error.what();
                        scriptLibraryMessageIsError_ = true;
                    }
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", entry.path.filename().string().c_str());
            }
            if (!entry.file.description.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", entry.file.description.c_str());
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (validSelection && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const auto& entry = entries[static_cast<std::size_t>(selectedLibraryScript_)];
        ImGui::SetTooltip("%s\n%s", entry.file.description.c_str(),
                          entry.path.string().c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!validSelection);
    if (ImGui::Button("Load")) {
        const auto& entry = entries[static_cast<std::size_t>(selectedLibraryScript_)];
        // Reload from disk so edits made outside the application are picked up.
        try {
            const auto file = BrushScriptFile::Load(entry.path);
            LoadScriptIntoEditor(file);
            scriptLibraryMessage_ = "Loaded " + file.name + ".";
            scriptLibraryMessageIsError_ = false;
        } catch (const std::exception& error) {
            scriptLibraryMessage_ = error.what();
            scriptLibraryMessageIsError_ = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasLibrary);
    if (ImGui::Button("Refresh")) {
        RescanScriptLibrary();
        scriptLibraryMessage_.clear();
    }
    ImGui::EndDisabled();
    if (hasLibrary && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Rescan %s", brushScriptDirectory_.string().c_str());
    }

    ImGui::InputText("Name", &scriptName_);
    ImGui::InputTextMultiline("Description", &scriptDescription_,
                              ImVec2(0.0F, ImGui::GetTextLineHeight() * 2.5F));
    const bool named = !scriptName_.empty();
    ImGui::BeginDisabled(!named || !hasLibrary);
    if (ImGui::Button("Save to library")) {
        const auto path = brushScriptDirectory_ / BrushScriptFileName(scriptName_);
        std::error_code ignored;
        if (std::filesystem::exists(path, ignored)) {
            scriptOverwritePath_ = path;
            ImGui::OpenPopup("Replace Brush Script?");
        } else {
            SaveScriptToLibrary(path);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!hasLibrary) {
            ImGui::SetTooltip("No script library folder is configured.");
        } else if (!named) {
            ImGui::SetTooltip("Enter a name to save the script.");
        } else {
            ImGui::SetTooltip("Saves the editor's script as %s in %s.",
                              BrushScriptFileName(scriptName_).string().c_str(),
                              brushScriptDirectory_.string().c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import...")) {
        BeginProjectDialog(ProjectAction::ImportScript);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!named);
    if (ImGui::Button("Export...")) {
        BeginProjectDialog(ProjectAction::ExportScript);
    }
    ImGui::EndDisabled();
    if (!named && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Enter a name to export the script.");
    }

    if (ImGui::BeginPopupModal("Replace Brush Script?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Replace %s?", scriptOverwritePath_.string().c_str());
        if (ImGui::Button("Replace")) {
            SaveScriptToLibrary(scriptOverwritePath_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!scriptLibraryMessage_.empty()) {
        const auto colour = scriptLibraryMessageIsError_ ? ImVec4(1.0F, 0.4F, 0.4F, 1.0F) :
                                                           ImVec4(0.6F, 0.9F, 0.6F, 1.0F);
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::TextWrapped("%s", scriptLibraryMessage_.c_str());
        ImGui::PopStyleColor();
    }
    for (const auto& message : brushLibraryErrors_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.72F, 0.32F, 1.0F));
        ImGui::TextWrapped("Skipped: %s", message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

void UIManager::RenderScriptParameters(ScriptedBrushTool& tool, bool custom) {
    const auto definitions = custom ? scriptParameters_ : tool.GetParameters();
    auto values = custom ? scriptParameterValues_ : tool.GetParameterValues();
    if (!definitions.empty()) {
        ImGui::SeparatorText("Script parameters");
        for (std::size_t i = 0; i < definitions.size(); ++i) {
            auto parameter = definitions[i];
            auto value = values[i];
            double displayScale = 1.0;
            if (!parameter.units.empty()) {
                const auto unit = std::find_if(definitions.begin(), definitions.end(),
                    [&](const auto& candidate) { return candidate.name == parameter.units; });
                const auto index = static_cast<std::size_t>(unit - definitions.begin());
                if (std::get<std::string>(values[index]) == "Miles") {
                    displayScale = 0.621371192;
                    parameter.minimum *= displayScale;
                    parameter.maximum *= displayScale;
                    value = std::get<double>(value) * displayScale;
                }
            }
            bool changed = false;
            ImGui::PushID(static_cast<int>(i));
            const auto label = (parameter.label.empty() ? parameter.name : parameter.label) +
                               "###ScriptParameter";
            if (parameter.type == ScriptParameter::Type::Choice) {
                auto& text = std::get<std::string>(value);
                if (ImGui::BeginCombo(label.c_str(), text.c_str())) {
                    for (const auto& choice : parameter.choices) {
                        if (ImGui::Selectable(choice.c_str(), choice == text)) {
                            text = choice;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            } else if (parameter.type == ScriptParameter::Type::Integer) {
                std::int64_t number = static_cast<std::int64_t>(std::get<double>(value));
                if (parameter.control == ScriptParameter::Control::Slider) {
                    const auto minimum = static_cast<std::int64_t>(parameter.minimum);
                    const auto maximum = static_cast<std::int64_t>(parameter.maximum);
                    changed = ImGui::SliderScalar(label.c_str(), ImGuiDataType_S64, &number,
                        &minimum, &maximum, "%lld", ImGuiSliderFlags_AlwaysClamp);
                } else {
                    const std::int64_t step = 1;
                    const std::int64_t fastStep = 10;
                    const bool spin = parameter.control == ScriptParameter::Control::Spin;
                    changed = ImGui::InputScalar(label.c_str(), ImGuiDataType_S64, &number,
                                                 spin ? &step : nullptr, spin ? &fastStep : nullptr);
                }
                value = static_cast<double>(number);
            } else {
                auto& number = std::get<double>(value);
                if (parameter.control == ScriptParameter::Control::Slider) {
                    changed = ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, &number,
                        &parameter.minimum, &parameter.maximum, "%.6g",
                        ImGuiSliderFlags_AlwaysClamp |
                        (parameter.scale == ScriptParameter::Scale::Log ?
                            ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None));
                } else {
                    const double step = parameter.control == ScriptParameter::Control::Spin ?
                        (parameter.scale == ScriptParameter::Scale::Log ?
                            number * 0.01 : parameter.maximum * 0.01 - parameter.minimum * 0.01) : 0.0;
                    changed = ImGui::InputDouble(label.c_str(), &number, step, step * 10.0, "%.6g");
                }
            }
            ImGui::PopID();
            if (changed) {
                try {
                    if (displayScale != 1.0) {
                        value = std::get<double>(value) / displayScale;
                    }
                    definitions[i].ValidateValue(value);
                    if (!custom || scriptParametersInstalled_) {
                        tool.SetParameterValue(i, value);
                    }
                    if (custom) {
                        scriptParameterValues_[i] = std::move(value);
                    }
                    scriptParameterError_.clear();
                } catch (const std::invalid_argument& error) {
                    scriptParameterError_ = error.what();
                }
            }
        }
        ImGui::TextDisabled("Edit parameter definitions and defaults in the TOML file, then reload.");
    }
    if (!scriptParameterError_.empty()) {
        ImGui::TextWrapped("%s", scriptParameterError_.c_str());
    }
}

void UIManager::RenderScriptEditor(ScriptedBrushTool& tool) {
    RenderScriptLibrary();
    RenderScriptParameters(tool, true);
    const bool matchingParameters = scriptParametersInstalled_;
    auto& parameters = tool.GetSettings();
    constexpr std::array outputNames{"Normalised (-1 to 1 mapped to range)",
                                     "Elevation (clamped to range)"};
    int output = static_cast<int>(parameters.output);
    if (ImGui::Combo("Script output", &output, outputNames.data(),
                     static_cast<int>(outputNames.size()))) {
        parameters.output = static_cast<ScriptedBrushSettings::Output>(output);
    }

    const std::string compiled = tool.GetScript();
    if (!scriptDraft_) {
        scriptDraft_ = compiled;
    }
    ImGui::TextUnformatted("Script");
    ImGui::InputTextMultiline("##BrushScript", &*scriptDraft_,
                              ImVec2(-1.0F, ImGui::GetTextLineHeight() * 10.0F),
                              ImGuiInputTextFlags_AllowTabInput);
    const bool modified = *scriptDraft_ != compiled || !matchingParameters;
    ImGui::BeginDisabled(!modified);
    if (ImGui::Button("Compile")) {
        try {
            tool.SetScript(*scriptDraft_, scriptParameters_, scriptParameterValues_);
            tool.GetSettings().application = scriptApplication_;
            tool.GetSettings().continuous = scriptContinuous_;
            scriptParametersInstalled_ = true;
            scriptErrors_.clear();
            brushError_.clear();
        } catch (const ScriptError& error) {
            scriptErrors_ = scriptDiagnostics(error);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        scriptDraft_ = compiled;
        scriptParameters_ = tool.GetParameters();
        scriptParameterValues_ = tool.GetParameterValues();
        scriptApplication_ = tool.GetSettings().application;
        scriptContinuous_ = tool.GetSettings().continuous;
        scriptParametersInstalled_ = true;
        scriptParameterError_.clear();
        scriptErrors_.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Default")) {
        scriptDraft_ = std::string(ScriptedBrushTool::DefaultScript);
    }
    if (modified) {
        ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.32F, 1.0F),
                           "Uncompiled changes. Strokes use the last compiled script.");
    }
    for (const auto& message : scriptErrors_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::CollapsingHeader("Script reference")) {
        ImGui::TextWrapped(
            "The value of the final statement is the brush output. Inputs: x, y, z "
            "(unit position, domain-warped when enabled), lat, lon (degrees), elevation "
            "(current value), weight (falloff x selection), distance and radius (brush "
            "units), lower and upper (ordered range), elapsed (seconds per dab), "
            "rate (configured elevation units per second).");
        ImGui::TextWrapped(
            "Noise (uses the seed, returns -1 to 1): simplex(x,y,z), valuenoise(x,y,z), "
            "fbm(x,y,z,freq,lacunarity,persistence,octaves), billow(...same...), "
            "valuefbm(...same...), ridged(...same...). Uses FastNoise2; value noise "
            "has cubic interpolation. Fractals use 1-16 octaves and nonnegative "
            "frequency, lacunarity and persistence. Coordinates at each octave "
            "must stay within +/-1e8.");
        ImGui::TextWrapped(
            "spectral(x,y,z,frequency,exponent,resolution): periodic power-law noise, "
            "exponent -4 to 8, resolution 8/16/32/64/128. "
            "gaussian(range,sigma,unit): pre-dab neighbourhood average; unit 0 is kilometres, "
            "1 is miles. TOML application='elevation' blends direct heights; application='delta' "
            "adds weight-scaled changes. continuous=true evaluates while held.");
        ImGui::TextWrapped(
            "Also available: + - * / %% ^, comparisons, if/else, for/while loops, "
            "var locals, abs, min, max, clamp(lo,x,hi), sqrt, exp, log, pow, sin, cos, "
            "floor, round, pi. Names are case-sensitive; return is not allowed. "
            "Script parameters are available by name; compare choice values with quoted "
            "strings, e.g. mode == 'one'. Parameter changes affect new strokes. "
            "Save/export preserves the TOML defaults, not the current control values.");
    }
}

void UIManager::FillSelection(SphericalGrid& grid, GlobeRenderer& renderer) {
    try {
        const auto snapshot = grid.GetSnapshot();
        std::vector<float> before;
        before.reserve(snapshot->GetVertices().size());
        for (const auto& vertex : snapshot->GetVertices()) {
            before.push_back(vertex.elevation);
        }
        auto after = before;
        auto settings = brushTools_.GetSettings();
        settings.coverage = BrushCoverage::Selection;
        brushTools_.GetSelectedTool().Apply(*snapshot, {0.0, 0.0, 1.0}, after, settings, 1.0);
        if (before != after) {
            undoStack_.Execute(std::make_unique<BrushStrokeCommand>(
                grid, renderer, std::move(before), std::move(after)));
        }
        brushError_.clear();
    } catch (const std::exception& error) {
        brushError_ = std::string("Fill selection failed: ") + error.what();
    }
}

void UIManager::RenderBrushPanel(SphericalGrid& grid, GlobeRenderer& renderer) {
    if (!brushTools_.IsActive()) {
        return;
    }
    bool open = true;
    auto& tool = brushTools_.GetSelectedTool();
    auto* scripted = dynamic_cast<ScriptedBrushTool*>(&tool);
    NoiseSettings* noiseCommon = scripted != nullptr &&
        scripted->GetSettings().application == BrushApplication::Noise ?
            &scripted->GetSettings() : nullptr;
    const std::string title = std::string(tool.GetName()) + "###BrushParameters";
    if (ImGui::Begin(title.c_str(), &open)) {
        if (!brushError_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
            ImGui::TextWrapped("Brush failed: %s", brushError_.c_str());
            ImGui::PopStyleColor();
        }
        auto& settings = brushTools_.GetSettings();
        if (noiseCommon != nullptr) {
            constexpr std::array operationNames{"Replace", "Add", "Subtract",
                                                "Minimum", "Maximum"};
            auto& operation = noiseCommon->operation;
            int selectedOperation = static_cast<int>(operation);
            if (ImGui::Combo("Operation", &selectedOperation, operationNames.data(),
                             static_cast<int>(operationNames.size()))) {
                operation = static_cast<NoiseSettings::Operation>(selectedOperation);
            }
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
        ImGui::DragFloat("Outer radius", &radius, 10.0F, 0.1F,
                         maximumRadius, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        radius = clipUiValue(radius, 0.1F, maximumRadius, 500.0F);
        settings.radius = radius;
        showRangeTooltip(0.1F, maximumRadius);
        if (noiseCommon != nullptr) {
            const double planetRadius = PlanetRadiusInUnit(*grid.GetSnapshot(), settings.distanceUnit);
            const double angularRadius = std::min(settings.radius / planetRadius,
                                                  std::numbers::pi);
            const double expectedVertices = static_cast<double>(options_.pointCount) *
                (1.0 - std::cos(angularRadius)) * 0.5;
            if (expectedVertices < 32.0) {
                ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.32F, 1.0F),
                    "Only about %.0f mesh points fall inside this brush. Increase the radius "
                    "or grid density to see a noise pattern.", expectedVertices);
            } else {
                ImGui::TextDisabled("About %.0f mesh points are affected at this radius.",
                                    expectedVertices);
            }
            ImGui::TextDisabled("Noise settings affect new strokes.");
        }
        float falloff = static_cast<float>(settings.falloffPercent);
        ImGui::SliderFloat("Falloff (%)", &falloff, 0.0F, 100.0F, "%.0f%%",
                           ImGuiSliderFlags_AlwaysClamp);
        falloff = clipUiValue(falloff, 0.0F, 100.0F, 25.0F);
        settings.falloffPercent = falloff;
        showRangeTooltip(0.0F, 100.0F);
        if (noiseCommon != nullptr) {
            NoiseSettings& common = *noiseCommon;
            ImGui::InputScalar("Seed", ImGuiDataType_U64, &common.seed);
            ImGui::SameLine();
            if (ImGui::Button("Randomise Seed")) {
                try {
                    std::random_device device;
                    std::seed_seq seedSequence{device(), device(), device(), device(),
                                               device(), device(), device(), device()};
                    std::mt19937_64 generator(seedSequence);
                    common.seed = generator();
                    simplexSeedError_.clear();
                } catch (const std::exception& error) {
                    simplexSeedError_ = error.what();
                }
            }
            if (!simplexSeedError_.empty()) {
                ImGui::TextWrapped("Could not randomise seed: %s", simplexSeedError_.c_str());
            }
            ImGui::SliderFloat("Between", &common.rangeBetween, -32767.0F, 32767.0F,
                               "%.0f", ImGuiSliderFlags_AlwaysClamp);
            common.rangeBetween = clipUiValue(common.rangeBetween, -32767.0F, 32767.0F,
                                              -32767.0F);
            showRangeTooltip(-32767.0F, 32767.0F);
            ImGui::SliderFloat("And", &common.rangeAnd, -32767.0F, 32767.0F,
                               "%.0f", ImGuiSliderFlags_AlwaysClamp);
            common.rangeAnd = clipUiValue(common.rangeAnd, -32767.0F, 32767.0F, 32767.0F);
            showRangeTooltip(-32767.0F, 32767.0F);

            ImGui::Checkbox("Domain warp", &common.domainWarpEnabled);
            if (common.domainWarpEnabled) {
                ImGui::SliderFloat("Warp frequency", &common.warpFrequency, 0.01F, 64.0F,
                                   "%.2f", ImGuiSliderFlags_Logarithmic |
                                               ImGuiSliderFlags_AlwaysClamp);
                common.warpFrequency = clipUiValue(common.warpFrequency, 0.01F, 64.0F, 4.0F);
                showRangeTooltip(0.01F, 64.0F);
                ImGui::SliderFloat("Warp strength", &common.warpStrength, 0.0F, 5.0F,
                                   "%.2f", ImGuiSliderFlags_AlwaysClamp);
                common.warpStrength = clipUiValue(common.warpStrength, 0.0F, 5.0F, 1.25F);
                showRangeTooltip(0.0F, 5.0F);
                ImGui::SliderInt("Warp octaves", &common.warpOctaves, 1, 16,
                                 "%d", ImGuiSliderFlags_AlwaysClamp);
                common.warpOctaves = std::clamp(common.warpOctaves, 1, 16);
                showRangeTooltip(1, 16);
                ImGui::SliderFloat("Warp lacunarity", &common.warpLacunarity, 1.0F, 4.0F,
                                   "%.2f", ImGuiSliderFlags_AlwaysClamp);
                common.warpLacunarity = clipUiValue(common.warpLacunarity, 1.0F, 4.0F, 2.0F);
                showRangeTooltip(1.0F, 4.0F);
                ImGui::SliderFloat("Warp persistence", &common.warpPersistence, 0.0F, 1.0F,
                                   "%.2f", ImGuiSliderFlags_AlwaysClamp);
                common.warpPersistence = clipUiValue(common.warpPersistence, 0.0F, 1.0F, 0.5F);
                showRangeTooltip(0.0F, 1.0F);
                ImGui::TextDisabled("Higher strength folds the noise field more dramatically.");
            }
        }
        if (scripted == scriptedTool_) {
            RenderScriptEditor(*scripted);
        } else if (scripted != nullptr) {
            RenderScriptParameters(*scripted, false);
        }
        ImGui::BeginDisabled(!grid.HasActiveLayer() || stroke_.has_value());
        if (ImGui::Button("Fill selection")) {
            FillSelection(grid, renderer);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "Apply once using selection feathering, ignoring radius and radial falloff.\n"
                "Without a selection, fill the active layer. Continuous brushes apply one second.\n"
                "Scripts receive distance=0 and the configured radius.");
        }
        ImGui::TextWrapped("Left-drag over the globe to apply this tool. Falloff sets the tapered portion of the outer radius: 0%% is a cylinder, 100%% is a cone.");
        if (tool.IsContinuous()) {
            ImGui::TextWrapped("The script runs continuously while held. The rate input is %.0f units per second, configured in File > Settings.",
                              settings.raiseLowerRate);
        } else {
            ImGui::TextWrapped("Each click or drag applies the operation once; holding the button or retracing the stroke does not build it up further.");
        }
    }
    ImGui::End();
    if (!open) {
        brushTools_.SetActive(false);
    }
}

void UIManager::RenderSelectionPanel() {
    if (selectionMode_ == SelectionMode::None) {
        return;
    }
    bool open = true;
    const char* title = selectionMode_ == SelectionMode::Radius
                            ? "Radius Selection###SelectionOptions"
                            : "Polygon Selection###SelectionOptions";
    if (ImGui::Begin(title, &open)) {
        if (selectionMode_ == SelectionMode::Radius) {
            int unit = static_cast<int>(selectionSettings_.distanceUnit);
            constexpr std::array unitNames{"Kilometres", "Miles"};
            if (ImGui::Combo("Distance unit", &unit, unitNames.data(),
                             static_cast<int>(unitNames.size()))) {
                if (unit != static_cast<int>(selectionSettings_.distanceUnit)) {
                    selectionSettings_.radius *= unit == static_cast<int>(BrushDistanceUnit::Miles)
                                                     ? 0.621371192 : 1.609344;
                    selectionSettings_.distanceUnit = static_cast<BrushDistanceUnit>(unit);
                }
            }
            float radius = static_cast<float>(selectionSettings_.radius);
            const float maximumRadius = selectionSettings_.distanceUnit ==
                                                BrushDistanceUnit::Kilometres
                                            ? 20020.0F : 12500.0F;
            if (ImGui::DragFloat("Outer radius", &radius, 10.0F, 0.1F,
                                 maximumRadius, "%.1f")) {
                selectionSettings_.radius = std::clamp(static_cast<double>(radius), 0.1,
                                                        static_cast<double>(maximumRadius));
            }
            float falloff = static_cast<float>(selectionSettings_.falloffPercent);
            if (ImGui::SliderFloat("Falloff (%)", &falloff, 0.0F, 100.0F, "%.0f%%")) {
                selectionSettings_.falloffPercent = falloff;
            }
            ImGui::TextWrapped("Left-drag to select. Hold Shift when starting to add to the existing selection. The inner radius is fully selected; the outer edge feathers to zero.");
        } else {
            ImGui::TextWrapped("Click the globe to add polygon points. Hold Shift on the first point to add to the existing selection. Double-click or press Enter to finish; press Escape to cancel.");
            ImGui::Text("Points: %zu", polygonPoints_.size());
        }
    }
    ImGui::End();
    if (!open) {
        SetSelectionMode(SelectionMode::None);
    }
}

void UIManager::SetSelectionMode(SelectionMode mode) {
    brushTools_.SetActive(false);
    selectionMode_ = mode;
    polygonPoints_.clear();
    polygonAdditive_ = false;
}

void UIManager::RenderValueSelectionDialog(SphericalGrid& grid,
                                           GlobeRenderer& renderer) {
    if (valueSelectionOpen_) {
        valueSelectionError_.clear();
        ImGui::OpenPopup("Select By Value");
        valueSelectionOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Select By Value", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const float inputOffset = ImGui::GetCursorPosX() + ImGui::CalcTextSize("Between").x +
                                  ImGui::GetStyle().ItemSpacing.x;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Between");
        ImGui::SameLine(inputOffset);
        ImGui::InputDouble("##LowerBound", &selectionLowerBound_, 0.0, 0.0, "%.3f");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("and");
        ImGui::SameLine(inputOffset);
        ImGui::InputDouble("##UpperBound", &selectionUpperBound_, 0.0, 0.0, "%.3f");
        ImGui::SliderFloat("Feather (%)", &selectionFeatherPercent_, 0.0F, 100.0F, "%.0f%%");
        ImGui::TextWrapped("0%% selects the entire range with hard edges. 100%% tapers linearly from full selection at the range midpoint to zero at both bounds.");
        if (!valueSelectionError_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.35F, 0.3F, 1.0F));
            ImGui::TextWrapped("%s", valueSelectionError_.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::Button("OK")) {
            try {
                if (selectionLowerBound_ > selectionUpperBound_) {
                    std::swap(selectionLowerBound_, selectionUpperBound_);
                }
                const auto snapshot = grid.GetSnapshot();
                auto after = SelectByValue(*snapshot, selectionLowerBound_,
                                          selectionUpperBound_, selectionFeatherPercent_);
                undoStack_.Execute(std::make_unique<SelectionCommand>(
                    grid, renderer, selectionWeightsOf(*snapshot), snapshot->HasSelection(),
                    std::move(after), true));
                polygonPoints_.clear();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& error) {
                valueSelectionError_ = error.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void UIManager::ApplyTerrainFilter(const Filter& filter, SphericalGrid& grid,
                                   GlobeRenderer& renderer) {
    const auto snapshot = grid.GetSnapshot();
    std::vector<float> before;
    before.reserve(snapshot->GetVertices().size());
    for (const auto& vertex : snapshot->GetVertices()) {
        before.push_back(vertex.elevation);
    }
    auto after = filter.Apply(*snapshot, before);
    if (before != after) {
        undoStack_.Execute(std::make_unique<TerrainFilterCommand>(
            grid, renderer, std::move(before), std::move(after)));
    }
}

void UIManager::RenderFillFilterDialog(SphericalGrid& grid,
                                       GlobeRenderer& renderer) {
    if (fillFilterOpen_) {
        ImGui::OpenPopup("Fill Filter");
        fillFilterOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Fill Filter", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    constexpr std::array operationNames{"Replace", "Raise", "Lower", "Minimum", "Maximum"};
    int operation = static_cast<int>(pendingFillSettings_.operation);
    if (ImGui::Combo("Operation", &operation, operationNames.data(),
                     static_cast<int>(operationNames.size()))) {
        pendingFillSettings_.operation = static_cast<FillOperation>(operation);
    }
    ImGui::SliderFloat("Fill value", &pendingFillSettings_.value,
                       -32767.0F, 32767.0F, "%.0f", ImGuiSliderFlags_AlwaysClamp);
    pendingFillSettings_.value = clipUiValue(
        pendingFillSettings_.value, -32767.0F, 32767.0F, 0.0F);
    showRangeTooltip(-32767.0F, 32767.0F);
    ImGui::TextWrapped("Raise adds the fill value; Lower subtracts it. Selection weights blend each operation, and no active selection affects the entire terrain.");
    if (!fillFilterError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.35F, 0.3F, 1.0F));
        ImGui::TextWrapped("%s", fillFilterError_.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::Button("Apply")) {
        try {
            auto& filter = dynamic_cast<FillFilter&>(filters_.GetFilter("Fill"));
            filter.SetSettings(pendingFillSettings_);
            ApplyTerrainFilter(filter, grid, renderer);
            fillFilterError_.clear();
            ImGui::CloseCurrentPopup();
        } catch (const std::exception& error) {
            fillFilterError_ = error.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        fillFilterError_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void UIManager::RenderAbsoluteMagnitudeNoiseDialog(SphericalGrid& grid, GlobeRenderer& renderer) {
    if (absoluteMagnitudeNoiseOpen_) {
        ImGui::OpenPopup("Absolute Magnitude Noise");
        absoluteMagnitudeNoiseOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Absolute Magnitude Noise", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::SliderFloat("Amplitude (feet)", &pendingNoiseAmplitudeFeet_, 0.0F, 500.0F,
                       "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.0F);
    ImGui::TextWrapped("Adds independent uniform random offsets from -amplitude to +amplitude, multiplied by selection feathering. No active selection affects the whole globe, including underwater terrain. Elevations are clamped to [-32767, 32767] metres.");
    if (!absoluteMagnitudeNoiseError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.35F, 0.3F, 1.0F));
        ImGui::TextWrapped("%s", absoluteMagnitudeNoiseError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Apply")) {
        try {
            auto& filter = dynamic_cast<AbsoluteMagnitudeNoiseFilter&>(
                filters_.GetFilter("Absolute Magnitude Noise"));
            filter.SetAmplitudeFeet(pendingNoiseAmplitudeFeet_);
            ApplyTerrainFilter(filter, grid, renderer);
            absoluteMagnitudeNoiseError_.clear();
            ImGui::CloseCurrentPopup();
        } catch (const std::exception& error) {
            absoluteMagnitudeNoiseError_ = error.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        absoluteMagnitudeNoiseError_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void UIManager::RenderInciseFlowDialog(SphericalGrid& grid, GlobeRenderer& renderer) {
    if (inciseFlowOpen_) {
        ImGui::OpenPopup("Incise Flow");
        inciseFlowOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Incise Flow", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::InputDouble("Incision rate (m/pass)", &pendingInciseFlowSettings_.incisionRate, 0.0, 0.0, "%.6g");
    ImGui::InputDouble("Reference area (runoff)", &pendingInciseFlowSettings_.referenceArea, 0.0, 0.0, "%.6g");
    ImGui::InputDouble("Flow exponent (m)", &pendingInciseFlowSettings_.flowExponent, 0.0, 0.0, "%.6g");
    ImGui::InputDouble("Slope exponent (n)", &pendingInciseFlowSettings_.slopeExponent, 0.0, 0.0, "%.6g");
    ImGui::InputDouble("Precipitation (per vertex)", &pendingInciseFlowSettings_.precipitation, 0.0, 0.0, "%.6g");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 560.0F);
    ImGui::TextWrapped("One pass: C * (A / Aref)^m * S^n. Runoff starts at the precipitation value per vertex; Aref uses the same units (vertex count when precipitation is 1). Slopes use m/km.");
    ImGui::TextWrapped("Typical m: 0.3 to 0.8; default n: 1. Reference area must be positive; other parameters must be non-negative. All parameters must be finite. Planet radius is set in File -> Planet -> Geometry.");
    ImGui::TextWrapped("Sea-level/ocean sinks and unselected outlets remain fixed. Selection weights blend incision; no selection processes the globe. Pits and flats without downhill receivers remain unchanged: run Fill basins first if needed. Results are limited to [-32767, 32767] m.");
    if (!inciseFlowError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.35F, 0.3F, 1.0F));
        ImGui::TextWrapped("%s", inciseFlowError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Apply")) {
        try {
            auto& filter = dynamic_cast<InciseFlowFilter&>(filters_.GetFilter("Incise flow"));
            filter.SetSettings(pendingInciseFlowSettings_);
            ApplyTerrainFilter(filter, grid, renderer);
            inciseFlowError_.clear();
            ImGui::CloseCurrentPopup();
        } catch (const std::exception& error) {
            inciseFlowError_ = error.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        inciseFlowError_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void UIManager::ApplyActiveLayerColorMaps(SphericalGrid& grid, GlobeRenderer& renderer) {
    if (!grid.HasActiveLayer()) {
        renderer.ClearLayerStack();
        return;
    }
    if (colorMapDirectory_.empty()) {
        renderer.UpdateElevations(grid.GetSnapshot()->GetVertices());
        return;
    }
    const auto& layer = grid.GetElevationLayer();
    renderer.SetColorMap(GetColorMap(std::string(layer.GetColorMap())));
    renderer.SetSelectionColorMap(GetColorMap(std::string(layer.GetSelectionColorMap())));
    colorMapName_ = layer.GetColorMap();
    selectionColorMapName_ = layer.GetSelectionColorMap();
}

const ColorMap& UIManager::GetColorMap(const std::string& name) {
    const auto found = colorMapCache_.find(name);
    if (found != colorMapCache_.end()) {
        return found->second;
    }
    auto [inserted, wasInserted] = colorMapCache_.emplace(
        name, ColorMap::Load(colorMapDirectory_ / name));
    (void)wasInserted;
    return inserted->second;
}

void UIManager::RenderLayerPanel(SphericalGrid& grid, GlobeRenderer& renderer) {
    const auto colorMaps = [&]() {
        std::vector<std::filesystem::path> maps;
        if (colorMapDirectory_.empty()) {
            return maps;
        }
        try {
            for (const auto& entry : std::filesystem::directory_iterator(colorMapDirectory_)) {
                if (entry.is_regular_file() && entry.path().extension() == ".map") {
                    maps.push_back(entry.path());
                }
            }
        } catch (const std::exception& error) {
            layerError_ = error.what();
        }
        std::sort(maps.begin(), maps.end());
        return maps;
    };
    if (ImGui::Begin("Layers")) {
        if (layerDialogOpen_) {
            std::fill(newLayerName_.begin(), newLayerName_.end(), '\0');
            const std::string defaultName = "Elevation " +
                                            std::to_string(grid.GetLayers().size() + 1);
            std::copy_n(defaultName.c_str(),
                        std::min(defaultName.size(), newLayerName_.size() - 1),
                        newLayerName_.begin());
            const auto& templateLayer = grid.HasActiveLayer()
                                            ? grid.GetElevationLayer()
                                            : *grid.GetLayers().front();
            newLayerColorMap_ = std::string(templateLayer.GetColorMap());
            newLayerSelectionColorMap_ = std::string(templateLayer.GetSelectionColorMap());
            ImGui::OpenPopup("New Layer");
            layerDialogOpen_ = false;
        }
        if (deleteLayerDialogOpen_) {
            ImGui::OpenPopup("Delete Layer");
            deleteLayerDialogOpen_ = false;
        }
        if (ImGui::Button("New layer")) {
            layerError_.clear();
            layerDialogOpen_ = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(grid.GetLayers().size() <= 1 || !grid.HasActiveLayer());
        if (ImGui::Button("Delete layer")) {
            pendingLayerDeletion_ = std::string(grid.GetActiveLayer().GetName());
            deleteLayerDialogOpen_ = true;
        }
        ImGui::EndDisabled();
        if (!layerError_.empty()) {
            ImGui::TextWrapped("%s", layerError_.c_str());
        }

        const auto renderMapChoice = [&](const char* label, std::string value,
                                         ElevationLayer* layer) {
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            const ImVec2 previewSize(98.0F, ImGui::GetFrameHeight());
            ImGui::InvisibleButton(label, previewSize);
            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            if (!colorMapDirectory_.empty()) {
                try {
                    const auto stops = GetColorMap(value).GetStops();
                    constexpr int gradientSegments = 24;
                    for (int segment = 0; segment < gradientSegments; ++segment) {
                        const float t = (static_cast<float>(segment) + 0.5F) /
                                        static_cast<float>(gradientSegments);
                        const float elevation = -32767.0F + t * 65534.0F;
                        std::size_t high = 1;
                        while (high + 1 < stops.size() && elevation > stops[high].x) {
                            ++high;
                        }
                        const auto& lowStop = stops[high - 1];
                        const auto& highStop = stops[high];
                        const float blend = (elevation - lowStop.x) /
                                            (highStop.x - lowStop.x);
                        const glm::vec3 color = glm::mix(
                            glm::vec3(lowStop.y, lowStop.z, lowStop.w),
                            glm::vec3(highStop.y, highStop.z, highStop.w),
                            std::clamp(blend, 0.0F, 1.0F));
                        const float x0 = minimum.x + previewSize.x * segment /
                                                     gradientSegments;
                        const float x1 = minimum.x + previewSize.x * (segment + 1) /
                                                     gradientSegments;
                        drawList->AddRectFilled(
                            ImVec2(x0, minimum.y), ImVec2(x1, maximum.y),
                            ImGui::ColorConvertFloat4ToU32(
                                ImVec4(color.r, color.g, color.b, 1.0F)));
                    }
                } catch (const std::exception& error) {
                    layerError_ = error.what();
                }
            }
            drawList->AddRect(minimum, maximum, ImGui::GetColorU32(ImGuiCol_Border));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", value.c_str());
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const std::string comboId = std::string("##") + label;
            if (ImGui::BeginCombo(comboId.c_str(), value.c_str())) {
                for (const auto& path : colorMaps()) {
                    const std::string name = path.filename().string();
                    if (ImGui::Selectable(name.c_str(), name == value)) {
                        value = name;
                        if (std::string_view(label) == "Legend") {
                            layer->SetColorMap(name);
                        } else {
                            layer->SetSelectionColorMap(name);
                        }
                        if (grid.HasActiveLayer() && layer == &grid.GetElevationLayer()) {
                            try {
                                ApplyActiveLayerColorMaps(grid, renderer);
                                layerError_.clear();
                            } catch (const std::exception& error) {
                                layerError_ = error.what();
                            }
                        }
                    }
                }
                ImGui::EndCombo();
            }
        };

        bool reordered = false;
        for (std::size_t index = 0; index < grid.GetLayers().size(); ++index) {
            auto& layer = *grid.GetLayers()[index];
            ImGui::PushID(static_cast<int>(index));
            const bool isActive = grid.HasActiveLayer() &&
                                  grid.GetActiveLayer().GetName() == layer.GetName();
            bool visible = layer.IsVisible();
            if (ImGui::Checkbox("##visible", &visible)) {
                if (isActive && !visible) {
                    if (stroke_ || selectionStroke_) {
                        layerError_ = "Finish the current stroke before hiding the active layer.";
                    } else {
                        grid.SetLayerVisible(layer.GetName(), false);
                        brushTools_.SetActive(false);
                        undoStack_.Clear();
                    }
                } else {
                    grid.SetLayerVisible(layer.GetName(), visible);
                }
            }
            ImGui::SameLine();
            const std::string activeLabel = std::string(layer.GetName()) +
                                            (isActive ? " *" : "");
            if (ImGui::Selectable(activeLabel.c_str(), isActive)) {
                if (!isActive) {
                    try {
                        if (stroke_ || selectionStroke_) {
                            throw std::logic_error(
                                "Finish the current stroke before switching layers.");
                        }
                        undoStack_.Clear();
                        grid.SetActiveLayer(layer.GetName());
                        ApplyActiveLayerColorMaps(grid, renderer);
                        renderer.UpdateSelection(grid.GetSnapshot()->GetVertices(),
                                                 grid.GetSnapshot()->HasSelection());
                        layerError_.clear();
                    } catch (const std::exception& error) {
                        layerError_ = error.what();
                    }
                }
            }
            if (ImGui::BeginDragDropSource()) {
                const std::string payload(layer.GetName());
                ImGui::SetDragDropPayload("OSWALD_LAYER", payload.c_str(), payload.size() + 1);
                ImGui::TextUnformatted(payload.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const auto* payload = ImGui::AcceptDragDropPayload("OSWALD_LAYER")) {
                    const std::string source(static_cast<const char*>(payload->Data));
                    try {
                        grid.MoveLayer(source, index);
                        reordered = true;
                    } catch (const std::exception& error) {
                        layerError_ = error.what();
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (nameEditLayer_ != &layer) {
                nameEditLayer_ = &layer;
                std::fill(layerNameBuffer_.begin(), layerNameBuffer_.end(), '\0');
                const auto name = layer.GetName();
                std::copy_n(name.data(), std::min(name.size(), layerNameBuffer_.size() - 1),
                            layerNameBuffer_.begin());
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("Name", layerNameBuffer_.data(), layerNameBuffer_.size())) {
                try {
                    grid.RenameLayer(layer.GetName(), layerNameBuffer_.data());
                    layerError_.clear();
                } catch (const std::exception& error) {
                    layerError_ = error.what();
                }
            }
            ImGui::Text("Type: Elevation (Raster)");
            if (ImGui::TreeNode("Properties")) {
                constexpr std::array blendModes{"Overlay", "Add", "Subtract", "Multiply",
                                                "Minimum", "Maximum"};
                int blendMode = static_cast<int>(layer.GetBlendMode());
                if (ImGui::Combo("Blend mode", &blendMode, blendModes.data(),
                                 static_cast<int>(blendModes.size()))) {
                    layer.SetBlendMode(static_cast<LayerBlendMode>(blendMode));
                }
                std::array<char, 1024> description{};
                const auto descriptionText = layer.GetDescription();
                std::copy_n(descriptionText.data(),
                            std::min(descriptionText.size(), description.size() - 1),
                            description.begin());
                if (ImGui::InputTextMultiline("Description", description.data(),
                                              description.size(), ImVec2(-FLT_MIN, 54.0F))) {
                    layer.SetDescription(description.data());
                }
                ImGui::BeginDisabled(layer.GetType() != LayerType::Raster);
                renderMapChoice("Legend", std::string(layer.GetColorMap()), &layer);
                renderMapChoice("Selected legend",
                                std::string(layer.GetSelectionColorMap()), &layer);
                if (!layer.GetValues().empty()) {
                    std::array<float, 32> bins{};
                    for (const float value : layer.GetValues()) {
                        const auto bin = static_cast<std::size_t>(std::clamp(
                            (value + 32767.0F) / 65534.0F * static_cast<float>(bins.size()),
                            0.0F, static_cast<float>(bins.size() - 1)));
                        bins[bin] += 1.0F;
                    }
                    float peakFrequency = 0.0F;
                    const float valueCount = static_cast<float>(layer.GetValues().size());
                    for (auto& frequency : bins) {
                        frequency /= valueCount;
                        peakFrequency = std::max(peakFrequency, frequency);
                    }
                    ImGui::TextUnformatted("Hypsography");
                    ImGui::PlotHistogram("##hypsography", bins.data(),
                                         static_cast<int>(bins.size()), 0, nullptr,
                                         0.0F, peakFrequency,
                                         ImVec2(-FLT_MIN, 64.0F));
                }
                ImGui::EndDisabled();
                ImGui::TreePop();
            }
            ImGui::PopID();
            if (reordered) {
                break;
            }
        }
    }

    if (ImGui::BeginPopupModal("New Layer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", newLayerName_.data(), newLayerName_.size());
        constexpr const char* layerTypes[]{"Elevation (Raster)"};
        int layerType = 0;
        ImGui::BeginDisabled();
        ImGui::Combo("Layer type", &layerType, layerTypes, 1);
        ImGui::EndDisabled();
        if (colorMapDirectory_.empty()) {
            ImGui::TextDisabled("No colour maps are available.");
        } else {
            const auto maps = colorMaps();
            const auto choose = [&](const char* label, std::string& selected) {
                if (ImGui::BeginCombo(label, selected.c_str())) {
                    for (const auto& path : maps) {
                        const std::string name = path.filename().string();
                        if (ImGui::Selectable(name.c_str(), selected == name)) {
                            selected = name;
                        }
                    }
                    ImGui::EndCombo();
                }
            };
            choose("Legend", newLayerColorMap_);
            choose("Selected legend", newLayerSelectionColorMap_);
        }
        if (ImGui::Button("Create")) {
            try {
                if (stroke_ || selectionStroke_) {
                    throw std::logic_error(
                        "Finish the current stroke before adding a layer.");
                }
                auto& created = grid.AddElevationLayer(newLayerName_.data(),
                    newLayerColorMap_, newLayerSelectionColorMap_);
                undoStack_.Clear();
                nameEditLayer_ = &created;
                ApplyActiveLayerColorMaps(grid, renderer);
                renderer.UpdateSelection(grid.GetSnapshot()->GetVertices(),
                                         grid.GetSnapshot()->HasSelection());
                layerError_.clear();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& error) {
                layerError_ = error.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete Layer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Delete layer \"%s\"?", pendingLayerDeletion_.c_str());
        if (ImGui::Button("Delete")) {
            try {
                if (stroke_ || selectionStroke_) {
                    throw std::logic_error(
                        "Finish the current stroke before deleting a layer.");
                }
                const bool wasActive = grid.HasActiveLayer() &&
                    grid.GetActiveLayer().GetName() == pendingLayerDeletion_;
                grid.RemoveLayer(pendingLayerDeletion_);
                if (wasActive) {
                    undoStack_.Clear();
                    ApplyActiveLayerColorMaps(grid, renderer);
                    renderer.UpdateSelection(grid.GetSnapshot()->GetVertices(),
                                             grid.GetSnapshot()->HasSelection());
                }
                nameEditLayer_ = nullptr;
                layerError_.clear();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& error) {
                layerError_ = error.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
    if (colorMapDirectory_.empty()) {
        renderer.ClearLayerStack();
        renderer.SetLayerVisible(grid.HasActiveLayer() && grid.GetActiveLayer().IsVisible());
        return;
    }
    try {
        std::vector<GlobeRenderLayer> renderLayers;
        renderLayers.reserve(grid.GetLayers().size());
        for (const auto& layer : grid.GetLayers()) {
            renderLayers.emplace_back(*layer, GetColorMap(std::string(layer->GetColorMap())),
                GetColorMap(std::string(layer->GetSelectionColorMap())));
        }
        renderer.SetLayerStack(std::move(renderLayers));
        renderer.SetLayerVisible(true);
    } catch (const std::exception& error) {
        renderer.ClearLayerStack();
        renderer.SetLayerVisible(true);
        layerError_ = error.what();
    }
}

void UIManager::FinishPolygonSelection(SphericalGrid& grid, GlobeRenderer& renderer) {
    if (polygonPoints_.size() >= 3) {
        const auto snapshot = grid.GetSnapshot();
        auto after = SelectSphericalPolygon(*snapshot, polygonPoints_);
        auto before = selectionWeightsOf(*snapshot);
        if (polygonAdditive_ && snapshot->HasSelection()) {
            std::transform(after.begin(), after.end(), before.begin(), after.begin(),
                           [](float added, float existing) { return std::max(added, existing); });
        }
        undoStack_.Execute(std::make_unique<SelectionCommand>(
            grid, renderer, std::move(before), snapshot->HasSelection(),
            std::move(after), true));
    }
    SetSelectionMode(SelectionMode::None);
}

void UIManager::ProcessBrushInput(SphericalGrid& grid, GlobeRenderer& renderer,
                                  const glm::mat4& modelMatrix, float zoom) {
    auto& io = ImGui::GetIO();
    const auto projection = camera_ == nullptr ? ViewProjection::Orthographic
                                               : camera_->GetProjection();
    const double mapCenterLongitude = camera_ == nullptr ? 0.0 : camera_->GetMapCenterLongitude();
    const auto mvp = brushMvp(io.DisplaySize, modelMatrix, zoom, projection);
    const bool captured = io.WantCaptureMouse;
    if (stroke_ && (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || !brushTools_.IsActive())) {
        auto before = std::move(stroke_->original);
        auto after = std::move(stroke_->working);
        stroke_.reset();
        // Tools clamp their output, but never let a bad value reach the grid's validation:
        // restore non-finite values and clamp the rest before committing the stroke.
        for (std::size_t index = 0; index < after.size() && index < before.size(); ++index) {
            if (!std::isfinite(after[index])) {
                after[index] = before[index];
            }
            after[index] = std::clamp(after[index], -32767.0f, 32767.0f);
        }
        if (before != after) {
            try {
                undoStack_.Execute(std::make_unique<BrushStrokeCommand>(
                    grid, renderer, std::move(before), std::move(after)));
            } catch (const std::exception& error) {
                brushError_ = std::string("Brush stroke discarded: ") + error.what();
                std::vector<float> current;
                const auto snapshot = grid.GetSnapshot();
                current.reserve(snapshot->GetVertices().size());
                for (const auto& vertex : snapshot->GetVertices()) {
                    current.push_back(vertex.elevation);
                }
                renderer.UpdateLayerElevations(grid.GetElevationLayer(), current);
            }
        }
    }

    std::optional<glm::dvec3> cursorCenter;
    if (brushTools_.IsActive() && !captured) {
        cursorCenter = pickView(io.MousePos, io.DisplaySize, mvp, projection);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && cursorCenter) {
            Stroke stroke;
            const auto snapshot = grid.GetSnapshot();
            stroke.original.reserve(snapshot->GetVertices().size());
            for (const auto& vertex : snapshot->GetVertices()) {
                stroke.original.push_back(vertex.elevation);
            }
            stroke.working = stroke.original;
            stroke_ = std::move(stroke);
        }
    }

    if (stroke_ && brushTools_.IsActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !captured && cursorCenter) {
        const auto snapshot = grid.GetSnapshot();
        const auto& tool = brushTools_.GetSelectedTool();
        std::vector<std::size_t> changed;
        // Tools leave their output untouched when Apply throws, so a failed dab is skipped.
        const auto applyTool = [&](std::vector<float>& values) {
            try {
                auto result = tool.Apply(*snapshot, *cursorCenter, values,
                                         brushTools_.GetSettings(), io.DeltaTime);
                brushError_.clear();
                return result;
            } catch (const std::exception& error) {
                brushError_ = error.what();
                return std::vector<std::size_t>{};
            }
        };
        if (tool.IsContinuous()) {
            changed = applyTool(stroke_->working);
        } else {
            // Evaluate each dab against the pre-stroke terrain and keep the strongest
            // effect per vertex, so overlapping or stationary dabs never compound.
            if (stroke_->scratch.empty()) {
                stroke_->scratch = stroke_->original;
            }
            const auto dabChanged = applyTool(stroke_->scratch);
            for (const auto index : dabChanged) {
                const float original = stroke_->original[index];
                if (std::abs(stroke_->scratch[index] - original) >
                    std::abs(stroke_->working[index] - original)) {
                    stroke_->working[index] = stroke_->scratch[index];
                    changed.push_back(index);
                }
                stroke_->scratch[index] = original;
            }
        }
        if (!changed.empty()) {
            renderer.UpdateLayerElevations(grid.GetElevationLayer(), stroke_->working, changed);
        }
    }

    if (brushTools_.IsActive() && cursorCenter && !captured) {
        const auto& settings = brushTools_.GetSettings();
        const double planetRadius = PlanetRadiusInUnit(*grid.GetSnapshot(), settings.distanceUnit);
        const double outerRadius = settings.radius;
        const double innerRadius = outerRadius * (1.0 - settings.falloffPercent / 100.0);
        constexpr ImU32 white = IM_COL32(255, 255, 255, 230);
        drawBrushRing(*cursorCenter, outerRadius, planetRadius, modelMatrix, mvp,
                      io.DisplaySize, white, projection, mapCenterLongitude);
        drawBrushRing(*cursorCenter, innerRadius, planetRadius, modelMatrix, mvp,
                      io.DisplaySize, white, projection, mapCenterLongitude);
    }
}

void UIManager::ProcessSelectionInput(SphericalGrid& grid, GlobeRenderer& renderer,
                                      const glm::mat4& modelMatrix, float zoom) {
    auto& io = ImGui::GetIO();
    const auto projection = camera_ == nullptr ? ViewProjection::Orthographic
                                               : camera_->GetProjection();
    const double mapCenterLongitude = camera_ == nullptr ? 0.0 : camera_->GetMapCenterLongitude();
    const auto mvp = brushMvp(io.DisplaySize, modelMatrix, zoom, projection);
    const bool captured = io.WantCaptureMouse;
    const bool radiusMode = selectionMode_ == SelectionMode::Radius;
    const bool polygonMode = selectionMode_ == SelectionMode::Polygon;

    if (selectionStroke_ && (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || !radiusMode)) {
        auto before = std::move(selectionStroke_->before);
        auto after = std::move(selectionStroke_->working);
        const bool wasActive = selectionStroke_->wasActive;
        selectionStroke_.reset();
        if (before != after || !wasActive) {
            undoStack_.Execute(std::make_unique<SelectionCommand>(
                grid, renderer, std::move(before), wasActive, std::move(after), true));
        }
    }

    std::optional<glm::dvec3> cursorCenter;
    if (radiusMode || polygonMode) {
        cursorCenter = pickView(io.MousePos, io.DisplaySize, mvp, projection);
        if (!captured && isInsideViewport(io.MousePos, io.DisplaySize) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !cursorCenter) {
            if (io.KeyShift) {
                return;
            }
            selectionStroke_.reset();
            polygonPoints_.clear();
            const auto snapshot = grid.GetSnapshot();
            if (snapshot->HasSelection()) {
                auto before = selectionWeightsOf(*snapshot);
                std::vector<float> after(before.size(), 0.0F);
                undoStack_.Execute(std::make_unique<SelectionCommand>(
                    grid, renderer, std::move(before), true, std::move(after), false));
            } else {
                renderer.UpdateSelection(snapshot->GetVertices(), false);
            }
            return;
        }
    }

    if (radiusMode && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !captured && cursorCenter) {
        const auto snapshot = grid.GetSnapshot();
        SelectionStroke stroke;
        stroke.before = selectionWeightsOf(*snapshot);
        stroke.wasActive = snapshot->HasSelection();
        if (io.KeyShift && stroke.wasActive) {
            stroke.working = stroke.before;
        } else {
            stroke.working.assign(stroke.before.size(), 0.0F);
        }
        stroke.renderVertices = snapshot->GetVertices();
        selectionStroke_ = std::move(stroke);
    }

    if (radiusMode && selectionStroke_ && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !captured && cursorCenter) {
        const auto snapshot = grid.GetSnapshot();
        const auto changed = radiusSelectionTool_.Apply(
            *snapshot, *cursorCenter, selectionStroke_->working, selectionSettings_);
        if (!changed.empty()) {
            for (std::size_t i = 0; i < selectionStroke_->working.size(); ++i) {
                selectionStroke_->renderVertices[i].selectionWeight =
                    selectionStroke_->working[i];
            }
            renderer.UpdateSelection(selectionStroke_->renderVertices, true);
        }
    }

    if (polygonMode && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !captured && cursorCenter) {
        if (polygonPoints_.empty()) {
            polygonAdditive_ = io.KeyShift;
        }
        const auto addPoint = [&] {
            if (polygonPoints_.empty() ||
                glm::length(polygonPoints_.back() - *cursorCenter) > 1e-8) {
                polygonPoints_.push_back(*cursorCenter);
            }
        };
        addPoint();
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            FinishPolygonSelection(grid, renderer);
        }
    }

    if (polygonMode && !captured && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        FinishPolygonSelection(grid, renderer);
    } else if (polygonMode && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        SetSelectionMode(SelectionMode::None);
    }

    if (radiusMode && cursorCenter && !captured) {
        const double planetRadius = PlanetRadiusInUnit(*grid.GetSnapshot(), selectionSettings_.distanceUnit);
        const double outerRadius = selectionSettings_.radius;
        const double innerRadius = outerRadius *
                                   (1.0 - selectionSettings_.falloffPercent / 100.0);
        constexpr ImU32 selectionRing = IM_COL32(255, 190, 40, 240);
        drawBrushRing(*cursorCenter, outerRadius, planetRadius, modelMatrix, mvp,
                      io.DisplaySize, selectionRing, projection, mapCenterLongitude);
        drawBrushRing(*cursorCenter, innerRadius, planetRadius, modelMatrix, mvp,
                      io.DisplaySize, selectionRing, projection, mapCenterLongitude);
    }

    if (polygonMode && !polygonPoints_.empty()) {
        auto* drawList = ImGui::GetForegroundDrawList();
        ImVec2 previous{};
        bool previousVisible = false;
        for (const auto point : polygonPoints_) {
            ImVec2 projected{};
            bool visible = false;
            if (projectPoint(point, modelMatrix, mvp, io.DisplaySize, projected, visible,
                             projection, mapCenterLongitude) &&
                visible && previousVisible) {
                drawList->AddLine(previous, projected, IM_COL32(255, 190, 40, 240), 2.0F);
            }
            previous = projected;
            previousVisible = visible;
        }
        if (cursorCenter && previousVisible) {
            ImVec2 projected{};
            bool visible = false;
            if (projectPoint(*cursorCenter, modelMatrix, mvp, io.DisplaySize, projected, visible,
                             projection, mapCenterLongitude) &&
                visible) {
                drawList->AddLine(previous, projected, IM_COL32(255, 190, 40, 180), 1.5F);
            }
        }
    }
}

void UIManager::BeginProjectDialog(ProjectAction action) {
    projectAction_ = action;
    projectDialogOpen_ = true;
    projectError_.clear();
    overwritePath_.clear();
    projectFileName_ = action == ProjectAction::Open || action == ProjectAction::ImportScript ? "" :
        action == ProjectAction::ExportScript ? BrushScriptFileName(scriptName_).string() :
        action == ProjectAction::ExportImage ? "map.png" :
        action == ProjectAction::Export ? "elevation.png" :
        projectPath_.empty() ? "project.oswald" : projectPath_.filename().string();
    if (action == ProjectAction::Export || action == ProjectAction::ExportImage) {
        const std::array extensions{".png", ".bmp", ".tga", ".tif"};
        projectFileName_ = std::filesystem::path(projectFileName_)
                               .replace_extension(extensions[exportFormat_]).string();
    }
    try {
        const bool script = action == ProjectAction::ImportScript ||
                            action == ProjectAction::ExportScript;
        const auto directory = script ?
            (!scriptBrowserDirectory_.empty() ? scriptBrowserDirectory_ :
             !brushScriptDirectory_.empty() && std::filesystem::is_directory(brushScriptDirectory_) ?
                 brushScriptDirectory_ : std::filesystem::current_path()) :
            !projectPath_.empty() ? projectPath_.parent_path() :
            projectBrowser_.GetDirectory().empty() ? std::filesystem::current_path() :
                                                    projectBrowser_.GetDirectory();
        projectBrowser_.Navigate(directory, true);
        projectDirectory_ = projectBrowser_.GetDirectory().string();
    } catch (const std::exception& error) {
        projectError_ = error.what();
    }
}

void UIManager::SaveProject(SphericalGrid& grid) {
    if (projectPath_.empty()) {
        BeginProjectDialog(ProjectAction::Save);
        return;
    }
    try {
        ProjectFile::Save(projectPath_, grid);
    } catch (const std::exception& error) {
        projectError_ = error.what();
        projectErrorOpen_ = true;
    }
}

void UIManager::PerformProjectAction(SphericalGrid& grid, GlobeRenderer& renderer,
                                     const std::filesystem::path& path) {
    if (projectAction_ == ProjectAction::ImportScript) {
        LoadScriptIntoEditor(BrushScriptFile::Load(path));
        scriptBrowserDirectory_ = path.parent_path();
        scriptLibraryMessage_ = "Imported " + path.filename().string() + ".";
        scriptLibraryMessageIsError_ = false;
    } else if (projectAction_ == ProjectAction::ExportScript) {
        CurrentScriptFile().Save(path);
        scriptBrowserDirectory_ = path.parent_path();
        scriptLibraryMessage_ = "Exported " + path.filename().string() + ".";
        scriptLibraryMessageIsError_ = false;
        std::error_code ignored;
        if (!brushScriptDirectory_.empty() &&
            std::filesystem::equivalent(path.parent_path(), brushScriptDirectory_, ignored)) {
            RescanScriptLibrary();
        }
    } else if (projectAction_ == ProjectAction::Save) {
        ProjectFile::Save(path, grid);
        projectPath_ = std::filesystem::absolute(path);
    } else if (projectAction_ == ProjectAction::Export) {
        ElevationRaster::Export(path, grid, exportWidth_, exportWidth_ / 2,
            static_cast<ElevationRaster::TiffSampleType>(exportTiffSampleType_));
    } else if (projectAction_ == ProjectAction::ExportImage) {
        const auto& layer = grid.GetActiveLayer();
        const auto& map = GetColorMap(std::string(layer.GetColorMap()));
        const auto* selectionMap = exportSelectionHighlight_ ?
            &GetColorMap(std::string(layer.GetSelectionColorMap())) : nullptr;
        ElevationRaster::ExportImage(path, grid, map, selectionMap, exportWidth_, exportWidth_ / 2);
    } else {
        auto loaded = ProjectFile::Load(path);
        std::vector<GlobeRenderLayer> renderLayers;
        if (!colorMapDirectory_.empty()) {
            for (const auto& layer : loaded->GetLayers()) {
                renderLayers.emplace_back(*layer, GetColorMap(std::string(layer->GetColorMap())),
                    GetColorMap(std::string(layer->GetSelectionColorMap())));
            }
        }
        renderer.ClearLayerStack();
        renderer.UploadGrid(*loaded->GetSnapshot());
        ProjectFile::Install(grid, *loaded);
        if (!colorMapDirectory_.empty()) {
            renderer.SetLayerStack(std::move(renderLayers));
        }
        renderer.SetLayerVisible(true);
        undoStack_.Clear();
        stroke_.reset();
        selectionStroke_.reset();
        polygonPoints_.clear();
        brushTools_.SetActive(false);
        SetSelectionMode(SelectionMode::None);
        raster_.reset();
        loadedRasterPath_.clear();
        nameEditLayer_ = nullptr;
        layerError_.clear();
        options_ = grid.GetSnapshot()->GetOptions();
        previousOptions_ = options_;
        if (grid.HasActiveLayer()) {
            ApplyActiveLayerColorMaps(grid, renderer);
        }
        projectPath_ = std::filesystem::absolute(path);
    }
}

void UIManager::RenderProjectDialogs(SphericalGrid& grid, GlobeRenderer& renderer) {
    if (planetOpen_) {
        pendingMetadata_ = grid.GetMetadata();
        pendingPlanetRadius_ = grid.GetSnapshot()->GetOptions().planetRadius;
        pendingPointCount_ = static_cast<int>(grid.GetSnapshot()->GetOptions().pointCount);
        planetError_.clear();
        ImGui::OpenPopup("Planet");
        planetOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(540, 320), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal("Planet", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::BeginTabBar("PlanetTabs")) {
            if (ImGui::BeginTabItem("Geometry")) {
                ImGui::InputDouble("Planet radius (km)", &pendingPlanetRadius_, 0.0, 0.0, "%.6g");
                ImGui::SliderInt("Point Count (N)", &pendingPointCount_, 1'000, 1'000'000,
                                 "%d", ImGuiSliderFlags_AlwaysClamp);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.0F);
                ImGui::TextWrapped("Changing point count regenerates the grid and clears undo history, using the loaded raster if available. Changing radius alone preserves terrain and selection.");
                ImGui::PopTextWrapPos();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Metadata")) {
                ImGui::InputTextMultiline("Description", &pendingMetadata_.description, ImVec2(380, 150));
                ImGui::InputText("Author", &pendingMetadata_.author);
                std::string date = grid.GetMetadata().savedAt.empty() ?
                    "Not yet saved" : grid.GetMetadata().savedAt;
                ImGui::InputText("Date (UTC)", &date, ImGuiInputTextFlags_ReadOnly);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (!planetError_.empty()) {
            ImGui::TextWrapped("%s", planetError_.c_str());
        }
        if (ImGui::Button("Apply")) {
            try {
                if (!std::isfinite(pendingPlanetRadius_) || pendingPlanetRadius_ <= 0.0) {
                    throw std::invalid_argument("Planet radius must be finite and positive.");
                }
                if (pendingPointCount_ < 1000 || pendingPointCount_ > 1'000'000) {
                    throw std::invalid_argument("Point count must be between 1,000 and 1,000,000.");
                }
                options_ = grid.GetSnapshot()->GetOptions();
                options_.planetRadius = pendingPlanetRadius_;
                options_.pointCount = static_cast<std::size_t>(pendingPointCount_);
                if (options_.pointCount != grid.GetSnapshot()->GetOptions().pointCount) {
                    StartGridGeneration(grid, renderer);
                } else {
                    grid.SetPlanetRadius(pendingPlanetRadius_);
                    previousOptions_ = options_;
                }
                grid.GetMetadata().description = pendingMetadata_.description;
                grid.GetMetadata().author = pendingMetadata_.author;
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& error) {
                planetError_ = error.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (projectDialogOpen_) {
        ImGui::OpenPopup("Project File");
        projectDialogOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(680, 480), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal("Project File")) {
        const bool opening = projectAction_ == ProjectAction::Open ||
                             projectAction_ == ProjectAction::ImportScript;
        const bool scriptFile = projectAction_ == ProjectAction::ImportScript ||
                                projectAction_ == ProjectAction::ExportScript;
        const bool imageExport = projectAction_ == ProjectAction::ExportImage;
        const bool exporting = projectAction_ == ProjectAction::Export || imageExport;
        ImGui::TextUnformatted(projectAction_ == ProjectAction::ImportScript ? "Import Brush Script" :
                               projectAction_ == ProjectAction::ExportScript ? "Export Brush Script" :
                               opening ? "Open Project" :
                               imageExport ? "Export Image" : exporting ? "Export Data" : "Save Project");
        const auto navigate = [&](const std::filesystem::path& directory) {
            try {
                projectBrowser_.Navigate(directory, true);
                projectDirectory_ = projectBrowser_.GetDirectory().string();
                projectError_.clear();
            } catch (const std::exception& error) {
                projectError_ = error.what();
            }
        };
        const bool go = ImGui::InputText("Folder", &projectDirectory_,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Go") || go) {
            navigate(projectDirectory_);
        }
        if (ImGui::Button("Up")) {
            navigate(projectBrowser_.GetDirectory().parent_path());
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            navigate(projectBrowser_.GetDirectory());
        }
        std::filesystem::path nextDirectory;
        bool perform = false;
        if (ImGui::BeginChild("ProjectFiles", ImVec2(0, 170), true)) {
            for (const auto& entry : projectBrowser_.GetEntries()) {
                ImGui::PushID(&entry);
                const auto position = ImGui::GetCursorScreenPos();
                const auto name = entry.path.filename().string();
                if (ImGui::Selectable("##file", name == projectFileName_,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (entry.isDirectory) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            nextDirectory = entry.path;
                        }
                    } else {
                        projectFileName_ = name;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && opening) {
                            perform = true;
                        }
                    }
                }
                const auto label = (entry.isDirectory ? "[Folder] " : "") + name;
                ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text),
                                                   label.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (!nextDirectory.empty()) {
            navigate(nextDirectory);
        }
        perform |= ImGui::InputText("File name", &projectFileName_,
                                    ImGuiInputTextFlags_EnterReturnsTrue);
        const bool changedFileName = ImGui::IsItemEdited();
        if (exporting) {
            if (changedFileName) {
                auto extension = std::filesystem::path(projectFileName_).extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (extension == ".png") exportFormat_ = 0;
                if (extension == ".bmp") exportFormat_ = 1;
                if (extension == ".tga") exportFormat_ = 2;
                if (extension == ".tif" || extension == ".tiff") exportFormat_ = 3;
            }
            if (ImGui::Combo("Format", &exportFormat_, "PNG\0BMP\0TGA\0TIFF\0")) {
                const std::array extensions{".png", ".bmp", ".tga", ".tif"};
                projectFileName_ = std::filesystem::path(projectFileName_)
                                       .replace_extension(extensions[exportFormat_]).string();
            }
            ImGui::InputInt("Width (pixels)", &exportWidth_);
            ImGui::Text("Height: %d (2:1, maximum 8192 x 4096)", exportWidth_ / 2);
            if (exportFormat_ == 3) {
                if (!imageExport) {
                    ImGui::Combo("TIFF samples", &exportTiffSampleType_,
                        "8-bit grayscale\0Signed 16-bit elevations\0Float32 elevations\0");
                    if (exportTiffSampleType_ == 1) {
                        ImGui::TextWrapped("Direct elevations [-32767, 32767], rounded to whole units.");
                    } else if (exportTiffSampleType_ == 2) {
                        ImGui::TextWrapped("Direct Float32 elevations; preserves fractional values.");
                    }
                }
                ImGui::TextWrapped("TIFF also writes .tfw and .tif/.tiff.aux.xml sidecars "
                                   "for WGS 84 (EPSG:4326). Keep them with the image.");
            }
            if (imageExport) {
                ImGui::Checkbox("Include selection highlighting", &exportSelectionHighlight_);
                ImGui::TextWrapped("8-bit RGB through the active layer's colourmap. "
                                   "Selection highlighting matches the globe's colourmaps and weights. "
                                   "No graticule or globe lighting is included.");
            } else if (exportFormat_ != 3 || exportTiffSampleType_ == 0) {
                ImGui::TextWrapped("8-bit grayscale: -32767 = black, +32767 = white "
                                   "(about 257 elevation units per gray level).");
            }
            ImGui::TextWrapped("Cumulative elevation through the active layer. North is at the top.");
        }
        if (!projectError_.empty()) {
            ImGui::TextWrapped("%s", projectError_.c_str());
        }
        ImGui::BeginDisabled(projectFileName_.empty());
        perform |= ImGui::Button(projectAction_ == ProjectAction::ImportScript ? "Import" :
                                 opening ? "Open" :
                                 exporting || scriptFile ? "Export" : "Save");
        ImGui::EndDisabled();
        if (perform && !projectFileName_.empty()) {
            try {
                auto path = std::filesystem::path(projectFileName_);
                if (path.is_relative()) {
                    path = projectBrowser_.GetDirectory() / path;
                }
                if (!opening && path.extension().empty()) {
                    const std::array extensions{".png", ".bmp", ".tga", ".tif"};
                    path += exporting ? extensions[exportFormat_] :
                            scriptFile ? ".toml" : ".oswald";
                }
                if (exporting && (exportWidth_ < 2 || exportWidth_ > 8192 || exportWidth_ % 2)) {
                    throw std::invalid_argument("Export width must be even and between 2 and 8192.");
                }
                const auto outputs = exporting ? ElevationRaster::ExportPaths(path) :
                    std::vector<std::filesystem::path>{path};
                const bool exists = std::any_of(outputs.begin(), outputs.end(),
                    [](const auto& output) { return std::filesystem::exists(output); });
                if (!opening && exists) {
                    overwritePath_ = path;
                    ImGui::OpenPopup("Overwrite File?");
                } else {
                    PerformProjectAction(grid, renderer, path);
                    projectError_.clear();
                    ImGui::CloseCurrentPopup();
                }
            } catch (const std::exception& error) {
                projectError_ = error.what();
            }
        }
        bool completedOverwrite = false;
        const bool overwriteOpen = ImGui::IsPopupOpen("Overwrite File?");
        if (ImGui::BeginPopupModal("Overwrite File?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Replace %s?", overwritePath_.string().c_str());
            if ((projectAction_ == ProjectAction::Export || projectAction_ == ProjectAction::ExportImage) &&
                ElevationRaster::ExportPaths(overwritePath_).size() > 1) {
                ImGui::TextWrapped("This also replaces the TIFF georeferencing sidecars.");
            }
            if (ImGui::Button("Replace")) {
                try {
                    PerformProjectAction(grid, renderer, overwritePath_);
                    projectError_.clear();
                    completedOverwrite = true;
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception& error) {
                    projectError_ = error.what();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (completedOverwrite) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || (!overwriteOpen && ImGui::IsKeyPressed(ImGuiKey_Escape))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (projectErrorOpen_) {
        ImGui::OpenPopup("Project Error");
        projectErrorOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Project Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", projectError_.c_str());
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool UIManager::Render(SphericalGrid& grid, GlobeRenderer& renderer,
                       const glm::mat4& modelMatrix, float zoom) {
    renderer.SetWireframeEnabled(viewportWireframe_);
    renderer.SetGraticuleEnabled(viewportGraticule_);
    renderer.SetEquirectangular(camera_ != nullptr &&
                                camera_->GetProjection() == ViewProjection::Equirectangular);

    PollGridGeneration(grid, renderer);
    const bool generating = gridFuture_.valid() || gridProgressOpen_;
    bool requestExit = false;
    const auto runHistory = [&](bool (UndoStack::*step)(), const char* name) {
        try {
            (undoStack_.*step)();
        } catch (const std::exception& error) {
            projectError_ = std::string(name) + " failed: " + error.what();
            projectErrorOpen_ = true;
        }
    };
    const auto undo = [&] { runHistory(&UndoStack::Undo, "Undo"); };
    const auto redo = [&] { runHistory(&UndoStack::Redo, "Redo"); };
    const bool projectBusy = generating || stroke_.has_value() || selectionStroke_.has_value();
    if (!projectBusy && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_O)) {
            if (ImGui::GetIO().KeyShift) {
                rasterOpen_ = true;
                errorMessage_.clear();
            } else {
                BeginProjectDialog(ProjectAction::Open);
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_S)) {
            if (ImGui::GetIO().KeyShift) {
                BeginProjectDialog(ProjectAction::Save);
            } else {
                SaveProject(grid);
            }
        }
    }
    if (!generating && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
            undo();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
            redo();
        }
    }
    if (camera_ != nullptr) {
        const auto* drawList = ImGui::GetForegroundDrawList();
        ImGui::SetNextWindowPos(ImVec2(14.0F, 28.0F), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.75F);
        ImGui::Begin("Viewport Controls", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        if (ImGui::Button("Reset")) {
            camera_->Reset();
        }
        ImGui::SameLine();
        bool spinEnabled = camera_->IsAutoSpinEnabled();
        if (ImGui::Checkbox("Spin", &spinEnabled)) {
            camera_->SetAutoSpin(spinEnabled);
        }
        ImGui::SameLine();
        bool wireframe = viewportWireframe_;
        if (ImGui::Checkbox("Wireframe", &wireframe)) {
            viewportWireframe_ = wireframe;
        }
        ImGui::SameLine();
        bool graticule = viewportGraticule_;
        if (ImGui::Checkbox("Graticule", &graticule)) {
            viewportGraticule_ = graticule;
        }
        ImGui::End();
    }

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::BeginDisabled(projectBusy);
            if (ImGui::MenuItem("Open", "Ctrl+O")) {
                BeginProjectDialog(ProjectAction::Open);
            }
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                SaveProject(grid);
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
                BeginProjectDialog(ProjectAction::Save);
            }
            if (ImGui::BeginMenu("Export", grid.HasActiveLayer())) {
                if (ImGui::MenuItem("Export data...")) {
                    BeginProjectDialog(ProjectAction::Export);
                }
                if (ImGui::MenuItem("Export Image...")) {
                    BeginProjectDialog(ProjectAction::ExportImage);
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Planet...")) {
                planetOpen_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Open Raster...", "Ctrl+Shift+O", false, grid.HasActiveLayer())) {
                rasterOpen_ = true;
                errorMessage_.clear();
            }
            ImGui::EndDisabled();
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
        if (ImGui::BeginMenu("Select")) {
            if (ImGui::MenuItem("Radius Selection", nullptr,
                                selectionMode_ == SelectionMode::Radius)) {
                SetSelectionMode(selectionMode_ == SelectionMode::Radius
                                     ? SelectionMode::None : SelectionMode::Radius);
            }
            if (ImGui::MenuItem("Polygon Selection", nullptr,
                                selectionMode_ == SelectionMode::Polygon)) {
                SetSelectionMode(selectionMode_ == SelectionMode::Polygon
                                     ? SelectionMode::None : SelectionMode::Polygon);
            }
            if (ImGui::MenuItem("By value...")) {
                valueSelectionOpen_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Selection")) {
                const auto snapshot = grid.GetSnapshot();
                if (snapshot->HasSelection()) {
                    auto before = selectionWeightsOf(*snapshot);
                    std::vector<float> after(before.size(), 0.0F);
                    undoStack_.Execute(std::make_unique<SelectionCommand>(
                        grid, renderer, std::move(before), true, std::move(after), false));
                }
            }
            if (ImGui::MenuItem("Invert Selection")) {
                const auto snapshot = grid.GetSnapshot();
                auto before = selectionWeightsOf(*snapshot);
                auto after = before;
                if (snapshot->HasSelection()) {
                    std::transform(after.begin(), after.end(), after.begin(),
                                   [](float weight) { return 1.0F - weight; });
                } else {
                    std::fill(after.begin(), after.end(), 1.0F);
                }
                undoStack_.Execute(std::make_unique<SelectionCommand>(
                    grid, renderer, std::move(before), snapshot->HasSelection(),
                    std::move(after), true));
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Brushes")) {
            for (const auto& tool : brushTools_.GetTools()) {
                const std::string name(tool->GetName());
                const bool selected = brushTools_.IsActive() &&
                    brushTools_.GetSelectedTool().GetName() == name;
                if (ImGui::MenuItem(name.c_str(), nullptr, selected)) {
                    SetSelectionMode(SelectionMode::None);
                    brushTools_.SelectTool(name);
                    brushTools_.SetActive(!selected);
                    scriptParameterError_.clear();
                }
                if (ImGui::IsItemHovered()) {
                    const auto entry = std::find_if(scriptLibrary_.entries.begin(), scriptLibrary_.entries.end(),
                        [&](const auto& candidate) { return candidate.file.name == name; });
                    if (entry != scriptLibrary_.entries.end()) {
                        ImGui::SetTooltip("%s\n%s", entry->file.description.c_str(), entry->path.string().c_str());
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Refresh brushes", nullptr, false, !stroke_)) {
                RescanScriptLibrary();
            }
            if (!brushLibraryErrors_.empty() && ImGui::BeginMenu("Brush errors")) {
                for (const auto& error : brushLibraryErrors_) {
                    ImGui::TextWrapped("%s", error.c_str());
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Filter")) {
            if (ImGui::MenuItem("Fill...")) {
                const auto& filter = dynamic_cast<const FillFilter&>(
                    filters_.GetFilter("Fill"));
                pendingFillSettings_ = filter.GetSettings();
                fillFilterError_.clear();
                fillFilterOpen_ = true;
            }
            if (ImGui::BeginMenu("Erosion", grid.HasActiveLayer() && !projectBusy)) {
                if (ImGui::MenuItem("Fill basins...")) {
                    try {
                        ApplyTerrainFilter(filters_.GetFilter("Fill basins"), grid, renderer);
                        fillBasinsError_.clear();
                    } catch (const std::exception& error) {
                        fillBasinsError_ = error.what();
                        fillBasinsErrorOpen_ = true;
                    }
                }
                if (ImGui::MenuItem("Incise flow...")) {
                    const auto& filter = dynamic_cast<const InciseFlowFilter&>(
                        filters_.GetFilter("Incise flow"));
                    pendingInciseFlowSettings_ = filter.GetSettings();
                    inciseFlowError_.clear();
                    inciseFlowOpen_ = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Absolute Magnitude Noise...", nullptr, false,
                                grid.HasActiveLayer() && !projectBusy)) {
                const auto& filter = dynamic_cast<const AbsoluteMagnitudeNoiseFilter&>(
                    filters_.GetFilter("Absolute Magnitude Noise"));
                pendingNoiseAmplitudeFeet_ = filter.GetAmplitudeFeet();
                absoluteMagnitudeNoiseError_.clear();
                absoluteMagnitudeNoiseOpen_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Mesh")) {
            const auto snapshot = grid.GetSnapshot();
            ImGui::BeginDisabled(!snapshot->HasSelection());
            if (ImGui::MenuItem("Refine Selection")) {
                try {
                    auto refined = snapshot->RefineSelectedRegion();
                    if (refined) {
                        undoStack_.Execute(std::make_unique<AdaptiveMeshCommand>(
                            grid, renderer, snapshot, std::move(refined)));
                        meshError_.clear();
                    } else {
                        meshError_ = "No selected faces are available for refinement.";
                        meshErrorOpen_ = true;
                    }
                } catch (const std::exception& error) {
                    meshError_ = error.what();
                    meshErrorOpen_ = true;
                }
            }
            if (ImGui::MenuItem("Coarsen Flat Selection")) {
                try {
                    auto coarsened = snapshot->CoarsenSelectedRegion(1.0F);
                    if (coarsened) {
                        undoStack_.Execute(std::make_unique<AdaptiveMeshCommand>(
                            grid, renderer, snapshot, std::move(coarsened)));
                        meshError_.clear();
                    } else {
                        meshError_ = "No selected, locally uniform vertices are available for coarsening.";
                        meshErrorOpen_ = true;
                    }
                } catch (const std::exception& error) {
                    meshError_ = error.what();
                    meshErrorOpen_ = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            const auto projection = camera_ == nullptr ? ViewProjection::Orthographic
                                                       : camera_->GetProjection();
            if (ImGui::BeginMenu("Projection")) {
                if (ImGui::MenuItem("Orthographic", nullptr,
                                    projection == ViewProjection::Orthographic) && camera_ != nullptr) {
                    camera_->SetProjection(ViewProjection::Orthographic);
                }
                if (ImGui::MenuItem("Equirectangular", nullptr,
                                    projection == ViewProjection::Equirectangular) && camera_ != nullptr) {
                    camera_->SetProjection(ViewProjection::Equirectangular);
                }
                ImGui::EndMenu();
            }
            ImGui::BeginDisabled(!grid.HasActiveLayer());
            if (ImGui::BeginMenu("Legend")) {
                const auto renderColorMapChoices = [&](const char* label, bool selected) {
                    if (!ImGui::BeginMenu(label)) {
                        return;
                    }
                    if (colorMapDirectory_.empty()) {
                        ImGui::TextDisabled("No colour map directory configured");
                    } else {
                        try {
                            std::vector<std::filesystem::path> maps;
                            for (const auto& entry :
                                 std::filesystem::directory_iterator(colorMapDirectory_)) {
                                if (entry.is_regular_file() && entry.path().extension() == ".map") {
                                    maps.push_back(entry.path());
                                }
                            }
                            std::sort(maps.begin(), maps.end());
                            if (maps.empty()) {
                                ImGui::TextDisabled("No colour maps available");
                            }
                            for (const auto& path : maps) {
                                const auto name = path.filename().string();
                                const bool active = selected ? selectionColorMapName_ == name
                                                             : colorMapName_ == name;
                                if (!ImGui::MenuItem(name.c_str(), nullptr, active)) {
                                    continue;
                                }
                                try {
                                    auto map = ColorMap::Load(path);
                                    if (selected) {
                                        renderer.SetSelectionColorMap(map);
                                        selectionColorMapName_ = name;
                                        grid.GetElevationLayer().SetSelectionColorMap(name);
                                    } else {
                                        renderer.SetColorMap(map);
                                        colorMapName_ = name;
                                        grid.GetElevationLayer().SetColorMap(name);
                                    }
                                } catch (const std::exception& error) {
                                    legendError_ = error.what();
                                    legendErrorOpen_ = true;
                                }
                            }
                        } catch (const std::exception& error) {
                            legendError_ = error.what();
                            legendErrorOpen_ = true;
                        }
                    }
                    ImGui::EndMenu();
                };
                renderColorMapChoices("Unselected points", false);
                renderColorMapChoices("Selected points", true);
                ImGui::EndMenu();
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    RenderLayerPanel(grid, renderer);

    if (legendErrorOpen_) {
        ImGui::OpenPopup("Legend Error");
        legendErrorOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Legend Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", legendError_.c_str());
        if (ImGui::Button("Close")) {
            legendError_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (meshErrorOpen_) {
        ImGui::OpenPopup("Mesh Operation");
        meshErrorOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Mesh Operation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", meshError_.c_str());
        if (ImGui::Button("Close")) {
            meshError_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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
        ImGui::TextWrapped("Supported images: PNG, JPEG, BMP, TGA, TIFF, PSD, GIF, HDR, PIC, "
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
        pendingRaiseLowerRate_ = brushTools_.GetSettings().raiseLowerRate;
        pendingGraticuleColor_ = graticuleColor_;
        pendingHighlightedGraticuleColor_ = highlightedGraticuleColor_;
        pendingWireframeColor_ = wireframeColor_;
        pendingWireframeSelectedColor_ = wireframeSelectedColor_;
        ImGui::OpenPopup("Settings");
        settingsOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        float poissonVariability = static_cast<float>(options_.poissonVariability);
        if (ImGui::SliderFloat("Poisson Variability (J)", &poissonVariability,
                               0.0F, 1.0F, "%.2f")) {
            options_.poissonVariability = poissonVariability;
        }
        ImGui::InputDouble("Raise/Lower speed (units/s)", &pendingRaiseLowerRate_,
                           1.0, 1024.0, "%.0f");
        int relaxation = static_cast<int>(options_.relaxationIterations);
        if (ImGui::SliderInt("Relaxation Passes (Lloyd)", &relaxation, 0, 20)) {
            options_.relaxationIterations = static_cast<std::uint32_t>(relaxation);
        }
        ImGui::TextWrapped("Relaxation evens out Poisson-distributed sites towards more regular Voronoi cells.");
        ImGui::ColorEdit3("Graticule colour", glm::value_ptr(pendingGraticuleColor_));
        ImGui::ColorEdit3("Highlighted graticule colour",
                          glm::value_ptr(pendingHighlightedGraticuleColor_));
        ImGui::ColorEdit3("Wireframe colour (unselected)",
                          glm::value_ptr(pendingWireframeColor_));
        ImGui::ColorEdit3("Wireframe colour (selected)",
                          glm::value_ptr(pendingWireframeSelectedColor_));
        if (!errorMessage_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.35F, 0.3F, 1.0F));
            ImGui::TextWrapped("%s", errorMessage_.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::Button("Apply")) {
            try {
                if (!std::isfinite(pendingRaiseLowerRate_) || pendingRaiseLowerRate_ <= 0.0) {
                    throw std::invalid_argument("Raise/Lower speed must be finite and positive.");
                }
                brushTools_.GetSettings().raiseLowerRate = pendingRaiseLowerRate_;
                renderer.SetGraticuleColors(pendingGraticuleColor_,
                                            pendingHighlightedGraticuleColor_);
                renderer.SetWireframeColors(pendingWireframeColor_,
                                            pendingWireframeSelectedColor_);
                wireframeColor_ = pendingWireframeColor_;
                wireframeSelectedColor_ = pendingWireframeSelectedColor_;
                graticuleColor_ = pendingGraticuleColor_;
                highlightedGraticuleColor_ = pendingHighlightedGraticuleColor_;
                errorMessage_.clear();
                const auto& currentOptions = grid.GetSnapshot()->GetOptions();
                if (options_.pointCount != currentOptions.pointCount ||
                    options_.poissonVariability != currentOptions.poissonVariability ||
                    options_.seed != currentOptions.seed ||
                    options_.relaxationIterations != currentOptions.relaxationIterations) {
                    StartGridGeneration(grid, renderer);
                }
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
    RenderProjectDialogs(grid, renderer);
    const bool projectModal = ImGui::IsPopupOpen("Project File") ||
        ImGui::IsPopupOpen("Planet") || ImGui::IsPopupOpen("Project Error");
    const bool valueSelectionDialog = valueSelectionOpen_ || ImGui::IsPopupOpen("Select By Value");
    RenderValueSelectionDialog(grid, renderer);
    const bool fillFilterDialog = fillFilterOpen_ || ImGui::IsPopupOpen("Fill Filter");
    RenderFillFilterDialog(grid, renderer);
    const bool inciseFlowDialog = inciseFlowOpen_ || ImGui::IsPopupOpen("Incise Flow");
    RenderInciseFlowDialog(grid, renderer);
    const bool noiseDialog = absoluteMagnitudeNoiseOpen_ ||
        ImGui::IsPopupOpen("Absolute Magnitude Noise");
    RenderAbsoluteMagnitudeNoiseDialog(grid, renderer);
    if (fillBasinsErrorOpen_) {
        ImGui::OpenPopup("Fill Basins Error");
        fillBasinsErrorOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Fill Basins Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", fillBasinsError_.c_str());
        if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const bool fillBasinsDialog = ImGui::IsPopupOpen("Fill Basins Error");
    RenderGridProgressDialog();
    if (generating) {
        return requestExit;
    }
    if (!ImGui::IsPopupOpen("Open Raster") && !ImGui::IsPopupOpen("Settings") &&
        !valueSelectionDialog && !fillFilterDialog && !fillBasinsDialog &&
        !inciseFlowDialog && !noiseDialog && !projectModal) {
        RenderBrushPanel(grid, renderer);
        RenderSelectionPanel();
    }
    if (!valueSelectionDialog && !fillFilterDialog && !fillBasinsDialog &&
        !inciseFlowDialog && !noiseDialog && !projectModal) {
        ProcessBrushInput(grid, renderer, modelMatrix, zoom);
        ProcessSelectionInput(grid, renderer, modelMatrix, zoom);
    }

    const auto& io = ImGui::GetIO();
    if (grid.HasActiveLayer() && !io.WantCaptureMouse &&
        isInsideViewport(io.MousePos, io.DisplaySize)) {
        const auto projection = camera_ == nullptr ? ViewProjection::Orthographic
                               : camera_->GetProjection();
        const auto point = inspectHoverPoint(io.MousePos, io.DisplaySize, modelMatrix, zoom,
                             projection);
        if (point) {
            const auto normalized = glm::normalize(*point);
            const double latitude = glm::degrees(std::atan2(normalized.z,
                std::sqrt(normalized.x * normalized.x + normalized.y * normalized.y)));
            const double longitude = glm::degrees(std::atan2(normalized.y, normalized.x));
            const auto snapshot = grid.GetSnapshot();
            const auto nearestIndex = snapshot->FindNearest(normalized);
            const double elevation = grid.GetActiveCompositedElevation(nearestIndex,
                stroke_ ? std::span<const float>(stroke_->working) : std::span<const float>{});
            const double feet = elevation * 3.28083989501312;
            const std::string label = "Lat " + std::to_string(latitude) + "° / Lon " +
                                     std::to_string(longitude) + "°\nElev " +
                                     std::to_string(elevation) + " m / " +
                                     std::to_string(feet) + " ft";
            ImGui::SetNextWindowPos(ImVec2(std::min(io.MousePos.x + 18.0F, io.DisplaySize.x - 260.0F),
                                           std::min(io.MousePos.y + 18.0F, io.DisplaySize.y - 80.0F)),
                                    ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.7F);
            if (ImGui::Begin("Hover Readout", nullptr,
                             ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing)) {
                ImGui::TextUnformatted(label.c_str());
            }
            ImGui::End();
        }
    }
    return requestExit;
}