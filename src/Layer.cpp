#include "Layer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {

bool validLayerType(LayerType type) {
    return type == LayerType::Raster || type == LayerType::Vector;
}

bool validBlendMode(LayerBlendMode blendMode) {
    switch (blendMode) {
    case LayerBlendMode::Overlay:
    case LayerBlendMode::Add:
    case LayerBlendMode::Subtract:
    case LayerBlendMode::Multiply:
    case LayerBlendMode::Minimum:
    case LayerBlendMode::Maximum:
        return true;
    }
    return false;
}

bool validDataType(LayerDataType dataType) {
    return dataType == LayerDataType::Float32;
}

void validateValues(std::span<const float> values) {
    for (const float value : values) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("Raster layer values must be finite.");
        }
    }
}

void validateElevationValues(std::span<const float> elevations) {
    for (const float elevation : elevations) {
        if (!std::isfinite(elevation) || elevation < -32767.0F ||
            elevation > 32767.0F) {
            throw std::invalid_argument(
                "Elevations must be finite and in the range [-32767, 32767].");
        }
    }
}

}

float BlendElevations(float lower, float upper, LayerBlendMode mode) {
    if (!std::isfinite(lower) || !std::isfinite(upper)) {
        throw std::invalid_argument("Blended elevations must be finite.");
    }
    float result = upper;
    switch (mode) {
    case LayerBlendMode::Overlay: break;
    case LayerBlendMode::Add: result = lower + upper; break;
    case LayerBlendMode::Subtract: result = lower - upper; break;
    case LayerBlendMode::Multiply: result = lower * upper; break;
    case LayerBlendMode::Minimum: result = std::min(lower, upper); break;
    case LayerBlendMode::Maximum: result = std::max(lower, upper); break;
    default: throw std::invalid_argument("Layer blend mode is invalid.");
    }
    return std::clamp(result, -32767.0F, 32767.0F);
}

Layer::Layer(std::string name, std::string description, LayerType type,
             LayerBlendMode blendMode, std::string colorMap, LayerDataType dataType)
    : name_(std::move(name)), description_(std::move(description)), type_(type),
      blendMode_(blendMode), colorMap_(std::move(colorMap)), dataType_(dataType) {
    if (name_.empty()) {
        throw std::invalid_argument("Layer names must not be empty.");
    }
    if (!validLayerType(type_)) {
        throw std::invalid_argument("Layer type is invalid.");
    }
    if (!validBlendMode(blendMode_)) {
        throw std::invalid_argument("Layer blend mode is invalid.");
    }
    if (colorMap_.empty()) {
        throw std::invalid_argument("Layer colour maps must not be empty.");
    }
    selectionColorMap_ = "grayscale.map";
    if (!validDataType(dataType_)) {
        throw std::invalid_argument("Layer data type is invalid.");
    }
}

void Layer::SetName(std::string name) {
    if (name.empty()) {
        throw std::invalid_argument("Layer names must not be empty.");
    }
    name_ = std::move(name);
}

void Layer::SetBlendMode(LayerBlendMode blendMode) {
    if (!validBlendMode(blendMode)) {
        throw std::invalid_argument("Layer blend mode is invalid.");
    }
    blendMode_ = blendMode;
}

void Layer::SetColorMap(std::string colorMap) {
    if (colorMap.empty()) {
        throw std::invalid_argument("Layer colour maps must not be empty.");
    }
    colorMap_ = std::move(colorMap);
}

void Layer::SetSelectionColorMap(std::string colorMap) {
    if (colorMap.empty()) {
        throw std::invalid_argument("Layer colour maps must not be empty.");
    }
    selectionColorMap_ = std::move(colorMap);
}

RasterLayer::RasterLayer(std::string name, std::string description, std::string colorMap,
                         std::vector<float> values)
    : Layer(std::move(name), std::move(description), LayerType::Raster,
            LayerBlendMode::Overlay, std::move(colorMap), LayerDataType::Float32),
      values_(std::move(values)) {
    validateValues(values_);
}

void RasterLayer::SetValues(std::span<const float> values) {
    validateValues(values);
    values_.assign(values.begin(), values.end());
}

ElevationLayer::ElevationLayer()
    : RasterLayer("Elevation", "Elevation values mapped to the spherical grid.",
                  "default.map") {}

ElevationLayer::ElevationLayer(std::string name, std::string colorMap,
                               std::string selectionColorMap, std::vector<float> values)
    : RasterLayer(std::move(name), "Elevation values mapped to the spherical grid.",
                  std::move(colorMap), std::move(values)) {
    validateElevationValues(GetValues());
    SetSelectionColorMap(std::move(selectionColorMap));
}

void ElevationLayer::SetElevations(std::span<const float> elevations) {
    validateElevationValues(elevations);
    SetValues(elevations);
}
