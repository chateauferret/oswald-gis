#include "GlobeRenderer.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void checkGl(const char* operation) {
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        throw std::runtime_error(std::string(operation) + ": OpenGL error " + std::to_string(error));
    }
}

struct Shader {
    GLuint id;
    ~Shader() { glDeleteShader(id); }
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    explicit Shader(GLenum type) : id(glCreateShader(type)) {}
};

void compileShader(const Shader& shader, const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open shader: " + path.string());
    }
    const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        throw std::runtime_error("Failed to read shader: " + path.string());
    }
    const char* text = source.c_str();
    glShaderSource(shader.id, 1, &text, nullptr);
    glCompileShader(shader.id);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader.id, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        GLint length = 0;
        glGetShaderiv(shader.id, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetShaderInfoLog(shader.id, length, nullptr, log.data());
        throw std::runtime_error("Shader compilation failed: " + path.string() + "\n" + log);
    }
}

GLint uniform(GLuint program, const char* name) {
    const GLint location = glGetUniformLocation(program, name);
    if (location < 0) {
        throw std::runtime_error(std::string("Missing shader uniform: ") + name);
    }
    return location;
}

template <class T>
GLsizeiptr bufferSize(std::span<const T> data) {
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max()) / sizeof(T)) {
        throw std::invalid_argument("GPU buffer size exceeds OpenGL's addressable range.");
    }
    return static_cast<GLsizeiptr>(data.size_bytes());
}

GLuint linkProgram(const std::filesystem::path& vertexPath,
                   const std::filesystem::path& fragmentPath = {}) {
    Shader vertex(fragmentPath.empty() ? GL_COMPUTE_SHADER : GL_VERTEX_SHADER);
    Shader fragment(GL_FRAGMENT_SHADER);
    compileShader(vertex, vertexPath);
    if (!fragmentPath.empty()) {
        compileShader(fragment, fragmentPath);
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex.id);
    if (!fragmentPath.empty()) {
        glAttachShader(program, fragment.id);
    }
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());
        glDeleteProgram(program);
        throw std::runtime_error("Shader linking failed:\n" + log);
    }
    glDetachShader(program, vertex.id);
    if (!fragmentPath.empty()) {
        glDetachShader(program, fragment.id);
    }
    return program;
}

bool validColor(const glm::vec3& color) {
    return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) &&
           glm::all(glm::greaterThanEqual(color, glm::vec3(0.0F))) &&
           glm::all(glm::lessThanEqual(color, glm::vec3(1.0F)));
}

} // namespace

GlobeRenderer::Resources::~Resources() {
    for (const auto& [layer, buffer] : layerElevationBuffers) {
        (void)layer;
        glDeleteBuffers(1, &buffer);
    }
    for (const auto& [map, texture] : layerColorTextures) {
        (void)map;
        glDeleteTextures(1, &texture);
    }
    for (const auto& [map, texture] : layerSelectionColorTextures) {
        (void)map;
        glDeleteTextures(1, &texture);
    }
    glDeleteVertexArrays(1, &wireVao);
    glDeleteBuffers(1, &wirePositionBuffer);
    glDeleteBuffers(1, &wireWeightBuffer);
    if (wireProgram != 0) {
        glDeleteProgram(wireProgram);
    }
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &positionBuffer);
    glDeleteBuffers(1, &elevationBuffer);
    glDeleteBuffers(1, &compositeElevationBuffer);
    if (compositeProgram != 0) {
        glDeleteProgram(compositeProgram);
    }
    glDeleteBuffers(1, &selectionBuffer);
    glDeleteBuffers(1, &indexBuffer);
    glDeleteTextures(1, &colorTexture);
    glDeleteTextures(1, &selectionColorTexture);
    if (program != 0) {
        glDeleteProgram(program);
    }
}

