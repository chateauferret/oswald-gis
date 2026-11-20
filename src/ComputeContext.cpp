#include "ComputeContext.hpp"
#include "ColorMap.hpp"
#include "Filter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr std::size_t workgroupSize = 256;

void checkGlError(const char* operation) {
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        throw std::runtime_error(std::string(operation) + " failed with OpenGL error " +
                                 std::to_string(error) + ".");
    }
}

GLsizeiptr checkedBufferSize(std::size_t bytes) {
    if (bytes > static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max())) {
        throw std::length_error("Compute buffer exceeds OpenGL's addressable range.");
    }
    return static_cast<GLsizeiptr>(bytes);
}

template <typename T>
GLuint createBuffer(std::span<const T> data) {
    GLuint buffer = 0;
    glCreateBuffers(1, &buffer);
    if (buffer == 0) {
        throw std::runtime_error("OpenGL could not allocate a compute buffer.");
    }
    try {
        glNamedBufferStorage(buffer, checkedBufferSize(data.size_bytes()),
                             data.empty() ? nullptr : data.data(), 0);
        checkGlError("Compute buffer allocation");
    } catch (...) {
        glDeleteBuffers(1, &buffer);
        throw;
    }
    return buffer;
}

std::string shaderLog(GLuint shader) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1) {
        return {};
    }
    std::string log(static_cast<std::size_t>(length), '\0');
    GLsizei written = 0;
    glGetShaderInfoLog(shader, length, &written, log.data());
    log.resize(static_cast<std::size_t>(std::max<GLsizei>(0, written)));
    return log;
}

std::string programLog(GLuint program) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1) {
        return {};
    }
    std::string log(static_cast<std::size_t>(length), '\0');
    GLsizei written = 0;
    glGetProgramInfoLog(program, length, &written, log.data());
    log.resize(static_cast<std::size_t>(std::max<GLsizei>(0, written)));
    return log;
}

glm::dvec3 normalizedDirection(glm::dvec3 direction) {
    const double length = glm::length(direction);
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z) || !std::isfinite(length) || length <= 0.0) {
        throw std::invalid_argument("GPU selection directions must be finite and nonzero.");
    }
    return direction / length;
}

double polygonWinding(glm::dvec3 point, std::span<const glm::dvec3> polygon) {
    double winding = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[i];
        const auto b = polygon[(i + 1) % polygon.size()];
        const double numerator = glm::dot(point, glm::cross(a, b));
        const double denominator = glm::dot(a, b) -
            glm::dot(point, a) * glm::dot(point, b);
        winding += std::atan2(numerator, denominator);
    }
    return winding;
}

} // namespace

GpuMesh::GpuMesh(const SphericalGridSnapshot& snapshot) {
    const auto& vertices = snapshot.GetVertices();
    const auto& halfEdges = snapshot.GetHalfEdges();
    if (vertices.empty() || vertices.size() >= std::numeric_limits<std::uint32_t>::max() ||
        halfEdges.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("Grid topology exceeds the GPU mesh index range.");
    }

    std::vector<std::array<float, 4>> positions;
    positions.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        positions.push_back({vertex.position.x, vertex.position.y, vertex.position.z, 0.0F});
    }

    std::vector<std::uint32_t> offsets(vertices.size() + 1, 0);
    for (const auto& edge : halfEdges) {
        if (edge.originVertex < 0 ||
            static_cast<std::size_t>(edge.originVertex) >= vertices.size() ||
            edge.nextEdge < 0 ||
            static_cast<std::size_t>(edge.nextEdge) >= halfEdges.size()) {
            throw std::invalid_argument("Grid contains an invalid half-edge while building GPU topology.");
        }
        auto& count = offsets[static_cast<std::size_t>(edge.originVertex) + 1];
        if (count == std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("Grid vertex degree exceeds the GPU mesh index range.");
        }
        ++count;
    }
    for (std::size_t i = 1; i < offsets.size(); ++i) {
        if (offsets[i] > std::numeric_limits<std::uint32_t>::max() - offsets[i - 1]) {
            throw std::length_error("Grid topology exceeds the GPU mesh index range.");
        }
        offsets[i] += offsets[i - 1];
    }
    if (offsets.back() != halfEdges.size()) {
        throw std::invalid_argument("Grid topology has inconsistent half-edge counts.");
    }

    std::vector<std::uint32_t> neighbors(halfEdges.size());
    std::vector<float> distancesKm(halfEdges.size());
    std::vector<double> preciseDistancesKm(halfEdges.size());
    auto cursor = offsets;
    const double radiusKm = snapshot.GetOptions().planetRadius;
    for (const auto& edge : halfEdges) {
        const auto origin = static_cast<std::size_t>(edge.originVertex);
        const auto neighbor = static_cast<std::size_t>(
            halfEdges[static_cast<std::size_t>(edge.nextEdge)].originVertex);
        if (neighbor >= vertices.size()) {
            throw std::invalid_argument("Grid contains an invalid half-edge endpoint.");
        }
        const glm::dvec3 from(vertices[origin].position);
        const glm::dvec3 to(vertices[neighbor].position);
        const double fromLength = glm::length(from);
        const double toLength = glm::length(to);
        if (!std::isfinite(fromLength) || !std::isfinite(toLength) ||
            fromLength <= 0.0 || toLength <= 0.0) {
            throw std::invalid_argument("Grid contains a non-finite or zero-length vertex position.");
        }
        const double cosine = std::clamp(glm::dot(from / fromLength, to / toLength), -1.0, 1.0);
        const double distance = radiusKm * std::acos(cosine);
        if (!std::isfinite(distance) || distance > std::numeric_limits<float>::max()) {
            throw std::overflow_error("Grid edge distance cannot be represented on the GPU.");
        }
        const auto slot = cursor[origin]++;
        neighbors[slot] = static_cast<std::uint32_t>(neighbor);
        distancesKm[slot] = static_cast<float>(distance);
        preciseDistancesKm[slot] = distance;
    }

    std::array<GLuint, 5> buffers{};
    try {
        buffers[0] = createBuffer<std::array<float, 4>>(positions);
        buffers[1] = createBuffer<std::uint32_t>(offsets);
        buffers[2] = createBuffer<std::uint32_t>(neighbors);
        buffers[3] = createBuffer<float>(distancesKm);
        buffers[4] = createBuffer<double>(preciseDistancesKm);
    } catch (...) {
        glDeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
        throw;
    }
    positions_ = buffers[0];
    neighborOffsets_ = buffers[1];
    neighbors_ = buffers[2];
    edgeDistancesKm_ = buffers[3];
    preciseEdgeDistancesKm_ = buffers[4];
    vertexCount_ = vertices.size();
    directedEdgeCount_ = halfEdges.size();
}

GpuMesh::~GpuMesh() {
    Release();
}

GpuMesh::GpuMesh(GpuMesh&& source) noexcept {
    *this = std::move(source);
}

GpuMesh& GpuMesh::operator=(GpuMesh&& source) noexcept {
    if (this != &source) {
        Release();
        positions_ = std::exchange(source.positions_, 0);
        neighborOffsets_ = std::exchange(source.neighborOffsets_, 0);
        neighbors_ = std::exchange(source.neighbors_, 0);
        edgeDistancesKm_ = std::exchange(source.edgeDistancesKm_, 0);
        preciseEdgeDistancesKm_ = std::exchange(source.preciseEdgeDistancesKm_, 0);
        vertexCount_ = std::exchange(source.vertexCount_, 0);
        directedEdgeCount_ = std::exchange(source.directedEdgeCount_, 0);
    }
    return *this;
}

void GpuMesh::Release() noexcept {
    const std::array<GLuint, 5> buffers{
        positions_, neighborOffsets_, neighbors_, edgeDistancesKm_, preciseEdgeDistancesKm_};
    glDeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    positions_ = 0;
    neighborOffsets_ = 0;
    neighbors_ = 0;
    edgeDistancesKm_ = 0;
    preciseEdgeDistancesKm_ = 0;
    vertexCount_ = 0;
    directedEdgeCount_ = 0;
}

