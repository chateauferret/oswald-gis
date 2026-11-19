# Project Architecture & Technical Specification: oswald-gis MVP

This is the maintained architecture document, retaining its original MVP filename.
The implementation now includes the post-MVP features in [roadmap.md](./roadmap.md),
including layers, persistence/export, projections, FastNoise2, and TOML-driven tools.

## 1. System Vision & Architectural Strategy

The `oswald-gis` project is a specialized, interactive Geographic Information System (GIS) and terrain worldbuilding framework tailored for fantasy and fictional planetary creation. Unlike general-purpose GIS packages such as QGIS or ArcGIS—which focus on 2D cartographic projections and complex vector attribute tables—`oswald-gis` prioritizes geodetically sound planetary representation, procedural terrain synthesis, and intuitive artist-centric editing workflows.

### 1.1 Shift from Cubed-Sphere Matrices to Isotropic Point Clouds

Traditional spherical heightfield tools frequently employ a Quadsphere architecture, projecting six 2D square quad matrices onto the faces of a cube. While this approach enables direct $O(1)$ array indexing `[face][row][col]` and fast matrix convolutions, it introduces severe structural limitations for procedural terrain generation:

* **Grid Orientation Artifacts**: Fixed cardinal grid axes cause artificial "stair-casing" during hydraulic erosion simulation, forcing water flow along horizontal or vertical array lines rather than natural drainage channels.
* **Corner Singularities & Seam Logic**: Quad spheres suffer from area distortion near cube corners ($\sim 30\%$ area variation) and require complex topological rotation matrices to stitch flow and analytical windows across face boundaries.

To eliminate these constraints, the `oswald-gis` Minimum Viable Product (MVP) replaces quad-matrix structures with a **Variable-Density Spherical Poisson-Delaunay Point Cloud**. Vertices are sampled uniformly over the unit sphere $S^2$ with Poisson-disk minimum spacing, then connected via a 3D spherical Delaunay triangulation. This produces a topologically closed, seamless 2D manifold without face boundaries, corner singularities, or cardinal alignment biases.

### 1.2 Minimum Viable Product (MVP) Scope

The objective for the MVP framework is strictly functional logic: establishing an interactive, geodetically grounded virtual globe capable of ingesting equirectangular elevation rasters, displaying terrain in a 3D orthographic viewport via custom GLSL color mapping, and providing real-world metric navigation controls. 

That initial scope has been implemented and extended: the current application
includes undoable editing, multi-layer stack management, atmospheric rim shading,
adaptive refinement/coarsening, project persistence/export, equirectangular view,
and scripted brushes. The MVP scope above is historical, not a list of current
limitations.

---

## 2. Technology Stack & Development Environment

The application framework is implemented in modern C++20 and targets native Linux desktop environments, bypassing browser WebGPU sandboxing and driver translation bottlenecks by targeting desktop OpenGL 4.5 directly.

### 2.1 Core Dependency Matrix

The software stack integrates industry-standard libraries managed directly via CMake:

| Library / Tool | Version / Standard | Functional Purpose within Framework |
| :--- | :--- | :--- |
| **C++ Compiler** | C++20 (`GCC 11+` / `Clang 13+`) | Core application runtime, memory management, and graph data structures. |
| **CMake** | `4.0+` | Cross-platform build automation and dependency fetching via `FetchContent`. |
| **IDE & Assistant** | VS Code + GitHub Copilot | Code editing, CMake Tools integration, and AI-assisted C++ development. |
| **Windowing & Input** | GLFW `3.4` | Native X11/Wayland window creation, event dispatch, and OpenGL context management. |
| **OpenGL Loader** | GLAD (`OpenGL 4.5 Core`) | Runtime function pointer resolution for desktop OpenGL 4.5 Direct State Access (DSA). |
| **Mathematics** | GLM `1.0.1` | 3D vector, matrix, quaternion transform, and orthographic projection math. |
| **User Interface** | Dear ImGui `1.90.4` | Immediate-mode UI layout (`imgui_impl_glfw` + `imgui_impl_opengl3`). |
| **Spatial Indexing** | `nanoflann 1.5.5` | Header-only 3D k-d tree spatial index over vertex arrays for $O(\log N)$ queries. |
| **Spherical Triangulation** | Qhull `8.0.2` | Reentrant 3D convex hull, yielding spherical Delaunay triangles for the half-edge graph. |
| **Raster Decoding** | `stb_image` | Single-header decoding of common equirectangular image formats. |
| **TIFF / GeoTIFF I/O** | System libtiff | Numeric and RGB TIFF import/export, GeoTIFF tags, and precision-preserving elevation samples. |
| **Noise** | FastNoise2 `1.1.1` | Native Simplex, Value, White, fBm, and ridged generators; pinned FastSIMD dependency and strict floating point. |
| **Scripting** | exprtk `0.0.3` | Header-only expression parser/evaluator for user scripts that generate brush values. |
| **Brush script files** | toml++ `3.4.0` | Header-only TOML parser for saved brush scripts. |

### 2.2 VS Code & Copilot Workflow Integration

Development is configured around VS Code using the `CMake Tools` extension for one-click compilation, target selection, and debugging (`gdb`/`lldb`). GitHub Copilot assists with C++ and GLSL development.

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
|  - SphericalGrid (Poisson-sampled std::vector<GridVertex>)         |
|  - Spherical Half-Edge Topology Graph                             |
|  - 3D k-d Tree Index (nanoflann Adaptor)                          |
|  - Layer Collection (independent Float32 Elevation rasters)       |
|  - Separate Per-Vertex Selection Channel                         |
+---------------------------------+---------------------------------+
                                  |
                                  | Uploads Position &
                                  | Elevation Attributes
                                  v
