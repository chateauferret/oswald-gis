### Project Roadmap: oswald-gis

#### Implementation Roadmap

Phases 1-18 record the original implementation milestones. Phase 19 describes
the current scripting-based brush architecture and supersedes the earlier
native brush registration and parameter UI. See
[the architecture specification](./oswald-gis-mvp-architecture.md) for component
boundaries, script format, and build/test details.

1. **Phase 1: Build System & Context Setup**  
   Configure CMake with FetchContent for GLFW, GLAD (OpenGL 4.5 Core), GLM, Dear ImGui, and nanoflann. Verify window creation and basic rendering loop.

2. **Phase 2: Model Implementation**  
   Implement seeded area-uniform Poisson-disk sampling on $S^2$, spherical Delaunay triangulation, and half-edge graph structures. Integrate the nanoflann 3D k-d tree adaptor.

3. **Phase 3: Render Viewport & Shaders**  
   Set up OpenGL DSA buffer lifecycle (`glCreateBuffers`, `glNamedBufferData`), compile the GLSL hypsometric elevation shader pair, and establish orthographic camera projection.

4. **Phase 4: Controller & UI Interactivity**  
   Implement pitch-clamped mouse drag rotation, scroll-wheel zoom, double-click raycast re-centering, and the Dear ImGui `File -> Settings...` dialog.

5. **Phase 5: Raster Ingestion & Verification**  
   Decode and resample equirectangular images via `File -> Open Raster...`, map luminosity to elevation range $[-32,767, +32,767]$, and upload sampled elevations to the model and GPU.

6. **Phase 6: Undo/Redo Architecture & Command Pattern**  
   Build a C++ `UndoStack` managing atomic command objects supporting `redo()` and `undo()`. Wrap brush strokes, selection changes, filter operations, and raster imports into reversible commands now or whenever they are implemented in the upcoming phases. Add `Edit -> Undo` / `Redo` options to the main menu bar with standard keyboard shortcuts (`Ctrl+Z`, `Ctrl+Y`).

7. **Phase 7: Interactive Brush & Sculpting Engine**  
   Implemented a modular C++ `BrushTool` base class and tool manager, originally with separate native Elevation and Raise/Lower subclasses. These Tools menu entries now use TOML-defined `ScriptedBrushTool` instances (Phase 19.5); native implementations remain as reference implementations for behavior tests. Both utilize `nanoflann` k-d tree radius searches, geodesic distances (in km/miles), and shared falloff calculations. The parameter panel exposes operation, outer radius, and falloff (percentage). Elevation additionally has a value slider and no intensity control; Raise/Lower has an intensity slider and no value slider. The outer radius is the fixed brush boundary; the inner radius is the outer radius multiplied by one minus the falloff percentage. At 0% the profile is a cylinder with no taper; at 100% the inner radius is zero and the profile is a cone. Elevation operations are one-shot: each click or drag is evaluated against pre-stroke terrain, so holding or retracing does not build up the effect; each vertex keeps the strongest falloff-weighted result reached during the stroke. Raise/Lower accumulates while held, with intensity controlling its per-second speed. The default speed at 100% intensity is 8192 elevation units per second, configurable in File -> Settings; lower intensities scale it linearly. Results clamp to [-32767, +32767]. Changing only brush speed preserves terrain and undo history.

8. **Phase 8: Selection System & Per-Vertex Masking**  
   Add a per-vertex selection weight channel ($0.0$ to $1.0$) to the spherical vertex layout. Implement geodesic radius and polygon selection tools on $S^2$, along with selection clearing and inversion operations. Selection tool works the same way as the brushes, with two concentric circles and tapering / feathering, and the values are applied not to the data layers but to the selection channel. With a selection tool active, an unshifted left-click on empty viewport space clears the selection; ordinary menu and widget interactions and clicks outside the viewport leave it unchanged. Update GLSL fragment shaders to render a semi-transparent selection mask overlay, with opacity according to the selection value at each data point. Brush tools and filters are applied to the currently active layer only and, if there is a selection, their effect is multiplied by the selection value of each data point. The selection tools are accessed through a menu item under a new "Select" item on the main menu.

