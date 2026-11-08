### Project Roadmap: oswald-gis

#### Implementation Roadmap

1. **Phase 1: Build System & Context Setup**  
   Configure CMake with FetchContent for GLFW, GLAD (OpenGL 4.5 Core), GLM, Dear ImGui, and nanoflann. Verify window creation and basic rendering loop.

2. **Phase 2: Model Implementation**  
   Implement `SphericalFibonacciGrid` point distribution math, spatial jittering, and half-edge graph structures. Integrate the nanoflann 3D k-d tree adaptor.

3. **Phase 3: Render Viewport & Shaders**  
   Set up OpenGL DSA buffer lifecycle (`glCreateBuffers`, `glNamedBufferData`), compile the GLSL hypsometric elevation shader pair, and establish orthographic camera projection.

4. **Phase 4: Controller & UI Interactivity**  
   Implement pitch-clamped mouse drag rotation, scroll-wheel zoom, double-click raycast re-centering, and the Dear ImGui `File -> Settings...` dialog.

5. **Phase 5: Raster Ingestion & Verification**  
   Decode and resample equirectangular images via `File -> Open Raster...`, map luminosity to elevation range $[-32,767, +32,767]$, and upload sampled elevations to the model and GPU.

6. **Phase 6: Undo/Redo Architecture & Command Pattern**  
   Build a C++ `UndoStack` managing atomic command objects supporting `redo()` and `undo()`. Wrap brush strokes, selection changes, filter operations, and raster imports into reversible commands now or whenever they are implemented in the upcoming phases. Add `Edit -> Undo` / `Redo` options to the main menu bar with standard keyboard shortcuts (`Ctrl+Z`, `Ctrl+Y`).

7. **Phase 7: Interactive Brush & Sculpting Engine**  
   Design a modular C++ `BrushTool` base class and a tool manager. Implement an interactive elevation editing tool (with operations to replace value, raise, lower, minimum, maximum) utilizing `nanoflann` k-d tree radius searches, calculating geodesic distances (in km/miles) and falloff curves. Build a dockable Dear ImGui tool options panel for chosen operation, brush size, falloff (percentage), and intensity (also percentage). When the brush tool is selected, show it as a cursor on the sphere with two geodesic concentric circles in white: the outer circle showing the size of the brush including falloff; the inner circle excluding falloff. When values are painted, within the inner circle apply the selected operation and value, multiplied by the intensity; within the outer circle the same but multiplied also by the falloff tapered from the inner circle to the outer, so that the value is multiplied by 1.0 at the edge of the inner circle tapering to 0.0 at the edge of the outer circle.

8. **Phase 8: Selection System & Per-Vertex Masking**  
   Add a per-vertex selection weight channel ($0.0$ to $1.0$) to the spherical vertex layout. Implement geodesic radius and polygon selection tools on $S^2$, along with selection clearing and inversion operations. Selection tool works the same way as the brushes, with two concentric circles and tapering, and the values are applied not to the data layers but to the selection channel. Update GLSL fragment shaders to render a semi-transparent selection mask overlay, with opacity according to the selection value at each data point. Brush tools and filters are applied to the currntly active layer only and, if there is a selection, their effect is multiplied by the selection value of each data point.

9. **Phase 9: Selection-Weighted Global Filter & Plugin Framework**  
   Establish a C++ `Filter` plugin interface for global and selection-bounded terrain processing. Implement Fill filter (value replacement/blending) Provide a dialog box which captures the fill value (-32767 to +32767) and operation (replace values, raise, lower, minimum, maximum). Compute operations across all vertices weighted by their selection mask values.

10. **Phase 10: Adaptive Regional Detail & Mesh Refinement**  
   Implement local spherical mesh refinement by inserting points into selected regions via incremental Delaunay insertion on $S^2$. Add mesh coarsening/decimation for removing detail in uniform regions while maintaining 2D manifold topology. Ensure attribute interpolation across newly generated vertices.

11. **Phase 11: Advanced GLSL Shading & Dynamic Color Legends**  
   Enhance GLSL shaders with an object-space antialiased latitude/longitude graticule pass (highlighting the Equator and Prime Meridian), atmospheric rim glow, and dynamic 1D Color Lookup Table (LUT) textures. Add runtime legend switching under `View -> Legend`. Have the graticule density adapt to the zoom level so that the number of parallels and meridians displayed at any time remains approximately constant.

12. **Phase 12: Viewport HUD, Live Tooltips & Navigation Extras**  
   Add floating on-screen viewport action buttons (camera reset, auto-spin toggle, wireframe toggle, graticule toggle). Implement real-time cursor hover inspection displaying unprojected geographic coordinates ($\text{Lat}^\circ, \text{Lon}^\circ$) and interpolated elevation in physical units ($m$/$ft$) via `nanoflann` k-d tree lookups.

13. **Phase 13: Data layer architecture**
   Create an abstract "Layer" class for data layers and have the Elevation layer inherit from this. Give each layer a name, a description, a type (list to be defined later), a blend mode, a colourmap, and a datatype. For now, all layers will be raster data mapped to the spherical grid, but in a later phase we can add vector layers. The selection channel is not a layer but is separate and selections apply to all raster layers.

14. **Phase 14: Save, Open and Export**
   Design a sutiable binary file format for the model, to include grid geometry and point locations, contents of all layers and metadata (metadata will be added in a later phase). Place "Open", "Save", "Save As..." and "Export" menu items on the "File" menu. On saving a project, have the user select a filepath in an Open File dialog if necessary and serialise the project data to the file, On opening, reconstruct the grid, data and metadata from a selected file. On export, save the elevation data as an equirectangula grayscale image (reversing the import process) in a selected file path and format. Note: as other layer types are added in later phases, we will allow the user to select one or more files to export. 

15. **Phase 15: Additional Tools**
   Add `BrushTool` subclasses which perform undoable actions as follows, in each case capturing the parameters shown in a dockable panel:
   
   15.1. Simplex noise. Capture frequncy (float), lacunarity (float), persistence (float), number of octaves (integer), whether billowed (boolean). Noise is computed in 3D space for the 3D position of each point in the spherical grid so that it is not distorted on the sphere. If the billowed flag is true, the absolute value of the noise is taken and then scaled to its full range.
   15.2. Gaussian blur. Capture range (kilometres / miles) and sigma (float). Apply the gaussian blur taking all the data points within the given range as inpu and using the supplied sigma value.
   15.3. Clamp. Capture maximum and minimum values (same datatype as the active layer). Replace values above the maximum with the maximum value, and values below the minimum with the minimum value.

16. **Phase 16: Projections**
   Add a menu item "View -> Projection" which allows the projection of the map/globe view to be changed. For now, support projections "Orthographic" (the current globe view) and "Equirectangular". If Equirectangular is selected, display the data not as an interactive globe, but as an interactive flat map in the Equirectangular projection and allow it to be panned and scaled in the same way as the globe. Allow it to wrap around horizontally, but not vertically. 