ComputeContext::ComputeContext() {
    if (glGetString(GL_VERSION) == nullptr || !GLAD_GL_VERSION_4_3) {
        throw std::runtime_error("GPU compute requires a current OpenGL 4.3 or newer context.");
    }
    glCreateBuffers(1, &errorBuffer_);
    if (errorBuffer_ == 0) {
        throw std::runtime_error("OpenGL could not allocate the compute error buffer.");
    }
    try {
        constexpr std::array<std::uint32_t, 2> clearValues{};
        glNamedBufferStorage(errorBuffer_, checkedBufferSize(sizeof(clearValues)),
                             clearValues.data(), GL_DYNAMIC_STORAGE_BIT);
        checkGlError("Compute error buffer allocation");
    } catch (...) {
        glDeleteBuffers(1, &errorBuffer_);
        errorBuffer_ = 0;
        throw;
    }
}

ComputeContext::~ComputeContext() {
    meshes_.clear();
    for (const auto& [key, buffer] : scratchBuffers_) {
        (void)key;
        glDeleteBuffers(1, &buffer);
    }
    for (const auto program : programs_) {
        glDeleteProgram(program);
    }
    glDeleteBuffers(1, &errorBuffer_);
}

GpuMesh& ComputeContext::MeshFor(std::shared_ptr<const SphericalGridSnapshot> snapshot) {
    if (!snapshot) {
        throw std::invalid_argument("Cannot build a GPU mesh for a null grid snapshot.");
    }
    for (auto entry = meshes_.begin(); entry != meshes_.end();) {
        if (entry->second.snapshot.expired()) {
            entry = meshes_.erase(entry);
        } else {
            ++entry;
        }
    }
    const auto found = meshes_.find(snapshot.get());
    if (found != meshes_.end()) {
        if (const auto cachedSnapshot = found->second.snapshot.lock();
            cachedSnapshot && cachedSnapshot == snapshot) {
            return found->second.mesh;
        }
        meshes_.erase(found);
    }
    auto [entry, inserted] = meshes_.try_emplace(snapshot.get());
    (void)inserted;
    entry->second.snapshot = snapshot;
    entry->second.mesh = GpuMesh(*snapshot);
    return entry->second.mesh;
}

GLuint ComputeContext::Scratch(std::size_t floatCount, std::size_t slot) {
    if (floatCount == 0 ||
        floatCount > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw std::invalid_argument("Compute scratch buffer size must be positive and representable.");
    }
    const auto key = std::pair{floatCount, slot};
    if (const auto found = scratchBuffers_.find(key); found != scratchBuffers_.end()) {
        return found->second;
    }
    GLuint buffer = 0;
    glCreateBuffers(1, &buffer);
    if (buffer == 0) {
        throw std::runtime_error("OpenGL could not allocate a compute scratch buffer.");
    }
    try {
        glNamedBufferStorage(buffer, checkedBufferSize(floatCount * sizeof(float)), nullptr,
                             GL_DYNAMIC_STORAGE_BIT);
        checkGlError("Compute scratch buffer allocation");
    } catch (...) {
        glDeleteBuffers(1, &buffer);
        throw;
    }
    scratchBuffers_.emplace(key, buffer);
    return buffer;
}

GLuint ComputeContext::LoadProgram(const std::filesystem::path& shaderPath) {
    const std::string cacheKey = shaderPath.lexically_normal().string();
    if (const auto found = programsByPath_.find(cacheKey); found != programsByPath_.end()) {
        return found->second;
    }
    std::ifstream input(shaderPath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open compute shader: " + shaderPath.string());
    }
    const std::string source(std::istreambuf_iterator<char>(input), {});
    if (!input.eof() && input.fail()) {
        throw std::runtime_error("Cannot read compute shader: " + shaderPath.string());
    }
    if (source.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max())) {
        throw std::length_error("Compute shader exceeds OpenGL's source length limit.");
    }

    return LoadProgram(source, cacheKey);
}

GLuint ComputeContext::LoadProgram(std::string_view source, std::string cacheKey) {
    if (const auto found = programsByPath_.find(cacheKey); found != programsByPath_.end()) {
        return found->second;
    }
    if (source.empty() || source.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max())) {
        throw std::length_error("Compute shader source is empty or exceeds OpenGL's source length limit.");
    }
    const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    if (shader == 0) {
        throw std::runtime_error("OpenGL could not create a compute shader.");
    }
    const std::string shaderSource(source);
    const char* text = shaderSource.c_str();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        const auto log = shaderLog(shader);
        glDeleteShader(shader);
        throw std::runtime_error("Compute shader compilation failed: " + log);
    }

    const GLuint program = glCreateProgram();
    if (program == 0) {
        glDeleteShader(shader);
        throw std::runtime_error("OpenGL could not create a compute program.");
    }
    glAttachShader(program, shader);
    glLinkProgram(program);
    glDeleteShader(shader);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        const auto log = programLog(program);
        glDeleteProgram(program);
        throw std::runtime_error("Compute program link failed: " + log);
    }
    try {
        programs_.push_back(program);
        programsByPath_.emplace(cacheKey, program);
    } catch (...) {
        if (!programs_.empty() && programs_.back() == program) {
            programs_.pop_back();
        }
        glDeleteProgram(program);
        throw;
    }
    return program;
}

std::vector<float> ComputeContext::EvaluateBrushScript(
    std::string_view gpuScript, std::span<const GpuScriptInput> inputs,
    std::span<const GpuScriptParameter> parameters, std::uint64_t seed,
    std::span<const GpuScriptTerrainVertex> vertices,
    std::span<const float> elevations, double planetRadiusKm) {
    if (gpuScript.empty() || gpuScript.size() > 64 * 1024) {
        throw std::invalid_argument("GPU brush body must contain at most 64 KiB of GLSL.");
    }
    if (inputs.empty()) {
        return {};
    }
    if (inputs.size() > std::numeric_limits<GLuint>::max() ||
        parameters.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max()) ||
        vertices.size() != elevations.size() ||
        vertices.size() > std::numeric_limits<GLuint>::max() ||
        !std::isfinite(planetRadiusKm) || planetRadiusKm <= 0.0) {
        throw std::length_error("GPU brush input exceeds the OpenGL index range.");
    }
    for (const auto& parameter : parameters) {
        if (parameter.name.empty() ||
            !(std::isalpha(static_cast<unsigned char>(parameter.name.front())) ||
              parameter.name.front() == '_') ||
            !std::all_of(parameter.name.begin() + 1, parameter.name.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '_';
            }) || !std::isfinite(parameter.value)) {
            throw std::invalid_argument("GPU brush parameters must have valid names and finite values.");
        }
    }

    std::string source = R"(#version 450 core
layout(local_size_x = 256) in;
struct BrushInput {
    vec4 positionAndLatitude;
    vec4 terrain;
    vec4 brush;
    vec4 timing;
    uint vertexIndex;
    uint padding0;
    uint padding1;
    uint padding2;
};
layout(std430, binding = 0) readonly buffer Inputs { BrushInput inputs[]; };
layout(std430, binding = 1) writeonly buffer Results { float results[]; };
layout(std430, binding = 2) buffer Error { uint code; uint index; };
layout(std430, binding = 4) readonly buffer TerrainVertices { vec4 terrainPositions[]; };
layout(std430, binding = 5) readonly buffer TerrainElevations { float terrainElevations[]; };
uniform uint uInputCount;
uniform uint uTerrainVertexCount;
uniform uint uNoiseSeed;
uniform float uPlanetRadiusKm;
layout(std430, binding = 3) readonly buffer GpuParameters { float gpuParameters[]; };
float x;
float y;
float z;
float lat;
float lon;
float elevation;
float weight;
float distance;
float radius;
float lower;
float upper;
float elapsed;
float rate;
uint currentVertexIndex;
)";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        source += "float param_" + parameters[i].name + ";\n";
    }
    source += R"(
uint noiseHash(ivec3 cell) {
    uint value = uint(cell.x) * 0x8da6b343u ^ uint(cell.y) * 0xd8163841u ^
                 uint(cell.z) * 0xcb1ab31fu ^ uNoiseSeed;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}
