# Designing oswald-gis: Tectonic AI Terrain Generation Reference

## 1. Initial Architectural Concept (Conditional GAN Workflow)
To translate semantic segmentation maps or tectonic sketches into realistic Earth-like Digital Elevation Models (DEMs), a **Conditional GAN (cGAN)** architecture is ideal. While a standard U-Net (Pix2Pix) works, standard normalization layers can wash out semantic data across uniform zones. Using **Spatially-Adaptive Denormalization (SPADE/GauGAN)** blocks resolves this by modulating activations with the input features at every network layer.

```
[Sketch/Segmentation Map] ---> (Generator) -----------------> [Generated DEM]
                                                                     |
                                                                     v
[Sketch/Segmentation Map] ---> (Discriminator / Critic) <-----------+ or [Real DEM]
                                                                     |
                                                                     v
                                                             [Real or Fake?]
```

### Loss Matrix
* **Adversarial Loss (BCE / Hinge Loss):** Drives realistic high-frequency topographic output.
* **L1 / Reconstruction Loss:** Computes point-by-point absolute height differences to enforce global elevation adherence.
* **Feature Matching Loss:** Compares internal feature outputs from the discriminator layers to stabilize the optimization landscape.

---

## 2. Low-Resolution Macro Dataset & PyTorch Implementation
With data scaling to roughly 1.2 km per pixel (e.g., ~500 pixels spanning from Keswick in the Lake District to Lerwick in Shetland), the focus shifts from narrow micro-erosion textures to massive continent-scale structures. At this footprint, a standardized Pix2Pix pipeline (U-Net Generator + PatchGAN Discriminator) offers an optimal balance of training ease and feature preservation.

### Dataset Pipeline (PyTorch)
```python
import torch
from torch.utils.data import Dataset
import numpy as np

class TerrainDataset(Dataset):
    def __init__(self, seg_paths, dem_paths, num_classes=8):
        self.seg_paths = seg_paths
        self.dem_paths = dem_paths
        self.num_classes = num_classes

    def __len__(self):
        return len(self.seg_paths)

    def __getitem__(self, idx):
        seg = np.load(self.seg_paths[idx]).astype(np.int64) 
        dem = np.load(self.dem_paths[idx]).astype(np.float32)

        seg_tensor = torch.from_numpy(seg)
        seg_one_hot = torch.nn.functional.one_hot(seg_tensor, num_classes=self.num_classes)
        seg_one_hot = seg_one_hot.permute(2, 0, 1).float() # [Num_Classes, 512, 512]

        # Map to standard Tanh domain [-1, 1]
        dem_min, dem_max = -100.0, 4000.0 
        dem_norm = 2.0 * (dem - dem_min) / (dem_max - dem_min) - 1.0
        dem_tensor = torch.from_numpy(dem_norm).unsqueeze(0)

        return seg_one_hot, dem_tensor
```

---

## 3. Hydrological Continuity & Overlapping Tile Stitching
Standard convolutional patches suffer from a lack of long-range awareness, meaning cross-tile rivers often break or pool unrealistically at seams. To guarantee continuity:
1. **Distance-to-Drainage Field Anchor:** Append a raw continuous spatial gradient channel representing pixel proximity to major continental drainage channels. This explicitly anchors the network's macro-valleys.
2. **Overlapping Blending Matrix:** Slice input maps with a 32- or 64-pixel overlap margin, blending edges linearly using alpha weight maps.

```python
import numpy as np

def stitch_overlapping_tiles(tiles_dict, grid_shape, tile_size=512, overlap=64):
    num_rows, num_cols = grid_shape
    stride = tile_size - overlap
    
    master_h = (num_rows - 1) * stride + tile_size
    master_w = (num_cols - 1) * stride + tile_size
    
    master_dem = np.zeros((master_h, master_w), dtype=np.float32)
    weight_matrix = np.zeros((master_h, master_w), dtype=np.float32)
    
    ramp = np.ones(tile_size, dtype=np.float32)
    ramp[:overlap] = np.linspace(0.0, 1.0, overlap)
    ramp[-overlap:] = np.linspace(1.0, 0.0, overlap)
    tile_weights = np.outer(ramp, ramp)
    
    for (r, c), tile in tiles_dict.items():
        y_start = r * stride
        y_end = y_start + tile_size
        x_start = c * stride
        x_end = x_start + tile_size
        
        current_weights = tile_weights.copy()
        if r == 0: current_weights[:overlap, :] = 1.0
        if r == num_rows - 1: current_weights[-overlap:, :] = 1.0
        if c == 0: current_weights[:, :overlap] = 1.0
        if c == num_cols - 1: current_weights[:, -overlap:] = 1.0
        
        master_dem[y_start:y_end, x_start:x_end] += tile * current_weights
        weight_matrix[y_start:y_end, x_start:x_end] += current_weights

    weight_matrix = np.where(weight_matrix == 0, 1.0, weight_matrix)
    return master_dem / weight_matrix
```

---

## 4. Ground-Truth Feature Extraction Strategy
Instead of manually creating datasets, we reverse-engineer training sets from physical Earth data grids using morphological operators to isolate high-frequency ridges and local depressions.

