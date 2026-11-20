#include "MemoryStatusBar.hpp"

#include "GlobeRenderer.hpp"
#include "UndoStack.hpp"

#include <glad/gl.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace {

struct MemoryUsage {
    std::uint64_t usedKiB;
    std::uint64_t totalKiB;
};

#ifdef __linux__
std::optional<std::uint64_t> readKiB(std::istringstream& line) {
    std::uint64_t value = 0;
    std::string unit;
    if (line >> value >> unit && unit == "kB") {
        return value;
    }
    return std::nullopt;
}
#endif

std::optional<MemoryUsage> readCpuMemory() {
#ifdef __linux__
    std::ifstream meminfo("/proc/meminfo");
    std::optional<std::uint64_t> total;
    std::optional<std::uint64_t> available;
    std::string text;
    while ((!total || !available) && std::getline(meminfo, text)) {
        if (text.starts_with("MemTotal:") || text.starts_with("MemAvailable:")) {
            std::istringstream line(text);
            std::string key;
            line >> key;
            (key == "MemTotal:" ? total : available) = readKiB(line);
        }
    }
    if (total && available && *total > 0 && *available <= *total) {
        return MemoryUsage{*total - *available, *total};
    }
#endif
    return std::nullopt;
}

std::optional<std::uint64_t> readProcessMemory() {
#ifdef __linux__
    std::ifstream status("/proc/self/status");
    std::string text;
    while (std::getline(status, text)) {
        if (text.starts_with("VmRSS:")) {
            std::istringstream line(text.substr(6));
            return readKiB(line);
        }
    }
#endif
    return std::nullopt;
}

std::optional<MemoryUsage> readGpuMemory() {
    if (!GLAD_GL_NVX_gpu_memory_info) {
        return std::nullopt;
    }
    GLint total = 0;
    GLint available = -1;
    glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &total);
    glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &available);
    if (total <= 0 || available < 0 || available > total) {
        return std::nullopt;
    }
    return MemoryUsage{static_cast<std::uint64_t>(total - available),
                       static_cast<std::uint64_t>(total)};
}

void formatMemory(std::array<char, 80>& label, const char* name,
                  const std::optional<MemoryUsage>& usage) {
    if (!usage) {
        std::snprintf(label.data(), label.size(), "%s: unavailable", name);
        return;
    }
    constexpr double kiBPerMiB = 1024.0;
    std::snprintf(label.data(), label.size(), "%s: %.1f / %.1f MiB", name,
                  static_cast<double>(usage->usedKiB) / kiBPerMiB,
                  static_cast<double>(usage->totalKiB) / kiBPerMiB);
}

} // namespace

void MemoryStatusBar::Render(const UndoStack& history, const GlobeRenderer* renderer) {
    const auto terrainBytes = renderer ? renderer->GetTerrainBufferBytes() : 0;
    const auto pointCount = renderer ? renderer->GetTerrainPointCount() : 0;
    if (!sampled_ || sampledRevision_ != history.Revision() ||
        sampledTerrainBytes_ != terrainBytes || sampledPointCount_ != pointCount) {
        auto& [cpu, gpu, process, terrain, points, edits] = labels_;
        formatMemory(cpu, "CPU RAM", readCpuMemory());
        formatMemory(gpu, "GPU VRAM", readGpuMemory());
        if (const auto rss = readProcessMemory()) {
            std::snprintf(process.data(), process.size(), "Oswald RAM: %.2f MiB",
                          static_cast<double>(*rss) / 1024.0);
        } else {
            std::snprintf(process.data(), process.size(), "Oswald RAM: unavailable");
        }
        if (renderer) {
            std::snprintf(terrain.data(), terrain.size(), "Terrain buffers: %.2f MiB",
                          static_cast<double>(terrainBytes) / (1024.0 * 1024.0));
            std::snprintf(points.data(), points.size(), "Data points: %zu",
                          pointCount);
        }
        std::snprintf(edits.data(), edits.size(), "Undo: %zu / Redo: %zu",
                      history.UndoCount(), history.Size() - history.UndoCount());
        sampledRevision_ = history.Revision();
        sampledTerrainBytes_ = terrainBytes;
        sampledPointCount_ = pointCount;
        sampled_ = true;
    }

    auto* viewport = ImGui::GetMainViewport();
    const auto& style = ImGui::GetStyle();
    const float gap = 2.0F * style.ItemSpacing.x;
    const float availableWidth = viewport->Size.x - 2.0F * style.WindowPadding.x;
    std::array<bool, std::tuple_size_v<decltype(labels_)>> sameLine{};
    float width = 0.0F;
    int rows = 1;
    for (std::size_t i = 0; i < labels_.size(); ++i) {
        if (labels_[i][0] == '\0') {
            continue;
        }
        const float labelWidth = ImGui::CalcTextSize(labels_[i].data()).x;
        if (width > 0.0F && width + gap + labelWidth <= availableWidth) {
            sameLine[i] = true;
            width += gap + labelWidth;
        } else {
            rows += width > 0.0F ? 1 : 0;
            width = labelWidth;
        }
    }
    const float height = ImGui::GetTextLineHeight() + 2.0F * style.WindowPadding.y +
                         (rows - 1) * ImGui::GetTextLineHeightWithSpacing();
    constexpr auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::BeginViewportSideBar("##MemoryStatusBar", viewport, ImGuiDir_Down, height, flags)) {
        for (std::size_t i = 0; i < labels_.size(); ++i) {
            if (labels_[i][0] == '\0') {
                continue;
            }
            if (sameLine[i]) {
                ImGui::SameLine(0.0F, gap);
            }
            ImGui::TextUnformatted(labels_[i].data());
        }
    }
    ImGui::End();
}