float latticeNoise(ivec3 cell) {
    return float(noiseHash(cell)) * (2.0 / 4294967295.0) - 1.0;
}
float valuenoise(vec3 point) {
    ivec3 cell = ivec3(floor(point));
    vec3 f = fract(point);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(latticeNoise(cell + ivec3(0,0,0)),
                  latticeNoise(cell + ivec3(1,0,0)), f.x);
    float b = mix(latticeNoise(cell + ivec3(0,1,0)),
                  latticeNoise(cell + ivec3(1,1,0)), f.x);
    float c = mix(latticeNoise(cell + ivec3(0,0,1)),
                  latticeNoise(cell + ivec3(1,0,1)), f.x);
    float d = mix(latticeNoise(cell + ivec3(0,1,1)),
                  latticeNoise(cell + ivec3(1,1,1)), f.x);
    return mix(mix(a, b, f.y), mix(c, d, f.y), f.z);
}
float valuenoise(float px, float py, float pz) {
    return valuenoise(vec3(px, py, pz));
}
vec3 noiseGradient(ivec3 cell) {
    uint h = noiseHash(cell) % 12u;
    if (h == 0u) return vec3(1,1,0);
    if (h == 1u) return vec3(-1,1,0);
    if (h == 2u) return vec3(1,-1,0);
    if (h == 3u) return vec3(-1,-1,0);
    if (h == 4u) return vec3(1,0,1);
    if (h == 5u) return vec3(-1,0,1);
    if (h == 6u) return vec3(1,0,-1);
    if (h == 7u) return vec3(-1,0,-1);
    if (h == 8u) return vec3(0,1,1);
    if (h == 9u) return vec3(0,-1,1);
    if (h == 10u) return vec3(0,1,-1);
    return vec3(0,-1,-1);
}
float simplex(vec3 point) {
    const float F3 = 1.0 / 3.0;
    const float G3 = 1.0 / 6.0;
    ivec3 cell = ivec3(floor(point + dot(point, vec3(F3))));
    vec3 p0 = point - (vec3(cell) - dot(vec3(cell), vec3(G3)));
    ivec3 i1;
    ivec3 i2;
    if (p0.x >= p0.y) {
        if (p0.y >= p0.z) { i1 = ivec3(1,0,0); i2 = ivec3(1,1,0); }
        else if (p0.x >= p0.z) { i1 = ivec3(1,0,0); i2 = ivec3(1,0,1); }
        else { i1 = ivec3(0,0,1); i2 = ivec3(1,0,1); }
    } else {
        if (p0.y < p0.z) { i1 = ivec3(0,0,1); i2 = ivec3(0,1,1); }
        else if (p0.x < p0.z) { i1 = ivec3(0,1,0); i2 = ivec3(0,1,1); }
        else { i1 = ivec3(0,1,0); i2 = ivec3(1,1,0); }
    }
    vec3 p1 = p0 - vec3(i1) + G3;
    vec3 p2 = p0 - vec3(i2) + 2.0 * G3;
    vec3 p3 = p0 - 1.0 + 3.0 * G3;
    float n0 = 0.0, n1 = 0.0, n2 = 0.0, n3 = 0.0;
    float t0 = 0.6 - dot(p0,p0), t1 = 0.6 - dot(p1,p1);
    float t2 = 0.6 - dot(p2,p2), t3 = 0.6 - dot(p3,p3);
    if (t0 > 0.0) { t0 *= t0; n0 = t0*t0*dot(noiseGradient(cell),p0); }
    if (t1 > 0.0) { t1 *= t1; n1 = t1*t1*dot(noiseGradient(cell+i1),p1); }
    if (t2 > 0.0) { t2 *= t2; n2 = t2*t2*dot(noiseGradient(cell+i2),p2); }
    if (t3 > 0.0) { t3 *= t3; n3 = t3*t3*dot(noiseGradient(cell+1),p3); }
    return clamp(32.0 * (n0+n1+n2+n3), -1.0, 1.0);
}
float simplex(float px, float py, float pz) { return simplex(vec3(px,py,pz)); }
float fractalNoise(vec3 point, float frequency, float lacunarity,
                   float persistence, int octaves, bool useValue) {
    if (octaves < 1 || octaves > 16 || isnan(frequency) || isinf(frequency) ||
        frequency < 0.0 || isnan(lacunarity) || isinf(lacunarity) || lacunarity < 0.0 ||
        isnan(persistence) || isinf(persistence) || persistence < 0.0) return 0.0;
    float total = 0.0, norm = 0.0, amplitude = 1.0;
    point *= frequency;
    for (int octave = 0; octave < 16; ++octave) {
        if (octave >= octaves) break;
        total += (useValue ? valuenoise(point) : simplex(point)) * amplitude;
        norm += amplitude;
        point *= lacunarity;
        amplitude *= persistence;
    }
    return norm > 0.0 ? clamp(total / norm, -1.0, 1.0) : 0.0;
}
float fbm(float px, float py, float pz, float frequency, float lacunarity,
          float persistence, int octaves) {
    return fractalNoise(vec3(px,py,pz),frequency,lacunarity,persistence,octaves,false);
}
float valuefbm(float px, float py, float pz, float frequency, float lacunarity,
               float persistence, int octaves) {
    return fractalNoise(vec3(px,py,pz),frequency,lacunarity,persistence,octaves,true);
}
float billow(float px, float py, float pz, float frequency, float lacunarity,
             float persistence, int octaves) {
    return 2.0 * abs(fbm(px,py,pz,frequency,lacunarity,persistence,octaves)) - 1.0;
}
float ridged(float px, float py, float pz, float frequency, float lacunarity,
             float persistence, int octaves) {
    return 1.0 - 2.0 * abs(fbm(px,py,pz,frequency,lacunarity,persistence,octaves));
}
float spectral(float px, float py, float pz, float frequency,
               float exponent, float resolution) {
    if (resolution < 8.0 || resolution > 128.0 || isnan(resolution) ||
        isinf(resolution) || exponent < -4.0 || exponent > 8.0 ||
        isnan(exponent) || isinf(exponent)) return 0.0;
    int octaves = clamp(int(round(log2(resolution))), 1, 16);
    return fractalNoise(vec3(px,py,pz),frequency,2.0,pow(2.0,-exponent),octaves,true);
}
vec3 warpPoint(vec3 point, float frequency, float strength, float lacunarity,
               float persistence, int octaves) {
    vec3 displacement = vec3(
        fractalNoise(point + vec3(19.1, 7.7, 3.4),frequency,lacunarity,persistence,octaves,false),
        fractalNoise(point + vec3(5.2, 31.7, 11.3),frequency,lacunarity,persistence,octaves,false),
        fractalNoise(point + vec3(13.8, 2.9, 27.1),frequency,lacunarity,persistence,octaves,false));
    return point + displacement * strength;
}
float warp_x(float px, float py, float pz, float frequency, float strength,
             float lacunarity, float persistence, int octaves) {
    return warpPoint(vec3(px,py,pz),frequency,strength,lacunarity,persistence,octaves).x;
}
float warp_y(float px, float py, float pz, float frequency, float strength,
             float lacunarity, float persistence, int octaves) {
    return warpPoint(vec3(px,py,pz),frequency,strength,lacunarity,persistence,octaves).y;
}
float warp_z(float px, float py, float pz, float frequency, float strength,
             float lacunarity, float persistence, int octaves) {
    return warpPoint(vec3(px,py,pz),frequency,strength,lacunarity,persistence,octaves).z;
}
float gaussian(float range, float sigma, float unit) {
    if (!(range > 0.0) || !(sigma > 0.0) || (unit != 0.0 && unit != 1.0) ||
        isnan(range) || isinf(range) || isnan(sigma) || isinf(sigma)) {
        return uintBitsToFloat(0x7fc00000u);
    }
    vec3 origin = normalize(terrainPositions[currentVertexIndex].xyz);
    float planetRadius = unit == 0.0 ? uPlanetRadiusKm : uPlanetRadiusKm / 1.609344;
    float weightedSum = 0.0;
    float weightSum = 0.0;
    for (uint vertex = 0u; vertex < uTerrainVertexCount; ++vertex) {
        vec3 point = normalize(terrainPositions[vertex].xyz);
        float angle = atan(length(cross(origin, point)), clamp(dot(origin, point), -1.0, 1.0));
        float distance = planetRadius * angle;
        if (distance <= range) {
            float ratio = distance / sigma;
            float sampleWeight = exp(-0.5 * ratio * ratio);
            weightedSum += terrainElevations[vertex] * sampleWeight;
            weightSum += sampleWeight;
        }
    }
    return weightSum > 0.0 ? weightedSum / weightSum : uintBitsToFloat(0x7fc00000u);
}
float evaluateGpuScript() {
)";
    source.append(gpuScript);
    source += R"(
}
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uInputCount) return;
    BrushInput sampleData = inputs[i];
    x = sampleData.positionAndLatitude.x;
    y = sampleData.positionAndLatitude.y;
    z = sampleData.positionAndLatitude.z;
    lat = sampleData.positionAndLatitude.w;
    lon = sampleData.terrain.x;
    elevation = sampleData.terrain.y;
    weight = sampleData.terrain.z;
    distance = sampleData.terrain.w;
    radius = sampleData.brush.x;
    lower = sampleData.brush.y;
    upper = sampleData.brush.z;
    elapsed = sampleData.brush.w;
    rate = sampleData.timing.x;
    currentVertexIndex = sampleData.vertexIndex;
)";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        source += "    param_" + parameters[i].name + " = gpuParameters[" +
                  std::to_string(i) + "];\n";
    }
    source += R"(    float value = evaluateGpuScript();
    if (isnan(value) || isinf(value)) {
        if (atomicCompSwap(code, 0u, 1u) == 0u) index = sampleData.vertexIndex;
        return;
    }
    results[i] = value;
}
)";

    std::string cacheKey = "gpu-brush:";
    cacheKey += source;
    const GLuint program = LoadProgram(source, std::move(cacheKey));
    std::vector<float> parameterValues;
    parameterValues.reserve(std::max<std::size_t>(parameters.size(), 1));
    for (const auto& parameter : parameters) {
        parameterValues.push_back(parameter.value);
    }
    if (parameterValues.empty()) {
        parameterValues.push_back(0.0F);
    }
    GLuint inputBuffer = 0;
    GLuint verticesBuffer = 0;
    GLuint elevationsBuffer = 0;
    GLuint parameterBuffer = 0;
    try {
        inputBuffer = createBuffer(inputs);
        verticesBuffer = createBuffer(vertices);
        elevationsBuffer = createBuffer(elevations);
        parameterBuffer = createBuffer(std::span<const float>(parameterValues));
        const GLuint outputBuffer = Scratch(inputs.size(), 40);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, inputBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, outputBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, parameterBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, verticesBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, elevationsBuffer);
        glProgramUniform1ui(program, glGetUniformLocation(program, "uInputCount"),
                            static_cast<GLuint>(inputs.size()));
        glProgramUniform1ui(program, glGetUniformLocation(program, "uTerrainVertexCount"),
                            static_cast<GLuint>(vertices.size()));
        glProgramUniform1f(program, glGetUniformLocation(program, "uPlanetRadiusKm"),
                           static_cast<float>(planetRadiusKm));
        const auto seed32 = static_cast<std::uint32_t>(seed) ^
                            static_cast<std::uint32_t>(seed >> 32);
        glProgramUniform1ui(program, glGetUniformLocation(program, "uNoiseSeed"), seed32);
        ClearErrors();
        Dispatch1D(program, inputs.size());
        if (const auto error = CheckErrors()) {
            throw std::invalid_argument("GPU brush body returned a non-finite value at vertex " +
                                        std::to_string(error->index) + ".");
        }
        auto result = Readback(outputBuffer, inputs.size());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0);
        if (parameterBuffer != 0) glDeleteBuffers(1, &parameterBuffer);
        if (elevationsBuffer != 0) glDeleteBuffers(1, &elevationsBuffer);
        if (verticesBuffer != 0) glDeleteBuffers(1, &verticesBuffer);
        if (inputBuffer != 0) glDeleteBuffers(1, &inputBuffer);
        return result;
    } catch (...) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0);
        if (parameterBuffer != 0) {
            glDeleteBuffers(1, &parameterBuffer);
        }
        glDeleteBuffers(1, &elevationsBuffer);
        glDeleteBuffers(1, &verticesBuffer);
        glDeleteBuffers(1, &inputBuffer);
        throw;
    }
}

