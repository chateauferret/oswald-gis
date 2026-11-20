#pragma once

#include <array>
#include <cstdint>

class GlobeRenderer;
class UndoStack;

class MemoryStatusBar {
public:
    void Render(double now, const UndoStack& history, const GlobeRenderer* renderer = nullptr);

private:
    double nextSampleTime_ = 0.0;
    std::uint64_t sampledRevision_ = 0;
    std::array<std::array<char, 80>, 5> labels_{};
};
