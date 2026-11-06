# Project Architecture & Technical Specification: oswald-gis MVP

## 1. System Vision & Architectural Strategy

The `oswald-gis` project is a specialized, interactive Geographic Information System (GIS) and terrain worldbuilding framework tailored for fantasy and fictional planetary creation. Unlike general-purpose GIS packages such as QGIS or ArcGIS—which focus on 2D cartographic projections and complex vector attribute tables—`oswald-gis` prioritizes geodetically sound planetary representation, procedural terrain synthesis, and intuitive artist-centric editing workflows.

### 1.1 Shift from Cubed-Sphere Matrices to Isotropic Point Clouds

Traditional spherical heightfield tools frequently employ a Quadsphere architecture, projecting six 2D square quad matrices onto the faces of a cube. While this approach enables direct $O(1)$ array indexing `[face][row][col]` and fast matrix convolutions, it introduces severe structural limitations for procedural terrain generation:

* **Grid Orientation Artifacts**: Fixed cardinal grid axes cause artificial "stair-casing" during hydraulic erosion simulation, forcing water flow along horizontal or vertical array lines rather than natural drainage channels.
* **Corner Singularities & Seam Logic**: Quad spheres suffer from area distortion near cube corners ($\sim 30\%$ area variation) and require complex topological rotation matrices to stitch flow and analytical windows across face boundaries.

To eliminate these constraints, the `oswald-gis` Minimum Viable Product (MVP) replaces quad-matrix structures with a **Variable-Density Spherical Fibonacci-Delaunay Point Cloud**. Vertices are distributed across the unit sphere $S^2$ using a sunflower-inspired Fibonacci lattice perturbed by controlled spatial jittering, and connected via a 3D spherical Delaunay triangulation. This produces a topologically closed, seamless 2D manifold without face boundaries, corner singularities, or cardinal alignment biases.

### 1.2 Minimum Viable Product (MVP) Scope

The objective for the MVP framework is strictly functional logic: establishing an interactive, geodetically grounded virtual globe capable of ingesting equirectangular elevation rasters, displaying terrain in a 3D orthographic viewport via custom GLSL color mapping, and providing real-world metric navigation controls. 

Advanced data editing, multi-layer stack management, and atmospheric shading are intentionally deferred to post-MVP iterations to ensure the foundational graphics and data architecture functions flawlessly.

---

## 2. Technology Stack & Development Environment

The application framework is implemented in modern C++20 and targets native Linux desktop environments, bypassing browser WebGPU sandboxing and driver translation bottlenecks by targeting desktop OpenGL 4.5 directly.

### 2.1 Core Dependency Matrix

The software stack integrates industry-standard libraries managed directly via CMake:

| Library / Tool | Version / Standard | Functional Purpose within Framework |
| :--- | :--- | :--- |
| **C++ Compiler** | C++20 (`GCC 11+` / `Clang 13+`) | Core application runtime, memory management, and graph data structures. |
| **CMake** | `3.20+` | Cross-platform build automation and dependency fetching via `FetchContent`. |
| **IDE & Assistant** | VS Code + Google Copilot | Code editing, CMake Tools integration, and AI-assisted C++ development. |
| **Windowing & Input** | GLFW `3.4` | Native X11/Wayland window creation, event dispatch, and OpenGL context management. |
| **OpenGL Loader** | GLAD (`OpenGL 4.5 Core`) | Runtime function pointer resolution for desktop OpenGL 4.5 Direct State Access (DSA). |
| **Mathematics** | GLM `1.0.1` | 3D vector, matrix, quaternion transform, and orthographic projection math. |
| **User Interface** | Dear ImGui `1.90.4` | Immediate-mode UI layout (`imgui_impl_glfw` + `imgui_impl_opengl3`). |
| **Spatial Indexing** | `nanoflann 1.5.5` | Header-only 3D k-d tree spatial index over vertex arrays for $O(\log N)$ queries. |
| **Spherical Triangulation** | Qhull `8.0.2` | Reentrant 3D convex hull, yielding spherical Delaunay triangles for the half-edge graph. |

### 2.2 VS Code & Copilot Workflow Integration