void ComputeContext::Dispatch1D(GLuint program, std::size_t count) {
    if (program == 0 || count == 0) {
        throw std::invalid_argument("Compute dispatch requires a valid program and nonzero count.");
    }
    GLint maxGroups = 0;
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &maxGroups);
    const std::size_t groups = 1 + (count - 1) / workgroupSize;
    if (maxGroups <= 0 || groups > static_cast<std::size_t>(maxGroups)) {
        throw std::length_error("Compute dispatch exceeds the GPU workgroup count limit.");
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, errorBufferBinding, errorBuffer_);
    glUseProgram(program);
    glDispatchCompute(static_cast<GLuint>(groups), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    checkGlError("Compute dispatch");
}

std::vector<float> ComputeContext::Readback(GLuint buffer, std::size_t floatCount) const {
    if (buffer == 0 || floatCount > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw std::invalid_argument("Compute readback requires a valid buffer and representable size.");
    }
    const auto bytes = floatCount * sizeof(float);
    GLint64 bufferBytes = 0;
    glGetNamedBufferParameteri64v(buffer, GL_BUFFER_SIZE, &bufferBytes);
    if (bufferBytes < 0 || bytes > static_cast<std::uint64_t>(bufferBytes)) {
        throw std::out_of_range("Compute readback exceeds the buffer's allocated size.");
    }
    std::vector<float> result(floatCount);
    if (floatCount != 0) {
        glGetNamedBufferSubData(buffer, 0, checkedBufferSize(bytes), result.data());
        checkGlError("Compute buffer readback");
    }
    return result;
}

void ComputeContext::ClearErrors() {
    constexpr std::array<std::uint32_t, 2> clearValues{};
    glNamedBufferSubData(errorBuffer_, 0, checkedBufferSize(sizeof(clearValues)),
                         clearValues.data());
    checkGlError("Compute error flag reset");
}

std::optional<ComputeError> ComputeContext::CheckErrors() {
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    std::array<std::uint32_t, 2> values{};
    glGetNamedBufferSubData(errorBuffer_, 0, checkedBufferSize(sizeof(values)), values.data());
    checkGlError("Compute error flag readback");
    if (values[0] == 0) {
        return std::nullopt;
    }
    ClearErrors();
    if (values[0] < static_cast<std::uint32_t>(ComputeError::Code::NonFinite) ||
        values[0] > static_cast<std::uint32_t>(ComputeError::Code::OutOfRange)) {
        throw std::runtime_error("GPU compute returned an unknown error code.");
    }
    return ComputeError{static_cast<ComputeError::Code>(values[0]), values[1]};
}