GlobeRenderer::GlobeRenderer(const std::filesystem::path& shaderDirectory) {
    if (!GLAD_GL_VERSION_4_5) {
        throw std::runtime_error("Globe rendering requires OpenGL 4.5.");
    }
    Shader vertex(GL_VERTEX_SHADER);
    Shader fragment(GL_FRAGMENT_SHADER);
    compileShader(vertex, shaderDirectory / "globe.vert");
    compileShader(fragment, shaderDirectory / "globe.frag");
    resources_.program = glCreateProgram();
    glAttachShader(resources_.program, vertex.id);
    glAttachShader(resources_.program, fragment.id);
    glLinkProgram(resources_.program);
    GLint linked = GL_FALSE;
    glGetProgramiv(resources_.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(resources_.program, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetProgramInfoLog(resources_.program, length, nullptr, log.data());
        throw std::runtime_error("Globe shader linking failed:\n" + log);
    }
    glDetachShader(resources_.program, vertex.id);
    glDetachShader(resources_.program, fragment.id);
    matrixLocation_ = uniform(resources_.program, "uModelViewProjection");
    viewDirectionLocation_ = uniform(resources_.program, "uViewDirectionObject");
    graticuleSpacingLocation_ = uniform(resources_.program, "uGraticuleSpacing");
    graticuleColorLocation_ = uniform(resources_.program, "uGraticuleColor");
    highlightedGraticuleColorLocation_ =
        uniform(resources_.program, "uHighlightedGraticuleColor");
    selectionActiveLocation_ = uniform(resources_.program, "uSelectionActive");
    graticuleEnabledLocation_ = uniform(resources_.program, "uGraticuleEnabled");
    glProgramUniform1i(resources_.program, uniform(resources_.program, "uColorLut"), 1);
    glProgramUniform1i(resources_.program,
                       uniform(resources_.program, "uSelectionColorLut"), 2);

    resources_.compositeProgram = linkProgram(shaderDirectory / "elevation.comp");
    compositeCountLocation_ = uniform(resources_.compositeProgram, "uVertexCount");
    compositeModeLocation_ = uniform(resources_.compositeProgram, "uBlendMode");
    glCreateVertexArrays(1, &resources_.vao);
    glCreateBuffers(1, &resources_.positionBuffer);
    glCreateBuffers(1, &resources_.elevationBuffer);
    glCreateBuffers(1, &resources_.compositeElevationBuffer);
    glCreateBuffers(1, &resources_.selectionBuffer);
    glCreateBuffers(1, &resources_.indexBuffer);
    glCreateTextures(GL_TEXTURE_1D, 1, &resources_.colorTexture);
    glCreateTextures(GL_TEXTURE_1D, 1, &resources_.selectionColorTexture);
    glEnableVertexArrayAttrib(resources_.vao, 0);
    glVertexArrayAttribFormat(resources_.vao, 0, 3, GL_FLOAT, GL_FALSE,
                             0);
    glVertexArrayAttribBinding(resources_.vao, 0, 0);
    glEnableVertexArrayAttrib(resources_.vao, 1);
    glVertexArrayAttribFormat(resources_.vao, 1, 1, GL_FLOAT, GL_FALSE,
                             0);
    glVertexArrayAttribBinding(resources_.vao, 1, 1);
    glEnableVertexArrayAttrib(resources_.vao, 2);
    glVertexArrayAttribFormat(resources_.vao, 2, 1, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(resources_.vao, 2, 2);
    glVertexArrayVertexBuffer(resources_.vao, 0, resources_.positionBuffer, 0, sizeof(glm::vec3));
    glVertexArrayVertexBuffer(resources_.vao, 1, resources_.elevationBuffer, 0, sizeof(float));
    glVertexArrayVertexBuffer(resources_.vao, 2, resources_.selectionBuffer, 0, sizeof(float));
    glVertexArrayElementBuffer(resources_.vao, resources_.indexBuffer);

    resources_.wireProgram = linkProgram(shaderDirectory / "wire.vert", shaderDirectory / "wire.frag");
    wireMatrixLocation_ = uniform(resources_.wireProgram, "uModelViewProjection");
    wireDepthBiasLocation_ = uniform(resources_.wireProgram, "uDepthBias");
    wireSelectionActiveLocation_ = uniform(resources_.wireProgram, "uSelectionActive");
    wireUnselectedColorLocation_ = uniform(resources_.wireProgram, "uUnselectedColor");
    wireSelectedColorLocation_ = uniform(resources_.wireProgram, "uSelectedColor");
    glCreateVertexArrays(1, &resources_.wireVao);
    glCreateBuffers(1, &resources_.wirePositionBuffer);
    glCreateBuffers(1, &resources_.wireWeightBuffer);
    glEnableVertexArrayAttrib(resources_.wireVao, 0);
    glVertexArrayAttribFormat(resources_.wireVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(resources_.wireVao, 0, 0);
    glEnableVertexArrayAttrib(resources_.wireVao, 1);
    glVertexArrayAttribFormat(resources_.wireVao, 1, 1, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(resources_.wireVao, 1, 1);
    glVertexArrayVertexBuffer(resources_.wireVao, 0, resources_.wirePositionBuffer, 0, sizeof(glm::vec3));
    glVertexArrayVertexBuffer(resources_.wireVao, 1, resources_.wireWeightBuffer, 0, sizeof(float));
    checkGl("Creating globe resources");
}

void GlobeRenderer::UploadGrid(const SphericalGridSnapshot& snapshot) {
    UploadMesh(snapshot.GetVertices(), snapshot.GetTriangleIndices(), snapshot.HasSelection());
}

void GlobeRenderer::UploadMesh(std::span<const GridVertex> vertices,
                               std::span<const std::uint32_t> indices, bool selectionActive) {
    if (vertices.empty() || indices.empty() || indices.size() % 3 != 0 ||
        indices.size() > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) {
        throw std::invalid_argument("Mesh must contain vertices and complete indexed triangles.");
    }
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) ||
            !std::isfinite(vertex.position.z) || glm::dot(vertex.position, vertex.position) == 0.0F ||
            !std::isfinite(vertex.elevation)) {
            throw std::invalid_argument("Mesh positions must be finite and nonzero; elevations must be finite.");
        }
    }
    if (std::any_of(indices.begin(), indices.end(), [&](auto index) { return index >= vertices.size(); })) {
        throw std::invalid_argument("Mesh contains an out-of-range index.");
    }
    const auto indexBytes = bufferSize(indices);
    std::vector<glm::vec3> positions;
    std::vector<float> elevations;
    std::vector<float> selectionWeights;
    positions.reserve(vertices.size());
    elevations.reserve(vertices.size());
    selectionWeights.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        positions.push_back(vertex.position);
        elevations.push_back(vertex.elevation);
        if (!std::isfinite(vertex.selectionWeight) ||
            vertex.selectionWeight < 0.0F || vertex.selectionWeight > 1.0F) {
            throw std::invalid_argument("Selection weights must be finite and in the range [0, 1].");
        }
        selectionWeights.push_back(vertex.selectionWeight);
    }
    const auto positionBytes = bufferSize(std::span<const glm::vec3>(positions));
    const auto elevationBytes = bufferSize(std::span<const float>(elevations));
    for (const auto& [layer, buffer] : resources_.layerElevationBuffers) {
        (void)layer;
        glDeleteBuffers(1, &buffer);
    }
    resources_.layerElevationBuffers.clear();
    glNamedBufferData(resources_.positionBuffer, positionBytes, positions.data(), GL_STATIC_DRAW);
    glNamedBufferData(resources_.elevationBuffer, elevationBytes, elevations.data(), GL_DYNAMIC_DRAW);
    glNamedBufferData(resources_.compositeElevationBuffer, elevationBytes, nullptr, GL_DYNAMIC_DRAW);
    compositeDirty_ = true;
    composedLayers_.clear();
    const auto selectionBytes = bufferSize(std::span<const float>(selectionWeights));
    glNamedBufferData(resources_.selectionBuffer, selectionBytes, selectionWeights.data(), GL_DYNAMIC_DRAW);
    glNamedBufferData(resources_.indexBuffer, indexBytes, indices.data(), GL_STATIC_DRAW);
    checkGl("Uploading globe mesh");
    indexCount_ = static_cast<GLsizei>(indices.size());
    vertexCount_ = vertices.size();
    selectionActive_ = selectionActive;
    BuildVoronoi(vertices, indices);
    UploadWireWeights(vertices);
}