Development is configured around VS Code using the `CMake Tools` extension for one-click compilation, target selection, and debugging (`gdb`/`lldb`). Google Copilot is integrated to accelerate boilerplate generation for GLSL shader uniforms, GLFW input handlers, and C++ struct bindings.

---

## 3. Model-View-Controller (MVC) Application Architecture

The software is structured strictly around a Model-View-Controller design pattern, decoupling underlying planetary data and spatial indices from the rendering loop and windowing code.

```
+-------------------------------------------------------------------+
|                           CONTROLLER                              |
|  - GLFW Input Callbacks (Mouse Drag, Scroll Zoom, Double Click)   |
|  - Dear ImGui UI Handlers (File Menu, Settings Modal Dialog)      |
|  - Camera Matrix Manipulator (Yaw / Clamped Pitch Controls)       |
+---------------------------------+---------------------------------+
                                  |
               Triggers Grid      | Queries Lat/Lon
               Regeneration /     | Direction & Updates
               Raster Loading     | Camera Rotation
                                  v
+---------------------------------+---------------------------------+
|                              MODEL                                |
|  - SphericalFibonacciGrid (std::vector<GridVertex>)              |
|  - Spherical Half-Edge Topology Graph                             |
|  - 3D k-d Tree Index (nanoflann Adaptor)                          |
|  - Single-Channel Elevation Data (-32767 to +32767 feet/meters)   |
+---------------------------------+---------------------------------+
                                  |
                                  | Uploads Position &
                                  | Elevation Attributes
                                  v
+---------------------------------+---------------------------------+
|                              VIEW                                 |
|  - OpenGL 4.5 GPU Buffers (VAO, VBO, EBO via DSA)                |
|  - GLSL Hypsometric Color Mapping Shader                         |
|  - 3D Orthographic Globe Projection Viewport                     |
+-------------------------------------------------------------------+
```

### 3.1 Component Responsibilities

* **Model (`SphericalFibonacciGrid`)**: Owns the CPU-side vertex buffers, topological edge connectivities, elevation arrays, and the `nanoflann` 3D k-d tree index. It exposes thread-safe methods for grid generation, local refinement, and spatial point sampling.
* **View (`GlobeRenderer`)**: Manages GPU-side OpenGL resources (Vertex Array Objects, Vertex Buffer Objects, Element Buffer Objects). It compiles GLSL shaders and renders the spherical mesh using an orthographic projection matrix.
* **Controller (`CameraController` & `UIManager`)**: Processes window input events. It translates 2D mouse drags into 3D camera rotations, handles double-click raycasts for globe re-centering, and renders Dear ImGui interface overlays.

---

## 4. Spherical Fibonacci-Delaunay Grid Model

### 4.1 Fibonacci Lattice Generation Mathematics

To distribute $N$ points evenly across the surface of the unit sphere $S^2$ without polar clustering or latitude stretching, vertices are positioned using a sunflower-inspired Fibonacci distribution. For vertex index $i \in [0, N-1]$:

$$\cos(\phi_i) = 1 - \frac{2i + 1}{N}, \quad \sin(\phi_i) = \sqrt{\max(0, 1 - \cos^2(\phi_i))}$$

$$\theta_i = 2\pi \cdot \frac{i}{\Phi}, \quad \text{where } \Phi = \frac{1 + \sqrt{5}}{2} \approx 1.61803398875$$

$$\mathbf{P}_i = \begin{pmatrix} \sin(\phi_i)\cos(\theta_i) \\ \sin(\phi_i)\sin(\theta_i) \\ \cos(\phi_i) \end{pmatrix}$$

### 4.2 Controlled Spatial Jittering

To break up regular lattice alignments that induce unnatural directional biases during procedural terrain synthesis, a controlled displacement vector $\boldsymbol{\delta}_i$ is added to each base coordinate $\mathbf{P}_i$. The jitter magnitude is scaled relative to the average inter-point distance $r \approx \sqrt{4\pi / N}$:

$$\boldsymbol{\delta}_i = J \cdot \frac{r}{2} \cdot \mathbf{u}_i, \quad \text{where } \mathbf{u}_i \sim \text{Uniform}(\mathbb{B}^3), \; J \in [0.0, 1.0]$$