std::vector<float> ComputeContext::ApplyFilter(
    const Filter& filter, std::shared_ptr<const SphericalGridSnapshot> snapshot,
    std::span<const float> elevations, const std::filesystem::path& shaderDirectory) {
    if (!snapshot) {
        throw std::invalid_argument("Cannot run a GPU filter without a grid snapshot.");
    }
    if (elevations.size() != snapshot->GetVertices().size()) {
        throw std::invalid_argument("GPU filter elevation count must match the grid vertex count.");
    }
    if (elevations.empty()) {
        return {};
    }
    if (const auto* glacial = dynamic_cast<const GlacialErosionFilter*>(&filter)) {
        const auto& settings = glacial->GetSettings();
        const auto& vertices = snapshot->GetVertices();
        const std::size_t count = elevations.size();
        if (count > std::numeric_limits<std::size_t>::max() / 8) {
            throw std::length_error("Glacial compute scratch dimensions are not representable.");
        }

        bool hasSink = false;
        float minimumElevation = std::numeric_limits<float>::max();
        std::vector<float> weights;
        weights.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const float weight = snapshot->HasSelection()
                ? vertices[i].selectionWeight : 1.0F;
            if (!std::isfinite(weight) || weight < 0.0F || weight > 1.0F) {
                throw std::invalid_argument(
                    "GPU glacial selection weights must be finite and in [0, 1].");
            }
            weights.push_back(weight);
            hasSink |= elevations[i] <= 0.0F ||
                (snapshot->HasSelection() && weight == 0.0F);
            minimumElevation = std::min(minimumElevation, elevations[i]);
        }

        std::vector<double> thickness(count, settings.initialIceThickness);
        const GLuint sourceBuffer = Scratch(count, 0);
        const GLuint weightBuffer = Scratch(count, 1);
        const GLuint outputBuffer = Scratch(count, 2);
        const GLuint thicknessBuffer = Scratch(count * 2, 3);
        const GLuint surfaceBuffer = Scratch(count * 2, 4);
        const GLuint rateBuffer = Scratch(count * 2, 5);
        const GLuint flowBuffer = Scratch(count * 8, 6);
        const GLuint depthBuffer = Scratch(count * 2, 7);
        const GLuint weightSumBuffer = Scratch(count * 2, 8);
        const GLuint erosionBuffer = Scratch(count * 2, 9);
        glNamedBufferSubData(sourceBuffer, 0, checkedBufferSize(elevations.size_bytes()),
                             elevations.data());
        glNamedBufferSubData(weightBuffer, 0,
                             checkedBufferSize(weights.size() * sizeof(float)), weights.data());
        glNamedBufferSubData(thicknessBuffer, 0,
                             checkedBufferSize(thickness.size() * sizeof(double)),
                             thickness.data());
        checkGlError("GPU glacial input upload");

        auto& mesh = MeshFor(snapshot);
        const GLuint program = LoadProgram(shaderDirectory / "glacial.comp");
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sourceBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, weightBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, outputBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, mesh.Positions());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, mesh.NeighborOffsets());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, mesh.Neighbors());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, mesh.PreciseEdgeDistancesKm());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, thicknessBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, surfaceBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, rateBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, flowBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, depthBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 13, weightSumBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, erosionBuffer);

        const auto setDouble = [&](const char* name, double value) {
            if (!std::isfinite(value)) {
                throw std::overflow_error(
                    std::string("GPU glacial parameter is not representable: ") + name);
            }
            glProgramUniform1d(program, glGetUniformLocation(program, name), value);
        };
        glProgramUniform1ui(program, glGetUniformLocation(program, "uVertexCount"),
                            static_cast<GLuint>(count));
        glProgramUniform1i(program, glGetUniformLocation(program, "uHasSelection"),
                           snapshot->HasSelection() ? 1 : 0);
        glProgramUniform1i(program, glGetUniformLocation(program, "uHasSink"),
                           hasSink ? 1 : 0);
        setDouble("uMinimumElevation", minimumElevation);
        setDouble("uEquilibriumLineAltitude", settings.equilibriumLineAltitude);
        setDouble("uElaLatitudeGradient", settings.elaLatitudeGradient);
        setDouble("uAccumulationRate", settings.accumulationRate);
        setDouble("uAblationRate", settings.ablationRate);
        setDouble("uDurationYears", settings.durationYears);
        setDouble("uGlacialErodibility", settings.glacialErodibility);
        setDouble("uIceViscosityFactor", settings.iceViscosityFactor);
        setDouble("uThicknessExponent", settings.thicknessExponent);
        setDouble("uVelocityExponent", settings.velocityExponent);
        setDouble("uTroughHalfWidth", settings.troughHalfWidth);
        ClearErrors();
        for (GLint pass = 0; pass <= 4; ++pass) {
            glProgramUniform1i(program, glGetUniformLocation(program, "uPass"), pass);
            Dispatch1D(program, count);
            if (pass == 0) {
                if (const auto error = CheckErrors()) {
                    if (error->code == ComputeError::Code::NonFinite ||
                        error->code == ComputeError::Code::OutOfRange) {
                        throw std::invalid_argument(
                            "GPU glacial erosion rejected invalid input at vertex " +
                            std::to_string(error->index) + ".");
                    }
                    throw std::overflow_error(
                        "GPU glacial mass balance overflowed at vertex " +
                        std::to_string(error->index) + ".");
                }
            }
        }
        if (const auto error = CheckErrors()) {
            if (error->code == ComputeError::Code::NonFinite ||
                error->code == ComputeError::Code::OutOfRange) {
                throw std::invalid_argument(
                    "GPU glacial erosion rejected invalid input at vertex " +
                    std::to_string(error->index) + ".");
            }
            throw std::overflow_error(
                "GPU glacial erosion produced an invalid intermediate at vertex " +
                std::to_string(error->index) + ".");
        }
        auto result = Readback(outputBuffer, count);
        for (GLuint binding = 0; binding <= 14; ++binding) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, 0);
        }
        return result;
    }

    enum class Operation : GLint { Fill, Precipiton, Thermal, Grooving, Scour, Noise };
    Operation operation{};
    float parameter0 = 0.0F;
    float parameter1 = 0.0F;
    float parameter2 = 0.0F;
    float parameter3 = 0.0F;
    float parameter4 = 0.0F;
    glm::vec3 direction(0.0F);
    std::uint32_t randomSeed = 0;
    if (const auto* fill = dynamic_cast<const FillFilter*>(&filter)) {
        operation = Operation::Fill;
        parameter0 = fill->GetSettings().value;
        parameter1 = static_cast<float>(fill->GetSettings().operation);
    } else if (const auto* precipiton = dynamic_cast<const PrecipitonFilter*>(&filter)) {
        operation = Operation::Precipiton;
        parameter0 = static_cast<float>(precipiton->GetSettings().diffusionCoefficient);
    } else if (const auto* thermal = dynamic_cast<const ThermalWeatheringFilter*>(&filter)) {
        operation = Operation::Thermal;
        parameter0 = static_cast<float>(std::tan(
            thermal->GetSettings().angleOfReposeDegrees * (std::acos(-1.0) / 180.0)));
    } else if (const auto* grooving = dynamic_cast<const CratonicGroovingFilter*>(&filter)) {
        const auto& settings = grooving->GetSettings();
        operation = Operation::Grooving;
        direction = glm::vec3(glm::normalize(settings.iceFlowDirection));
        parameter0 = static_cast<float>(settings.erodibility);
        parameter1 = static_cast<float>(settings.grooveCoefficient);
        parameter2 = static_cast<float>(settings.anisotropy);
    } else if (const auto* scour = dynamic_cast<const CratonicScourFilter*>(&filter)) {
        const auto& settings = scour->GetSettings();
        operation = Operation::Scour;
        direction = glm::vec3(glm::normalize(settings.iceFlowDirection));
        parameter0 = static_cast<float>(settings.erodibility);
        parameter1 = static_cast<float>(settings.stossAbrasionCoefficient);
        parameter2 = static_cast<float>(settings.pluckingRate);
        parameter3 = static_cast<float>(settings.cratonElevation);
        parameter4 = static_cast<float>(settings.planationAttenuation);
    } else if (const auto* noise = dynamic_cast<const AbsoluteMagnitudeNoiseFilter*>(&filter)) {
        operation = Operation::Noise;
        parameter0 = static_cast<float>(noise->GetAmplitudeFeet() * 0.3048);
        std::random_device randomDevice;
        randomSeed = randomDevice();
    } else {
        throw std::invalid_argument("This filter does not have a GPU implementation in Phase G2.");
    }
    const auto& vertices = snapshot->GetVertices();
    const bool hasSelection = snapshot->HasSelection();
    bool hasSink = false;
    float minimumElevation = std::numeric_limits<float>::max();
    std::vector<float> weights;
    weights.reserve(vertices.size());
    for (std::size_t i = 0; i < elevations.size(); ++i) {
        const float elevation = elevations[i];
        const float weight = hasSelection ? vertices[i].selectionWeight : 1.0F;
        if (!std::isfinite(weight) || weight < 0.0F || weight > 1.0F) {
            throw std::invalid_argument("GPU filter selection weights must be finite and in [0, 1].");
        }
        weights.push_back(weight);
        hasSink |= elevation <= 0.0F || (hasSelection && weight == 0.0F);
        minimumElevation = std::min(minimumElevation, elevation);
    }
    const GLuint program = LoadProgram(shaderDirectory / "terrain.comp");
    const auto setFloat = [&](const char* name, float value) {
        if (!std::isfinite(value)) {
            throw std::overflow_error(std::string("GPU filter parameter is not representable: ") + name);
        }
        glProgramUniform1f(program, glGetUniformLocation(program, name), value);
    };
    const GLuint inputBuffer = Scratch(elevations.size(), 0);
    const GLuint weightBuffer = Scratch(weights.size(), 1);
    const GLuint outputBuffer = Scratch(elevations.size(), 2);
    glNamedBufferSubData(inputBuffer, 0, checkedBufferSize(elevations.size_bytes()),
                         elevations.data());
    glNamedBufferSubData(weightBuffer, 0, checkedBufferSize(weights.size() * sizeof(float)),
                         weights.data());
    checkGlError("GPU filter input upload");
    auto& mesh = MeshFor(snapshot);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, inputBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, weightBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, outputBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, mesh.Positions());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, mesh.NeighborOffsets());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, mesh.Neighbors());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, mesh.EdgeDistancesKm());
    glProgramUniform1i(program, glGetUniformLocation(program, "uOperation"),
                       static_cast<GLint>(operation));
    glProgramUniform1ui(program, glGetUniformLocation(program, "uVertexCount"),
                        static_cast<GLuint>(elevations.size()));
    glProgramUniform1i(program, glGetUniformLocation(program, "uHasSelection"),
                       hasSelection ? 1 : 0);
    glProgramUniform1i(program, glGetUniformLocation(program, "uHasSink"),
                       hasSink ? 1 : 0);
    setFloat("uMinimumElevation", minimumElevation);
    setFloat("uParameter0", parameter0);
    setFloat("uParameter1", parameter1);
    setFloat("uParameter2", parameter2);
    setFloat("uParameter3", parameter3);
    setFloat("uParameter4", parameter4);
    glProgramUniform3f(program, glGetUniformLocation(program, "uFlowDirection"),
                       direction.x, direction.y, direction.z);
    glProgramUniform1ui(program, glGetUniformLocation(program, "uRandomSeed"), randomSeed);
    ClearErrors();
    Dispatch1D(program, elevations.size());
    if (const auto error = CheckErrors()) {
        if (error->code == ComputeError::Code::NonFinite ||
            error->code == ComputeError::Code::OutOfRange) {
            throw std::invalid_argument("GPU filter rejected invalid elevation input at vertex " +
                                        std::to_string(error->index) + ".");
        }
        throw std::overflow_error("GPU filter produced a non-finite result at vertex " +
                                  std::to_string(error->index) + ".");
    }
    auto result = Readback(outputBuffer, elevations.size());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, 0);
    return result;
}