void GlobeRenderer::BuildVoronoi(std::span<const GridVertex> vertices,
                                 std::span<const std::uint32_t> indices) {
    // Each Delaunay face yields a Voronoi vertex at its circumcentre; each Delaunay edge
    // yields a Voronoi edge joining the circumcentres of the two faces that share it.
    const std::size_t faceCount = indices.size() / 3;
    std::vector<glm::vec3> centers(faceCount);
    for (std::size_t face = 0; face < faceCount; ++face) {
        const glm::dvec3 a(vertices[indices[3 * face]].position);
        const glm::dvec3 b(vertices[indices[3 * face + 1]].position);
        const glm::dvec3 c(vertices[indices[3 * face + 2]].position);
        glm::dvec3 normal = glm::cross(b - a, c - a);
        if (glm::dot(normal, a + b + c) < 0.0) {
            normal = -normal;
        }
        const double length = glm::length(normal);
        centers[face] = length > 0.0 ? glm::vec3(normal / length)
                                     : glm::vec3(glm::normalize(a + b + c));
    }

    struct EdgeRecord {
        std::uint64_t key;
        std::uint32_t face;
        std::uint32_t low;
        std::uint32_t high;
    };
    std::vector<EdgeRecord> edges;
    edges.reserve(indices.size());
    for (std::size_t face = 0; face < faceCount; ++face) {
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto u = indices[3 * face + corner];
            const auto v = indices[3 * face + (corner + 1) % 3];
            const auto low = std::min(u, v);
            const auto high = std::max(u, v);
            edges.push_back({(static_cast<std::uint64_t>(low) << 32) | high,
                             static_cast<std::uint32_t>(face), low, high});
        }
    }
    std::sort(edges.begin(), edges.end(),
              [](const EdgeRecord& l, const EdgeRecord& r) { return l.key < r.key; });

    std::vector<glm::vec3> positions;
    positions.reserve(edges.size());
    wireSites_.clear();
    wireSites_.reserve(edges.size() / 2);
    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
        if (edges[i].key != edges[i + 1].key) {
            continue;
        }
        positions.push_back(centers[edges[i].face]);
        positions.push_back(centers[edges[i + 1].face]);
        wireSites_.push_back({edges[i].low, edges[i].high});
        ++i;
    }
    glNamedBufferData(resources_.wirePositionBuffer,
                      bufferSize(std::span<const glm::vec3>(positions)), positions.data(),
                      GL_STATIC_DRAW);
    wireVertexCount_ = static_cast<GLsizei>(positions.size());
    checkGl("Uploading Voronoi wireframe");
}

