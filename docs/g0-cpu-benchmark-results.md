# G0 CPU benchmark results

## Run details

- Date: 2026-11-20 (UTC).
- Host: Intel Core i7-14700F, 28 logical CPUs available; 31 GiB RAM and 47 GiB
  swap. NVIDIA GeForce RTX 4070 SUPER (driver 595.99.02); benchmarks use CPU
  only.
- Linux 7.0.0-42-generic; GCC 15.2.0; CMake Release build.
- Source: working tree based on commit `3104424`, including the uncommitted G0
  benchmark and parallelization changes.
- Three timed iterations per case, within one process. Cases run sequentially;
  results are wall-clock milliseconds. No CPU affinity or exclusive access to
  the machine was configured. The runner was extended during the run to share
  grid setup across cases; the timed operations themselves were unchanged.
- A 16 GiB virtual-address-space limit applies to each process. The 1M cases
  used a 300-second timeout. At 5M, generation and regrid used 600 seconds
  per process; the remaining cases shared a prepared grid in one process with
  a 3600-second timeout. At 10M, generation cases used 900 seconds each, the
  standard group used 1800 seconds, and the heavy group used 3600 seconds.
  A timeout includes grid construction and all three iterations, not just the
  time shown in the table.
- Default 2048×1024 raster; default brush and filter parameters except where
  configured by the runner. See the [benchmark runner](../tests/ComputeBenchmarks.cpp)
  and [plan](oswald-gis-gpu-compute-plan.md#g0-benchmark-runner) for workload
  details and the distinction between end-to-end and isolated measurements.

## Timings

Each entry lists the three iterations followed by their median, in milliseconds.
Generation, regrid, refinement and coarsening include the work documented in
the plan; export includes encoding and disk I/O. Measurements include thread
startup and any per-application adjacency construction. The spectral case has
a cold first iteration and warm subsequent iterations.

### 1,000,000 vertices

| Case | Iterations (ms) | Median (ms) |
|---|---:|---:|
| absolute_noise | 6.804, 6.592, 6.587 | 6.592 |
| clamp | 2.817, 2.635, 2.522 | 2.635 |
| coarsen | 11029.200, 10990.600, 11050.400 | 11029.200 |
| export_data | 110.141, 114.581, 117.246 | 114.581 |
| export_image | 194.316, 207.845, 205.927 | 205.927 |
| fill | 1.454, 1.444, 1.441 | 1.444 |
| fill_basins | 397.720, 392.051, 394.467 | 394.467 |
| fill_maximum | 1.458, 1.459, 1.443 | 1.458 |
| fill_minimum | 1.489, 1.538, 1.451 | 1.489 |
| fill_raise | 1.454, 1.454, 1.445 | 1.454 |
| gaussian | 794.193, 732.340, 950.849 | 794.193 |
| glacial | 218.010, 176.429, 179.949 | 179.949 |
| grid_generation | 6391.590, 6042.860, 5950.130 | 6042.860 |
| grooving | 68.422, 69.558, 70.262 | 69.558 |
| histogram | 1.257, 1.254, 1.254 | 1.254 |
| hover | 0.001149, 0.000381, 0.000199 | 0.000381 |
| import_raster | 97.081, 96.696, 98.318 | 97.081 |
| incise | 135.676, 140.585, 141.613 | 140.585 |
| lloyd_generation | 9703.240, 9491.580, 9405.870 | 9491.580 |
| precipiton | 67.187, 72.774, 78.998 | 72.774 |
| refine | 4265.650, 4254.430, 4268.280 | 4265.650 |
| regrid | 7254.460, 7209.340, 7181.390 | 7209.340 |
| ridge_noise | 208.171, 208.353, 208.759 | 208.353 |
| scour | 80.344, 89.316, 87.057 | 87.057 |
| scripted | 339.169, 338.823, 339.010 | 339.010 |
| select_clear | 0.307, 0.157, 0.157 | 0.157 |
| select_invert | 0.341, 0.162, 0.160 | 0.162 |
| select_polygon | 95.410, 94.974, 95.159 | 95.159 |
| select_radius | 0.667, 0.576, 0.413 | 0.576 |
| select_value | 1.368, 1.310, 1.101 | 1.310 |
| simplex | 203.110, 203.392, 203.126 | 203.126 |
| spectral_noise | 120.019, 116.860, 116.935 | 116.935 |
| thermal | 76.077, 83.648, 72.370 | 76.077 |
| value_noise | 139.492, 139.814, 139.447 | 139.492 |

### 5,000,000 vertices

| Case | Iterations (ms) | Median (ms) |
|---|---:|---:|
| absolute_noise | 32.4594, 32.4485, 32.1713 | 32.4485 |
| clamp | 15.3682, 15.7581, 15.64 | 15.64 |
| coarsen | 61603.3, 61092.2, 60977.7 | 61092.2 |
| export_data | 140.072, 137.568, 139.058 | 139.058 |
| export_image | 273.36, 267.842, 268.451 | 268.451 |
| fill | 7.29682, 7.51371, 7.26123 | 7.29682 |
| fill_basins | 2392.62, 2400.45, 2398.08 | 2398.08 |
| fill_maximum | 7.70755, 7.2466, 7.24853 | 7.24853 |
| fill_minimum | 7.2296, 7.2259, 7.23499 | 7.2296 |
| fill_raise | 7.22988, 7.24221, 7.51969 | 7.24221 |
| gaussian | 23969.5, 24939.8, 26506.7 | 24939.8 |
| glacial | 1402.05, 1183.09, 1163.61 | 1183.09 |
| grid_generation | 37960.6, 36977.4, 36869.3 | 36977.4 |
| grooving | 498.735, 497.307, 503.275 | 498.735 |
| histogram | 6.25052, 6.2782, 6.25716 | 6.25716 |
| hover | 0.001532, 0.000487, 0.000339 | 0.000487 |
| import_raster | 437.869, 434.217, 433.636 | 434.217 |
| incise | 972.56, 947.995, 960.841 | 960.841 |
| lloyd_generation | 57313.6, 57278.6, 56734.3 | 57278.6 |
| precipiton | 469.336, 479.465, 480.663 | 479.465 |
| refine | 24023.7, 23640, 24241.9 | 24023.7 |
| regrid | 45898.5, 45739.4, 46648 | 45898.5 |
| ridge_noise | 1038.94, 1033.68, 1034.97 | 1034.97 |
| scour | 534.871, 618.786, 564.837 | 564.837 |
| scripted | 1759.26, 1783.27, 1768.61 | 1768.61 |
| select_clear | 1.2002, 1.48594, 1.42938 | 1.42938 |
| select_invert | 1.93566, 1.1151, 1.11187 | 1.1151 |
| select_polygon | 474.052, 474.793, 475.569 | 474.793 |
| select_radius | 2.94754, 2.65461, 2.4029 | 2.65461 |
| select_value | 6.56021, 6.57733, 6.95377 | 6.57733 |
| simplex | 1019.74, 1020.33, 1016.83 | 1019.74 |
| spectral_noise | 604.454, 600.042, 603.562 | 603.562 |
| thermal | 513.764, 511.156, 519.621 | 513.764 |
| value_noise | 694.253, 695.547, 707.535 | 695.547 |

### 10,000,000 vertices

The `refine` case fails with "The spherical mesh has reached its
10,000,000-vertex limit"; it cannot refine a grid at the maximum size.
All other cases completed. The peak resident memory of the heavy-case process
was approximately 9.72 GiB.

| Case | Iterations (ms) | Median (ms) |
|---|---:|---:|
| absolute_noise | 64.6969, 64.6913, 64.7127 | 64.6969 |
| clamp | 28.7551, 28.6078, 28.6304 | 28.6304 |
| coarsen | 127618, 128910, 129630 | 128910 |
| export_data | 167.347, 167.35, 167.335 | 167.347 |
| export_image | 319.81, 319.009, 318.04 | 319.009 |
| fill | 14.4306, 14.4316, 14.4271 | 14.4306 |
| fill_basins | 4986.83, 4975.58, 4980.62 | 4980.62 |
| fill_maximum | 14.4812, 14.4728, 14.4605 | 14.4728 |
| fill_minimum | 14.4199, 14.439, 14.4636 | 14.439 |
| fill_raise | 14.4751, 14.4593, 14.4382 | 14.4593 |
| gaussian | 103087, 103888, 103845 | 103845 |
| glacial | 2453.84, 2427.83, 2567.98 | 2453.84 |
| grid_generation | 80747.1, 77712.7, 77428.9 | 77712.7 |
| grooving | 1136.99, 1102.89, 1106.35 | 1106.35 |
| histogram | 12.4734, 12.4815, 12.4651 | 12.4734 |
| hover | 0.00099, 0.000457, 0.000455 | 0.000457 |
| import_raster | 843.044, 848.552, 848.251 | 848.251 |
| incise | 2158.56, 2162.03, 2149.45 | 2158.56 |
| lloyd_generation | 136402, 134510, 134236 | 134510 |
| precipiton | 1059.94, 1069.9, 1068.11 | 1068.11 |
| regrid | 114604, 113172, 112681 | 113172 |
| ridge_noise | 2059.25, 2068.71, 2056.46 | 2059.25 |
| scour | 1185.17, 1194.29, 1263.63 | 1194.29 |
| scripted | 3348.87, 3347.76, 3348.67 | 3348.67 |
| select_clear | 2.79195, 2.94966, 2.88429 | 2.88429 |
| select_invert | 3.99294, 3.47676, 3.44546 | 3.47676 |
| select_polygon | 942.569, 942.881, 942.06 | 942.569 |
| select_radius | 6.25058, 4.9688, 4.66354 | 4.9688 |
| select_value | 13.0273, 12.9973, 12.9445 | 12.9973 |
| simplex | 2020.99, 2021.45, 2022.02 | 2021.45 |
| spectral_noise | 1198.55, 1193.85, 1195.83 | 1195.83 |
| thermal | 1119.51, 1150.65, 1102.43 | 1119.51 |
| value_noise | 1377.47, 1381.03, 1384.93 | 1381.03 |

## Observations

- The Gaussian median grew from 794 ms at 1M vertices to 24.9 s at 5M and
  103.8 s at 10M with the same 100 km search radius. Neighbourhood density
  grows with grid size, so this is not a constant-work-per-vertex test.
- At 10M, median end-to-end generation was 77.7 s, regrid 113.2 s, and
  coarsen 128.9 s. These include topology work, not isolated kernel times.
- The export timings use 2048×1024, not the 8192×4096 maximum. The hover
  operation is a single lookup, so its sub-microsecond readings are sensitive
  to timer overhead.
- These are CPU baseline measurements, not GPU speedup measurements. They
  include normal variation from a shared workstation and are not a controlled
  comparison against the former single-threaded implementations.