std::vector<float> ComputeContext::ApplySelection(
    SelectionComputeOperation operation,
    std::shared_ptr<const SphericalGridSnapshot> snapshot,
    std::span<const float> currentWeights, double lowerBound, double upperBound,
    double featherPercent, const std::filesystem::path& shaderDirectory) {
    if (!snapshot || currentWeights.size() != snapshot->GetVertices().size()) {
        throw std::invalid_argument("GPU selection weights must match a valid grid snapshot.");
    }
    if (operation == SelectionComputeOperation::ByValue &&
        (!std::isfinite(lowerBound) || !std::isfinite(upperBound) ||
         lowerBound < -32767.0 || upperBound > 32767.0 || lowerBound > upperBound ||
         !std::isfinite(featherPercent) || featherPercent < 0.0 || featherPercent > 100.0)) {
        throw std::invalid_argument("GPU value selection bounds or feather are invalid.");
    }
    if (currentWeights.empty()) {
        return {};
    }
    std::vector<float> elevations;
    elevations.reserve(snapshot->GetVertices().size());
    for (const auto& vertex : snapshot->GetVertices()) {
        elevations.push_back(vertex.elevation);
    }
    const GLuint program = LoadProgram(shaderDirectory / "selection.comp");
    const GLuint elevationBuffer = Scratch(elevations.size(), 10);
    const GLuint inputBuffer = Scratch(currentWeights.size(), 11);
    const GLuint outputBuffer = Scratch(currentWeights.size(), 12);
    glNamedBufferSubData(elevationBuffer, 0, checkedBufferSize(elevations.size() * sizeof(float)),
                         elevations.data());
    glNamedBufferSubData(inputBuffer, 0, checkedBufferSize(currentWeights.size_bytes()),
                         currentWeights.data());
    checkGlError("GPU selection input upload");
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, elevationBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, inputBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, outputBuffer);
    glProgramUniform1i(program, glGetUniformLocation(program, "uOperation"),
                       static_cast<GLint>(operation));
    glProgramUniform1ui(program, glGetUniformLocation(program, "uVertexCount"),
                        static_cast<GLuint>(currentWeights.size()));
    glProgramUniform1i(program, glGetUniformLocation(program, "uHasSelection"),
                       snapshot->HasSelection() ? 1 : 0);
    const float lower = static_cast<float>(lowerBound);
    const float upper = static_cast<float>(upperBound);
    const float feather = static_cast<float>(featherPercent);
    if (!std::isfinite(lower) || !std::isfinite(upper) || !std::isfinite(feather)) {
        throw std::overflow_error("GPU selection parameters cannot be represented in FP32.");
    }
    glProgramUniform3f(program, glGetUniformLocation(program, "uBounds"),
                       lower, upper, feather);
    Dispatch1D(program, currentWeights.size());
    auto result = Readback(outputBuffer, currentWeights.size());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
    return result;
}

std::vector<float> ComputeContext::ApplyRadiusSelection(
    std::shared_ptr<const SphericalGridSnapshot> snapshot,
    std::span<const float> currentWeights, glm::dvec3 center, double radius,
    double falloffPercent, double planetRadius,
    const std::filesystem::path& shaderDirectory) {
    if (!snapshot || currentWeights.size() != snapshot->GetVertices().size()) {
        throw std::invalid_argument("GPU radius selection weights must match a valid grid snapshot.");
    }
    if (!std::isfinite(radius) || radius <= 0.0 ||
        !std::isfinite(falloffPercent) || falloffPercent < 0.0 ||
        falloffPercent > 100.0 || !std::isfinite(planetRadius) || planetRadius <= 0.0) {
        throw std::invalid_argument("GPU radius selection radius or falloff is invalid.");
    }
    center = normalizedDirection(center);
    if (currentWeights.empty()) {
        return {};
    }
    std::vector<float> weights(currentWeights.begin(), currentWeights.end());
    const GLuint program = LoadProgram(shaderDirectory / "selection.comp");
    const GLuint inputBuffer = Scratch(weights.size(), 13);
    const GLuint outputBuffer = Scratch(weights.size(), 14);
    glNamedBufferSubData(inputBuffer, 0, checkedBufferSize(weights.size() * sizeof(float)),
                         weights.data());
    checkGlError("GPU radius selection input upload");
    auto& mesh = MeshFor(snapshot);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, inputBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, outputBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, mesh.Positions());
    glProgramUniform1i(program, glGetUniformLocation(program, "uOperation"), 3);
    glProgramUniform1ui(program, glGetUniformLocation(program, "uVertexCount"),
                        static_cast<GLuint>(weights.size()));
    glProgramUniform3f(program, glGetUniformLocation(program, "uCenter"),
                       static_cast<float>(center.x), static_cast<float>(center.y),
                       static_cast<float>(center.z));
    const float radiusValue = static_cast<float>(radius);
    const float planetRadiusValue = static_cast<float>(planetRadius);
    const float falloffValue = static_cast<float>(falloffPercent);
    if (!std::isfinite(radiusValue) || !std::isfinite(planetRadiusValue) ||
        !std::isfinite(falloffValue)) {
        throw std::overflow_error("GPU radius selection parameters cannot be represented in FP32.");
    }
    glProgramUniform3f(program, glGetUniformLocation(program, "uRadius"),
                       radiusValue, falloffValue, planetRadiusValue);
    Dispatch1D(program, weights.size());
    auto result = Readback(outputBuffer, weights.size());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, 0);
    return result;
}

