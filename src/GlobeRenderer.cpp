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
    glDeleteBuffers(1, &indexBuffer);
    glDeleteBuffers(1, &colorBuffer);
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
    lightLocation_ = uniform(resources_.program, "uLightDir");
    oceanCountLocation_ = uniform(resources_.program, "uOceanStopCount");
    landCountLocation_ = uniform(resources_.program, "uLandStopCount");

    glCreateVertexArrays(1, &resources_.vao);
    glCreateBuffers(1, &resources_.positionBuffer);
    glCreateBuffers(1, &resources_.elevationBuffer);
    glCreateBuffers(1, &resources_.indexBuffer);
    glCreateBuffers(1, &resources_.colorBuffer);
    glEnableVertexArrayAttrib(resources_.vao, 0);
    glVertexArrayAttribFormat(resources_.vao, 0, 3, GL_FLOAT, GL_FALSE,
                             0);
    glVertexArrayAttribBinding(resources_.vao, 0, 0);
    glEnableVertexArrayAttrib(resources_.vao, 1);
    glVertexArrayAttribFormat(resources_.vao, 1, 1, GL_FLOAT, GL_FALSE,
                             0);
    glVertexArrayAttribBinding(resources_.vao, 1, 1);
    glVertexArrayVertexBuffer(resources_.vao, 0, resources_.positionBuffer, 0, sizeof(glm::vec3));
    glVertexArrayVertexBuffer(resources_.vao, 1, resources_.elevationBuffer, 0, sizeof(float));
    glVertexArrayElementBuffer(resources_.vao, resources_.indexBuffer);
    checkGl("Creating globe resources");
}

void GlobeRenderer::UploadGrid(const SphericalGridSnapshot& snapshot) {
    UploadMesh(snapshot.GetVertices(), snapshot.GetTriangleIndices());
}

void GlobeRenderer::UploadMesh(std::span<const GridVertex> vertices,
                               std::span<const std::uint32_t> indices) {
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
    positions.reserve(vertices.size());
    elevations.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        positions.push_back(vertex.position);
        elevations.push_back(vertex.elevation);
    }
    const auto positionBytes = bufferSize(std::span<const glm::vec3>(positions));
    const auto elevationBytes = bufferSize(std::span<const float>(elevations));
    glNamedBufferData(resources_.positionBuffer, positionBytes, positions.data(), GL_STATIC_DRAW);
    glNamedBufferData(resources_.elevationBuffer, elevationBytes, elevations.data(), GL_DYNAMIC_DRAW);
    glNamedBufferData(resources_.indexBuffer, indexBytes, indices.data(), GL_STATIC_DRAW);
    checkGl("Uploading globe mesh");
    indexCount_ = static_cast<GLsizei>(indices.size());
    vertexCount_ = vertices.size();
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
    const auto ocean = map.GetOceanStops();
    const auto land = map.GetLandStops();
    if (ocean.size() < 2 || land.size() < 2) {
        throw std::invalid_argument("Colour map must contain both ocean and land stops.");
    }
    if (ocean.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max()) ||
        land.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max()) - ocean.size()) {
        throw std::invalid_argument("Colour map exceeds shader indexing limits.");
    }
    std::vector<glm::vec4> stops(ocean.begin(), ocean.end());
    stops.insert(stops.end(), land.begin(), land.end());
    static_assert(sizeof(glm::vec4) == 4 * sizeof(float));
    const auto bytes = bufferSize(std::span<const glm::vec4>(stops));
    GLint64 maximumBytes = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maximumBytes);
    if (bytes > maximumBytes) {
        throw std::invalid_argument("Colour map exceeds this GPU's storage buffer limit.");
    }
    glNamedBufferData(resources_.colorBuffer, bytes, stops.data(), GL_DYNAMIC_DRAW);
    glProgramUniform1i(resources_.program, oceanCountLocation_, static_cast<GLint>(ocean.size()));
    glProgramUniform1i(resources_.program, landCountLocation_, static_cast<GLint>(land.size()));
    checkGl("Uploading colour map");
    hasColorMap_ = true;
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
    glProgramUniform3f(resources_.program, lightLocation_, -0.3F, -1.0F, 0.7F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, resources_.colorBuffer);
    glBindVertexArray(resources_.vao);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glUseProgram(0);
    checkGl("Drawing globe");
}