9. **Phase 9: Selection-Weighted Global Filter & Plugin Framework**  
   Establish a C++ `Filter` plugin interface and registry for global and selection-bounded terrain processing. Implement the Fill filter with an operation dialog and a value range of [-32767, +32767]. Support Replace (blend toward the value), Raise (add the value), Lower (subtract the value), Minimum, and Maximum. With no active selection, apply to every vertex; otherwise blend each operation by its per-vertex selection weight. Clamp results to the elevation range and record each filter application as one undoable command.

10. **Phase 10: Advanced GLSL Shading & Dynamic Color Legends**  
   Enhance GLSL shaders with an object-space antialiased latitude/longitude graticule pass (highlighting the Equator and Prime Meridian), atmospheric rim glow, and dynamic 1D Color Lookup Table (LUT) textures. Have the graticule density adapt to the zoom level so that the number of parallels and meridians displayed at any time remains approximately constant. Add runtime legend (colourmap) switching under `View -> Legend` for both selected and unselected data points. 

11. **Phase 11: Viewport HUD, Live Tooltips & Navigation Extras**  
   Add floating on-screen viewport action buttons (camera reset, auto-spin toggle, wireframe toggle, graticule toggle). Implement real-time cursor hover inspection displaying unprojected geographic coordinates ($\text{Lat}^\circ, \text{Lon}^\circ$) and interpolated elevation in physical units ($m$/$ft$) via `nanoflann` k-d tree lookups.

12. **Phase 12: Adaptive Regional Detail & Mesh Refinement**  
   Implemented undoable `Mesh -> Refine Selection` and `Mesh -> Coarsen Flat Selection` actions. Refinement inserts spherical face-centroid points in selected faces, skipping fac es less than half the selected area's area-weighted median face size so an already-dense seam is not over-refined. Tiny selections can seed incident faces. It uses incremental hull-cavity insertion for small batches and a single Delaunay rebuild for larger batches; elevation and selection weight are interpolated barycentrically. Coarsening first removes eligible refinement-created sites to reverse local refinement; otherwise it preserves the selected-region seam, replaces flat interior sites with approximately half as many area-weighted Poisson-spaced spherical samples from fully selected faces, interpolates attributes, and applies two constrained spherical Lloyd-relaxation passes. A 1.35-spacing boundary buffer avoids excess density near the seam. Both operations preserve closed manifold topology and immutable snapshots.

13. **Phase 13: Data layer architecture**
   Implemented an abstract `Layer` metadata interface and raster-layer value storage, with the grid-owned `ElevationLayer` providing the current Float32 raster and remaining synchronized with immutable mesh snapshots. Layers carry a name, description, type (Elevation; other raster and vector types reserved for later), blend mode, colourmap name, and datatype. The elevation layer is active by default. Selection remains a separate per-vertex channel and is not stored as a layer.

14. **Phase 14: Layer management UI**
   Implemented a Layers panel with independent Elevation raster values, active-layer switching, visibility, inline rename, drag-and-drop ordering, and confirmed deletion (the final layer is protected). New layers can be named and created with selected/unselected colourmaps and become active above the previous active layer. Per-layer properties include all six blend modes, editable descriptions, gradient previews and selectors for both legends, and a value-frequency histogram. Visible layers combine elevation values bottom-to-top before applying the topmost visible layer's colourmaps, rather than blending RGB colours. Subtract means lower minus upper, and each operation clamps to [-32767, 32767]. Edits target the active layer's raw values; activating a hidden layer makes it visible, and hiding the active layer deactivates it. The single hover readout shows location and the cumulative elevation through the active layer (including visible layers below it and brush previews, but excluding layers above it). Selection remains a separate shared channel.

