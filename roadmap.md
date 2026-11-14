### Project Roadmap: oswald-gis

#### Implementation Roadmap

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
   Design a modular C++ `BrushTool` base class and a tool manager. Provide separate `ElevationBrushTool` (Replace, Minimum, Maximum) and `RaiseLowerBrushTool` (Raise, Lower) subclasses, selectable from the Tools menu. Both utilize `nanoflann` k-d tree radius searches, geodesic distances (in km/miles), and shared falloff calculations. Build a dockable Dear ImGui tool options panel for the chosen operation, outer radius, and falloff (percentage). The Elevation Brush additionally has a value slider and no intensity control; the Raise/Lower Brush has an intensity slider (percentage) and no value slider. The outer radius is the fixed brush boundary; the inner radius is the outer radius multiplied by one minus the falloff percentage. At 0% the inner and outer radii coincide and the profile is a cylinder with no taper; at 100% the inner radius is zero and the profile is a cone. Elevation Brush operations are one-shot: each click or drag applies the operation once, evaluated against the pre-stroke terrain, so holding the button or retracing the stroke does not build up the effect; each vertex keeps the strongest falloff-weighted result reached during the stroke. While the Raise/Lower Brush is held, intensity controls its per-second speed. The default Raise/Lower speed at 100% intensity is 8192 elevation units per second, configurable in File -> Settings; lower intensities scale this speed linearly. Clamp elevations to [-32767, +32767]. Changing only the brush speed must preserve terrain and undo history. The outer falloff linearly blends each operation's result at the inner circle back to the existing value at the outer edge.

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

   15.1. Implemented File -> Metadata... with a multiline project description, author field, and read-only UTC last-saved date. Apply commits metadata edits; Cancel discards them. Successful saves update the date and persist all three fields.

   15.2. Implemented a versioned, little-endian `.oswald` binary project format preserving exact point locations, triangle geometry, adaptive refinement provenance, selection, all raw layer values and properties, stack order, active layer (or none), and metadata. File -> Open (Ctrl+O), Save (Ctrl+S), and Save As... (Ctrl+Shift+S) use an in-app file browser with filename/folder entry, navigation, errors, and overwrite confirmation. Failed reads do not replace the current project; writes use a temporary sibling file and atomic rename. Open reconstructs topology/spatial indexing and clears old undo/tool/import state. File -> Export writes cumulative elevations through the active layer as north-up, equirectangular PNG/BMP/TGA/TIFF. PNG/BMP/TGA use 8-bit grayscale, mapping [-32767, 32767] to [0, 255]. TIFF offers user-selectable 8-bit grayscale, direct signed 16-bit elevations rounded to whole units, or direct Float32 elevations (default, preserving fractional values). TIFF embeds WGS 84 (EPSG:4326) GeoTIFF georeferencing and also includes a `.tfw` world file and a `.tif.aux.xml`/`.tiff.aux.xml` CRS sidecar for QGIS; overwrite checks include sidecars. Export defaults to 2048 x 1024 with selectable 2:1 resolution up to 8192 x 4096 and nearest-site sampling. Raster import remains available as Open Raster... (Ctrl+Shift+O), including unsigned integer TIFF imagery and stripped single-band Int16/Float32 TIFF elevations via libtiff, preserving numeric precision. Imports assume global equirectangular rasters, without regional reprojection or NoData masking. CPU round-trip/corrupt-file/export tests and GPU-backed dialog interaction tests cover both parts, including sample precision and sidecar overwrite handling.

   Export is now a File submenu with **Export data...** for numeric/grayscale output and **Export Image...** for RGB artwork through the active layer's colourmap. Both sample cumulative data through the active layer and support PNG/BMP/TGA/TIFF at the same global equirectangular resolutions. Image export offers optional selection highlighting using the layer's two colourmaps and selection weights, matching the globe; it omits lighting/graticules/UI overlays. RGB TIFF always uses three unsigned 8-bit channels with the same GeoTIFF georeferencing and sidecars, independent of the numeric TIFF precision choice. Neither export mutates model data or selection.

16. **Phase 16: Additional Tools**
   Implemented three one-shot `BrushTool` subclasses with dockable parameters. Each tool edits the active layer, respects the shared brush radius, falloff, and selection weight, and records the completed stroke as one undoable action:

   16.1. **Simplex noise.** Captures operation (Replace, Add, Subtract, Minimum, or Maximum), Between/And output range, unsigned 64-bit random seed, frequency, lacunarity, persistence, octave count, and billowed mode. The seed can be entered numerically or randomised from the panel. Deterministic fractal simplex noise is sampled in 3D using each spherical vertex position, normalized across octaves, and mapped into the ordered range values. Billowed mode takes the absolute noise and rescales it across the selected range.

   16.2. **Gaussian blur.** Captures a geodesic input range and sigma in kilometres or miles. Each affected vertex is blurred from the pre-stroke active-layer values using a normalized Gaussian-weighted neighborhood, avoiding order-dependent in-place smearing.
   
   16.3. **Clamp.** Captures minimum and maximum values in the active layer's Float32 datatype and clamps affected values to the ordered range.

17. **Phase 17: Projections**
   Implemented `View -> Projection` choices for Orthographic and Equirectangular views. Equirectangular mode renders the globe mesh as a flat longitude/latitude map, unwraps and duplicates seam-crossing faces and wire edges for horizontal wrapping, and leaves the poles vertically bounded. Mouse dragging pans with longitude wrapping and latitude clamping, scrolling scales the map, double-click recenters, and brush/selection/hover picking remains geographic in either projection.

18. **Phase 18: More Noises**

   Implemented three deterministic one-shot noise brushes. They share the Simplex operation/range controls, per-vertex selection weighting, falloff, undoable stroke handling, and optional fractal 3D domain warping with configurable frequency, strength, octaves, lacunarity, and persistence.

   18.1. **Value Noise.** Hashes scalar values at 3D lattice corners and interpolates them with selectable linear, cubic Hermite, or quintic Hermite curves. Supports frequency, amplitude, lacunarity, persistence, octaves, and seed.

   18.2. **Ridge Noise.** Generates ridged multifractal terrain from multi-octave coherent noise, with configurable octaves, lacunarity, persistence, ridge offset, and gain exponent.

   18.3. **Power-Law Spectral Noise.** Generates a seeded 3D white-noise field, filters its Fourier coefficients by a power-law spectral exponent, and samples the inverse-transformed periodic field. Grid resolution, frequency, and spectral exponent are configurable; the expensive field is cached until its seed, resolution, or exponent changes.