#include "ProjectFile.hpp"
#include "AtomicFile.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

constexpr std::array<char, 8> magic{'O', 'S', 'W', 'A', 'L', 'D', '\r', '\n'};
constexpr std::uint32_t version = 1;
constexpr std::uint32_t maxString = 1'048'576;
constexpr std::uint32_t maxLayers = 1024;
constexpr auto noActiveLayer = std::numeric_limits<std::uint32_t>::max();

class Writer {
public:
    explicit Writer(const std::filesystem::path& path) : output(path, std::ios::binary) {
        output.exceptions(std::ios::failbit | std::ios::badbit);
    }
    void Bytes(const char* data, std::size_t count) {
        output.write(data, static_cast<std::streamsize>(count));
    }
    void U32(std::uint32_t value) {
        std::array<char, 4> bytes{};
        for (int i = 0; i < 4; ++i) {
            bytes[i] = static_cast<char>((value >> (8 * i)) & 255);
        }
        Bytes(bytes.data(), bytes.size());
    }
    void Float(float value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void Double(double value) {
        const auto bits = std::bit_cast<std::uint64_t>(value);
        U32(static_cast<std::uint32_t>(bits));
        U32(static_cast<std::uint32_t>(bits >> 32));
    }
    void String(std::string_view value) {
        if (value.size() > maxString || value.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("Project text is too long or contains a null character.");
        }
        U32(static_cast<std::uint32_t>(value.size()));
        Bytes(value.data(), value.size());
    }
    void Close() { output.close(); }
private:
    std::ofstream output;
};

class Reader {
public:
    explicit Reader(const std::filesystem::path& path)
        : input(path, std::ios::binary), remaining(std::filesystem::file_size(path)) {
        if (!input) {
            throw std::runtime_error("Cannot open project: " + path.string());
        }
    }
    void Require(std::uint64_t count) const {
        if (count > remaining) {
            throw std::runtime_error("Project file is truncated.");
        }
    }
    void Bytes(char* data, std::size_t count) {
        Require(count);
        input.read(data, static_cast<std::streamsize>(count));
        if (!input) {
            throw std::runtime_error("Cannot read project file.");
        }
        remaining -= count;
    }
    std::uint32_t U32() {
        std::array<char, 4> bytes{};
        Bytes(bytes.data(), bytes.size());
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
        }
        return value;
    }
    float Float() {
        const float value = std::bit_cast<float>(U32());
        if (!std::isfinite(value)) {
            throw std::runtime_error("Project contains a non-finite value.");
        }
        return value;
    }
    double Double() {
        const std::uint64_t low = U32();
        const std::uint64_t high = U32();
        const double value = std::bit_cast<double>(low | (high << 32));
        if (!std::isfinite(value)) {
            throw std::runtime_error("Project contains a non-finite option.");
        }
        return value;
    }
    bool Bool() {
        const auto value = U32();
        if (value > 1) {
            throw std::runtime_error("Project contains an invalid boolean.");
        }
        return value != 0;
    }
    std::string String() {
        const auto count = U32();
        if (count > maxString) {
            throw std::runtime_error("Project text exceeds the size limit.");
        }
        Require(count);
        std::string result(count, '\0');
        Bytes(result.data(), count);
        if (result.find('\0') != std::string::npos) {
            throw std::runtime_error("Project text contains a null character.");
        }
        return result;
    }
    void Finish() const {
        if (remaining != 0) {
            throw std::runtime_error("Project contains unexpected trailing data.");
        }
    }
private:
    std::ifstream input;
    std::uint64_t remaining;
};

std::string SaveDate() {
    const auto now = std::time(nullptr);
    const auto* utc = std::gmtime(&now);
    if (utc == nullptr) {
        throw std::runtime_error("Cannot determine the project save date.");
    }
    std::ostringstream result;
    result << std::put_time(utc, "%Y-%m-%dT%H:%M:%SZ");
    return result.str();
}

}

