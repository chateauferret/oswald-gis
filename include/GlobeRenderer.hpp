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
                    std::span<const std::uint32_t> indices, bool selectionActive = false);
    void UpdateElevations(std::span<const GridVertex> vertices);
    void UpdateSelection(std::span<const GridVertex> vertices, bool active);
    void SetColorMap(const ColorMap& map);
    void SetSelectionColorMap(const ColorMap& map);
    void Draw(int framebufferWidth, int framebufferHeight,
              const glm::mat4& modelMatrix = glm::mat4(1.0F), float zoom = 1.0F);

private:
    // Must be destroyed while the creating OpenGL context is still current.
    struct Resources {
        GLuint vao = 0;
        GLuint positionBuffer = 0;
        GLuint elevationBuffer = 0;
        GLuint selectionBuffer = 0;
        GLuint indexBuffer = 0;
        GLuint colorBuffer = 0;
        GLuint selectionColorBuffer = 0;
        GLuint program = 0;
        ~Resources();
    } resources_;
    GLint matrixLocation_ = -1;
    GLint stopCountLocation_ = -1;
    GLint selectionStopCountLocation_ = -1;
    GLint selectionActiveLocation_ = -1;
    GLsizei indexCount_ = 0;
    std::size_t vertexCount_ = 0;
    bool hasColorMap_ = false;
    bool hasSelectionColorMap_ = false;
    bool selectionActive_ = false;

    void UploadColorMap(const ColorMap& map, GLuint buffer, GLint countLocation);
};