void GlobeRenderer::UploadWireWeights(std::span<const GridVertex> vertices) {
    std::vector<float> weights;
    weights.reserve(wireSites_.size() * 2);
    for (const auto& sites : wireSites_) {
        const float weight = 0.5F * (vertices[sites[0]].selectionWeight +
                                     vertices[sites[1]].selectionWeight);
        weights.push_back(weight);
        weights.push_back(weight);
    }
    glNamedBufferData(resources_.wireWeightBuffer,
                      bufferSize(std::span<const float>(weights)), weights.data(),
                      GL_DYNAMIC_DRAW);
    checkGl("Uploading Voronoi selection weights");
}

void GlobeRenderer::UpdateSelection(std::span<const GridVertex> vertices, bool active) {
    if (vertices.size() != vertexCount_ || vertices.empty()) {
        throw std::invalid_argument("Selection update must match the uploaded vertex count.");
    }
    std::vector<float> selectionWeights;
    selectionWeights.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.selectionWeight) ||
            vertex.selectionWeight < 0.0F || vertex.selectionWeight > 1.0F) {
            throw std::invalid_argument("Selection weights must be finite and in the range [0, 1].");
        }
        selectionWeights.push_back(vertex.selectionWeight);
    }
    const auto bytes = bufferSize(std::span<const float>(selectionWeights));
    glNamedBufferSubData(resources_.selectionBuffer, 0, bytes, selectionWeights.data());
    checkGl("Updating globe selection");
    UploadWireWeights(vertices);
    selectionActive_ = active;
}

void GlobeRenderer::UpdateElevations(std::span<const GridVertex> vertices) {
    if (vertices.size() != vertexCount_ || vertices.empty()) {
        throw std::invalid_argument("Elevation update must match the uploaded vertex count.");
    }
    std::vector<float> elevations;
    elevations.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.elevation)) {
            throw std::invalid_argument("Elevation update contains a non-finite value.");
        }
        elevations.push_back(vertex.elevation);
    }
    const auto bytes = bufferSize(std::span<const float>(elevations));
    glNamedBufferSubData(resources_.elevationBuffer, 0, bytes, elevations.data());
    checkGl("Updating globe elevations");
}