$$\mathbf{P}'_i = \frac{\mathbf{P}_i + \boldsymbol{\delta}_i}{\|\mathbf{P}_i + \boldsymbol{\delta}_i\|}$$

Re-projecting $\mathbf{P}'_i$ onto $\|\mathbf{P}'_i\| = 1.0$ ensures all vertices lie exactly on the surface of unit sphere $S^2$.

### 4.3 Topological Data Structure & Half-Edge Graph

Vertices are linked into a convex spherical triangulation. The topology is stored as a **Spherical Half-Edge Graph**:

```cpp
struct GridVertex {
    glm::vec3 position;  // Position vector on S^2 (|P| = 1.0)
    float elevation;     // Terrain elevation (-32767.0 to +32767.0)
};

struct HalfEdge {
    int32_t originVertex;
    int32_t twinEdge;
    int32_t nextEdge;
    int32_t faceIndex;
};
```

Because the mesh forms a closed 2D manifold in $\mathbb{R}^3$, neighborhood traversals (e.g., flow routing or slope calculation) cross boundaries continuously without special case logic or border-stitching lookups.

Phase 2 computes the convex hull with Qhull's reentrant API (`qhull Qt`).
For unit-sphere points enclosing the origin, its triangular facets form the
spherical Delaunay mesh. Faces are wound outward; each has three half-edges
with reciprocal twins and a three-edge `nextEdge` cycle. Triangle indices are
stored in the same order for later GPU upload. A valid grid has $2N-4$ faces
and $6N-12$ half-edges. Generation rejects an incomplete or open hull rather
than publishing a partially valid model.

### 4.4 Metric Distance Scaling

Rather than measuring distances in pixels or cell array steps, spatial dimensions are calculated directly in real-world kilometers or miles on $S^2$. The physical arc distance $d$ between two points $\mathbf{P}_1, \mathbf{P}_2$ is defined by:

$$d = R \cdot \arccos(\mathbf{P}_1 \cdot \mathbf{P}_2)$$

where $R$ represents the planet's mean radius in kilometers ($R = 6,371\text{ km}$ by default).

---

## 5. Adaptive Regional Level-of-Detail (LOD) Strategy

To support high-detail local mapping in regions of interest while maintaining a low memory footprint across unmapped or flat ocean basins, `oswald-gis` implements an adaptive regional LOD system.

### 5.1 Dynamic Refinement Protocol (Subdivision)

1. **Region Selection**: The user defines a spatial selection domain on the globe using a 3D raycast brush or bounding spherical polygon.
2. **Local Point Insertion**: Additional vertices are generated within the selected domain using localized Poisson-disk sampling or high-density jittered Fibonacci points.
3. **Cavity Re-triangulation**: An incremental Bowyer-Watson spherical Delaunay algorithm updates the local triangulation cavity without rebuilding the global mesh.
4. **Data Transfer**: New vertices inherit elevation values from their enclosing parent triangle via barycentric interpolation:

$$\text{Elevation}(\mathbf{P}_{\text{new}}) = w_A \cdot \text{Elev}(A) + w_B \cdot \text{Elev}(B) + w_C \cdot \text{Elev}(C)$$

### 5.2 Dynamic Coarsening Protocol (Simplification)

1. **Decimation Selection**: When coarsening a region, low-importance or flat vertices within the selection domain are flagged for removal.
2. **Vertex Removal & Hole Triangulation**: Flagged points are deleted from the vertex buffer, and the surrounding cavity on $S^2$ is re-triangulated. 
3. **Seamless Manifold Maintenance**: Because the surface remains a continuous 2D Delaunay manifold, vertex removal introduces no T-junction cracks, edge skirts, or visual rendering gaps.

---

## 6. Spatial Indexing Engine (`nanoflann` k-d Tree)

Unstructured point clouds lack direct 2D matrix indexing `[row][col]`. To enable real-time coordinate inspection, raycasting, and GIS raster sampling, `oswald-gis` integrates `nanoflann`—a header-only C++ library providing zero-copy 3D k-d tree indexing over `std::vector<GridVertex>`.

### 6.1 Zero-Copy Adaptor Implementation

