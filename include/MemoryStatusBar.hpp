#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

class GlobeRenderer;
class UndoStack;

class MemoryStatusBar {
public:
    void Render(const UndoStack& history, const GlobeRenderer* renderer = nullptr);

private:
    std::uint64_t sampledRevision_ = 0;
    std::uint64_t sampledTerrainBytes_ = 0;
    std::size_t sampledPointCount_ = 0;
    bool sampled_ = false;
    std::array<std::array<char, 80>, 6> labels_{};
};
