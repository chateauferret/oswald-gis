# GPU Compute Acceleration Plan: oswald-gis

This document lists the processes that could run as GPU compute, how hard each
would be to implement, and the expected benefit. It also compares architecture
options. G0 has CPU parallelization and a benchmark runner; see the
[benchmark results](g0-cpu-benchmark-results.md) for measured CPU timings.
Speedup figures below are estimates, not measurements.

## 1. Context

**Target hardware:** NVIDIA GeForce RTX 4070 SUPER. It has 12 GB of VRAM and
about 504 GB/s of memory bandwidth. It does about 35 TFLOPS in single precision
(FP32), but double precision (FP64) runs at only 1/64 of that speed.

**Current state:**

- Grids can have up to 10,000,000 vertices (`GridOptions::maximumPointCount`).
- Filters, brushes, selection and export run on the CPU. Stencil filter passes,
  native Gaussian brush neighbourhoods and raster export lookups use multiple
  threads; flow accumulation and priority flood remain sequential. Grid
  generation runs in the background (`std::async`).
- The neighbour structure is a half-edge graph. Spatial lookups go through a
  nanoflann k-d tree.
- The GPU already holds positions, selection weights, one elevation buffer per
  layer, and a composite buffer. The
  [elevation.comp](../shaders/elevation.comp) shader blends the layers together.

**Goal:** make dense grids (1–10M vertices) interactive. That means:

- erosion and filter passes take milliseconds instead of seconds;
- many erosion iterations can run without copying data back to the CPU
  between them;
- Fill selection, large brushes and export stay responsive.

## 2. Constraints that apply to every GPU process

| Concern | Implication |
|---|---|
| **Precision** | GPU kernels must use FP32. Precompute values once in double on the CPU: edge distances (`R·acos`), tangents and latitudes. Add sums with Kahan/compensated summation where values build up (runoff, Gaussian sums). |
| **Tests** | Many tests compare against the CPU reference implementations. Keep the CPU code as the *reference backend*. Give GPU tests tolerances such as `\|Δ\| ≤ 1e-3 m` or a relative bound, instead of exact matches. |
| **Determinism** | Floating-point atomic adds give slightly different results from run to run. Prefer *gather* (each vertex reads its neighbours) over *scatter* (each vertex writes to its neighbours). Thermal and glacial are both written as scatter today and can be rewritten as gather. |
| **No partial writes / errors** | Today, invalid input or numeric overflow throws and leaves the layer unchanged. On the GPU: write into scratch buffers and set an atomic error flag (non-finite value, overflow, out of range). Read the flag once at the end and only copy the result into the layer buffer if it is clear. |
| **Undo** | `TerrainFilterCommand` and `BrushStrokeCommand` keep CPU copies of the values. Copying 10M floats (40 MB) back from the GPU takes about 2–3 ms over PCIe 4. Use a persistently mapped buffer and a fence so the copy doesn't stall. |
| **UI responsiveness** | GL commands run on the render thread. Split long jobs into dispatches of about 5–20 ms each, spread across frames, using `glFenceSync`. That also gives progress and cancel. Short dispatches also help on Linux, where a GPU that drives a display may have a kernel time limit. |
| **Topology on GPU** | Upload one immutable `GpuMesh` per snapshot (see §5.2). Snapshots are immutable, so caching by snapshot pointer is safe. |
| **Memory at 10M vertices** | Positions 120 MB (already resident); neighbour lists (CSR offsets + indices) ≈ 280 MB; edge distances (FP32) 240 MB; 4–6 scratch float buffers ≈ 240 MB; each layer 40 MB. That is well under 2 GB. Check the budget with `GL_NVX_gpu_memory_info`, which the status bar already uses. |
| **Memory locality** | Neighbour reads are scattered across memory. Reordering vertices along a spherical Morton/Hilbert curve on the GPU side only (with a permutation table) can make stencil kernels 2–4× faster. File order and indices stay the same. |

## 3. Candidate processes