```cpp
#include <nanoflann.hpp>
#include <vector>
#include <glm/glm.hpp>

struct PointCloudAdaptor {
    const std::vector<GridVertex>& points;

    inline size_t kdtree_get_point_count() const { return points.size(); }

    inline float kdtree_get_pt(const size_t idx, const size_t dim) const {
        if (dim == 0) return points[idx].position.x;
        if (dim == 1) return points[idx].position.y;
        return points[idx].position.z;
    }

    template <class BBOX>
    bool kdtree_get_bbox(BBOX&) const { return false; }
};

using KDTree3D = nanoflann::KDTreeSingleIndexAdaptor<
    nanoflann::L2_Simple_Adaptor<float, PointCloudAdaptor>,
    PointCloudAdaptor,
    3, // 3D Spatial Dimensions (X, Y, Z)
    std::size_t
>;
```

### 6.2 Query Workflows

* **Nearest-Neighbor Lookups ($O(\log N)$)**: Maps geographic coordinates $(\text{Lat}, \text{Lon})$ or mouse raycasts to the closest vertex in microseconds.
* **Radius Searches**: Retrieves all vertex indices within a specified physical arc radius $r_{\text{world}}$ for regional sculpting brushes and selection masks. Cost is output-dependent, not purely $O(\log N)$.

### 6.3 Phase 2 Model API and Ownership

`SphericalFibonacciGrid` is implemented in `include/SphericalGrid.hpp` and
`src/SphericalGrid.cpp`. Construction and `GenerateGrid(GridOptions)` accept
1,000 to 1,000,000 points, finite jitter strength in [0, 1], and an explicit
32-bit seed. Defaults are 10,000 points, jitter 0.15, and seed 42. Elevations
start at zero. Seeded jitter uses rejection sampling uniformly inside the
unit ball, then reprojects each perturbed point onto the unit sphere.
Positions are stored as `glm::vec3`, accurate to float precision.

`GetSnapshot()` returns a shared, immutable `SphericalGridSnapshot`, exposing
`GetVertices()`, `GetHalfEdges()`, and `GetTriangleIndices()`. Each snapshot
owns its vectors and zero-copy k-d tree; it cannot be copied or moved.
Generation builds a complete replacement before atomically publishing it.
Existing readers retain their previous data and index; failed regeneration
leaves the last valid snapshot intact. Concurrent generators are allowed,
with the last completed publication becoming current. Readers must retain
their snapshot handle while using references to its arrays.

Snapshot queries are safe concurrently:

* `FindNearest(direction)` normalizes a finite, nonzero 3D direction and returns
  a vertex index using float Euclidean chord distance on the unit sphere.
* `FindWithinRadius(direction, arcRadius, planetRadius = 6371.0)` takes the
  arc radius and planet radius in the same physical unit (kilometers by
  default). It converts arc to chord distance for k-d tree candidates, then
  filters in double precision using angular distance. The boundary is
  inclusive, indices are sorted ascending, and radii at or beyond half the
  circumference select the whole globe.

Invalid parameters throw `std::invalid_argument`; geometry/index failures
throw `std::runtime_error` with Qhull diagnostics on stderr. The application
reports startup exceptions and exits unsuccessfully. Elevation editing,
local refinement, and coarsening remain future work.

---

## 7. Graphics & Render Pipeline (OpenGL 4.5 & GLSL)

The rendering engine utilizes OpenGL 4.5 Core Profile with Direct State Access (DSA) to minimize driver overhead and state management complexity.

### 7.1 GPU Buffer Initialization

```cpp
GLuint vao, vbo, ebo;
glCreateVertexArrays(1, &vao);
glCreateBuffers(1, &vbo);
glCreateBuffers(1, &ebo);

// Upload Vertex Attribute Buffer
glNamedBufferData(vbo, vertices.size() * sizeof(GridVertex), vertices.data(), GL_DYNAMIC_DRAW);
glNamedBufferData(ebo, indices.size() * sizeof(uint32_t), indices.data(), GL_DYNAMIC_DRAW);

// Configure Vertex Array Attributes via DSA
glEnableVertexArrayAttrib(vao, 0); // Position (vec3)
glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(GridVertex, position));
glVertexArrayAttribBinding(vao, 0, 0);

glEnableVertexArrayAttrib(vao, 1); // Elevation (float)
glVertexArrayAttribFormat(vao, 1, 1, GL_FLOAT, GL_FALSE, offsetof(GridVertex, elevation));
glVertexArrayAttribBinding(vao, 1, 0);

glVertexArrayVertexBuffer(vao, 0, vbo, 0, sizeof(GridVertex));
glVertexArrayElementBuffer(vao, ebo);
```