void GlobeRenderer::UpdateLayerElevations(const ElevationLayer& layer,
                                          std::span<const float> elevations,
                                          std::span<const std::size_t> changedIndices) {
    if (elevations.size() != vertexCount_ || elevations.size() != layer.GetValueCount()) {
        throw std::invalid_argument("Layer elevation update must match the uploaded vertex count.");
    }
    const auto found = resources_.layerElevationBuffers.find(&layer);
    GLuint targetBuffer = resources_.elevationBuffer;
    if (found != resources_.layerElevationBuffers.end()) {
        targetBuffer = found->second;
    } else if (layerStackEnabled_) {
        throw std::logic_error("Layer elevation buffer has not been initialized.");
    }
    if (changedIndices.empty()) {
        for (const float elevation : elevations) {
            if (!std::isfinite(elevation)) {
                throw std::invalid_argument("Layer elevation update contains a non-finite value.");
            }
        }
        glNamedBufferSubData(targetBuffer, 0, bufferSize(elevations), elevations.data());
    } else {
        std::vector<std::size_t> ordered(changedIndices.begin(), changedIndices.end());
        std::sort(ordered.begin(), ordered.end());
        ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
        if (std::any_of(ordered.begin(), ordered.end(), [&](std::size_t index) {
                return index >= elevations.size() || !std::isfinite(elevations[index]);
            })) {
            throw std::invalid_argument("Changed layer elevation index is invalid.");
        }
        for (std::size_t cursor = 0; cursor < ordered.size();) {
            const std::size_t start = ordered[cursor];
            std::size_t end = cursor + 1;
            while (end < ordered.size() && ordered[end] == ordered[end - 1] + 1) {
                ++end;
            }
            const std::size_t count = end - cursor;
            glNamedBufferSubData(targetBuffer,
                                 static_cast<GLintptr>(start * sizeof(float)),
                                 bufferSize(elevations.subspan(start, count)),
                                 elevations.data() + start);
            cursor = end;
        }
    }
    compositeDirty_ = true;
    checkGl("Updating layer elevations");
}

void GlobeRenderer::SetColorMap(const ColorMap& map) {
    UploadColorMap(map, resources_.colorTexture);
    hasColorMap_ = true;
}

void GlobeRenderer::SetSelectionColorMap(const ColorMap& map) {
    UploadColorMap(map, resources_.selectionColorTexture);
    hasSelectionColorMap_ = true;
}

void GlobeRenderer::SetLayerStack(std::vector<GlobeRenderLayer> layers) {
    for (const auto& layer : layers) {
        if (layer.layer == nullptr ||
            layer.colorMap == nullptr || layer.selectionColorMap == nullptr ||
            layer.layer->GetValueCount() != vertexCount_) {
            throw std::invalid_argument("Layer elevation count must match the uploaded mesh.");
        }
    }
    for (auto buffer = resources_.layerElevationBuffers.begin();
         buffer != resources_.layerElevationBuffers.end();) {
        const bool present = std::any_of(layers.begin(), layers.end(), [&](const auto& layer) {
            return layer.layer == buffer->first;
        });
        if (present) {
            ++buffer;
        } else {
            glDeleteBuffers(1, &buffer->second);
            buffer = resources_.layerElevationBuffers.erase(buffer);
        }
    }
    for (const auto& layer : layers) {
        if (resources_.layerElevationBuffers.contains(layer.layer)) {
            continue;
        }
        GLuint buffer = 0;
        glCreateBuffers(1, &buffer);
        if (buffer == 0) {
            throw std::runtime_error("Could not allocate a layer elevation buffer.");
        }
        const auto values = layer.layer->GetValues();
        glNamedBufferData(buffer, bufferSize(values), values.data(), GL_DYNAMIC_DRAW);
        resources_.layerElevationBuffers.emplace(layer.layer, buffer);
        compositeDirty_ = true;
    }
    checkGl("Setting globe layer stack");
    layerStack_ = std::move(layers);
    layerStackEnabled_ = true;
}

void GlobeRenderer::ClearLayerStack() {
    layerStack_.clear();
    layerStackEnabled_ = false;
}

GLuint GlobeRenderer::GetLayerTexture(const ColorMap& map, bool selection) {
    auto& textures = selection ? resources_.layerSelectionColorTextures
                               : resources_.layerColorTextures;
    const auto found = textures.find(&map);
    if (found != textures.end()) {
        return found->second;
    }
    GLuint texture = 0;
    glCreateTextures(GL_TEXTURE_1D, 1, &texture);
    if (texture == 0) {
        throw std::runtime_error("Could not allocate a layer colour-map texture.");
    }
    try {
        UploadColorMap(map, texture);
        textures.emplace(&map, texture);
    } catch (...) {
        glDeleteTextures(1, &texture);
        throw;
    }
    return texture;
}

