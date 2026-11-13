#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class LayerType {
    Raster,
    Vector
};

enum class LayerBlendMode {
    Overlay,
    Add,
    Subtract,
    Multiply,
    Minimum,
    Maximum
};

float BlendElevations(float lower, float upper, LayerBlendMode mode);

enum class LayerDataType {
    Float32
};

class Layer {
public:
    Layer(std::string name, std::string description, LayerType type,
          LayerBlendMode blendMode, std::string colorMap, LayerDataType dataType);
    virtual ~Layer() = default;

    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;
    Layer(Layer&&) = delete;
    Layer& operator=(Layer&&) = delete;

    std::string_view GetName() const { return name_; }
    void SetName(std::string name);
    std::string_view GetDescription() const { return description_; }
    void SetDescription(std::string description) { description_ = std::move(description); }
    LayerType GetType() const { return type_; }
    LayerBlendMode GetBlendMode() const { return blendMode_; }
    void SetBlendMode(LayerBlendMode blendMode);
    std::string_view GetColorMap() const { return colorMap_; }
    void SetColorMap(std::string colorMap);
    std::string_view GetSelectionColorMap() const { return selectionColorMap_; }
    void SetSelectionColorMap(std::string colorMap);
    LayerDataType GetDataType() const { return dataType_; }
    bool IsVisible() const { return visible_; }
    void SetVisible(bool visible) { visible_ = visible; }

    virtual std::size_t GetValueCount() const = 0;

private:
    std::string name_;
    std::string description_;
    LayerType type_;
    LayerBlendMode blendMode_;
    std::string colorMap_;
    std::string selectionColorMap_;
    LayerDataType dataType_;
    bool visible_ = true;
};

class RasterLayer : public Layer {
public:
    RasterLayer(std::string name, std::string description, std::string colorMap,
                std::vector<float> values = {});

    std::span<const float> GetValues() const { return values_; }
    std::size_t GetValueCount() const override { return values_.size(); }

protected:
    void SetValues(std::span<const float> values);

private:
    std::vector<float> values_;
};

class ElevationLayer final : public RasterLayer {
public:
    ElevationLayer();
    ElevationLayer(std::string name, std::string colorMap,
                   std::string selectionColorMap, std::vector<float> values = {});

private:
    friend class SphericalGrid;
    void SetElevations(std::span<const float> elevations);
};