### 7.2 GLSL Shading Pipeline

The maintained GLSL 450 core shader pair is
[globe.vert](./shaders/globe.vert) and [globe.frag](./shaders/globe.frag).
The vertex shader applies `uModelViewProjection` and passes elevation and
surface normal to the fragment shader. Interpolated normals are normalized
before directional lighting, with a minimum diffuse factor of 0.15.

Colour values and stop positions are external shader data, not GLSL constants.
The fragment shader binary-searches the relevant ocean/land stop range in a
read-only `std430` shader storage buffer and interpolates adjacent RGB values
exactly, without a sampled lookup texture. Elevation is clamped to the original
range [-32767, +32767]. Negative elevations use the ocean ramp; zero and above
use the land ramp, preserving the discontinuity at sea level.

### 7.3 External Colour-Map Format

[default.map](./assets/colormaps/default.map) preserves the original palette.
Each map contains one `[ocean]` section and one `[land]` section, in either order:

```text
# position red green blue
[ocean]
0.0 0.02 0.08 0.25
1.0 0.15 0.55 0.65

[land]
0.0 0.18 0.48 0.18
0.4 0.72 0.68 0.42
0.8 0.45 0.38 0.30
1.0 0.95 0.95 0.98
```

Positions and RGB components are finite normalized values in [0, 1]. Ocean
positions run from deepest elevation to sea level from below; land positions
run from sea level to maximum elevation. Each section needs at least two
strictly increasing positions with endpoints exactly 0 and 1. Blank lines and
`#` comments (including trailing comments) are supported. Extra fields,
unknown or duplicate sections, missing endpoints, and invalid numbers are
explicit errors with the file path and line number.

`ColorMap::Load(path)` parses and validates the file without an OpenGL context.
`GlobeRenderer::SetColorMap(map)` uploads ocean stops followed by land stops
and updates the counts. A future menu can load a selected map and call this
method without rebuilding the mesh or recompiling shaders. There is no fixed
application stop-count cap; maps must fit the GPU storage-buffer and shader
integer-indexing limits, which are checked explicitly. Failed file loading
does not change the active renderer palette.

### 7.4 Renderer Lifecycle and Viewport

`GlobeRenderer` owns the shader program, VAO, vertex buffer, index buffer, and
colour-map storage buffer through RAII. Construct, upload, draw, and destroy
it on the thread with its OpenGL 4.5 context current. Destruction occurs before
the GLFW window and context are destroyed, including startup failure paths.
Shader compilation/linking errors include driver logs; OpenGL operation errors
are surfaced as exceptions and the application reports them on stderr.