std::vector<float> ComputeContext::ApplyPolygonSelection(
    std::shared_ptr<const SphericalGridSnapshot> snapshot,
    std::span<const glm::dvec3> polygon,
    const std::filesystem::path& shaderDirectory) {
    if (!snapshot || polygon.size() < 3) {
        throw std::invalid_argument(
            "GPU polygon selection requires a valid snapshot and at least three points.");
    }
    std::vector<glm::dvec3> normalizedPolygon;
    normalizedPolygon.reserve(polygon.size());
    glm::dvec3 reference(0.0);
    for (const auto point : polygon) {
        normalizedPolygon.push_back(normalizedDirection(point));
        reference += normalizedPolygon.back();
    }
    reference = normalizedDirection(reference);
    const double referenceWinding = polygonWinding(reference, normalizedPolygon);
    if (!std::isfinite(referenceWinding) ||
        std::abs(referenceWinding) <= std::acos(-1.0)) {
        throw std::invalid_argument("Spherical polygon does not define a stable interior.");
    }
    if (normalizedPolygon.size() > static_cast<std::size_t>(
            std::numeric_limits<GLuint>::max())) {
        throw std::length_error("GPU polygon selection has too many vertices.");
    }
    if (normalizedPolygon.size() > std::numeric_limits<std::size_t>::max() / 4) {
        throw std::length_error("GPU polygon selection buffer size is not representable.");
    }
    const auto vertexCount = snapshot->GetVertices().size();
    if (vertexCount == 0) {
        return {};
    }
    std::vector<std::array<float, 4>> polygonValues;
    polygonValues.reserve(normalizedPolygon.size());
    for (const auto point : normalizedPolygon) {
        polygonValues.push_back({static_cast<float>(point.x), static_cast<float>(point.y),
                                 static_cast<float>(point.z), 0.0F});
    }
    const GLuint program = LoadProgram(shaderDirectory / "selection.comp");
    const GLuint polygonBuffer = Scratch(polygonValues.size() * 4, 15);
    const GLuint outputBuffer = Scratch(vertexCount, 16);
    glNamedBufferSubData(polygonBuffer, 0, checkedBufferSize(polygonValues.size() * sizeof(float) * 4),
                         polygonValues.data());
    checkGlError("GPU polygon selection input upload");
    auto& mesh = MeshFor(snapshot);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, outputBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, mesh.Positions());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, polygonBuffer);
    glProgramUniform1i(program, glGetUniformLocation(program, "uOperation"), 4);
    glProgramUniform1ui(program, glGetUniformLocation(program, "uVertexCount"),
                        static_cast<GLuint>(vertexCount));
    glProgramUniform1ui(program, glGetUniformLocation(program, "uPolygonCount"),
                        static_cast<GLuint>(polygonValues.size()));
    const float winding = static_cast<float>(referenceWinding);
    if (!std::isfinite(winding)) {
        throw std::overflow_error("GPU polygon winding cannot be represented in FP32.");
    }
    glProgramUniform1f(program, glGetUniformLocation(program, "uReferenceWinding"), winding);
    Dispatch1D(program, vertexCount);
    auto result = Readback(outputBuffer, vertexCount);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0);
    return result;
}

std::array<float, 32> ComputeContext::Histogram(
    std::span<const float> elevations, const std::filesystem::path& shaderDirectory) {
    std::array<float, 32> bins{};
    if (elevations.empty()) {
        return bins;
    }
    const GLuint program = LoadProgram(shaderDirectory / "histogram.comp");
    const GLuint inputBuffer = Scratch(elevations.size(), 20);
    GLuint binBuffer = 0;
    glCreateBuffers(1, &binBuffer);
    if (binBuffer == 0) {
        throw std::runtime_error("OpenGL could not allocate a histogram buffer.");
    }
    try {
        const std::array<std::uint32_t, 32> initial{};
        glNamedBufferStorage(binBuffer, checkedBufferSize(sizeof(initial)), initial.data(),
                             GL_DYNAMIC_STORAGE_BIT);
        glNamedBufferSubData(inputBuffer, 0, checkedBufferSize(elevations.size_bytes()),
                             elevations.data());
        checkGlError("GPU histogram buffer upload");
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, inputBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, binBuffer);
        glProgramUniform1ui(program, glGetUniformLocation(program, "uVertexCount"),
                            static_cast<GLuint>(elevations.size()));
        Dispatch1D(program, elevations.size());
        std::array<std::uint32_t, 32> counts{};
        glGetNamedBufferSubData(binBuffer, 0, checkedBufferSize(sizeof(counts)), counts.data());
        checkGlError("GPU histogram readback");
        for (std::size_t i = 0; i < bins.size(); ++i) {
            bins[i] = static_cast<float>(counts[i]);
        }
    } catch (...) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
        glDeleteBuffers(1, &binBuffer);
        throw;
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glDeleteBuffers(1, &binBuffer);
    return bins;
}

std::vector<float> ComputeContext::SampleRaster(
    std::shared_ptr<const SphericalGridSnapshot> snapshot,
    std::span<const float> rasterSamples, int width, int height,
    bool elevationSamples, const std::filesystem::path& shaderDirectory) {
    if (!snapshot || width <= 0 || height <= 0 ||
        static_cast<std::size_t>(width) >
            std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(height) ||
        rasterSamples.size() != static_cast<std::size_t>(width) * height) {
        throw std::invalid_argument("GPU raster sampling requires valid dimensions and samples.");
    }
    const std::size_t count = snapshot->GetVertices().size();
    if (count == 0) {
        return {};
    }
    if (count > std::numeric_limits<GLuint>::max()) {
        throw std::length_error("GPU raster sampling exceeds the vertex index range.");
    }
    for (const float sample : rasterSamples) {
        if (!std::isfinite(sample)) {
            throw std::invalid_argument("GPU raster samples must be finite.");
        }
    }

    constexpr double pi = 3.14159265358979323846;
    std::vector<std::array<double, 2>> coordinates;
    coordinates.reserve(count);
    for (const auto& vertex : snapshot->GetVertices()) {
        const double z = std::clamp(static_cast<double>(vertex.position.z), -1.0, 1.0);
        const double latitude = std::asin(z);
        const double longitude = std::atan2(static_cast<double>(vertex.position.y),
                                            static_cast<double>(vertex.position.x));
        coordinates.push_back({longitude / (2.0 * pi) + 0.5,
                               0.5 - latitude / pi});
    }

    GLuint texture = 0;
    GLuint coordinateBuffer = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture);
    if (texture == 0) {
        throw std::runtime_error("OpenGL could not allocate a raster texture.");
    }
    try {
        coordinateBuffer = createBuffer(std::span<const std::array<double, 2>>(coordinates));
        glTextureStorage2D(texture, 1, GL_R32F, width, height);
        glTextureSubImage2D(texture, 0, 0, 0, width, height, GL_RED, GL_FLOAT,
                            rasterSamples.data());
        checkGlError("GPU raster texture upload");

        const GLuint program = LoadProgram(shaderDirectory / "raster_sample.comp");
        const GLuint output = Scratch(count, 30);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, output);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, coordinateBuffer);
        glBindTextureUnit(0, texture);
        glProgramUniform1i(program, glGetUniformLocation(program, "uRaster"), 0);
        glProgramUniform1ui(program, glGetUniformLocation(program, "uVertexCount"),
                            static_cast<GLuint>(count));
        glProgramUniform1i(program, glGetUniformLocation(program, "uWidth"), width);
        glProgramUniform1i(program, glGetUniformLocation(program, "uHeight"), height);
        glProgramUniform1i(program, glGetUniformLocation(program, "uElevationSamples"),
                           elevationSamples ? 1 : 0);
        ClearErrors();
        Dispatch1D(program, count);
        if (const auto error = CheckErrors()) {
            if (error->code == ComputeError::Code::NonFinite ||
                error->code == ComputeError::Code::OutOfRange) {
                throw std::invalid_argument(
                    "GPU raster sampling produced invalid data at vertex " +
                    std::to_string(error->index) + ".");
            }
            throw std::overflow_error(
                "GPU raster sampling failed at vertex " + std::to_string(error->index) + ".");
        }
        auto result = Readback(output, count);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0);
        glBindTextureUnit(0, 0);
        glDeleteBuffers(1, &coordinateBuffer);
        glDeleteTextures(1, &texture);
        return result;
    } catch (...) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0);
        glBindTextureUnit(0, 0);
        if (coordinateBuffer != 0) {
            glDeleteBuffers(1, &coordinateBuffer);
        }
        glDeleteTextures(1, &texture);
        throw;
    }
}

