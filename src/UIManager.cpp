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
#include <stdexcept>
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

class SelectionCommand final : public UndoableCommand {
public:
    SelectionCommand(SphericalFibonacciGrid& grid, GlobeRenderer& renderer,
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

    SphericalFibonacciGrid& grid_;
    GlobeRenderer& renderer_;
    std::vector<float> before_;
    bool beforeActive_;
    std::vector<float> after_;
    bool afterActive_;
};

std::vector<float> selectionWeightsOf(const SphericalGridSnapshot& snapshot) {
    std::vector<float> weights;
    weights.reserve(snapshot.GetVertices().size());
    for (const auto& vertex : snapshot.GetVertices()) {
        weights.push_back(vertex.selectionWeight);
    }
    return weights;
}

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
    const auto& tool = brushTools_.GetSelectedTool();
    const bool raiseLower = dynamic_cast<const RaiseLowerBrushTool*>(&tool) != nullptr;
    const std::string title = std::string(tool.GetName()) + "###BrushParameters";
    if (ImGui::Begin(title.c_str(), &open)) {
        auto& settings = brushTools_.GetSettings();
        if (raiseLower) {
            constexpr std::array operationNames{"Raise", "Lower"};
            int operation = static_cast<int>(settings.raiseLowerOperation);
            if (ImGui::Combo("Operation", &operation, operationNames.data(),
                             static_cast<int>(operationNames.size()))) {
                settings.raiseLowerOperation = static_cast<RaiseLowerOperation>(operation);
            }
        } else {
            constexpr std::array operationNames{"Replace", "Minimum", "Maximum"};
            int operation = static_cast<int>(settings.operation);
            if (ImGui::Combo("Operation", &operation, operationNames.data(),
                             static_cast<int>(operationNames.size()))) {
                settings.operation = static_cast<BrushOperation>(operation);
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
        if (ImGui::DragFloat("Outer radius", &radius, 10.0F, 0.1F,
                             maximumRadius, "%.1f")) {
            settings.radius = std::clamp(static_cast<double>(radius), 0.1,
                                         static_cast<double>(maximumRadius));
        }
        float falloff = static_cast<float>(settings.falloffPercent);
        if (ImGui::SliderFloat("Falloff (%)", &falloff, 0.0F, 100.0F, "%.0f%%")) {
            settings.falloffPercent = falloff;
        }
        if (raiseLower) {
            float intensity = static_cast<float>(settings.intensityPercent);
            if (ImGui::SliderFloat("Intensity (%)", &intensity, 0.0F, 100.0F, "%.0f%%")) {
                settings.intensityPercent = intensity;
            }
        } else {
            ImGui::SliderFloat("Elevation", &settings.value, -32767.0F, 32767.0F,
                               "%.0f");
        }
        ImGui::TextWrapped("Left-drag over the globe to paint. Falloff sets the tapered portion of the outer radius: 0%% is a cylinder, 100%% is a cone.");
        if (raiseLower) {
            ImGui::TextWrapped("At 100%% intensity, raise/lower by %.0f units per second. Configure this speed in File > Settings.",
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

void UIManager::RenderValueSelectionDialog(SphericalFibonacciGrid& grid,
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

void UIManager::FinishPolygonSelection(SphericalFibonacciGrid& grid, GlobeRenderer& renderer) {
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
        const auto& tool = brushTools_.GetSelectedTool();
        if (tool.IsContinuous()) {
            tool.Apply(*snapshot, *cursorCenter, stroke_->working, brushTools_.GetSettings(),
                       io.DeltaTime);
        } else {
            // Evaluate each dab against the pre-stroke terrain and keep the strongest
            // effect per vertex, so overlapping or stationary dabs never compound.
            if (stroke_->scratch.empty()) {
                stroke_->scratch = stroke_->original;
            }
            const auto changed = tool.Apply(*snapshot, *cursorCenter, stroke_->scratch,
                                            brushTools_.GetSettings(), io.DeltaTime);
            for (const auto index : changed) {
                const float original = stroke_->original[index];
                if (std::abs(stroke_->scratch[index] - original) >
                    std::abs(stroke_->working[index] - original)) {
                    stroke_->working[index] = stroke_->scratch[index];
                }
                stroke_->scratch[index] = original;
            }
        }
        for (std::size_t i = 0; i < stroke_->working.size(); ++i) {
            stroke_->renderVertices[i].elevation = stroke_->working[i];
        }
        renderer.UpdateElevations(stroke_->renderVertices);
    }

    if (brushTools_.IsActive() && cursorCenter && !captured) {
        const auto& settings = brushTools_.GetSettings();
        const double planetRadius = settings.distanceUnit == BrushDistanceUnit::Kilometres
                                        ? earthRadiusKilometres : earthRadiusMiles;
        const double outerRadius = settings.radius;
        const double innerRadius = outerRadius * (1.0 - settings.falloffPercent / 100.0);
        constexpr ImU32 white = IM_COL32(255, 255, 255, 230);
        drawBrushRing(*cursorCenter, outerRadius, planetRadius, modelMatrix, mvp,
                      io.DisplaySize, white);
        drawBrushRing(*cursorCenter, innerRadius, planetRadius, modelMatrix, mvp,
                      io.DisplaySize, white);
    }
}

void UIManager::ProcessSelectionInput(SphericalFibonacciGrid& grid, GlobeRenderer& renderer,
                                      const glm::mat4& modelMatrix, float zoom) {
    auto& io = ImGui::GetIO();
    const auto mvp = brushMvp(io.DisplaySize, modelMatrix, zoom);
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
        cursorCenter = pickSphere(io.MousePos, io.DisplaySize, mvp);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !cursorCenter) {
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
        const double planetRadius = selectionSettings_.distanceUnit ==
                                            BrushDistanceUnit::Kilometres
                                        ? earthRadiusKilometres : earthRadiusMiles;
        const double outerRadius = selectionSettings_.radius;
        const double innerRadius = outerRadius *
                                   (1.0 - selectionSettings_.falloffPercent / 100.0);
        constexpr ImU32 selectionRing = IM_COL32(255, 190, 40, 240);
        drawBrushRing(*cursorCenter, outerRadius, planetRadius, modelMatrix, mvp,
                      io.DisplaySize, selectionRing);
        drawBrushRing(*cursorCenter, innerRadius, planetRadius, modelMatrix, mvp,
                      io.DisplaySize, selectionRing);
    }

    if (polygonMode && !polygonPoints_.empty()) {
        auto* drawList = ImGui::GetForegroundDrawList();
        ImVec2 previous{};
        bool previousVisible = false;
        for (const auto point : polygonPoints_) {
            ImVec2 projected{};
            bool visible = false;
            if (projectPoint(point, modelMatrix, mvp, io.DisplaySize, projected, visible) &&
                visible && previousVisible) {
                drawList->AddLine(previous, projected, IM_COL32(255, 190, 40, 240), 2.0F);
            }
            previous = projected;
            previousVisible = visible;
        }
        if (cursorCenter && previousVisible) {
            ImVec2 projected{};
            bool visible = false;
            if (projectPoint(*cursorCenter, modelMatrix, mvp, io.DisplaySize, projected, visible) &&
                visible) {
                drawList->AddLine(previous, projected, IM_COL32(255, 190, 40, 180), 1.5F);
            }
        }
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
    if (!ImGui::IsPopupOpen("Select By Value") &&
        !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl) {
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
        if (ImGui::BeginMenu("Tools")) {
            for (const auto& tool : brushTools_.GetTools()) {
                const std::string name(tool->GetName());
                const bool selected = brushTools_.IsActive() &&
                    brushTools_.GetSelectedTool().GetName() == name;
                if (ImGui::MenuItem(name.c_str(), nullptr, selected)) {
                    SetSelectionMode(SelectionMode::None);
                    brushTools_.SelectTool(name);
                    brushTools_.SetActive(!selected);
                }
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
        pendingRaiseLowerRate_ = brushTools_.GetSettings().raiseLowerRate;
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
        ImGui::InputDouble("Raise/Lower speed (units/s)", &pendingRaiseLowerRate_,
                           1.0, 1024.0, "%.0f");
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
                const auto& currentOptions = grid.GetSnapshot()->GetOptions();
                if (options_.pointCount != currentOptions.pointCount ||
                    options_.jitterStrength != currentOptions.jitterStrength ||
                    options_.seed != currentOptions.seed) {
                    grid.GenerateGrid(options_);
                    undoStack_.Clear();
                    if (raster_) {
                        const auto elevations = raster_->SampleElevations(*grid.GetSnapshot());
                        grid.SetElevations(elevations);
                    }
                    renderer.UploadGrid(*grid.GetSnapshot());
                }
                brushTools_.GetSettings().raiseLowerRate = pendingRaiseLowerRate_;
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
    const bool valueSelectionDialog = valueSelectionOpen_ || ImGui::IsPopupOpen("Select By Value");
    RenderValueSelectionDialog(grid, renderer);
    if (!ImGui::IsPopupOpen("Open Raster") && !ImGui::IsPopupOpen("Settings") &&
        !valueSelectionDialog) {
        RenderBrushPanel();
        RenderSelectionPanel();
    }
    if (!valueSelectionDialog) {
        ProcessBrushInput(grid, renderer, modelMatrix, zoom);
        ProcessSelectionInput(grid, renderer, modelMatrix, zoom);
    }
    return requestExit;
}