**Complexity:** S = days, M = 1–2 weeks, L = several weeks / research,
XL = not recommended.

**Benefit** means the gain at 1–10M vertices compared with today's
single-threaded code.

| # | Process | Code | GPU pattern | Complexity | Benefit | Notes |
|---|---|---|---|---|---|---|
| 1 | Precipiton diffusion | `PrecipitonFilter` | Neighbour stencil, gather | **S** | **High** (50–200×) | Perfect GPU fit. Multi-iteration diffusion becomes interactive. |
| 2 | Thermal weathering | `ThermalWeatheringFilter` | Edge rule rewritten as gather: each vertex adds up its in/out transfers from its incident edges | **S** | **High** | Symmetric per-edge rule, so the gather version is exact and needs no atomics. |
| 3 | Cratonic grooving / scour | `CratonicGroovingFilter`, `CratonicScourFilter` | Stencil (gradient estimate, tangent projection) | **S** | **High** | Planation is per-vertex. |
| 4 | Glacial erosion | `GlacialErosionFilter` | 4 passes: mass balance (per vertex) → surface → slope/flow direction (stencil) → trough spread (scatter rewritten as gather) | **M** | **High** | The gather pass needs each neighbour's flow direction and weight sum, so it's a two-pass kernel. |
| 5 | Fill (constant / raise / min / max), Absolute Magnitude Noise, Clamp | `FillFilter`, etc. | Per vertex | **S** | Medium | Already cheap on the CPU (memory-bound). Worth doing mainly so data stays on the GPU between steps in a pipeline. The noise filter needs a counter-based random generator (e.g. PCG/Philox seeded per vertex) to keep redo exact. |
| 6 | Select by value / invert / clear / radius selection fill | `SelectionTool`, `UIManager` | Per vertex | **S** | Low–Medium | Trivial to port. |
| 7 | Polygon selection | `SelectionTool` | Per vertex × polygon edges | **S** | Medium–High for complex polygons | Cost is N × M, which suits the GPU well. |
| 8 | Layer histogram | Layers panel | Parallel reduction with shared-memory bins | **S** | Medium | Removes a full CPU pass whenever values change. |
| 9 | Export data / Export image | `ElevationRaster::ExportRaster` | See §4.4 | **M** | **High** | 8192×4096 means 33.5M k-d tree lookups today. The GPU could do it in under 100 ms. The colour map can also be applied on the GPU. |
| 10 | Raster import sampling | `ElevationRaster::SampleElevations` | Per vertex, hardware bilinear texture sampling | **S** | Low–Medium | Wrapping in longitude is free with `GL_REPEAT`. Latitude clamps at the poles. |
| 11 | Native noise brushes / noise Fill selection (Simplex, Value, Ridged, fBm, domain warp) | `Noise.cpp`, `BrushTool` | Per vertex | **M** | **High** for Fill selection and large brushes | A GLSL port of FastNoise2 is required for native GPU execution. Scripted brushes do not need a GLSL port; they invoke the shared C++ noise API through exprtk callbacks. |
| 12 | Spectral noise field | `buildSpectralField` | 3D FFT | M | Low | At most 128³ and cached. Only *sampling* the field is worth moving, which is easy with a 3D texture. |
| 13 | Gaussian blur | `SampleGaussian` | Neighbourhood gather; see §4.3 | **M** | **Very high** | Today: one radius search per vertex. Cost is N × k with k in the hundreds to thousands. This is the worst current hotspot for large blurs. |
| 14 | Scripted brushes (exprtk) | `ScriptedBrushTool` | See §4.5 | **L** | High | exprtk can't run on the GPU. |
| 15 | Incise flow (stream power) | `InciseFlowFilter` | Find receivers (stencil) → flow accumulation (see §4.2) → incision (per vertex) | **M–L** | **High** | Accumulation is the hard part. The other steps are easy. |
| 16 | Fill basins (priority flood) | `FillBasinsFilter` | See §4.1 | **L** | Medium (uncertain) | The algorithm is inherently sequential. The parallel versions need many iterations. |
| 17 | Lloyd relaxation (generation, coarsening) | `relaxOnce` | Per face scatter → per vertex gather over a vertex→face list | M | Low overall | Qhull re-triangulates after every step and that dominates the runtime. |
| 18 | Poisson sampling | `SphericalGridSnapshot` constructor | Parallel dart throwing in phase-grouped cells | L | Medium at 10M | Not bit-compatible with existing seeds. Grid generation already runs in the background. |
| 19 | Spherical Delaunay / convex hull (Qhull), refinement cavities | `triangulate`, `insertHullPoint` | GPU Delaunay (e.g. gDel3D-style) | **XL** | High but risky | Not recommended. A parallel CPU hull or a divide-and-conquer split by octant would be the better first step. |
| 20 | Regrid interpolation (Planet point-count change) | `GridInterpolator::Locate` | Per new point: greedy walk over the old mesh's neighbour lists | M | Medium | Qhull dominates here too. |
| 21 | Hover readout / single-point composite | `GetActiveCompositedElevation` | — | — | None | Leave on the CPU. |