```python
import numpy as np
from scipy import ndimage
from skimage.filters import gaussian
from skimage.morphology import white_tophat, black_tophat, disk, skeletonize

def extract_advanced_bathymetric_sketches(dem_tile, sea_level=0.0):
    dem_smooth = gaussian(dem_tile, sigma=2.0)
    footprint = disk(12)

    land_mask = (dem_tile > sea_level).astype(np.float32)

    # Channel 1: Convergent Lines
    land_heights = np.where(dem_smooth > sea_level, dem_smooth, sea_level)
    land_ridges = white_tophat(land_heights, footprint=footprint)
    land_ridge_mask = (land_ridges > (land_ridges.max() * 0.15)).astype(np.float32)

    ocean_depths = np.where(dem_smooth <= sea_level, dem_smooth, sea_level)
    ocean_trenches = black_tophat(ocean_depths, footprint=footprint)
    ocean_trench_mask = (ocean_trenches > (ocean_trenches.max() * 0.20)).astype(np.float32)
    convergent_lines = skeletonize((land_ridge_mask + ocean_trench_mask) > 0).astype(np.float32)

    # Channel 2: Divergent Lines (Subsea Spreading Centers)
    ocean_ridges = white_tophat(ocean_depths, footprint=footprint)
    ocean_ridge_mask = (ocean_ridges > (ocean_ridges.max() * 0.15)).astype(np.float32)
    divergent_lines = skeletonize(ocean_ridge_mask).astype(np.float32)

    # Channel 3: Hydrology Rivers
    land_valleys = black_tophat(land_heights, footprint=footprint)
    hydrology_lines = skeletonize(land_valleys > (land_valleys.max() * 0.15)).astype(np.float32)

    # Channel 4: Volcanic Seamounts
    seamounts = white_tophat(ocean_depths, footprint=disk(4))
    seamount_mask = ((seamounts > (seamounts.max() * 0.25)) & (ocean_ridge_mask == 0)).astype(np.float32)

    return np.stack([land_mask, convergent_lines, divergent_lines, hydrology_lines, seamount_mask], axis=0)
```

---

## 5. Virtual Globe Interface & LibTorch C++ Deployment
To eliminate map projection errors at polar coordinates, the application employs a **Spherical Fibonacci Lattice** structured into a **Delaunay Triangulation Graph**. 

Local patches are extracted into 2D tensor matrices via a **Gnomonic Projection** (which maps great-circle arcs perfectly to straight pencil lines), evaluated natively on the GPU via LibTorch, and written back to the vertex data structure without any Python interpreter dependence.

```cpp
#include <vector>
#include <cmath>

struct GlobeVertex {
    float lat; float lon;
    float land_ch; float conv_ch; float div_ch; float hydro_ch; float spot_ch;
    float elevation;
};

class GnomonicRasterizer {
private:
    static constexpr int TILE_RES = 512;
    static inline double DegToRad(double deg) { return deg * M_PI / 180.0; }

public:
    std::vector<float> RasterizeLocalViewpoint(
        double center_lat, double center_lon, 
        const std::vector<GlobeVertex>& globe_vertices, double view_scale = 0.05) 
    {
        std::vector<float> flat_tensor(5 * TILE_RES * TILE_RES, 0.0f);
        double lat0 = DegToRad(center_lat), lon0 = DegToRad(center_lon);
        double sin_lat0 = std::sin(lat0), cos_lat0 = std::cos(lat0);

        for (const auto& vertex : globe_vertices) {
            if (std::abs(vertex.lat - center_lat) > 10.0 || std::abs(vertex.lon - center_lon) > 10.0) continue;

            double lat = DegToRad(vertex.lat), lon = DegToRad(vertex.lon);
            double cos_c = sin_lat0 * std::sin(lat) + cos_lat0 * std::cos(lat) * std::cos(lon - lon0);
            if (cos_c <= 0.0001) continue; 

            double x = (std::cos(lat) * std::sin(lon - lon0)) / cos_c;
            double y = (cos_lat0 * std::sin(lat) - sin_lat0 * std::cos(lat) * std::cos(lon - lon0)) / cos_c;

            int px = static_cast<int>(((x + view_scale) / (2.0 * view_scale)) * (TILE_RES - 1));
            int py = static_cast<int>(((view_scale - y) / (2.0 * view_scale)) * (TILE_RES - 1));

            if (px >= 0 && px < TILE_RES && py >= 0 && py < TILE_RES) {
                int pixel_idx = py * TILE_RES + px;
                int stride = TILE_RES * TILE_RES;
                flat_tensor[0 * stride + pixel_idx] = vertex.land_ch;
                flat_tensor[1 * stride + pixel_idx] = vertex.conv_ch;
                flat_tensor[2 * stride + pixel_idx] = vertex.div_ch;
                flat_tensor[3 * stride + pixel_idx] = vertex.hydro_ch;
                flat_tensor[4 * stride + pixel_idx] = vertex.spot_ch;
            }
        }
        return flat_tensor;
    }
};
```