void ProjectFile::Save(const std::filesystem::path& path, SphericalGrid& grid) {
    const auto snapshot = grid.GetSnapshot();
    const auto& layers = grid.GetLayers();
    if (layers.empty() || layers.size() > maxLayers) {
        throw std::invalid_argument("Projects must contain between 1 and 1024 layers.");
    }
    const auto date = SaveDate();
    AtomicFile file(path);
    Writer writer(file.GetPath());
    writer.Bytes(magic.data(), magic.size());
    writer.U32(version);
    const auto& metadata = grid.GetMetadata();
    writer.String(metadata.description);
    writer.String(metadata.author);
    writer.String(date);
    const auto& options = snapshot->GetOptions();
    writer.U32(static_cast<std::uint32_t>(options.pointCount));
    writer.Double(options.poissonVariability);
    writer.U32(options.seed);
    writer.U32(options.relaxationIterations);
    const auto& vertices = snapshot->GetVertices();
    writer.U32(static_cast<std::uint32_t>(vertices.size()));
    writer.U32(static_cast<std::uint32_t>(snapshot->GetTriangleIndices().size()));
    writer.U32(snapshot->HasSelection() ? 1 : 0);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto& vertex = vertices[i];
        writer.Float(vertex.position.x);
        writer.Float(vertex.position.y);
        writer.Float(vertex.position.z);
        writer.Float(vertex.elevation);
        writer.Float(vertex.selectionWeight);
        writer.U32(snapshot->refinementVertices_[i]);
    }
    for (auto index : snapshot->GetTriangleIndices()) {
        writer.U32(index);
    }
    writer.U32(static_cast<std::uint32_t>(layers.size()));
    std::uint32_t active = noActiveLayer;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].get() == grid.activeLayer_) {
            active = static_cast<std::uint32_t>(i);
        }
    }
    writer.U32(active);
    for (const auto& layer : layers) {
        writer.U32(static_cast<std::uint32_t>(layer->GetType()));
        writer.U32(static_cast<std::uint32_t>(layer->GetDataType()));
        writer.String(layer->GetName());
        writer.String(layer->GetDescription());
        writer.String(layer->GetColorMap());
        writer.String(layer->GetSelectionColorMap());
        writer.U32(static_cast<std::uint32_t>(layer->GetBlendMode()));
        writer.U32(layer->IsVisible() ? 1 : 0);
        writer.U32(static_cast<std::uint32_t>(layer->GetValueCount()));
        for (float value : layer->GetValues()) {
            writer.Float(value);
        }
    }
    writer.Close();
    file.Commit();
    grid.GetMetadata().savedAt = date;
}