15. **Phase 15: Save, Open and Export**

   15.1. Implemented File -> Planet... with Geometry and Metadata tabs. Geometry holds the shared planet radius and grid point count; Metadata holds a multiline project description, author field, and read-only UTC last-saved date. Apply commits edits across both tabs; Cancel discards them. Successful saves update the date and persist metadata and geometry. Version 2 projects store radius; version 1 projects load with the default Earth radius.

   15.2. Implemented a versioned, little-endian `.oswald` binary project format preserving exact point locations, triangle geometry, adaptive refinement provenance, selection, all raw layer values and properties, stack order, active layer (or none), and metadata. File -> Open (Ctrl+O), Save (Ctrl+S), and Save As... (Ctrl+Shift+S) use an in-app file browser with filename/folder entry, navigation, errors, and overwrite confirmation. Failed reads do not replace the current project; writes use a temporary sibling file and atomic rename. Open reconstructs topology/spatial indexing and clears old undo/tool/import state. File -> Export writes cumulative elevations through the active layer as north-up, equirectangular PNG/BMP/TGA/TIFF. PNG/BMP/TGA use 8-bit grayscale, mapping [-32767, 32767] to [0, 255]. TIFF offers user-selectable 8-bit grayscale, direct signed 16-bit elevations rounded to whole units, or direct Float32 elevations (default, preserving fractional values). TIFF embeds WGS 84 (EPSG:4326) GeoTIFF georeferencing and also includes a `.tfw` world file and a `.tif.aux.xml`/`.tiff.aux.xml` CRS sidecar for QGIS; overwrite checks include sidecars. Export defaults to 2048 x 1024 with selectable 2:1 resolution up to 8192 x 4096 and nearest-site sampling. Raster import remains available as Open Raster... (Ctrl+Shift+O), including unsigned integer TIFF imagery and stripped single-band Int16/Float32 TIFF elevations via libtiff, preserving numeric precision. Imports assume global equirectangular rasters, without regional reprojection or NoData masking. CPU round-trip/corrupt-file/export tests and GPU-backed dialog interaction tests cover both parts, including sample precision and sidecar overwrite handling.

   Export is now a File submenu with **Export data...** for numeric/grayscale output and **Export Image...** for RGB artwork through the active layer's colourmap. Both sample cumulative data through the active layer and support PNG/BMP/TGA/TIFF at the same global equirectangular resolutions. Image export offers optional selection highlighting using the layer's two colourmaps and selection weights, matching the globe; it omits lighting/graticules/UI overlays. RGB TIFF always uses three unsigned 8-bit channels with the same GeoTIFF georeferencing and sidecars, independent of the numeric TIFF precision choice. Neither export mutates model data or selection.

16. **Phase 16: Additional Tools**
   Originally implemented three one-shot native `BrushTool` subclasses; their menu entries now use TOML scripts and generated parameter controls (Phase 19.5). Each tool edits the active layer, respects the shared brush radius, falloff, and selection weight, and records the completed stroke as one undoable action:

   16.1. **Simplex noise.** Captures operation (Replace, Add, Subtract, Minimum, or Maximum), Between/And output range, unsigned 64-bit random seed, frequency, lacunarity, persistence, octave count, and billowed mode. The seed can be entered numerically or randomised from the panel. Deterministic fractal simplex noise is sampled in 3D using each spherical vertex position, normalized across octaves, and mapped into the ordered range values. Billowed mode takes the absolute noise and rescales it across the selected range.

   16.2. **Gaussian blur.** Captures a geodesic input range and sigma in kilometres or miles. Each affected vertex is blurred from the pre-stroke active-layer values using a normalized Gaussian-weighted neighborhood, avoiding order-dependent in-place smearing.
   
   16.3. **Clamp.** Captures minimum and maximum values in the active layer's Float32 datatype and clamps affected values to the ordered range.

17. **Phase 17: Projections**
   Implemented `View -> Projection` choices for Orthographic and Equirectangular views. Equirectangular mode renders the globe mesh as a flat longitude/latitude map, unwraps and duplicates seam-crossing faces and wire edges for horizontal wrapping, and leaves the poles vertically bounded. Mouse dragging pans with longitude wrapping and latitude clamping, scrolling scales the map, double-click recenters, and brush/selection/hover picking remains geographic in either projection.

18. **Phase 18: More Noises**

   Implemented three deterministic one-shot noise brushes. They share the Simplex operation/range controls, per-vertex selection weighting, falloff, undoable stroke handling, and optional fractal 3D domain warping with configurable frequency, strength, octaves, lacunarity, and persistence.

   18.1. **Value Noise.** Uses FastNoise2 Value with cubic Hermite interpolation and native fBm. Supports frequency, amplitude, lacunarity, persistence, octaves, and seed.

   18.2. **Ridge Noise.** Uses FastNoise2 FractalRidged with a Simplex source, with configurable frequency, octaves, lacunarity, persistence, and seed.

   18.3. **Power-Law Spectral Noise.** Generates a seeded 3D white-noise field, filters its Fourier coefficients by a power-law spectral exponent, and samples the inverse-transformed periodic field. Grid resolution, frequency, and spectral exponent are configurable; the expensive field is cached until its seed, resolution, or exponent changes.
   