`UploadGrid(snapshot)` uses the model's render indices. `UploadMesh(vertices,
indices)` also accepts replacement elevation-bearing arrays, without modifying
the immutable model snapshot. Inputs are checked for finite positions and
elevations, complete triangles, valid indices, and buffer size limits.

`Draw(framebufferWidth, framebufferHeight)` establishes depth testing,
back-face culling, an orthographic projection, and a Z-up view from negative Y.
Projection bounds preserve the globe's circular silhouette and fit it within
both portrait and landscape framebuffers. Zero-sized framebuffers are skipped
while minimized; negative sizes and drawing before mesh/palette upload are
errors. The caller clears colour and depth each frame. Rotation and zoom remain
Phase 4 work; zero-initialized elevations currently produce a shaded green globe.

---

## 8. User Interface & Controller Specification

### 8.1 Menu Bar & Dialog Layout

Dear ImGui provides the main application interface overlay.

```
+-------------------------------------------------------------------+
| File                                                              |
|   ├── Open Raster...  (Ctrl+O)                                    |
|   ├── Settings...                                                 |
|   └── Exit                                                        |
+-------------------------------------------------------------------+
```

#### Settings Dialog (`File -> Settings...`)
* **Point Count ($N$)**: Int slider controlling target base vertex density ($1,000$ to $1,000,000$).
* **Jitter Strength ($J$)**: Float slider ($0.00$ to $1.00$).
* **Apply Button**: Triggers `SphericalFibonacciGrid::GenerateGrid()` and updates GPU VBO buffers.

### 8.2 Navigation & Viewport Controls

The navigation system provides interactive orthographic globe manipulation:

* **Mouse Drag (Left Button)**: Rotates globe around X/Y axes. Pitch is clamped to $[-1.45, +1.45]$ radians ($\sim \pm 83^\circ$) to prevent polar inversion.
* **Mouse Wheel**: Adjusts orthographic zoom scale factor smoothly.
* **Double Click (Left Button)**: Casts an orthographic ray from cursor screen space into 3D world space. If the ray intersects unit sphere $S^2$, the controller smoothly rotates the globe to center the clicked coordinate.

---

## 9. Data Ingestion & Cartographic Raster Pipeline

When the user selects an equirectangular image file via `File -> Open...`:

1. **Raster Loading**: The image is loaded into host memory (via `stb_image`) and scaled to a standard 2:1 aspect ratio format.
2. **Luminosity Extraction**: Pixel RGB values are converted to normalized luminosity $L \in [0.0, 1.0]$:

$$L = 0.2126 \cdot R + 0.7152 \cdot G + 0.0722 \cdot B$$

3. **Elevation Mapping**: Luminosity is scaled to 16-bit signed integer range $[-32767, +32767]$ feet/meters:

$$\text{Elevation} = -32767.0 + L \cdot (65534.0)$$

4. **Grid Populating**: For each vertex $\mathbf{P}_i = (x, y, z)$ in the Fibonacci point cloud:
   * Calculate geographic coordinates: $\text{Lat} = \arcsin(z)$, $\text{Lon} = \arctan(y, x)$.
   * Map $(\text{Lat}, \text{Lon})$ to normalized image pixel coordinates $(u, v) \in [0, 1] \times [0, 1]$.
   * Bilinearly sample the image luminosity and assign the calculated elevation to $\mathbf{P}_i$.
   * Upload updated elevation array to GPU VBO via `glNamedBufferSubData`.

---

## 10. CMake Configuration & Directory Layout

### 10.1 Directory Layout

```
oswald-gis/
├── CMakeLists.txt
├── include/
│   ├── ColorMap.hpp
│   ├── SphericalGrid.hpp
│   ├── GlobeRenderer.hpp
│   └── CameraController.hpp
├── src/
│   ├── ColorMap.cpp
│   ├── main.cpp
│   ├── SphericalGrid.cpp
│   └── GlobeRenderer.cpp
├── tests/
│   ├── SphericalGridTests.cpp
│   └── GlobeRendererTests.cpp
├── shaders/
│   ├── globe.vert
│   └── globe.frag
└── assets/
    ├── colormaps/
    │   └── default.map
    └── earth_elevation_default.png
```

This is the target MVP layout; camera and raster asset files remain for later
phases. GLAD sources are generated inside the build directory.

### 10.2 Production CMake Configuration

The maintained build configuration is [CMakeLists.txt](./CMakeLists.txt).
It pins the dependencies in Section 2.1 via `FetchContent`, generates GLAD,
builds `imgui_lib` with the GLFW/OpenGL3 backends, and exposes `oswald_model`
as an OpenGL-independent C++20 library. The `oswald_gis` executable links the
model and graphics dependencies. `spherical_grid_tests` links only the model
and thread library; no display is needed to run the model tests.

`oswald_view` builds the renderer and colour-map loader. Shaders and the default
map are copied at configuration time into the build's `resources` directory.
Editing a source shader/map triggers CMake regeneration on the next build.
The executable uses the configured absolute resource path, independent of the
working directory. Relocatable installation/resource discovery is not yet
implemented; moving the build requires reconfiguration.

Qhull uses the reentrant static target `qhullstatic_r`. Its source directory
is excluded from the default build, so only linked targets are built, not its
standalone tools or unused C++ wrapper (which uses pre-C++20 constructor syntax).
The application and model remain C++20. Its CMake 4 policy
compatibility floor is scoped to Qhull's configuration. Qhull is configured
before nanoflann so the dependencies share an uninstall target without a
name collision. Upstream examples and tests for nanoflann remain disabled.

### 10.3 Phase 1 Build and Run

The initial build creates an OpenGL 4.5 core-profile window and clears it each
frame. Configure and build it with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/oswald_gis
```

