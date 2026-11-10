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

} // namespace

GlobeRenderer::Resources::~Resources() {
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &positionBuffer);
    glDeleteBuffers(1, &elevationBuffer);
    glDeleteBuffers(1, &selectionBuffer);
    glDeleteBuffers(1, &indexBuffer);
    glDeleteBuffers(1, &colorBuffer);
    glDeleteBuffers(1, &selectionColorBuffer);
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
    stopCountLocation_ = uniform(resources_.program, "uStopCount");
    selectionStopCountLocation_ = uniform(resources_.program, "uSelectionStopCount");
    selectionActiveLocation_ = uniform(resources_.program, "uSelectionActive");

    glCreateVertexArrays(1, &resources_.vao);
    glCreateBuffers(1, &resources_.positionBuffer);
    glCreateBuffers(1, &resources_.elevationBuffer);
    glCreateBuffers(1, &resources_.selectionBuffer);
    glCreateBuffers(1, &resources_.indexBuffer);
    glCreateBuffers(1, &resources_.colorBuffer);
    glCreateBuffers(1, &resources_.selectionColorBuffer);
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
    glNamedBufferData(resources_.positionBuffer, positionBytes, positions.data(), GL_STATIC_DRAW);
    glNamedBufferData(resources_.elevationBuffer, elevationBytes, elevations.data(), GL_DYNAMIC_DRAW);
    const auto selectionBytes = bufferSize(std::span<const float>(selectionWeights));
    glNamedBufferData(resources_.selectionBuffer, selectionBytes, selectionWeights.data(), GL_DYNAMIC_DRAW);
    glNamedBufferData(resources_.indexBuffer, indexBytes, indices.data(), GL_STATIC_DRAW);
    checkGl("Uploading globe mesh");
    indexCount_ = static_cast<GLsizei>(indices.size());
    vertexCount_ = vertices.size();
    selectionActive_ = selectionActive;
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

void GlobeRenderer::SetColorMap(const ColorMap& map) {
    UploadColorMap(map, resources_.colorBuffer, stopCountLocation_);
    hasColorMap_ = true;
}

void GlobeRenderer::SetSelectionColorMap(const ColorMap& map) {
    UploadColorMap(map, resources_.selectionColorBuffer, selectionStopCountLocation_);
    hasSelectionColorMap_ = true;
}

void GlobeRenderer::UploadColorMap(const ColorMap& map, GLuint buffer, GLint countLocation) {
    const auto stops = map.GetStops();
    if (stops.size() < 2) {
        throw std::invalid_argument("Colour map must contain at least two stops.");
    }
    if (stops.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max())) {
        throw std::invalid_argument("Colour map exceeds shader indexing limits.");
    }
    static_assert(sizeof(glm::vec4) == 4 * sizeof(float));
    const auto bytes = bufferSize(std::span<const glm::vec4>(stops));
    GLint64 maximumBytes = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maximumBytes);
    if (bytes > maximumBytes) {
        throw std::invalid_argument("Colour map exceeds this GPU's storage buffer limit.");
    }
    glNamedBufferData(buffer, bytes, stops.data(), GL_DYNAMIC_DRAW);
    glProgramUniform1i(resources_.program, countLocation, static_cast<GLint>(stops.size()));
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
    if (indexCount_ == 0 || !hasColorMap_) {
        throw std::logic_error("Upload a mesh and colour map before drawing the globe.");
    }
    if (selectionActive_ && !hasSelectionColorMap_) {
        throw std::logic_error("Upload a selection colour map before drawing an active selection.");
    }
    const float aspect = static_cast<float>(framebufferWidth) / framebufferHeight;
    const float horizontal = 1.15F * std::max(aspect, 1.0F) / zoom;
    const float vertical = 1.15F * std::max(1.0F / aspect, 1.0F) / zoom;
    const auto projection = glm::ortho(-horizontal, horizontal, -vertical, vertical, 0.1F, 10.0F);
    const auto view = glm::lookAt(glm::vec3(0.0F, -3.0F, 0.0F),
                                  glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, 1.0F));
    const auto matrix = projection * view * modelMatrix;
    glViewport(0, 0, framebufferWidth, framebufferHeight);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glUseProgram(resources_.program);
    glProgramUniformMatrix4fv(resources_.program, matrixLocation_, 1, GL_FALSE, glm::value_ptr(matrix));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, resources_.colorBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, resources_.selectionColorBuffer);
    glProgramUniform1i(resources_.program, selectionActiveLocation_, selectionActive_ ? 1 : 0);
    glBindVertexArray(resources_.vao);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glUseProgram(0);
    checkGl("Drawing globe");
}