19. **Phase 19: Scripted Brushes**

   Integrated the exprtk expression library behind a `ScriptEngine` manager. The manager binds numeric/string variables, constants, and C++ functions into a shared symbol table, and compiles scripts with line and column diagnostics. It provides seeded `simplex`, `valuenoise`, `fbm`, `billow`, `valuefbm`, `ridged`, and `spectral` functions, and guards against runaway loops. `ScriptedBrushTool` additionally binds neighbourhood sampling through `gaussian`.

   19.1. **Custom Brush.** The Custom tool (formerly Scripted Brush) evaluates a user script for each affected vertex. Scripts can read the vertex's position, latitude/longitude, current elevation, weight, distance from the brush centre, brush radius, output range, elapsed seconds per dab (`elapsed`), and configured Raise/Lower speed (`rate`).
   - In the default `application="noise"` mode, the result is either normalised, so [-1, 1] maps to the Between/And range, or treated as an elevation clamped to that range. Direct elevation and delta modes are described in Phase 19.5.
   - Noise-mode brushes share operation, seed, range, and domain-warp controls; direct-mode brushes retain only the common radius/falloff/distance-unit controls plus their declared parameters.
   - The dockable panel has a multiline script editor with Compile, Revert, and Default buttons, plus a script reference.
   - Compile errors keep the previous script active. Runtime errors leave the layer unchanged and are reported in the panel.

   19.2. **Saved Brush Scripts.** Scripts are saved and loaded as TOML files with a name, description, optional output mode (`normalized` or `elevation`) and script, like `assets/brushes/fbm.toml`.
   - The Custom brush panel lists the `assets/brushes/` library folder, showing each script's description. Save to library asks before replacing an existing file.
   - Import... and Export... read and write script files anywhere through the file browser.
   - Invalid library files are reported and skipped.

   19.3. **Script Parameters.** Optional `[[parameter]]` tables define named `float`, `integer`, and `choice` inputs. Loading a script replaces all custom controls and initializes them to the file's defaults; scripts without parameters clear them. Common radius/falloff/distance-unit controls remain available; seed, output range, operation, and warping are shown for noise-mode scripts.
   - Numeric parameters require `control` (`slider`, `spin`, or `text`), `min`, `max`, and `default`. Bounds and defaults accept numeric strings or TOML numbers. Integers use 32-bit bounds; floats optionally specify `scale="linear"` (default) or `scale="log"` with strictly positive bounds. Float slider bounds must fit within half the maximum finite double magnitude, as required by the widget.
   - Choice parameters require a nonempty, unique string `choices` array and a matching string `default`, and render as a dropdown. Scripts read the selected string by name, e.g. `mode == 'one'`.
   - Names must be valid, unique script identifiers and cannot shadow built-in inputs, functions, constants, or reserved words. Invalid definitions/defaults are reported instead of partially loading.
   - Define root keys, including `script`, **before** the first `[[parameter]]` table (see `assets/brushes/fbm.toml`). Under TOML rules, keys following a table header belong to that table.
   - Parameter definitions are edited externally and reloaded with Load; the UI still edits and compiles the script. Save/export retains definitions and original defaults, not the current control values. Failed compilation preserves the previous compiled script and its parameters; Revert restores that active script and its controls.

   19.4. **FastNoise2 Backend.** CMake fetches FastNoise2 v1.1.1 (and its pinned FastSIMD dependency), with tools/tests disabled and strict floating point enabled for consistent SIMD results. `Noise.cpp` reuses thread-local native Simplex, Value, White, FractalFBm, and FractalRidged nodes. Native fractal sums are divided by total octave amplitude to retain the application's [-1, 1] output contract. Domain warping uses the new Simplex backend; the existing spectral FFT/filter now receives FastNoise2 White samples.
   - This intentionally changes generated patterns for existing seeds. Both halves of the application's 64-bit seed are folded into FastNoise2's 32-bit seed (`low32 + high32 * 0x9e3779b9`, modulo 2^32); collisions are unavoidable. Existing terrain and saved projects are unchanged until noise is applied again.
   - Value noise now always uses cubic interpolation; the interpolation selector is removed. Native ridge noise replaces the previous custom feedback algorithm; ridge-offset/gain-exponent controls are removed.
   - Scripts use `ridged(x, y, z, frequency, lacunarity, persistence, octaves)`, matching `fbm`/`valuefbm`. Existing nine-argument ridge calls must drop their last two arguments; old calls produce compile diagnostics rather than silently ignoring them.
   - Noise coordinates use float precision. Inputs must be finite; fractal frequency, lacunarity, and persistence must be nonnegative, with 1-16 octaves and float-representable lacunarity/persistence. Sample coordinates must stay within +/-1e8 at every octave, and total octave amplitude must fit within half the maximum finite float. Unsupported combinations report an error before a stroke changes elevations.

   19.5. **TOML-Driven Brushes.** All eight formerly built-in brushes are now shipped as scripts in `assets/brushes/`: Elevation, Raise/Lower, Simplex, Value, Ridge, Power-Law Spectral Noise, Gaussian Blur, and Clamp. The Brushes menu (formerly Tools) is built from valid, compilable files in that folder, alphabetically by their `name`, followed by Custom. Existing `fbm.toml` and `constant.toml` also appear as ordinary library tools. No brush is active until selected.
   - Add/edit/remove TOML files externally, then use Brushes -> Refresh brushes (or Refresh in Custom). Save to library also refreshes the menu. Refresh rebuilds library tools with their declared defaults, but preserves the Custom editor, its last compiled script, and its values. Library tools have independent controls, without the Custom script editor.
   - Invalid TOML, invalid scripts, duplicate names, and the reserved name `Custom` are skipped with diagnostics under Brushes -> Brush errors and in the Custom library panel. A broken or empty library still leaves Custom usable. Refresh is disabled during an active stroke.
   - Root `application` is optional: `"noise"` (default) retains shared operation, seed, output-range and domain-warp controls; `"elevation"` treats the script result as a height, clamps it to [-32767, 32767], and blends by falloff/selection; `"delta"` adds the script result multiplied by falloff/selection and then clamps. Direct modes ignore noise settings. `continuous=true` accumulates while held; omitted/false retains one-shot stroke semantics. These fields survive Custom load/save/export but are edited externally.
   - Raise/Lower returns `(direction * rate * intensity / 100 * elapsed)` in continuous delta mode, preserving per-second speed and saturation at feathered edges. Do not multiply by `weight` again: application modes apply it.
   - `gaussian(range, sigma, unit)` samples a Gaussian-weighted spherical neighbourhood of the current vertex from immutable pre-dab elevations; unit 0 means kilometres, 1 means miles. `spectral(x,y,z,frequency,exponent,resolution)` samples the existing cached FFT-filtered field with power-of-two resolution 8-128. Both report invalid inputs without partially changing a dab.
   - Parameters may specify an optional display `label`. A numeric float parameter can specify `units="<choice parameter name>"`, referencing a choice with `["Kilometres", "Miles"]`; stored/script values and bounds remain kilometres while the UI converts display/input values. Gaussian Blur uses this to switch distance units without changing the physical blur range or sigma.
   - Clamp accepts independently edited bounds in either order and orders them in its script. Elevation operation, Raise/Lower direction, billowed mode, and spectral resolution are choice parameters rather than hard-coded widgets.

   19.6. **Fill Selection.** Every brush parameters panel, including Custom, has a **Fill selection** button. It applies the brush once using its current compiled script and parameter values over the selection, accounting for selection feathering exactly once and ignoring the brush footprint's radius/radial falloff. With no active selection it fills the entire active layer; an empty active selection changes nothing. Continuous brushes apply one second at their current speed/intensity. Scripts receive `distance=0`, the configured `radius`, and `elapsed=1`. Each changed fill is one undoable action, leaves selection and other layers untouched, and reports script failures without partial writes. The button is disabled without an active layer or during a brush stroke.