void GlobeRenderer::SetGraticuleColors(const glm::vec3& ordinary,
                                       const glm::vec3& highlighted) {
    if (!validColor(ordinary) || !validColor(highlighted)) {
        throw std::invalid_argument("Graticule colours must be finite RGB values in [0, 1].");
    }
    graticuleColor_ = ordinary;
    highlightedGraticuleColor_ = highlighted;
}

void GlobeRenderer::SetWireframeColors(const glm::vec3& unselected, const glm::vec3& selected) {
    if (!validColor(unselected) || !validColor(selected)) {
        throw std::invalid_argument("Wireframe colours must be finite RGB values in [0, 1].");
    }
    wireframeColor_ = unselected;
    wireframeSelectedColor_ = selected;
}

void GlobeRenderer::SetWireframeEnabled(bool enabled) {
    wireframeEnabled_ = enabled;
}

void GlobeRenderer::SetGraticuleEnabled(bool enabled) {
    graticuleEnabled_ = enabled;
}

void GlobeRenderer::UploadColorMap(const ColorMap& map, GLuint texture) {
    const auto stops = map.GetStops();
    if (stops.size() < 2) {
        throw std::invalid_argument("Colour map must contain at least two stops.");
    }
    if (stops.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max())) {
        throw std::invalid_argument("Colour map exceeds shader indexing limits.");
    }
    static_assert(sizeof(glm::vec4) == 4 * sizeof(float));
    GLint maximumSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumSize);
    if (stops.size() > static_cast<std::size_t>(maximumSize)) {
        throw std::invalid_argument("Colour map exceeds this GPU's 1D texture size limit.");
    }
    glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_1D, texture);
    glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA32F, static_cast<GLsizei>(stops.size()), 0,
                 GL_RGBA, GL_FLOAT, stops.data());
    glBindTexture(GL_TEXTURE_1D, 0);
    checkGl("Uploading colour map");
}