+---------------------------------+---------------------------------+
|                              VIEW                                 |
|  - OpenGL 4.5 GPU Buffers (VAO, VBO, EBO via DSA)                |
|  - GLSL Hypsometric Color Mapping Shader                         |
|  - Orthographic Globe / Equirectangular Map Viewports            |
+-------------------------------------------------------------------+
```

### 3.1 Component Responsibilities

* **Model (`SphericalGrid`)**: Owns the CPU-side vertex buffers, topological edge connectivities, its data-layer collection, and the `nanoflann` 3D k-d tree index. `Layer` defines shared metadata, `RasterLayer` stores raster values, and each `ElevationLayer` has independent values synchronized with immutable mesh snapshots when active. Elevation- and selection-only snapshots share the geometry's immutable spatial index; it is rebuilt only when mesh geometry changes. The per-vertex selection channel remains separate from the layer collection. The model exposes cancellable grid generation, immutable snapshots, and spatial point sampling.
* **View (`GlobeRenderer`)**: Manages GPU-side OpenGL resources (Vertex Array Objects, Vertex Buffer Objects, Element Buffer Objects). Each layer has a persistent elevation buffer, and brush previews update changed vertex ranges in the active layer. A compute shader combines visible elevations bottom-to-top before a single globe draw, using the topmost visible layer's selected/unselected colour maps. Overlay replaces the accumulated value; Add, Subtract (lower minus upper), Multiply, Minimum, and Maximum operate on elevations, not RGB colours. Each operation clamps to the supported elevation range [-32767, 32767]; the bottommost visible layer seeds the result regardless of its blend mode. Composition is cached until values, order, visibility, or blend modes change. Selection, graticules, and rim shading are applied once, after composition.
* **Controller (`CameraController` & `UIManager`)**: Processes window input events. It translates 2D mouse drags into 3D camera rotations, handles double-click raycasts for globe re-centering, and renders Dear ImGui interface overlays.
* **Scripting (`ScriptEngine`)**: Wraps exprtk so scripts can combine maths, control flow, string-valued choices, and noise to generate values. The engine owns one symbol table shared by all scripts it compiles. It stores numeric and string variables at stable addresses, binds caller-owned numeric variables, constants, and C++ functions (fixed arity 0–4 or variadic), and compiles source into a `CompiledScript`. Compiled scripts keep the engine state alive and can be evaluated repeatedly. Built-in `simplex`, `valuenoise`, `fbm`, `billow`, `valuefbm`, and `ridged` functions use the shared FastNoise2-backed `Noise` wrappers; `spectral` uses the cached FFT-filtered field. These functions use an engine-level uint64 seed, set from C++ because a double cannot represent every seed.
  * Compile failures raise `ScriptError` with 1-based line and column diagnostics. Unknown symbols, file/console I/O, and `return` statements are rejected. Strings and comparisons such as `mode == 'one'` are supported.
  * Symbol names are case-sensitive.
  * Each loop entry is limited to a configurable iteration count, which defaults to 1,000,000.
  * exprtk evaluates an optimized expression tree; it does not generate machine code.
  * The engine is not thread-safe.
  * Infinite loops with literal true conditions are rejected at compile time.
* **Elevation safety**: Brushes blend falloff in double precision and clamp the result to [-32767, 32767].
  * Before committing a stroke, the UI restores non-finite values and clamps all values.
  * Failures when committing a stroke, or during undo or redo, are reported in the UI and the grid stays unchanged.
  * The frame loop catches per-frame exceptions, logs them, and recovers ImGui state, so a runtime error does not terminate the application.
* **Scripted brush (`ScriptedBrushTool`)**: The runtime used by every Brushes menu entry. Each instance owns a name, `ScriptEngine`, compiled script, parameter definitions/values, and application settings. Brushes are one-shot unless TOML specifies `continuous=true`.
  * Read-only inputs:
    * `x`, `y`, `z`: unit position, domain-warped when warping is enabled.
    * `lat`, `lon`: unwarped position in degrees.
    * `elevation`: the value before the dab.
    * `weight`: falloff multiplied by the selection weight.
    * `distance`, `radius`: brush distance units.
    * `lower`, `upper`: the ordered Between/And range.
    * `elapsed`: seconds for the current dab; `rate`: Raise/Lower speed configured in Settings.
  * Inputs and declared parameter values are reset for every vertex, so assignments do not modify subsequent vertices or UI control values.
  * `application="noise"` (default) supports normalized output mapped from [-1, 1] to Between/And, or elevation output clamped to that range, then applies the selected noise operation. It retains seed, output-range and domain-warp controls.
  * `application="elevation"` clamps the script result to [-32767, 32767] and blends toward it by falloff/selection. `application="delta"` adds the result multiplied by falloff/selection before clamping. Direct modes ignore noise settings.
  * All three modes use `ApplyNoiseSampler`. Continuous brushes evaluate against current working values; one-shot strokes evaluate dabs against pre-stroke values and keep the strongest effect per vertex, preventing accumulation when held or retraced.
  * That pipeline samples all vertices before writing. A failing script, a non-finite result, or a loop-limit violation therefore leaves the layer unchanged.
  * The UI catches the error and shows it in the brush panel.
  * `gaussian(range, sigma, unit)` samples a Gaussian-weighted spherical neighbourhood from immutable pre-dab values; unit 0 means kilometres and 1 means miles. Invalid arguments fail the dab without partial writes.
  * `UIManager` builds the registry from library scripts and retains a separate `Custom` instance (formerly Scripted Brush). Native brush classes remain reference implementations for tests, not automatically registered Brushes entries. `oswald_model` remains independent of exprtk.
  * Only Custom exposes the draft editor, Compile/Revert/Default, library Load/Save, and Import/Export. Library tools expose generated parameters without the editor. A failed compile keeps the previous script and parameters active; Revert restores them.
* **Brush script files (`BrushScriptFile`, `BrushScriptLibrary`)**: Scripts are saved as TOML files with root keys `name`, `description`, `output`, `script`, optional `application` and boolean `continuous`, followed by optional `[[parameter]]` tables, as in [assets/brushes/fbm.toml](./assets/brushes/fbm.toml). Root keys must precede the parameter tables.
  * `name` and `script` are required; `description` and `output` are optional, and unknown keys are ignored.
  * `output` is `"normalized"` (`"normalised"` is also accepted, case-insensitively) or `"elevation"`. Loading into Custom without it keeps the current Script output setting; standalone library tools default to normalized. UI saves write the current output setting.
  * Parameters are `float`, `integer`, or `choice`, with valid unique script identifiers that cannot shadow inputs/functions/constants/reserved words. Numeric parameters require `control` (`slider`, `spin`, `text`), `min < max`, and a valid `default`; numeric strings and TOML numbers are accepted. Integers use 32-bit bounds. Floats optionally use linear (default) or logarithmic scale; logarithmic bounds must be positive. Float slider bounds are limited to half the maximum finite double magnitude.
  * Choices require a nonempty array of unique strings and a matching string default; scripts read the selected string by name. An optional `label` changes display text without changing the script identifier.
  * Optional float `units` references a choice parameter with exactly `["Kilometres", "Miles"]`. Storage, bounds and script values stay in kilometres; the UI converts displayed/input values when Miles is selected. Gaussian Blur uses this to preserve physical range and sigma across unit changes.
  * Single-line values are written as basic strings. Multi-line scripts are written as `'''` literal strings so the file stays readable, falling back to escaped basic strings when the text contains quotes or control characters.
  * Files are written to a temporary file and renamed, so a failed save never truncates an existing script.
  * `BrushScriptLibrary::Scan` loads every `.toml` file in a folder, sorted by name, and reports files that fail to parse instead of aborting.
  * The application's library folder is the source tree's `assets/brushes/` directory (`OSWALD_BRUSH_SCRIPT_DIR`), set with `UIManager::SetBrushScriptDirectory`.
  * Brushes is populated alphabetically from compilable files, followed by Custom, Refresh brushes, and Brush errors when needed. Invalid files/scripts, duplicate names, and the reserved name `Custom` are skipped with diagnostics. An empty/broken library still leaves Custom usable. No brush is active until selected.
  * Refresh rebuilds library tools with their declared defaults while retaining Custom's instance, draft, compiled script, and control values. Save to library also refreshes Brushes. Refresh is disabled during a stroke. Files are read directly from the source library, not copied into build resources.
  * The Custom panel lists the library with each script's description, and has Load, Refresh, Name and Description fields.
  * Save to library writes `<name>.toml`, with unsafe file-name characters replaced by `_`, and asks before replacing an existing file.
  * Import... and Export... use the project file browser to read or write a script anywhere.
  * Loading or importing replaces the editor's name, description, draft, behavior metadata and parameter controls/defaults, plus output mode when present, then compiles the script. A script that fails to compile stays in the editor with diagnostics; strokes retain the last compiled script and parameters.
  * Saving/exporting writes the current draft, parameter definitions and original defaults, and behavior metadata, not current control values. Parameter definitions and behavior metadata are edited externally.
* **Noise backend**: `Noise.cpp` reuses thread-local native FastNoise2 Simplex, Value, White, FractalFBm and FractalRidged nodes. Value uses cubic interpolation. Native fractal sums are divided by total octave amplitude and clamped to [-1, 1]. Both halves of a uint64 seed are folded into FastNoise2's 32-bit seed as `low32 + high32 * 0x9e3779b9` modulo 2^32; collisions are unavoidable and patterns differ from the original in-house backend.
  * `fbm`, `billow`, `valuefbm` and `ridged` take `(x,y,z,frequency,lacunarity,persistence,octaves)`. Legacy nine-argument ridge scripts must remove offset/gain arguments; those controls and the Value interpolation selector no longer exist.
  * FastNoise2 uses float coordinates. Fractal frequency/lacunarity/persistence must be finite and nonnegative, octaves 1-16, with float-representable lacunarity/persistence, per-octave coordinates within +/-1e8, and total amplitude within half the maximum finite float.
  * `spectral(x,y,z,frequency,exponent,resolution)` uses FastNoise2 White samples and the existing periodic 3D FFT filter/cache. Frequency is in (0, 1e6], exponent in [-4, 8], and resolution is a power of two in [8, 128]. Domain warping uses the new Simplex backend. Saved terrain is not regenerated merely by opening a project.

The single hover readout reports the cumulative elevation through the active layer,
including visible layers below it and any in-progress brush preview, but excluding
layers above it. Painting and filters still edit the active layer's raw values;
composition does not modify the stored layer data.

---

## 4. Spherical Poisson-Delaunay Grid Model

### 4.1 Uniform Spherical Poisson Sampling

Each candidate is drawn uniformly by sampling its vertical coordinate and longitude independently:

$$z = 2u - 1, \qquad \theta = 2\pi v, \qquad u,v \sim \operatorname{Uniform}(0,1)$$

$$\mathbf{P} = \begin{pmatrix}\sqrt{1-z^2}\cos\theta \\\sqrt{1-z^2}\sin\theta \\z\end{pmatrix}$$

Candidates are accepted only when their chord distance from every existing
site is at least the Poisson exclusion radius. A spatial hash accelerates this
test. For $N$ requested sites, the minimum spacing is

$$s = (0.78 - 0.16J)\sqrt{\frac{4\pi}{N}}, \qquad J \in [0,1]$$

Higher $J$ permits closer neighbors and therefore a more variable random
pattern. A fixed seed makes sampling reproducible. Optional Lloyd passes run
after sampling to improve Voronoi-cell regularity.

### 4.2 Topological Data Structure & Half-Edge Graph

Vertices are linked into a convex spherical triangulation. The topology is stored as a **Spherical Half-Edge Graph**:

```cpp
struct GridVertex {
    glm::vec3 position;  // Position vector on S^2 (|P| = 1.0)
    float elevation;     // Terrain elevation (-32767.0 to +32767.0)
    float selectionWeight; // Per-vertex selection mask [0.0, 1.0]
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

Each vertex also stores a selection weight in [0, 1]. Snapshots track whether
a selection is active separately from the weights: no active selection leaves
brushes unrestricted, while an active all-zero mask prevents edits. Radius
selection uses geodesic distance and a linearly feathered outer ring; polygon
selection evaluates a spherical polygon against each vertex. Clear, invert,
and selection strokes are undoable.

### 4.3 Metric Distance Scaling

Rather than measuring distances in pixels or cell array steps, spatial dimensions are calculated directly in real-world kilometers or miles on $S^2$. The physical arc distance $d$ between two points $\mathbf{P}_1, \mathbf{P}_2$ is defined by:

$$d = R \cdot \arccos(\mathbf{P}_1 \cdot \mathbf{P}_2)$$

where $R$ represents the planet's mean radius in kilometers ($R = 6,371\text{ km}$ by default).

---

## 5. Adaptive Regional Level-of-Detail (LOD) Strategy

To support high-detail local mapping in regions of interest while maintaining a low memory footprint across unmapped or flat ocean basins, `oswald-gis` implements an adaptive regional LOD system.

### 5.1 Dynamic Refinement Protocol (Subdivision)

1. **Region Selection**: The user defines a spatial selection domain on the globe using the radius or polygon selection tools.
2. **Local Point Insertion**: `Mesh -> Refine Selection` considers faces with interpolated centroid selection weight at least 0.5. It skips faces smaller than half the area-weighted median selected-face area, so small, already-dense boundary triangles do not accumulate extra sites when a coarsened region is refined. If no centroid is selected, faces incident to a selected vertex can seed a tiny selection. Each eligible face receives one spherical face-centroid site.
3. **Cavity Re-triangulation**: Small batches insert each site incrementally into the spherical convex hull, removing visible facets and connecting their horizon to the new site. Larger batches are triangulated together with Qhull. Both paths produce a closed, outward-facing mesh.
4. **Data Transfer**: Elevation and selection weight are interpolated from the parent face using planar barycentric coordinates at the ray/triangle intersection:

$$\text{Elevation}(\mathbf{P}_{\text{new}}) = w_A \cdot \text{Elev}(A) + w_B \cdot \text{Elev}(B) + w_C \cdot \text{Elev}(C)$$

$$\text{SelectionWeight}(\mathbf{P}_{\text{new}}) = w_A \cdot s_A + w_B \cdot s_B + w_C \cdot s_C$$

### 5.2 Dynamic Coarsening Protocol (Simplification)

1. **Decimation Selection**: `Mesh -> Coarsen Flat Selection` first removes eligible vertices created by refinement in the selected region, restoring the prior site density. Otherwise, it preserves the selected-region seam and replaces flat selected interior sites with approximately half as many area-weighted Poisson-spaced spherical samples. New samples use wholly selected faces and maintain a 1.35-spacing buffer from fixed sites to avoid an edge-density spike.
2. **Attribute Transfer & Hole Triangulation**: New sites inherit elevation and selection weight from their source face; Qhull rebuilds the spherical Delaunay hull with the original exterior and seam vertices fixed.
3. **Constrained Relaxation**: Two damped spherical Lloyd steps move only selected interior vertices. Unselected vertices and the selection boundary remain fixed; the hull is rebuilt after each step.
4. **Seamless Manifold Maintenance**: The rebuilt mesh is checked for a closed half-edge topology and a convex outward hull, avoiding T-junction cracks or visual gaps.

Both operations publish new immutable snapshots and are undoable/redoable. The refinement API can optionally cap inserted points, while the default UI action processes every eligible face at the selected area's coarser scale. Coarsening can also be explicitly limited; the default preferentially reverses refinement and otherwise regenerates the selected interior at lower density.

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

`SphericalGrid` is implemented in `include/SphericalGrid.hpp` and
`src/SphericalGrid.cpp`. Construction and `GenerateGrid(GridOptions)` accept
1,000 to 1,000,000 points, Poisson variability in [0, 1], and an explicit
32-bit seed. Defaults are 10,000 points, variability 0.5, and seed 42.
Candidates are sampled uniformly over the sphere and rejected when too close
to accepted points. Elevations start at zero. Positions are stored as
`glm::vec3`, accurate to float precision.

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
* `FindWithinRadius(direction, arcRadius)` uses the planet's configured radius
  in kilometres. The three-argument overload takes an explicit radius and uses
  arc radius and planet radius in the same physical unit (kilometers by
  default). It converts arc to chord distance for k-d tree candidates, then
  filters in double precision using angular distance. The boundary is
  inclusive, indices are sorted ascending, and radii at or beyond half the
  circumference select the whole globe.

Invalid parameters throw `std::invalid_argument`; geometry/index failures
throw `std::runtime_error` with Qhull diagnostics on stderr. The application
reports startup exceptions and exits unsuccessfully. Selection-driven local
refinement and flat-region coarsening are implemented as described above.

---

## 7. Graphics & Render Pipeline (OpenGL 4.5 & GLSL)

The rendering engine utilizes OpenGL 4.5 Core Profile with Direct State Access (DSA) to minimize driver overhead and state management complexity.

### 7.1 GPU Buffer Initialization

The following is a simplified DSA layout example, not the current renderer's
complete buffer allocation. The implementation uses separate position,
elevation, selection and index buffers, per-layer elevation buffers, and a
compute-output buffer for composed elevations.

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

The maintained GLSL 450 core globe pipeline is
[globe.vert](./shaders/globe.vert), [globe.geom](./shaders/globe.geom), and
[globe.frag](./shaders/globe.frag). The geometry stage handles projection-specific
triangle output, including equirectangular seam wrapping. The separate
[elevation.comp](./shaders/elevation.comp) shader composes visible layers;
`wire.vert`/`wire.geom`/`wire.frag` render the optional wireframe.

Selection weights are uploaded as a separate per-vertex attribute and passed
to the fragment shader. With an active selection, the shader blends from the
selected palette at weight zero to the unselected palette at weight one. The
initial palettes are [grayscale.map](./assets/colormaps/grayscale.map) and
[default.map](./assets/colormaps/default.map), respectively. Each palette has
its own stop list and is evaluated at the same elevation.
Without an active selection, the unselected palette is used everywhere.
Brush edits multiply their falloff by the active selection weight.

The vertex shader passes object-space position to the fragment shader. The
fragment shader derives latitude and longitude there, then draws antialiased
parallels and meridians, with brighter Equator and Prime Meridian lines.
Angular spacing is inversely proportional to zoom, keeping the visible grid
density approximately stable while zooming. A view-dependent blue rim tint
adds atmospheric edge glow.
Graticule lines are antialiased to approximately one pixel, light grey by
default; the Equator and Prime Meridian are white by default. Both RGB colours
are configurable in `File -> Settings`.

Colour values and stop positions are external shader data, not GLSL constants.
Each palette is uploaded as a dynamic RGBA32F 1D texture whose texels store
ordered elevation/RGB stops. The fragment shader binary-searches the texture
and interpolates adjacent colours exactly. `View -> Legend` lists `.map` files
from the configured colour-map directory and changes the unselected and
selected palettes independently without rebuilding the mesh or shader.
Elevation is clamped to [-32767, +32767]. Zero is treated like any other
elevation: there is no ocean/land split or implicit sea-level threshold.

### 7.3 External Colour-Map Format

[default.map](./assets/colormaps/default.map) retains the original colours,
with an explicit narrow blue-to-green transition. Each map contains a single
list of elevation/RGB stops, without section headers:

```text
# elevation red green blue
-32767 0.02 0.08 0.25
-1 0.15 0.55 0.65
0 0.18 0.48 0.18
13106.8 0.72 0.68 0.42
26213.6 0.45 0.38 0.30
+32767 0.95 0.95 0.98
```

Elevations are finite values in [-32767, +32767], nominally in feet, and may
be fractional. Each line accepts either three finite normalized RGB floats
in [0, 1], three decimal byte integers in [0, 255], or one six-digit hexadecimal
`RRGGBB` colour (case-insensitive, without a prefix). For example, `0000FF`,
`0 0 255`, and `0.0 0.0 1.0` all specify pure blue. A decimal point or exponent
in any of the three components selects normalized floats for that line;
otherwise all three are decimal bytes (`0 0 1` means blue intensity 1/255).
Encodings may differ between lines. Values may be separated by any whitespace,
including spaces and tabs. Hex colours must be a single token. The
list needs at least two strictly increasing elevations, with endpoints exactly
-32767 and +32767. No stop at zero is required, and any colour transition may
be placed at any elevation. Blank lines and `#` comments (including trailing
comments) are supported. Section headers, extra fields, missing endpoints,
and invalid numbers are explicit errors with the file path and line number.

`ColorMap::Load(path)` parses and validates the file without an OpenGL context.
`ColorMap::GetStops()` exposes the ordered elevation/RGB list.
`GlobeRenderer::SetColorMap(map)` replaces the unselected palette texture;
`SetSelectionColorMap(map)` independently replaces the selected palette
texture. Maps must fit the GPU's 1D texture-size limit, which is checked
explicitly. Failed file loading does not change the active renderer palette.

### 7.4 Renderer Lifecycle and Viewport

`GlobeRenderer` owns the shader program, VAO, vertex buffer, index buffer, and
both colour-map textures through RAII. Construct, upload, draw, and destroy
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
errors. The caller clears colour and depth each frame. Rotation and zoom are
implemented by `CameraController`: the caller supplies the current model matrix
and zoom. Equirectangular mode renders a longitude/latitude map with horizontal
wrapping and bounded latitude, rather than a circular globe.

---

## 8. User Interface & Controller Specification

### 8.1 Menu Bar & Dialog Layout

Dear ImGui provides the main application interface overlay.

```
+-------------------------------------------------------------------+
| File                                                              |
|   ├── Open           (Ctrl+O)                                     |
|   ├── Save           (Ctrl+S)                                     |
|   ├── Save As...     (Ctrl+Shift+S)                               |
|   ├── Export                                                     |
|   ├── Planet...                                                  |
|   ├── Open Raster... (Ctrl+Shift+O)                               |
|   ├── Settings...                                                 |
|   └── Exit                                                        |
+-------------------------------------------------------------------+
```

The main menu also includes `Select -> Radius Selection`, `Polygon Selection`,
`Clear Selection`, and `Invert Selection`. Radius selection has outer-radius
and feather controls. Polygon selection accepts globe clicks and finishes on
a double-click or Enter; Escape cancels it.
With either Select tool active, an unshifted left-click on empty viewport space
outside the globe clears selection and any unfinished polygon. UI-captured
clicks and clicks outside the viewport leave selection unchanged. Clearing is
undoable and keeps the Select tool active.
Hold Shift when starting a radius stroke or placing the first polygon point
to add to the existing selection instead of replacing it. Overlapping weights
combine by taking their maximum, preserving feathering without accumulation.
Shift-clicks outside the globe leave the selection unchanged.

`Select -> By value...` replaces selection using the current elevation range.
The dialog accepts bounds in [-32767, 32767], labelled "Between" and "and"
to the left of the number boxes, and a feather
percentage f in [0, 100]. Full selection extends from the midpoint (100-f)% toward
each bound; the remaining intervals taper linearly to zero at the bounds.
Values outside the range are unselected. At 0% the entire inclusive range has
full weight with hard edges; at 100% selection tapers from full weight at the
range midpoint to zero at both bounds. Equal bounds
select exact matches only. OK swaps reversed bounds before applying one undoable selection change without
changing elevations; Cancel or Escape dismisses the dialog without applying.

#### Planet Dialog (`File -> Planet...`)
* **Geometry tab**: Positive finite planet radius in kilometres (default 6371)
  and point count. Radius is shared by incision, brush/noise distances, Gaussian
  blur, radius selection, and their viewport rings; miles use the same radius
  converted from kilometres. Changing radius alone preserves terrain, layers,
  selection, and undo history.
* **Point Count ($N$)**: Int slider controlling target base vertex density ($1,000$ to $1,000,000$).
  Applying a new count regenerates the grid using the loaded raster if available,
  remaps layers, and clears undo history, as in the former Settings control.
* **Metadata tab**: Description, author, and read-only last-saved UTC date.
  Apply commits both tabs; Cancel/Escape discards pending changes.

#### Settings Dialog (`File -> Settings...`)
* **Poisson Variability ($J$)**: Float slider ($0.00$ to $1.00$); higher values allow smaller minimum spacing.
* **Apply Button**: Triggers `SphericalGrid::GenerateGrid()` and updates GPU VBO buffers.
* **Raise/Lower speed**: Positive elevation units per second, default 8192; supplied
  to scripts as `rate`. Changing only this setting preserves terrain and undo history.
* **Display colours**: Graticule/highlight and selected/unselected wireframe colours.

### 8.2 Navigation & Viewport Controls

The navigation system provides interactive orthographic globe manipulation:

* **Mouse Drag (Left Button)**: Rotates globe around X/Y axes. Pitch is clamped to $[-1.45, +1.45]$ radians ($\sim \pm 83^\circ$) to prevent polar inversion.
* **Mouse Wheel**: Adjusts orthographic zoom scale factor smoothly.
* **Middle-Button Drag**: Rotates the globe regardless of the selected tool,
  respecting UI mouse capture. Middle and right clicks do not clear selection;
  only an uncaptured left click without Shift on empty viewport space clears it.
* **Double Click (Left Button)**: Casts an orthographic ray from cursor screen space into 3D world space. If the ray intersects unit sphere $S^2$, the controller smoothly rotates the globe to center the clicked coordinate.

`View -> Projection` switches between Orthographic and Equirectangular.
In Equirectangular view, dragging pans, longitude wraps, latitude is bounded,
the wheel scales the map, and double-click recenters geographically. Brush,
selection, and hover picking use the selected projection.

### 8.3 Consistent UI Controls

Use the same widget type and presentation wherever the same kind of value is
entered, whether in a dialog or a parameters panel. In particular, elevation
values in [-32767, +32767] use float sliders with the same supported range.
Script controls use their TOML datatype, control, label and scale, with numeric
validation before values reach the active script. Use the same
consistency rule for other data types and ranges rather than introducing a
different widget style for equivalent values.

### 8.4 Brushes Menu and Custom Editor

The Brushes menu (formerly Tools) is data-driven from `assets/brushes/*.toml`; see Section 3.1 for
validation, refresh, parameter binding, and application modes. The shipped
definitions replace all eight original menu tools:

| Tool | Definition | Application |
| --- | --- | --- |
| Elevation Brush | [elevation.toml](./assets/brushes/elevation.toml) | Direct height; Replace/Minimum/Maximum choice |
| Raise/Lower Brush | [raise_lower.toml](./assets/brushes/raise_lower.toml) | Continuous delta using `rate * elapsed * intensity / 100` |
| Simplex Noise | [simplex.toml](./assets/brushes/simplex.toml) | Normalized fBm, optional billowed output |
| Value Noise | [value.toml](./assets/brushes/value.toml) | Normalized Value fBm with amplitude |
| Ridge Noise | [ridge.toml](./assets/brushes/ridge.toml) | Normalized native ridged fractal |
| Power-Law Spectral Noise | [spectral.toml](./assets/brushes/spectral.toml) | Normalized periodic spectral field |
| Gaussian Blur | [gaussian_blur.toml](./assets/brushes/gaussian_blur.toml) | Direct Gaussian neighbourhood height |
| Clamp | [clamp.toml](./assets/brushes/clamp.toml) | Direct height clamped between ordered parameter bounds |

Other valid library files also appear in Brushes. Custom is a separate persistent
editor/tool, not a file-backed preset name. Definitions and defaults are edited
externally; Custom still permits editing/compiling the script body and saving
or exporting its definition. Per-tool values are independent. Refresh resets
library tools to file defaults, but does not replace Custom's state.

Every brush panel includes **Fill selection**. `UIManager` copies the current
`BrushSettings` with `coverage=BrushCoverage::Selection`, then calls the same
brush `Apply` pipeline once with `elapsed=1`. The shared weighting code visits
every selected vertex, using only its selection weight (or weight 1 everywhere
if selection is inactive), bypassing radius searches and radial falloff. An empty
active selection changes nothing. Scripts retain the configured radius but receive
distance zero because a selection fill has no brush centre. Continuous scripts
therefore apply one second at their configured rate/intensity. Gaussian/spectral
sampling, seed, output operation/range, and parameters are otherwise unchanged.
The action edits only the active layer and commits one `BrushStrokeCommand` when
values change, supporting undo/redo. Script evaluation uses immutable pre-fill
values and stages updates before writing; failures are reported in the panel
without partial edits. The button is disabled during a stroke or without an
active layer.

---

## 9. Data Ingestion & Cartographic Raster Pipeline

When the user selects an equirectangular image file via `File -> Open Raster...`
(`File -> Open` opens an `.oswald` project):

1. **Raster Loading**: The image is loaded into host memory (via `stb_image`) and scaled to a standard 2:1 aspect ratio format.
2. **Luminosity Extraction**: Pixel RGB values are converted to normalized luminosity $L \in [0.0, 1.0]$:

$$L = 0.2126 \cdot R + 0.7152 \cdot G + 0.0722 \cdot B$$

3. **Elevation Mapping**: Luminosity is mapped to Float32 elevation values in
   $[-32767, +32767]$ (not quantized to signed integers). Supported Int16/Float32
   TIFF elevations instead retain their numeric samples; see Section 10.6.

$$\text{Elevation} = -32767.0 + L \cdot (65534.0)$$

4. **Grid Populating**: For each Poisson-sampled vertex $\mathbf{P}_i = (x, y, z)$:
   * Calculate geographic coordinates: $\text{Lat} = \arcsin(z)$, $\text{Lon} = \arctan(y, x)$.
   * Map $(\text{Lat}, \text{Lon})$ to normalized image pixel coordinates $(u, v) \in [0, 1] \times [0, 1]$.
   * Bilinearly sample the image luminosity and assign the calculated elevation to $\mathbf{P}_i$.
   * Upload updated elevation array to GPU VBO via `glNamedBufferSubData`.

---

### 9.1 Absolute Magnitude Noise

`Filter -> Absolute Magnitude Noise...` opens an amplitude slider from 0 to
500 feet (default 100). Each Apply draws independent uniform random offsets
between minus and plus the amplitude, converts feet to metres (`1 ft = 0.3048 m`),
and adds each offset multiplied by the vertex's selection weight. With no active
selection, all vertices have full weight; an active empty selection changes
nothing. This operates on the current selection, not the history of Fill edits,
and affects underwater elevations too.

Final elevations are clamped to `[-32767, 32767] m` after feathering. Apply records
one undoable edit on the active elevation layer; redo restores the exact same
random result. Zero amplitude is a no-op. Cancel/Escape discards pending edits.

### 9.2 Basin Filling

`Filter -> Erosion -> Fill basins...` immediately runs the spherical priority-flood
operator from Section 2.1 of the [erosion functional spec](./oswald-gis-erosion-functional-spec.md)
on the active elevation layer, as one undoable edit. With no active selection it
processes the globe; otherwise unselected vertices remain fixed drainage outlets
and selection weights blend the filled elevations. Feathering intentionally
partially fills basins rather than guaranteeing complete drainage.

Vertices at or below sea level are fixed sinks, preserving the sea floor. If
there are no ocean or unselected outlets, all vertices tied at the global minimum
are used as sinks. Propagation follows the spherical Delaunay half-edge graph,
including across the poles and longitude seam. Flat or depressed terrain is
raised by `0.0001 m` above its resolved neighbor, or one Float32 representable step
when that increment would round away. If drainage would exceed the supported
`32767 m` elevation limit, the operation reports an error without applying changes.

### 9.3 Stream Power Incision

`Filter -> Erosion -> Incise flow...` opens a parameter dialog for a single pass of
Section 2.2 of the [erosion functional spec](./oswald-gis-erosion-functional-spec.md).
Defaults are incision rate `C = 1 m/pass`, reference runoff `Aref = 1`, flow
exponent `m = 0.5`, slope exponent `n = 1`, uniform precipitation `P = 1` per
vertex. Planet radius comes from `File -> Planet... -> Geometry` (default
`6371 km`), not this filter's dialog. Reference runoff must be positive;
the other parameters may be zero but not negative. All must be finite.

Each non-sink vertex routes to its steepest strictly downhill Delaunay neighbor,
using great-circle distance and slopes in metres per kilometre. Runoff starts at
`P` per vertex (vertex-count units when `P = 1`) and accumulates from highest to
lowest elevation. Incision uses the original slopes and accumulated runoff:
`C * (A / Aref)^m * S^n`. Changes are simultaneous, not affected by processing order.

Sink and selection rules match basin filling: vertices at or below sea level
and unselected vertices are fixed terminal outlets; unselected terrain does not
supply upstream runoff to the selection. With no outlets, tied global minima
are fixed sinks. Feathered selections blend the final incision, not runoff.
Local pits and flats without strictly downhill neighbors remain unchanged;
basin filling is a separate, optional preparatory operation.

The filter can carve land below sea level, but never changes existing ocean
sinks. Target elevations are clamped to `[-32767, 32767] m` before selection
blending, consistent with the Fill filter. Invalid parameters or numeric overflow
are reported in the dialog without changing terrain. Apply records one undoable
edit on the active elevation layer; Cancel leaves terrain untouched.

### 9.4 Precipiton Diffusion

`Filter -> Erosion -> Precipiton diffusion...` opens a parameter dialog for a
single pass of Section 2.3 of the erosion functional spec. The diffusion
coefficient defaults to `k = 1 km/pass`, may be zero, and must be finite and
non-negative. Great-circle neighbor distances use the planet radius set in
`File -> Planet... -> Geometry`.

For every non-sink vertex, the filter computes the distance-weighted Laplacian
from the original elevations across its Delaunay 1-ring and applies
`k * sum((z_neighbor - z_current) / distance)` simultaneously. Ocean-level or
underwater vertices and unselected vertices are fixed; when no such outlets
exist, tied global minima are fixed. Feathered selections blend the final
diffusion. Updated elevations are clamped to `[-32767, 32767] m`. Invalid
parameters or numeric overflow leave terrain unchanged; Apply creates one
undoable active-layer edit and Cancel has no effect.

### 9.5 Thermal Weathering

`Filter -> Erosion -> Thermal weathering...` opens a parameter dialog for a
single pass of Section 2.4 of the erosion functional spec. The angle of repose
defaults to `35 degrees`, accepts finite values in `[0, 90)`, and is typically
between `30 and 40 degrees`. Neighbor edge lengths use great-circle kilometres
from the configured planet radius.

Each Delaunay edge is considered once using the original elevations. For an
edge whose gradient exceeds `tan(theta)`, the filter transfers half the excess
height from the higher vertex to the lower vertex:
`0.5 * (high - low - distance * tan(theta))`. Per-vertex transfers accumulate
and apply simultaneously. Sea-level/ocean sinks, unselected vertices, and
global-minimum sinks when no other outlets exist remain fixed. Feathered
selections blend the resulting displacement; elevations are clamped to
`[-32767, 32767] m`. Invalid settings or numeric overflow leave terrain
unchanged; Apply creates one undoable active-layer edit and Cancel has no effect.

### 9.6 Glacial Erosion

`Filter -> Erosion -> Glacial erosion...` runs one configured glacial pass for
Sections 3.1–3.3 of the erosion functional spec. The ELA is
`equator ELA - latitude gradient * abs(latitude in degrees)` using the grid's
unit-sphere Z coordinate as latitude. The pass starts from the configured ice
thickness and applies the mass-balance equation over the selected duration;
ice thickness is clamped at zero. Defaults include a 1-year duration, no
initial ice, and the specified ice/mantle densities of 917/3300 kg/m³.

Ice surface slopes use great-circle Delaunay 1-ring distances in kilometres.
All lower neighbors contribute to the local downslope direction, while the
maximum positive slope drives Glen-law velocity and bedrock erosion. The flow
direction defines a local transverse cross-section: erosion is distributed
over the center and near-orthogonal 1-ring neighbors with the parabolic
`max(0, 1 - (r_perp / trough_half_width)^2)` weights, normalized per source
vertex. Isostatic depression is applied as `-(917 / 3300) * ice thickness`.
Mass balance, erosion, and load displacement use the original terrain and are
applied simultaneously.

All configurable rates, thicknesses, duration, and exponents must be finite
and non-negative; trough half-width must be finite and positive. As with the
other terrain erosion filters, sea-level/ocean sinks and unselected outlets
remain fixed, global-minimum sinks are used when no outlets exist, and
selection weights blend the final displacement. Elevations are clamped to
`[-32767, 32767] m`; invalid parameters or numeric overflow leave terrain
unchanged. Apply records one undoable active-layer edit; Cancel leaves terrain
untouched.

---

## 10. CMake Configuration & Directory Layout

### 10.1 Directory Layout

```
oswald-gis/
├── CMakeLists.txt
├── include/                 # Public model, renderer, controller and scripting headers
├── src/                     # Implementations, main.cpp and internal file helpers
├── tests/                   # CPU tests plus hidden-context GPU/UI integration tests
├── shaders/                 # Globe/wire vertex, geometry, fragment shaders; elevation.comp
└── assets/
    ├── colormaps/           # External selected/unselected colour palettes
    └── brushes/             # TOML scripts supplying Brushes menu entries
```

`include/` and `src/` include Layer, SphericalGrid, ProjectFile, ElevationRaster,
BrushTool, SelectionTool, Filter, Noise, CameraController, GlobeRenderer,
ScriptEngine, ScriptedBrushTool, BrushScriptFile, RasterFileBrowser and UIManager.
`ScriptParameter.hpp` defines the shared parameter metadata/value types;
`UndoStack.hpp` provides command history. GLAD and fetched dependencies are
generated/populated inside the build directory.

### 10.2 Production CMake Configuration

The maintained build configuration is [CMakeLists.txt](./CMakeLists.txt).
It pins the dependencies in Section 2.1 via `FetchContent`, generates GLAD,
builds `imgui_lib` with the GLFW/OpenGL3 backends, and exposes `oswald_model`
as an OpenGL-independent C++20 library. The `oswald_gis` executable links the
model and graphics dependencies. `spherical_grid_tests` links only the model
and thread library; no display is needed to run the model tests.
FastNoise2 is linked privately by `oswald_model`, with its tools, tests and
utility executables disabled and `FASTNOISE2_STRICT_FP=ON`. Its pinned FastSIMD
dependency provides CPU SIMD dispatch.
TIFF image I/O additionally requires a system libtiff development package
(`libtiff-dev` on Debian/Ubuntu, `libtiff-devel` on Fedora, or `libtiff` via
Homebrew). CMake discovers it with `find_package(TIFF REQUIRED)` and links
`TIFF::TIFF`; a nonstandard installation can be selected with CMake's
`TIFF_INCLUDE_DIR` and `TIFF_LIBRARY` cache variables.

`oswald_view` builds the renderer; the OpenGL-independent colour-map loader and
stop interpolator live in `oswald_model` for use by both rendering and RGB export.
exprtk is fetched as a header-only `exprtk` INTERFACE target and is included
only by `src/ScriptEngine.cpp` in the `oswald_scripting` library, keeping its
large compile cost in one translation unit. The scripting library links
`oswald_model` for the shared noise primitives and also contains the
`ScriptedBrushTool`. toml++ is fetched as a header-only INTERFACE target and is
included only by `src/BrushScriptFile.cpp`, also in `oswald_scripting`.
`script_engine_tests`, `scripted_brush_tool_tests` and `brush_script_file_tests`
cover the engine, brush and script files without a display.
`noise_tests` compares the wrappers against native FastNoise2 and checks seed
handling, ranges, validation and thread isolation. Scripted brush tests compile
all shipped definitions and compare all eight replacements with native reference
tools, including selection weights, tapered strokes, continuous deltas,
Gaussian distance units, spectral fields and domain warping.
Shaders and all `.map`
files in `assets/colormaps` are copied at configuration time into the build's
`resources` directory. Editing a source shader/map triggers CMake regeneration
on the next build.
The executable uses the configured absolute resource path, independent of the
working directory. Relocatable installation/resource discovery is not yet
implemented; moving the build requires reconfiguration.
Brush TOML files are read at runtime from the configured absolute source path
`OSWALD_BRUSH_SCRIPT_DIR`, currently `assets/brushes/`, rather than the copied
shader/colourmap resources.

Qhull uses the reentrant static target `qhullstatic_r`. Its source directory
is excluded from the default build, so only linked targets are built, not its
standalone tools or unused C++ wrapper (which uses pre-C++20 constructor syntax).
The application and model remain C++20. The top-level build sets
`CMAKE_POLICY_VERSION_MINIMUM` to at least 3.10 for fetched dependencies that
declare older policy versions. Qhull is configured
before nanoflann so the dependencies share an uninstall target without a
name collision. Upstream examples and tests for nanoflann remain disabled.

### 10.3 Phase 1 Build and Run

The application creates an OpenGL 4.5 core-profile window and renders the
generated globe with its UI. Configure and build it with:

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

`BUILD_TESTING` defaults to ON. The focused test checks spherical-area
uniformity, Poisson minimum spacing, unit lengths and seeded reproducibility,
zero elevations, outward winding,
Euler counts, reciprocal twins, closed vertex fans, the spherical Delaunay
condition, brute-force nearest/arc query agreement, inclusive radius
boundaries, invalid inputs, and snapshot lifetime during concurrent reads
and regeneration. The `scale`-labeled test validates the maximum 1,000,000
points with maximum Poisson variability, including topology and a spatial query.

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

### 10.6 Phase 4 and 5 Verification

`File -> Open Raster...` (or Ctrl+Shift+O) opens an in-app file dialog. It starts in
the working directory and remembers the last browsed folder. Use Up to navigate
to the parent folder, or select a folder and choose Open Folder. Open loads the
selected image; Enter or double-click also opens folders and files. Refresh
reloads the folder listing; Cancel or
Escape closes the dialog without changing the raster. The listing shows folders
and supported PNG, JPEG, BMP, TGA, TIFF (`.tif`/`.tiff`), PSD, GIF, HDR, PIC,
and binary PPM/PGM/PNM images, including uppercase extensions.
Navigation and decoding errors appear in the dialog. TIFF uses libtiff's
RGBA decoder (unsigned integer imagery, including grayscale and RGB, converted
to 8-bit channels before Rec. 709 luminance); other formats use stb_image.
Single-band, stripped, black-is-zero Int16 and Float32 TIFFs are instead read
as direct elevations, retaining precision through resampling and grid sampling.
Samples must be finite and in [-32767, 32767]; there is no NoData masking.
TIFF orientation tags are honoured for unrotated images; rotated orientations,
unsupported numeric layouts, corrupt images, and TIFFs exceeding
32768 pixels per side or 128 Mi pixels are rejected explicitly.
Import treats every image as a full-world equirectangular raster:
GeoTIFF tags and sidecars are not used to crop or reproject.
Regional/projected GIS rasters must first be converted externally.
The selected image is resampled to a 2048 x 1024 equirectangular
luminosity raster. The app maps longitude periodically across the image seam,
clamps latitude at the poles, bilinearly samples each spherical vertex, and
maps luminosity to elevations in [-32767, 32767]. Applying new grid settings
resamples the currently loaded raster onto the regenerated grid. Raster
elevation replacement publishes a new immutable model snapshot and updates the
renderer elevation buffer with `glNamedBufferSubData` without re-uploading
positions or triangle indices.

The CPU-only `elevation_raster` test checks image decoding, 2:1 resampling,
Rec. 709 luminance, bilinear sampling, seam wrapping, pole clamping, elevation
ranges, grid population, and snapshot lifetime. The CPU `raster_file_browser`
test covers format filtering, directory ordering, navigation, refresh, and
failed navigation. The GPU `raster_file_dialog` test exercises Ctrl+Shift+O, folder
navigation, image selection, decoding errors, double-click, and cancellation
using a hidden OpenGL window. The GPU `globe_renderer` test also verifies
elevation-only buffer updates. Run the CPU checks with:

```bash
ctest --test-dir build/ninja -R '^(spherical_grid(_scale)?|camera_controller|elevation_raster|raster_file_browser|color_map)$' --output-on-failure
```

With a working display and OpenGL 4.5, run the dialog interaction check with
`ctest --test-dir build/ninja -R '^raster_file_dialog$' --output-on-failure`.

### 10.7 Phase 15: Project Metadata, Persistence, and Export

`SphericalGrid` owns project description, author, and last-saved UTC timestamp.
`File -> Planet... -> Metadata` edits description and author; its date field is read-only.
Changes are applied explicitly or discarded with Cancel. Only a successful project
save changes the date (`YYYY-MM-DDTHH:MM:SSZ`); exporting does not change it.

`File -> Open` (Ctrl+O), `Save` (Ctrl+S), and `Save As...` (Ctrl+Shift+S) manage
binary `.oswald` projects. Save asks for a filename when the project is untitled;
subsequent saves reuse the path. Save As and Export ask before overwriting an
existing destination. The in-app browser supports a typed folder path and Go,
Up/Refresh, folder double-clicks, and typed or selected filenames. Enter submits
the filename; Cancel/Escape closes the dialog. File and validation errors remain
visible without closing the dialog.

Project format version 2 uses the following ordered layout. Version 1 projects
remain readable and default to an Earth radius of 6371 km. All integers and
IEEE-754 floats are little-endian; strings are a uint32 byte count followed by
text bytes (no terminator). String fields are limited to 1 MiB and cannot contain
null characters. Booleans are uint32 values 0 or 1.

| Section | Contents |
| --- | --- |
| Header | Eight bytes `OSWALD\r\n`, uint32 version (2). |
| Metadata | Description, author, last-saved date strings. |
| Grid options | uint32 requested point count, float64 Poisson variability, uint32 seed, uint32 relaxation iterations, float64 planet radius in km (omitted in version 1). |
| Mesh header | uint32 actual vertex count, uint32 triangle-index count, selection-active boolean. |
| Vertices | Per vertex: float32 x/y/z, raw snapshot elevation, selection weight; uint32 refinement-created flag. |
| Faces | uint32 vertex indices, three per triangle, preserving exact order. |
| Layer header | uint32 layer count, uint32 active-layer index (`0xffffffff` for none). |
| Layers | In top-to-bottom order: uint32 layer type and data type; name, description, unselected colour-map name, selected colour-map name strings; uint32 blend mode; visibility boolean; uint32 value count followed by float32 raw values. |

Both versions accept only Raster (type 0) / Float32 (data type 0) elevation layers.
Blend mode identifiers are Overlay=0, Add=1, Subtract=2, Multiply=3, Minimum=4,
Maximum=5. Each layer has exactly one value per vertex. Limits are 1,000,000
vertices and 1,024 layers. Generation options retain their existing limits.
Loading validates sizes against remaining file bytes before allocation, unit
positions, finite/range-checked attributes, layer names and enum identifiers,
active-layer consistency, closed mesh topology, and absence of trailing data.
It reconstructs half-edges and the spatial index from saved geometry rather than
regenerating points, preserving adaptive refinements. Colour-map assets are
referenced by name, not embedded; the UI validates their availability before
replacing the current project. Opening clears prior undo history, brush and
selection tool state, and the previous raster-import source.

Save and export first write a temporary file in a uniquely created sibling
directory, close it successfully, then rename it over the destination. Failed
writes leave the existing destination and last-saved date intact; temporary
files are cleaned up. This guards against partial-file replacement, but does
not promise power-loss durability (no explicit disk fsync).

`File -> Export -> Export data...` samples cumulative elevation through the active layer,
excluding layers above it and retaining contributions from visible layers below.
Export does not mutate layer data or selection. PNG, BMP, TGA, and TIFF are supported,
with default size 2048 x 1024 and selectable
2:1 sizes through 8192 x 4096. Pixel centres span longitudes -180 to +180 degrees
left-to-right and latitudes +90 to -90 degrees top-to-bottom, matching import.
Each pixel uses the nearest mesh site. PNG/BMP/TGA and the optional 8-bit TIFF
map elevations linearly from [-32767, 32767] to [0, 255], about 257 elevation
units per gray level. TIFF offers three sample representations in the export dialog:

- **8-bit grayscale:** the existing luminosity mapping, useful for image rasters.
- **Signed 16-bit elevations:** direct values rounded to whole units, with no offset/scale.
- **Float32 elevations (default):** direct values, preserving fractional elevations
  at each sampled mesh site without quantization.

Project saves preserve all mesh/layer data rather than a sampled raster. Export is
disabled when no layer is active.

TIFF exports use lossless LZW compression, embed GeoTIFF WGS84 geographic,
pixel-is-area keys and the global transform, and also write two georeferencing
sidecars: `name.tfw` contains the six world-file coefficients
`360/width, 0, 0, -180/height, -180+180/width, 90-90/height` (pixel-centre
coordinates); `name.tif.aux.xml` (or `name.tiff.aux.xml`) declares WGS 84
(EPSG:4326) in GDAL PAM format with longitude-first raster coordinates.
Keep both sidecars beside the image when importing it into QGIS. GDAL
recognizes the outer extent as [-180, 180] x [-90, 90], north-up.
Embedded georeferencing allows QGIS/GDAL to place the TIFF even without sidecars.
QGIS identifies the band as Byte, Int16, or Float32 according to the export choice;
Int16/Float32 values are actual elevations, not unsigned brightness values needing
rescaling. Display contrast/stretch is independent of stored elevation precision.
Overwrite confirmation covers existing sidecars even if the image is absent.
All three files are staged and closed before replacement; each replacement
is atomic, but the three-file set is not a single filesystem transaction.

`File -> Export -> Export Image...` exports a colourmapped RGB map rather than
numeric data. It shares the same cumulative-through-active-layer data,
north-up global equirectangular extent, nearest-site sampling, resolution
choices, file formats (PNG/BMP/TGA/TIFF), atomic staging, and overwrite checks.
The active layer's colourmap is sampled using clamped, piecewise-linear stop
interpolation, matching the globe shader. Output uses three 8-bit RGB channels;
TIFF image export always writes unsigned RGB bytes, independently of the data
export precision choice, and includes the same GeoTIFF tags and sidecars.

The image dialog offers **Include selection highlighting** (off by default).
When enabled and a selection is active, it blends the active layer's two
colourmaps using the sampled site's selection weight, exactly as the globe
shader does. Without the option or without an active selection, the normal
colourmap is used throughout. Selection, elevations, and layer metadata are
never changed by export. Map images omit globe lighting, graticules, and UI
overlays so they can be used as artwork or GIS raster layers.
Both export submenu actions are disabled without an active layer.

The CPU `project_file` test covers exact multi-layer/adaptive-mesh round trips,
metadata and dates, inactive projects, old-reader lifetime, failed-write safety,
and malformed/truncated/version-mismatched files. `elevation_raster` additionally
tests all export formats, actual default image dimensions, signed endpoint
mapping, geographic orientation, cumulative-layer semantics, TIFF RGB decoding
and orientation tags, georeferencing coefficients/CRS, and sidecar failure paths.
It also verifies raw TIFF sample tags/values for all three precisions, exact
fractional Float32 round trips, integer rounding, signed endpoints, and invalid samples.
RGB export tests compare every output pixel against cumulative data and
colourmap interpolation in each format, including selection weights 0, 0.25,
0.5, and 1, inactive selection, geographic orientation, and RGB TIFF tags.
The GPU-backed `raster_file_dialog` test also exercises metadata editing,
untitled Save, repeated Save, Save As, overwrite confirmation/cancellation, invalid and valid
project Open, undo-history clearing, both Export submenu actions, RGB selection
highlighting, TIFF precision selection, and image/sidecar overwrite handling.

### 10.8 Noise, Scripting, and Brush Library Verification

Build and run the focused CPU suites plus the GPU-backed UI/dialog suite with:

```bash
cmake --build build/ninja --target brush_tool_tests noise_tests \
    scripted_brush_tool_tests script_engine_tests brush_script_file_tests ui_manager_tests
ctest --test-dir build/ninja \
    -R '^(brush_tool|noise|scripted_brush_tool|script_engine|brush_script_file|raster_file_dialog)$' \
    --output-on-failure
```

The `ui_manager_tests` executable is registered with CTest as `raster_file_dialog`
and requires a working display/OpenGL 4.5 context. In addition to file workflows,
it covers data-driven Brushes entries, generated parameter controls, Gaussian
kilometre/mile display, external library edits and refresh, error reporting,
and preservation of Custom's editor/compiled state. Omit `raster_file_dialog`
from the expression for CPU-only validation.
Selection-fill tests compare all eight shipped scripts with whole-globe native
reference applications, with and without selection feathering. They also verify
footprint-independent coverage, fill-specific script inputs, current UI parameters,
one-second continuous fills, saturation, failed-script atomicity, and single-action
undo/redo.
