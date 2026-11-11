#pragma once

#include "ColorMap.hpp"
#include "SphericalGrid.hpp"

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <vector>

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
    void SetGraticuleColors(const glm::vec3& ordinary, const glm::vec3& highlighted);
    void SetWireframeColors(const glm::vec3& unselected, const glm::vec3& selected);
    void SetWireframeEnabled(bool enabled);
    bool IsWireframeEnabled() const { return wireframeEnabled_; }
    void SetGraticuleEnabled(bool enabled);
    bool IsGraticuleEnabled() const { return graticuleEnabled_; }
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
        GLuint colorTexture = 0;
        GLuint selectionColorTexture = 0;
        GLuint program = 0;
        GLuint wireVao = 0;
        GLuint wirePositionBuffer = 0;
        GLuint wireWeightBuffer = 0;
        GLuint wireProgram = 0;
        ~Resources();
    } resources_;
    GLint matrixLocation_ = -1;
    GLint viewDirectionLocation_ = -1;
    GLint graticuleSpacingLocation_ = -1;
    GLint graticuleColorLocation_ = -1;
    GLint highlightedGraticuleColorLocation_ = -1;
    GLint selectionActiveLocation_ = -1;
    GLint graticuleEnabledLocation_ = -1;
    GLsizei indexCount_ = 0;
    std::size_t vertexCount_ = 0;
    bool hasColorMap_ = false;
    bool hasSelectionColorMap_ = false;
    bool selectionActive_ = false;
    bool wireframeEnabled_ = false;
    bool graticuleEnabled_ = true;
    glm::vec3 graticuleColor_{0.75F};
    glm::vec3 highlightedGraticuleColor_{1.0F};
    glm::vec3 wireframeColor_{1.0F, 1.0F, 0.0F};
    glm::vec3 wireframeSelectedColor_{0.5F, 0.5F, 0.0F};
    GLint wireMatrixLocation_ = -1;
    GLint wireDepthBiasLocation_ = -1;
    GLint wireSelectionActiveLocation_ = -1;
    GLint wireUnselectedColorLocation_ = -1;
    GLint wireSelectedColorLocation_ = -1;
    GLsizei wireVertexCount_ = 0;
    // Delaunay sites on either side of each Voronoi edge, used to blend selection weights.
    std::vector<std::array<std::uint32_t, 2>> wireSites_;

    void UploadColorMap(const ColorMap& map, GLuint texture);
    void BuildVoronoi(std::span<const GridVertex> vertices, std::span<const std::uint32_t> indices);
    void UploadWireWeights(std::span<const GridVertex> vertices);
};