GLAD is generated during CMake configuration for the OpenGL 4.5 core profile.
On Linux, GLFW also needs the development packages for at least one supported
window-system backend (X11 or Wayland).

#### Debugging in VS Code

Install the C/C++ and CMake Tools extensions and GDB. Open the project folder,
run `CMake: Configure`, select the Debug build variant and `oswald_gis` as the
launch target. In Run and Debug, select **Debug Oswald GIS** and press F5.
The workspace launch configuration builds the application before starting GDB
and resolves its executable path through CMake Tools.

The workspace uses Ninja in `build/ninja`, separate from command-line builds in
`build`. Install Ninja (`ninja-build` on Ubuntu). This keeps both the main CMake
cache and FetchContent dependency caches on the same generator. When changing
generators, use a new build directory rather than reusing existing caches.

Use this application launch configuration rather than **Debug Active File**
on `CMakeLists.txt`: the CMake script is not the application executable.
Native GLFW backend dependencies and an OpenGL 4.5-capable display are still
required to run the application.

### 10.4 Phase 2 Verification

The application generates the default model at startup and prints its vertex
and face counts. Phase 3 now renders this model in the viewport.

```bash
cmake --build build/ninja --target oswald_gis spherical_grid_tests
ctest --test-dir build/ninja -R '^spherical_grid$' --output-on-failure
ctest --test-dir build/ninja -R '^spherical_grid_scale$' -V
```

`BUILD_TESTING` defaults to ON. The focused test checks the Fibonacci formula,
unit lengths, jitter bounds and seeds, zero elevations, outward winding,
Euler counts, reciprocal twins, closed vertex fans, the spherical Delaunay
condition, brute-force nearest/arc query agreement, inclusive radius
boundaries, invalid inputs, and snapshot lifetime during concurrent reads
and regeneration. The `scale`-labeled test validates the maximum 1,000,000
points with maximum jitter, including topology and a spatial query.

### 10.5 Phase 3 Verification

```bash
cmake --build build/ninja
ctest --test-dir build/ninja -R '^(color_map|globe_renderer)$' --output-on-failure
```

`color_map` is a CPU-only parser/validation test. `globe_renderer` is labeled
`gpu` and requires an available display and OpenGL 4.5 context. It uses a hidden
GLFW window and framebuffer pixel reads to verify the globe/background, original
palette colours and interpolation across sea level, elevation clamping, live
palette replacement with different stop counts and positions, mesh re-upload,
and circular aspect-correct rendering after resizing. It also exercises missing
and invalid shader files, shader linking failures, and invalid renderer inputs. GPU context failure is
reported as a test failure, not silently skipped. On headless systems run CPU
project tests with
`ctest --test-dir build/ninja -R '^(spherical_grid(_scale)?|color_map)$' --output-on-failure`.

---

## 11. Implementation Roadmap

1. **Phase 1: Build System & Context Setup**: Configure CMake with `FetchContent` for GLFW, GLAD (OpenGL 4.5 Core), GLM, Dear ImGui, and `nanoflann`. Verify window creation and basic rendering loop.
2. **Phase 2: Model Implementation**: Implement `SphericalFibonacciGrid` point distribution math, spatial jittering, and half-edge graph structures. Integrate the `nanoflann` 3D k-d tree adaptor.
3. **Phase 3: Render Viewport & Shaders**: Set up OpenGL DSA buffer lifecycle (`glCreateBuffers`, `glNamedBufferData`), compile the GLSL hypsometric elevation shader pair, and establish orthographic camera projection.
4. **Phase 4: Controller & UI Interactivity**: Implement pitch-clamped mouse drag rotation, scroll-wheel zoom, double-click raycast re-centering, and the Dear ImGui `File -> Settings...` dialog.
5. **Phase 5: Raster Ingestion & Verification**: Add equirectangular image loading via `File -> Open...`, map luminosity to elevation range $[-32,767, +32,767]$, and verify raster population on the Fibonacci point cloud.
