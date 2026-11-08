#pragma once

#include "ColorMap.hpp"
#include "SphericalGrid.hpp"

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <span>

class GlobeRenderer {
public:
    explicit GlobeRenderer(const std::filesystem::path& shaderDirectory);
    ~GlobeRenderer() = default;
    GlobeRenderer(const GlobeRenderer&) = delete;
    GlobeRenderer& operator=(const GlobeRenderer&) = delete;
    GlobeRenderer(GlobeRenderer&&) = delete;
    GlobeRenderer& operator=(GlobeRenderer&&) = delete;

    void UploadGrid(const SphericalGridSnapshot& snapshot);
    void UploadMesh(std::span<const GridVertex> vertices,
                    std::span<const std::uint32_t> indices);
    void UpdateElevations(std::span<const GridVertex> vertices);
    void SetColorMap(const ColorMap& map);
    void Draw(int framebufferWidth, int framebufferHeight,
              const glm::mat4& modelMatrix = glm::mat4(1.0F), float zoom = 1.0F);

private:
    // Must be destroyed while the creating OpenGL context is still current.
    struct Resources {
        GLuint vao = 0;
        GLuint positionBuffer = 0;
        GLuint elevationBuffer = 0;
        GLuint indexBuffer = 0;
        GLuint colorBuffer = 0;
        GLuint program = 0;
        ~Resources();
    } resources_;
    GLint matrixLocation_ = -1;
    GLint lightLocation_ = -1;
    GLint oceanCountLocation_ = -1;
    GLint landCountLocation_ = -1;
    GLsizei indexCount_ = 0;
    std::size_t vertexCount_ = 0;
    bool hasColorMap_ = false;
};
