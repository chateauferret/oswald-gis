#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct alignas(16) GpuScriptInput {
    std::array<float, 4> positionAndLatitude;
    std::array<float, 4> terrain;
    std::array<float, 4> brush;
    std::array<float, 4> timing;
    std::uint32_t vertexIndex;
    std::array<std::uint32_t, 3> padding{};
};

struct alignas(16) GpuScriptTerrainVertex {
    std::array<float, 4> position;
};

struct GpuScriptParameter {
    std::string name;
    float value = 0.0F;
};

class BrushScriptComputeBackend {
public:
    virtual ~BrushScriptComputeBackend() = default;
    virtual std::vector<float> EvaluateBrushScript(
        std::string_view gpuScript, std::span<const GpuScriptInput> inputs,
        std::span<const GpuScriptParameter> parameters, std::uint64_t seed,
        std::span<const GpuScriptTerrainVertex> vertices,
        std::span<const float> elevations, double planetRadiusKm) = 0;
};

static_assert(sizeof(GpuScriptInput) == 80);
static_assert(sizeof(GpuScriptTerrainVertex) == 16);