RasterExportSamples ComputeContext::SampleExport(
    std::shared_ptr<const SphericalGridSnapshot> snapshot,
    std::span<const float> elevations, std::span<const float> selectionWeights,
    int width, int height, const ColorMap* colorMap,
    const ColorMap* selectionColorMap,
    const std::filesystem::path& shaderDirectory) {
    if (!snapshot || width < 2 || height < 1 || width != height * 2 ||
        elevations.size() != snapshot->GetVertices().size() ||
        selectionWeights.size() != elevations.size()) {
        throw std::invalid_argument("GPU raster export dimensions or mesh data are invalid.");
    }
    if (elevations.empty() || elevations.size() > std::numeric_limits<GLuint>::max() ||
        static_cast<std::size_t>(width) >
            std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(height)) {
        throw std::length_error("GPU raster export dimensions exceed the supported range.");
    }
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    if (pixelCount > std::numeric_limits<GLuint>::max() ||
        pixelCount > std::numeric_limits<std::size_t>::max() / 3) {
        throw std::length_error("GPU raster export pixel count is not representable.");
    }
    for (std::size_t i = 0; i < elevations.size(); ++i) {
        if (!std::isfinite(elevations[i]) || elevations[i] < -32767.0F ||
            elevations[i] > 32767.0F || !std::isfinite(selectionWeights[i]) ||
            selectionWeights[i] < 0.0F || selectionWeights[i] > 1.0F) {
            throw std::invalid_argument("GPU raster export values are outside supported ranges.");
        }
    }

    constexpr int seedWidthLimit = 128;
    constexpr int seedHeightLimit = 64;
    const int seedWidth = std::min(width, seedWidthLimit);
    const int seedHeight = std::min(height, seedHeightLimit);
    std::vector<std::uint32_t> seeds(static_cast<std::size_t>(seedWidth) * seedHeight);
    const double pi = std::acos(-1.0);
    for (int y = 0; y < seedHeight; ++y) {
        const int pixelY = std::min(height - 1,
            static_cast<int>((static_cast<std::int64_t>(2 * y + 1) * height) /
                             (2 * seedHeight)));
        const double latitude = pi * (0.5 - (pixelY + 0.5) / height);
        const double radial = std::cos(latitude);
        for (int x = 0; x < seedWidth; ++x) {
            const int pixelX = std::min(width - 1,
                static_cast<int>((static_cast<std::int64_t>(2 * x + 1) * width) /
                                 (2 * seedWidth)));
            const double longitude = 2.0 * pi *
                ((pixelX + 0.5) / width - 0.5);
            const glm::dvec3 point(radial * std::cos(longitude),
                                   radial * std::sin(longitude), std::sin(latitude));
            const auto nearest = snapshot->FindNearest(point);
            if (nearest > std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("GPU raster seed exceeds the mesh index range.");
            }
            seeds[static_cast<std::size_t>(y) * seedWidth + x] =
                static_cast<std::uint32_t>(nearest);
        }
    }

    const auto packStops = [](const ColorMap* map) {
        std::vector<std::array<float, 4>> result;
        if (map != nullptr) {
            result.reserve(map->GetStops().size());
            for (const auto& stop : map->GetStops()) {
                result.push_back({stop.x, stop.y, stop.z, stop.w});
            }
        }
        return result;
    };
    const auto colors = packStops(colorMap);
    const auto selectionColors = packStops(selectionColorMap);
    if (colors.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max()) ||
        selectionColors.size() >
            static_cast<std::size_t>(std::numeric_limits<GLint>::max())) {
        throw std::length_error("GPU color map has too many stops.");
    }

    auto& mesh = MeshFor(snapshot);
    const GLuint program = LoadProgram(shaderDirectory / "raster_export.comp");
    const GLuint elevationBuffer = Scratch(elevations.size(), 31);
    const GLuint selectionBuffer = Scratch(selectionWeights.size(), 32);
    const GLuint pixelBuffer = Scratch(pixelCount, 33);
    const GLuint seedBuffer = Scratch(seeds.size(), 34);
    const GLuint colorBuffer = Scratch(pixelCount, 35);
    const std::array<float, 4> defaultStop{};
    const GLuint colorStopsBuffer = Scratch(std::max<std::size_t>(1, colors.size()) * 4, 36);
    const GLuint selectionStopsBuffer = Scratch(
        std::max<std::size_t>(1, selectionColors.size()) * 4, 37);
    glNamedBufferSubData(elevationBuffer, 0, checkedBufferSize(elevations.size_bytes()),
                         elevations.data());
    glNamedBufferSubData(selectionBuffer, 0,
                         checkedBufferSize(selectionWeights.size_bytes()),
                         selectionWeights.data());
    glNamedBufferSubData(seedBuffer, 0,
                         checkedBufferSize(seeds.size() * sizeof(std::uint32_t)),
                         seeds.data());
    if (colors.empty()) {
        glNamedBufferSubData(colorStopsBuffer, 0, checkedBufferSize(sizeof(defaultStop)),
                             defaultStop.data());
    } else {
        glNamedBufferSubData(colorStopsBuffer, 0,
                             checkedBufferSize(colors.size() * sizeof(colors[0])),
                             colors.data());
    }
    if (selectionColors.empty()) {
        glNamedBufferSubData(selectionStopsBuffer, 0, checkedBufferSize(sizeof(defaultStop)),
                             defaultStop.data());
    } else {
        glNamedBufferSubData(selectionStopsBuffer, 0,
                             checkedBufferSize(selectionColors.size() *
                                               sizeof(selectionColors[0])),
                             selectionColors.data());
    }
    checkGlError("GPU raster export input upload");

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, elevationBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, selectionBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, pixelBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, mesh.Positions());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, mesh.NeighborOffsets());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, mesh.Neighbors());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, seedBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, colorBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, colorStopsBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, selectionStopsBuffer);
    glProgramUniform1ui(program, glGetUniformLocation(program, "uVertexCount"),
                        static_cast<GLuint>(elevations.size()));
    glProgramUniform1ui(program, glGetUniformLocation(program, "uPixelCount"),
                        static_cast<GLuint>(pixelCount));
    glProgramUniform1i(program, glGetUniformLocation(program, "uWidth"), width);
    glProgramUniform1i(program, glGetUniformLocation(program, "uHeight"), height);
    glProgramUniform1i(program, glGetUniformLocation(program, "uSeedWidth"), seedWidth);
    glProgramUniform1i(program, glGetUniformLocation(program, "uSeedHeight"), seedHeight);
    glProgramUniform1i(program, glGetUniformLocation(program, "uHasSelection"),
                       snapshot->HasSelection() ? 1 : 0);
    glProgramUniform1i(program, glGetUniformLocation(program, "uUseColor"),
                       colorMap != nullptr ? 1 : 0);
    glProgramUniform1i(program, glGetUniformLocation(program, "uUseSelectionColor"),
                       selectionColorMap != nullptr ? 1 : 0);
    glProgramUniform1i(program, glGetUniformLocation(program, "uColorStopCount"),
                       static_cast<GLint>(colors.size()));
    glProgramUniform1i(program, glGetUniformLocation(program, "uSelectionColorStopCount"),
                       static_cast<GLint>(selectionColors.size()));
    ClearErrors();
    try {
        Dispatch1D(program, pixelCount);
        if (const auto error = CheckErrors()) {
            if (error->code == ComputeError::Code::NonFinite ||
                error->code == ComputeError::Code::OutOfRange) {
                throw std::invalid_argument(
                    "GPU raster export rejected invalid values at pixel " +
                    std::to_string(error->index) + ".");
            }
            throw std::overflow_error(
                "GPU raster nearest-site walk failed at pixel " +
                std::to_string(error->index) + ".");
        }
        RasterExportSamples result;
        result.elevations = Readback(pixelBuffer, pixelCount);
        if (colorMap != nullptr) {
            std::vector<std::uint32_t> packed(pixelCount);
            glGetNamedBufferSubData(colorBuffer, 0,
                                    checkedBufferSize(packed.size() * sizeof(packed[0])),
                                    packed.data());
            checkGlError("GPU raster color readback");
            result.rgb.resize(pixelCount);
            for (std::size_t i = 0; i < packed.size(); ++i) {
                result.rgb[i] = packed[i];
            }
        }
        for (GLuint binding = 0; binding <= 11; ++binding) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, 0);
        }
        return result;
    } catch (...) {
        for (GLuint binding = 0; binding <= 11; ++binding) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, 0);
        }
        throw;
    }
}