void GlobeRenderer::Draw(int framebufferWidth, int framebufferHeight,
                         const glm::mat4& modelMatrix, float zoom) {
    if (framebufferWidth < 0 || framebufferHeight < 0) {
        throw std::invalid_argument("Framebuffer dimensions must be nonnegative.");
    }
    if (!std::isfinite(zoom) || zoom <= 0.0F) {
        throw std::invalid_argument("Globe zoom must be finite and positive.");
    }
    if (framebufferWidth == 0 || framebufferHeight == 0) {
        return;
    }
    if (!layerStackEnabled_ && !layerVisible_) {
        return;
    }
    if (indexCount_ == 0 || (!layerStackEnabled_ && !hasColorMap_)) {
        throw std::logic_error("Upload a mesh and colour map before drawing the globe.");
    }
    if (selectionActive_ && !layerStackEnabled_ && !hasSelectionColorMap_) {
        throw std::logic_error("Upload a selection colour map before drawing an active selection.");
    }
    const float aspect = static_cast<float>(framebufferWidth) / framebufferHeight;
    const float horizontal = 1.15F * std::max(aspect, 1.0F) / zoom;
    const float vertical = 1.15F * std::max(1.0F / aspect, 1.0F) / zoom;
    const auto projection = glm::ortho(-horizontal, horizontal, -vertical, vertical, 0.1F, 10.0F);
    const auto view = glm::lookAt(glm::vec3(0.0F, -3.0F, 0.0F),
                                  glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, 1.0F));
    const auto matrix = projection * view * modelMatrix;
    const auto viewDirection = glm::normalize(glm::vec3(glm::inverse(modelMatrix) *
                                                         glm::vec4(0.0F, -1.0F, 0.0F, 0.0F)));
    glViewport(0, 0, framebufferWidth, framebufferHeight);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glUseProgram(resources_.program);
    glProgramUniformMatrix4fv(resources_.program, matrixLocation_, 1, GL_FALSE, glm::value_ptr(matrix));
    glProgramUniform3fv(resources_.program, viewDirectionLocation_, 1,
                        glm::value_ptr(viewDirection));
    glProgramUniform1f(resources_.program, graticuleSpacingLocation_,
                       glm::radians(15.0F) / std::max(zoom, 1.0F));
    glProgramUniform3fv(resources_.program, graticuleColorLocation_, 1,
                        glm::value_ptr(graticuleColor_));
    glProgramUniform3fv(resources_.program, highlightedGraticuleColorLocation_, 1,
                        glm::value_ptr(highlightedGraticuleColor_));
    glProgramUniform1i(resources_.program, graticuleEnabledLocation_, graticuleEnabled_ ? 1 : 0);
    glBindVertexArray(resources_.vao);
    bool renderedLayer = false;
    if (layerStackEnabled_) {
        std::vector<std::pair<const ElevationLayer*, LayerBlendMode>> visibleLayers;
        const GlobeRenderLayer* topLayer = nullptr;
        for (auto layerIt = layerStack_.rbegin(); layerIt != layerStack_.rend(); ++layerIt) {
            const auto& layer = *layerIt;
            if (!layer.layer->IsVisible()) {
                continue;
            }
            visibleLayers.emplace_back(layer.layer, layer.layer->GetBlendMode());
            topLayer = &layer;
        }
        if (topLayer != nullptr) {
            if (compositeDirty_ || composedLayers_ != visibleLayers) {
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                glUseProgram(resources_.compositeProgram);
                glProgramUniform1ui(resources_.compositeProgram, compositeCountLocation_,
                                   static_cast<GLuint>(vertexCount_));
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, resources_.compositeElevationBuffer);
                bool hasBaseLayer = false;
                for (const auto& [layer, mode] : visibleLayers) {
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0,
                                     resources_.layerElevationBuffers.at(layer));
                    glProgramUniform1i(resources_.compositeProgram, compositeModeLocation_,
                        static_cast<GLint>(hasBaseLayer ? mode : LayerBlendMode::Overlay));
                    glDispatchCompute(static_cast<GLuint>((vertexCount_ + 255) / 256), 1, 1);
                    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                    hasBaseLayer = true;
                }
                glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
                composedLayers_ = std::move(visibleLayers);
                compositeDirty_ = false;
            }
            glUseProgram(resources_.program);
            glVertexArrayVertexBuffer(resources_.vao, 1,
                                      resources_.compositeElevationBuffer, 0, sizeof(float));
            glBindTextureUnit(1, GetLayerTexture(*topLayer->colorMap, false));
            glBindTextureUnit(2, GetLayerTexture(*topLayer->selectionColorMap, true));
            glProgramUniform1i(resources_.program, selectionActiveLocation_,
                               selectionActive_ ? 1 : 0);
            glDisable(GL_BLEND);
            glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
            renderedLayer = true;
        }
    } else {
        glBindTextureUnit(1, resources_.colorTexture);
        glBindTextureUnit(2, resources_.selectionColorTexture);
        glProgramUniform1i(resources_.program, selectionActiveLocation_,
                           selectionActive_ ? 1 : 0);
        glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    }
    glVertexArrayVertexBuffer(resources_.vao, 1, resources_.elevationBuffer, 0, sizeof(float));
    glBindVertexArray(0);

    if (wireframeEnabled_ && wireVertexCount_ > 0 &&
        (!layerStackEnabled_ || renderedLayer)) {
        glUseProgram(resources_.wireProgram);
        glProgramUniformMatrix4fv(resources_.wireProgram, wireMatrixLocation_, 1, GL_FALSE,
                                  glm::value_ptr(matrix));
        glProgramUniform1f(resources_.wireProgram, wireDepthBiasLocation_, 0.006F);
        glProgramUniform1i(resources_.wireProgram, wireSelectionActiveLocation_,
                           selectionActive_ ? 1 : 0);
        glProgramUniform3fv(resources_.wireProgram, wireUnselectedColorLocation_, 1,
                            glm::value_ptr(wireframeColor_));
        glProgramUniform3fv(resources_.wireProgram, wireSelectedColorLocation_, 1,
                            glm::value_ptr(wireframeSelectedColor_));
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        glBindVertexArray(resources_.wireVao);
        glDrawArrays(GL_LINES, 0, wireVertexCount_);
        glBindVertexArray(0);
        glDisable(GL_LINE_SMOOTH);
        glDisable(GL_BLEND);
    }
    glUseProgram(0);
    checkGl("Drawing globe");
}