**New capabilities this would enable** (outside today's feature set):

- Iterative erosion loops: 100+ passes of diffusion, thermal and incision kept
  on the GPU, with a live preview.
- Derived display fields such as slope, hillshade and flow accumulation,
  written directly into buffers the renderer already reads.
- The LibTorch tectonic pipeline in section 5 of the
  [tectonic terrain spec](./oswald_gis_tectonic_terrain_spec.md).

## 4. The hard cases

### 4.1 Fill basins

Priority flood is a sequential algorithm. There are two GPU options:

- **Option A — Planchon–Darboux relaxation.** Set every non-sink vertex to +∞.
  Then repeat `W[i] = max(z[i], min over neighbours of W[j] + ε)` until nothing
  changes.
  - The number of iterations grows with the drainage diameter, roughly √N to N
    in the worst case. That could mean thousands of iterations at 10M vertices.
  - It also gives slightly different ε staircases from today's version.
- **Option B — hybrid.**
  - Run the relaxation on the GPU at a coarse level, or on independent
    catchments.
  - Then let the CPU priority flood finish the remaining unresolved vertices.

**Recommendation:** keep this on the CPU (and multithread it) until benchmarks
show it is the bottleneck.

### 4.2 Flow accumulation (incise flow)

Each vertex has a single receiver, so the drainage network is a forest of
trees. The vertices don't need to be sorted by elevation. There are two
approaches:

- **Donor counting:** count incoming donors with integer atomics, then
  repeatedly process every vertex whose donor count has reached zero, passing
  its area downstream. The number of iterations equals the longest river
  (often 10³–10⁴ vertices).
- **Pointer jumping / tree contraction:** O(log N) passes. More complex, but
  scales to 10M vertices.

Either way, the result can be deterministic if areas are added in fixed-point
integers (`uint64` atomics via `GL_NV_shader_atomic_int64` /
`GL_ARB_gpu_shader_int64`) instead of floats.

### 4.3 Gaussian blur

- **Exact version:** build a uniform 3D cell grid on the GPU, with cell size
  equal to the blur range, using a counting sort (prefix sum).
  - Each vertex then scans the 27 surrounding cells.
  - This matches the current semantics, apart from FP32 rounding.
- **Cheap approximation:** run *k* iterations of neighbour-based diffusion.
  - The result approximates a Gaussian, with σ² ≈ 2·k·D·Δt.
  - It is much faster, but the result is not identical. It could be offered as
    a separate "Fast blur".

### 4.4 Export nearest-site lookup

1. Render the mesh in the equirectangular projection (the existing geometry
   shader already handles the seam) into an R32UI framebuffer containing
   *triangle IDs*.
2. In a compute pass, for each pixel, start at the nearest corner of its
   triangle. Then walk greedily along neighbour edges until no neighbour is
   closer. On a Delaunay graph this walk finds the exact nearest site.
3. Apply the colour map and selection blending in the same pass. Read back 33M
   floats (134 MB), or 100 MB of RGB.

A simpler alternative is barycentric interpolation in the fragment shader. It
produces smoother output but changes the documented "nearest site" export
behaviour.

### 4.5 Scripted brushes

Options, from cheapest to most capable:

- **(a) Parallel CPU, no GPU.** Create one `CompiledScript` per thread (the
  engine is not thread-safe). Expect 8–16× on a typical desktop CPU. It keeps
  exprtk semantics exactly.
- **(b) Optional GLSL body in the TOML.** Implemented: an optional
  `gpu_script = '''return ...;'''` body is wrapped in a generated compute shader.
  It receives per-vertex inputs (`x`, `y`, `z`, `lat`, `lon`, `elevation`,
  `weight`, `distance`, `radius`, `lower`, `upper`, `elapsed`, `rate`) and
  parameters as `param_<name>`. Choice parameters are their zero-based option
  index. GLSL helpers provide `simplex`, `valuenoise`, `fbm`, `billow`,
  `valuefbm`, `ridged`, `spectral`, and `warp_x/y/z`.
  - The exprtk `script` remains required and is used as the CPU fallback.
    The GLSL body is used only when the GPU backend is explicitly selected.
  - A GLSL body returns a float; common brush range, blending, falloff and
    selection handling is applied after GPU evaluation.
  - GLSL noise helpers are shader implementations, not bit-identical FastNoise2
    replicas. Authors should keep the CPU and GPU bodies semantically suitable
    for their intended backends.
  - OpenGL compiles and caches generated programs at runtime. Shader compile or
    non-finite result errors fail the dab without partially changing terrain.
    `x`, `y`, and `z` include configured brush domain warping. GPU bodies are
    trusted GLSL and must use bounded loops.
- **(c) Translate exprtk into GLSL.** This needs a custom parser for the
  supported subset (exprtk doesn't expose its parse tree), handling for loops
  and string choices, and runtime loop limits. Large effort.

**Recommendation:** (a) is the fallback; (b) is available for brushes needing
GPU throughput; (c) remains unnecessary.

## 5. Architecture options

### 5.1 Compute API comparison

| Option | Advantages | Disadvantages | Fit |
|---|---|---|---|
| **OpenGL 4.5/4.6 GLSL compute** | Already in use: same context, data already on the GPU, no copying between APIs. Compiles at runtime, which helps scripted brushes. No new dependencies. The existing hidden-window GPU tests already cover it. Works on AMD/Intel too. | No library for sort or prefix sum, so you write your own. Subgroup operations come only through extensions (`GL_KHR_shader_subgroup`, NV extensions). Shares the render thread, so work must be split into chunks. Limited debugging (RenderDoc, Nsight Graphics). No async compute queue. | **Best first step.** |
| **Vulkan compute** (GL interop via `GL_EXT_memory_object` / semaphores, or migrate the renderer) | Explicit async compute queue, so erosion can run while rendering. Standard subgroup operations. SPIR-V, good validation layers, timestamp queries. Portable. | Lots of boilerplate. Interop is fiddly. Moving the whole renderer is a large project. Runtime compilation needs shaderc/glslang. | Good long-term option if the renderer ever moves to Vulkan. |
| **CUDA (+ CUB/Thrust)** with CUDA–GL interop | Fastest to write the hard algorithms: radix sort, scan, segmented reduce, atomics. Best profiler (Nsight Compute). Separate streams, so it doesn't block the render thread. Same ecosystem as LibTorch for the tectonic ML pipeline. Kernels are C++ and can share headers with CPU code. | NVIDIA only. Needs the nvcc toolchain in CMake, which must stay optional with a CPU fallback. Runtime compilation needs NVRTC. Licensing and distribution burden for the toolkit. | Strong for sort/scan-heavy work (incise, fill basins, Gaussian grid) and ML. |
| **SYCL** (AdaptiveCpp / oneAPI) | Single-source C++. The same kernel can run on CPU or GPU. Portable across vendors. | Young toolchains on Linux/NVIDIA. GL interop is weak. Heavy build. | Not worth it here. |
| **OpenCL 3.0** | Portable, compiles at runtime. | NVIDIA's support is minimal and stagnant. Poor tooling. Clunky GL interop. | Not recommended. |
| **C++ parallel algorithms** (`std::execution::par` via TBB; optionally `nvc++ -stdpar=gpu`) | One code path. The CPU version works with GCC today. nvc++ can move the same loops onto the GPU through unified memory. | GPU offload needs the NVHPC compiler for those translation units. Hidden data copies. No GL interop. | **Best CPU baseline.** GPU offload via nvc++ is experimental. |
| **WebGPU native** (Dawn / wgpu-native, WGSL) | Modern, portable, async queue. | The project deliberately avoided WebGPU. Another API next to GL. WGSL limitations (no 64-bit atomics, limited subgroups). | Not recommended. |

### 5.2 Recommended architecture

**Approach:** a hybrid, built in stages.

1. Use all CPU cores first (TBB or `std::execution::par`). This gives a fair
   baseline and speeds up what stays on the CPU (Qhull setup, priority flood,
   exprtk).
2. Use GLSL compute for stencil and per-vertex work, since the data is already
   on the GPU.
3. Revisit CUDA only if flow accumulation, fill basins or LibTorch make
   sort/scan the bottleneck. All kernels sit behind one backend interface, so
   this change stays contained.

**Components** (new, in `oswald_view` or a new `oswald_compute` library linking
GLAD):

```cpp
// Created per snapshot; cached by snapshot pointer.
struct GpuMesh {
    GLuint positions;        // existing buffer, reused
    GLuint neighborOffsets;  // uint[N+1]  (CSR built from half-edges)
    GLuint neighbors;        // uint[6N-12]
    GLuint edgeDistanceKm;   // float[6N-12], computed in double on the CPU
    GLuint edgeTangents;     // vec4[6N-12], only for glacial/cratonic
    GLuint permutation;      // optional Morton/Hilbert order
    std::size_t vertexCount;
};

class ComputeContext {        // owns programs, scratch pool, error buffer, fences
public:
    GpuMesh& MeshFor(const SphericalGridSnapshot&);
    GLuint Scratch(std::size_t floats);          // reusable scratch buffers
    void Dispatch1D(const Program&, std::size_t count);
    std::optional<ComputeError> CheckErrors();   // atomic flag readback
    std::vector<float> Readback(GLuint buffer, std::size_t count); // for undo
};

// Same signature as Filter::Apply, so UIManager/undo code is unchanged.
class TerrainBackend {
public:
    virtual std::vector<float> Apply(const Filter&, const SphericalGridSnapshot&,
                                     std::span<const float> elevations) = 0;
};
// CpuBackend wraps the existing Filter::Apply (reference + fallback);
// GlslBackend runs kernels and reads back once.
```

**Shader conventions:**

- One `.comp` file per kernel in `shaders/compute/`, copied to the resources
  folder by CMake like the existing shaders.
- `#include`-style shared GLSL for the neighbour iterator, clamping, the error
  flag and noise (via `GL_ARB_shading_language_include` or simple text
  concatenation).
- Workgroup size 256.
- Elevation clamping to [-32767, 32767] matches the CPU code.

**Multi-pass pipeline:** an "Erosion pipeline" dialog runs N × (diffusion →
thermal → incision) on the GPU, alternating between two buffers. It renders the
intermediate state directly, because the composite buffer can read the active
layer's GPU buffer. It reads back once and records **one** undo command.

**Settings:** add a `Compute backend: Auto / CPU / GPU` setting. Fall back to
the CPU on context, extension or memory failures. Show the active backend in
the status bar.

## 6. Phased roadmap

### G0 benchmark runner

Build `compute_benchmarks` with the project, preferably in a Release build.
For example:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target compute_benchmarks
build/release/compute_benchmarks --list
build/release/compute_benchmarks --case precipiton --vertices 1000000 --iterations 3
build/release/compute_benchmarks --case export_image --vertices 5000000 --raster-width 8192
build/release/compute_benchmarks --case precipiton --case thermal --vertices 1000000
```

Run each listed case at `1000000`, `5000000`, and `10000000` vertices for the
G0 matrix. Each invocation prints CSV columns `case,vertices,iteration,
milliseconds,checksum`. Grid setup and input copies are outside the timed
region except for the generation, regrid, refine and coarsen cases; those
measure their full public operations, including topology construction.
Generation also includes destruction. The default raster is 2048×1024; set
`--raster-width 8192` for 8192×4096. Import setup exports the input outside
the timed region. Cases use deterministic grid/elevation inputs, except
absolute-magnitude noise, which uses the application's random seed.
Brushes use full selection coverage and default settings; Gaussian uses a
100 km range and 25 km sigma. Spectral noise includes cold field creation in
the first iteration and uses its cache on subsequent iterations.
Repeat `--case` to run multiple cases against one prepared grid; this avoids
rebuilding it for each case. Generation cases still construct their own grids.

For example, after choosing an existing empty output directory:

```sh
for size in 1000000 5000000 10000000; do
  for case in $(build/release/compute_benchmarks --list); do
    build/release/compute_benchmarks --case "$case" --vertices "$size" \
      --iterations 3 > "results/${case}-${size}.csv"
  done
done
```

Create `results/` first and ensure sufficient storage, memory and time.
Each process exits nonzero on failure; inspect the exit status and stderr
before interpreting its CSV. `refine` at the maximum point count fails
because the grid cannot grow beyond that limit.

The public grid API does not expose Poisson sampling, triangulation, Lloyd
relaxation or interpolation as independent operations. `grid_generation`,
`lloyd_generation`, `refine`, `coarsen` and `regrid` measure the corresponding
end-to-end operations, not isolated stage timings. The histogram and
clear/invert selection cases measure their CPU loops, not UI, undo or GPU
upload. Export includes file encoding and disk I/O. No GPU backend or GPU
comparison is implied by these CPU measurements. Large grids, particularly
generation, glacial erosion and Gaussian blur, can require substantial RAM
and runtime; run them individually and record machine and build details.

`ctest --test-dir build/release -L benchmark_smoke` runs every case at 1,000
vertices. It checks executability, not benchmark performance.

| Phase | Scope | Rough effort |
|---|---|---|
| G0 | Benchmarks at 1M / 5M / 10M vertices for every process in §3. Multithread the CPU stencil filters, Gaussian and export lookups. | 1 week |
| G1 | `ComputeContext`, `GpuMesh` (CSR, distances), error flag, readback, backend setting, GPU-vs-CPU tolerance tests | 1–2 weeks |
| G2 | Precipiton, thermal, cratonic ×2, the Fill family, selection operations, histogram | 1–2 weeks |
| G3 | Glacial; iterative erosion pipeline with live preview | 1–2 weeks |
| G4 | Export (triangle-ID render + greedy walk + colour map), raster import sampling | 1 week |
| G5 | Shared script-callable noise API; native noise brushes and Fill selection on the GPU | 2 weeks |
| G6 | Gaussian (GPU cell grid built with a prefix sum) | 1–2 weeks |
| G7 | Incise flow (donor counting or pointer jumping) | 2–3 weeks |
| G8 | Decide whether to move to CUDA/Vulkan, the fill basins approach, and optional `gpu_script` brushes | — |

G1's OpenGL foundation lives in `ComputeContext.hpp` and `ComputeContext.cpp`.
It requires a current OpenGL 4.3+ context, uploads immutable snapshot positions
and CSR neighbour topology, and precomputes spherical edge distances in double
precision on the CPU before storing FP32 kilometres; the mesh also retains an
FP64 distance buffer for the glacial kernel to preserve the CPU reference's
precision. `ComputeContext` also owns compute programs, reusable scratch
buffers, an error SSBO at binding 2, and checked readback. The Settings dialog
exposes an in-memory Auto / CPU / GPU preference; G2 adds the first operations
that use this context.

G2 adds optional GPU implementations for Fill, Absolute Magnitude Noise,
Precipiton diffusion, thermal weathering, cratonic directional grooving and
cratonic scour/planation. Noise uses a per-dispatch seed and a counter-based
PCG hash indexed by vertex. These kernels use gather-only neighbour reads and
write to scratch output; invalid input or numeric results are reported before
the caller can apply the output.
The UI's GPU backend also handles value/radius/polygon selection, selection
clear/invert, and layer histograms. CPU filters and selection routines remain
the default Auto/CPU path and reference implementations. Deterministic
GPU/CPU filter and selection tests compare outputs with a 1e-3 tolerance;
histogram bins are checked exactly. Noise output is checked for its amplitude
and elevation-range bounds because the CPU filter uses a different
nondeterministic random stream. The UI stores the GPU result in its undo
command, so redo reuses the exact generated output. Clamp has no separate
filter in the current codebase.

G3 adds an optional five-dispatch GPU glacial pipeline: mass balance, ice
surface, downslope flow and erosion rate, gather-based trough spreading, then
selection blending and isostatic rebound. It keeps the CPU filter as the
reference implementation, uses double-precision intermediates and compares
results at the existing 1e-3 m tolerance, including mass-balance-only and
selection cases. Latitude uses a double-precision series with a polar expansion
because GLSL does not provide double-precision `asin`. The glacial dialog's
Preview applies a temporary result to the active layer; changing parameters or
pressing Cancel restores the original terrain, while Apply records the preview
as one undoable edit.

G4 routes raster import and export through the optional compute backend when
GPU is selected; the existing CPU paths remain the fallback. Import uploads
the normalized raster as an `R32F` texture and performs wrapped-longitude,
clamped-latitude bilinear sampling on the GPU. Per-vertex UVs are prepared in
double precision on the CPU, and the shader uses four texel fetches so sampling
coordinates match the CPU reference closely even at sharp raster transitions.
Export evaluates each equirectangular pixel on the GPU, applies the colour map
and optional selection blend there, and returns both elevations and RGB data.
Instead of the planned triangle-ID render, export seeds each pixel from a
coarse CPU nearest-site grid, then walks the spherical Delaunay 1-ring on the
GPU to the nearest site. GPU tests compare raster sampling within 0.05 m and
check nearest-site export elevations and colour-map output against the CPU
reference.

For scripted brushes, G5 keeps exprtk evaluation on the CPU and exposes noise
and other per-vertex operations as ordinary C++ callbacks. Existing calls
(`simplex`, `valuenoise`, `fbm`, `billow`, `valuefbm`, `ridged`, `spectral`,
and `gaussian`) are available regardless of the compute-backend preference.
The new `warp_x`, `warp_y`, and `warp_z` callbacks share the native noise
brush's domain-warp implementation and use the script engine's brush seed.
This keeps a script's function calls unchanged between CPU and GPU preference
modes without attempting to translate exprtk or dispatch individual scalar
calls to the GPU. Native noise brushes and bulk noise fills remain separate GPU
work; their shader implementations can be added without changing script
syntax.

## 7. Open questions

1. Is a ~1e-3 m difference between CPU and GPU results acceptable for
   undo/redo and tests? Or must GPU results be bit-reproducible on the same
   machine, which would require gather-only, atomic-free kernels?
2. Is a "Fast blur" approximation acceptable next to the exact Gaussian?
3. Should export keep nearest-site semantics, or switch to interpolated
   rendering?
4. Is the LibTorch/ML pipeline a firm plan? If so, it makes CUDA much more
   attractive.
5. Should GPU support be required, or always optional with the CPU path as the
   fallback? This draft assumes optional.