std::unique_ptr<SphericalGrid> ProjectFile::Load(const std::filesystem::path& path) {
    Reader reader(path);
    std::array<char, 8> header{};
    reader.Bytes(header.data(), header.size());
    if (header != magic || reader.U32() != version) {
        throw std::runtime_error("Not an Oswald project or unsupported project version.");
    }
    ProjectMetadata metadata{reader.String(), reader.String(), reader.String()};
    GridOptions options;
    options.pointCount = reader.U32();
    options.poissonVariability = reader.Double();
    options.seed = reader.U32();
    options.relaxationIterations = reader.U32();
    if (options.pointCount < 1000 || options.pointCount > 1'000'000 ||
        options.poissonVariability < 0 || options.poissonVariability > 1 ||
        options.relaxationIterations > 100) {
        throw std::runtime_error("Project grid options are invalid.");
    }
    const auto count = reader.U32();
    const auto indexCount = reader.U32();
    const bool selectionActive = reader.Bool();
    if (count < 4 || count > 1'000'000 || indexCount != (2 * count - 4) * 3) {
        throw std::runtime_error("Project mesh counts are invalid.");
    }
    reader.Require(std::uint64_t(count) * 24 + std::uint64_t(indexCount) * 4);
    std::vector<GridVertex> vertices(count);
    std::vector<std::uint8_t> refinement(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto& vertex = vertices[i];
        vertex.position.x = reader.Float();
        vertex.position.y = reader.Float();
        vertex.position.z = reader.Float();
        vertex.elevation = reader.Float();
        vertex.selectionWeight = reader.Float();
        refinement[i] = reader.Bool() ? 1 : 0;
        if (std::abs(glm::length(vertex.position) - 1.0F) > 1e-4F) {
            throw std::runtime_error("Project contains a point outside the unit sphere.");
        }
    }
    std::vector<std::uint32_t> indices(indexCount);
    for (auto& index : indices) {
        index = reader.U32();
    }
    auto snapshot = std::shared_ptr<const SphericalGridSnapshot>(new SphericalGridSnapshot(
        options, std::move(vertices), std::move(indices), std::move(refinement), selectionActive));
    const auto layerCount = reader.U32();
    const auto active = reader.U32();
    if (layerCount == 0 || layerCount > maxLayers ||
        (active != noActiveLayer && active >= layerCount)) {
        throw std::runtime_error("Project layer count or active layer is invalid.");
    }
    std::vector<std::unique_ptr<ElevationLayer>> layers;
    std::set<std::string> names;
    for (std::size_t i = 0; i < layerCount; ++i) {
        const auto type = reader.U32();
        const auto dataType = reader.U32();
        if (type != static_cast<std::uint32_t>(LayerType::Raster) ||
            dataType != static_cast<std::uint32_t>(LayerDataType::Float32)) {
            throw std::runtime_error("Project contains an unsupported layer type.");
        }
        auto name = reader.String();
        auto description = reader.String();
        auto colorMap = reader.String();
        auto selectionColorMap = reader.String();
        const auto mode = reader.U32();
        const bool visible = reader.Bool();
        const auto valueCount = reader.U32();
        if (!names.insert(name).second || valueCount != count ||
            mode > static_cast<std::uint32_t>(LayerBlendMode::Maximum)) {
            throw std::runtime_error("Project layer name, blend mode or value count is invalid.");
        }
        reader.Require(std::uint64_t(count) * sizeof(float));
        std::vector<float> values(count);
        for (auto& value : values) {
            value = reader.Float();
        }
        auto layer = std::make_unique<ElevationLayer>(
            std::move(name), std::move(colorMap), std::move(selectionColorMap), std::move(values));
        layer->SetDescription(std::move(description));
        layer->SetBlendMode(static_cast<LayerBlendMode>(mode));
        layer->SetVisible(visible);
        layers.push_back(std::move(layer));
    }
    reader.Finish();
    if (active != noActiveLayer) {
        if (!layers[active]->IsVisible()) {
            throw std::runtime_error("The project's active layer is hidden.");
        }
        const auto values = layers[active]->GetValues();
        for (std::size_t i = 0; i < count; ++i) {
            if (values[i] != snapshot->GetVertices()[i].elevation) {
                throw std::runtime_error("Project active layer and snapshot elevations disagree.");
            }
        }
    }
    auto grid = std::unique_ptr<SphericalGrid>(new SphericalGrid(nullptr));
    grid->snapshot_.store(std::move(snapshot));
    grid->layers_ = std::move(layers);
    grid->activeLayer_ = active == noActiveLayer ? nullptr : grid->layers_[active].get();
    grid->metadata_ = std::move(metadata);
    return grid;
}

void ProjectFile::Install(SphericalGrid& target, SphericalGrid& loaded) {
    if (&target == &loaded) {
        throw std::invalid_argument("Cannot install a project into itself.");
    }
    auto previous = target.snapshot_.exchange(loaded.snapshot_.load());
    loaded.snapshot_.store(std::move(previous));
    target.layers_.swap(loaded.layers_);
    std::swap(target.activeLayer_, loaded.activeLayer_);
    std::swap(target.metadata_, loaded.metadata_);
}
