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
   Implemented undoable `Mesh -> Refine Selection` and `Mesh -> Coarsen Flat Selection` actions. Refinement inserts spherical face-centroid points in selected faces, skipping faces less than half the selected area's area-weighted median face size so an already-dense seam is not over-refined. Tiny selections can seed incident faces. It uses incremental hull-cavity insertion for small batches and a single Delaunay rebuild for larger batches; elevation and selection weight are interpolated barycentrically. Coarsening first removes eligible refinement-created sites to reverse local refinement; otherwise it preserves the selected-region seam, replaces flat interior sites with approximately half as many area-weighted Poisson-spaced spherical samples from fully selected faces, interpolates attributes, and applies two constrained spherical Lloyd-relaxation passes. A 1.35-spacing boundary buffer avoids excess density near the seam. Both operations preserve closed manifold topology and immutable snapshots.

13. **Phase 13: Data layer architecture**
   Implemented an abstract `Layer` metadata interface and raster-layer value storage, with the grid-owned `ElevationLayer` providing the current Float32 raster and remaining synchronized with immutable mesh snapshots. Layers carry a name, description, type (Elevation; other raster and vector types reserved for later), blend mode, colourmap name, and datatype. The elevation layer is active by default. Selection remains a separate per-vertex channel and is not stored as a layer.

14. **Phase 14: Layer management UI**
   Implemented a Layers panel with independent Elevation raster values, active-layer switching, visibility, inline rename, drag-and-drop ordering, and confirmed deletion (the final layer is protected). New layers can be named and created with selected/unselected colourmaps and become active above the previous active layer. Per-layer properties include all six blend modes, editable descriptions, gradient previews and selectors for both legends, and a value-frequency histogram. Visible layers combine elevation values bottom-to-top before applying the topmost visible layer's colourmaps, rather than blending RGB colours. Subtract means lower minus upper, and each operation clamps to [-32767, 32767]. Edits target the active layer's raw values; activating a hidden layer makes it visible, and hiding the active layer deactivates it. The single hover readout shows location and the cumulative elevation through the active layer (including visible layers below it and brush previews, but excluding layers above it). Selection remains a separate shared channel.

15. **Phase 15: Save, Open and Export**
   Design a sutiable binary file format for the model, to include grid geometry and point locations, contents of all layers and metadata (metadata will be added in a later phase). Place "Open", "Save", "Save As..." and "Export" menu items on the "File" menu. On saving a project, have the user select a filepath in an Open File dialog if necessary and serialise the project data to the file, On opening, reconstruct the grid, data and metadata from a selected file. On export, save the elevation data as an equirectangula grayscale image (reversing the import process) in a selected file path and format. Note: as other layer types are added in later phases, we will allow the user to select one or more files to export. 

16. **Phase 16: Additional Tools**
   Add `BrushTool` subclasses which perform undoable actions as follows, in each case capturing the parameters shown in a dockable panel:

      16.1. Simplex noise. Capture frequncy (float), lacunarity (float), persistence (float), number of octaves (integer), whether billowed (boolean). Noise is computed in 3D space for the 3D position of each point in the spherical grid so that it is not distorted on the sphere. If the billowed flag is true, the absolute value of the noise is taken and then scaled to its full range.

      16.2. Gaussian blur. Capture range (kilometres / miles) and sigma (float). Apply the gaussian blur taking all the data points within the given range as inpu and using the supplied sigma value.
   
      16.3. Clamp. Capture maximum and minimum values (same datatype as the active layer). Replace values above the maximum with the maximum value, and values below the minimum with the minimum value.

17. **Phase 17: Projections**
   Add a menu item "View -> Projection" which allows the projection of the map/globe view to be changed. For now, support projections "Orthographic" (the current globe view) and "Equirectangular". If Equirectangular is selected, display the data not as an interactive globe, but as an interactive flat map in the Equirectangular projection and allow it to be panned and scaled in the same way as the globe. Allow it to wrap around horizontally, but not vertically. 
