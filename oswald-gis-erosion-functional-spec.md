# Functional Specification: Terrain Erosion Engine for oswald-gis

This specification defines the mathematical formulations, topological data structures, and algorithmic execution passes for the multi-regime terrain erosion engine in `oswald-gis`. Designed for a spherical Poisson-relaxed Delaunay mesh, the engine provides high-performance fluvial, glacial, and cratonic ice-scouring simulation routines built for C++20, OpenGL 4.5 Core, and Dear ImGui.

---

## 1. Geodetic Framework & Mesh Foundations on $S^2$

The terrain model operates on a topologically closed 2D manifold embedded in $\mathbb{R}^3$. The mesh is populated using a sunflower-inspired Fibonacci distribution with controlled spatial jittering or a Lloyd-relaxed Poisson point cloud, tessellated via spherical Delaunay triangulation.

### 1.1 Pairwise Geodesic Metrics

For any target vertex $v_i$ and neighbor vertex $v_j \in N(v_i)$ in the 1-ring Delaunay neighborhood of $v_i$, spatial relationships are defined using spherical trigonometry on the unit sphere $S^2$.

The physical great-circle arc distance $d(v_i, v_j)$ across the planetary surface is:

$$d(v_i, v_j) = R \cdot \arccos\left(\mathbf{p}_i \cdot \mathbf{p}_j\right)$$

The directional slope $S_{ij}$ between vertex $v_i$ and neighbor $v_j$ is defined as:

$$S_{ij} = \frac{z(v_i) - z(v_j)}{d(v_i, v_j)}$$

The maximum downhill slope $S_{\max}(v_i)$ and the primary downstream receiver vertex $v_{\text{down}}(v_i)$ are given by:

$$S_{\max}(v_i) = \max_{v_j \in N(v_i)} \left( S_{ij} \right)$$

$$v_{\text{down}}(v_i) = \arg\max_{v_j \in N(v_i)} \left( S_{ij} \right)$$

### 1.2 Specification of Terms

The terms used in the geodetic equations above are specified as follows:

| Term / Symbol | Variable Type & Units | Description & Physical Meaning |
| :--- | :--- | :--- |
| $R$ | Constant float ($\text{km}$) | Mean planetary radius (default Earth radius: $6,371.0\text{ km}$). |
| $\mathbf{p}_i$ | Normalized vector in $\mathbb{R}^3$ ($\|\mathbf{p}_i\| = 1.0$) | Unit direction vector of vertex $v_i$ on the unit sphere $S^2$. |
| $\mathbf{p}_j$ | Normalized vector in $\mathbb{R}^3$ ($\|\mathbf{p}_j\| = 1.0$) | Unit direction vector of 1-ring neighbor vertex $v_j$ on $S^2$. |
| $z(v_i)$ | Scalar float ($\text{m}$) | Bedrock or surface elevation of vertex $v_i$ relative to sea level ($0.0\text{ m}$). |
| $z(v_j)$ | Scalar float ($\text{m}$) | Bedrock or surface elevation of neighbor vertex $v_j$. |
| $d(v_i, v_j)$ | Scalar float ($\text{km}$) | Great-circle distance along the sphere surface between $v_i$ and $v_j$. |
| $S_{ij}$ | Dimensionless scalar ($\text{m}/\text{km}$) | Geodesic gradient between vertex $v_i$ and neighbor $v_j$. |
| $S_{\max}(v_i)$ | Dimensionless scalar ($\text{m}/\text{km}$) | Steepest positive downhill gradient departing from vertex $v_i$. |
| $v_{\text{down}}(v_i)$ | Unsigned integer index | Topological index of the neighbor receiving primary downhill runoff. |
| $N(v_i)$ | Set of vertex indices | 1-ring Delaunay topological neighbors adjacent to vertex $v_i$. |

---

## 2. Fluvial Erosion Engine (Wilbur-Style Macro-Erosion)

Fluvial erosion simulates the transport of surface material by liquid water. It combines continuous global flow routing with Stream Power carving, non-linear sediment diffusion, and slope repose enforcement.

### 2.1 Spherical Fill Basins

Local topographic depressions (pits) halt continuous flow routing. The Fill Basins operator eliminates non-endorheic depressions using a graph-based Priority Queue (Planchon-Darboux variant).

#### Step-by-Step Algorithm

1. **Sink Initialization**: Identify all coastal and oceanic vertices ($z(v_i) \le 0.0$) and push them into a minimum-priority queue $Q$ sorted by elevation $z$.
2. **Elevation Propagation**: While $Q$ is non-empty, pop vertex $v_u$. For each neighbor $v_v \in N(v_u)$:
   - If $z(v_v) > z(v_u)$ and $z(v_v)$ has not been finalized, mark $z(v_v)$ as resolved and push $v_v$ to $Q$.
   - If $z(v_v) \le z(v_u)$ and $v_v$ is not a designated endorheic sink, adjust its elevation:

$$z_{\text{filled}}(v_v) = z(v_u) + \epsilon$$

where $\epsilon = 1.0 \times 10^{-4}\text{ m}$ is a minimal positive elevation increment establishing a continuous downhill drainage path.

### 2.2 Stream Power Law Incise Flow

Valley carving uses a Stream Power Law formulation. Catchment drainage area $A(v_i)$ is computed by topologically sorting vertices by elevation and accumulating runoff downstream.

The flow accumulation update rule for each vertex $v_i$ processed from highest to lowest elevation is:

$$A\left(v_{\text{down}}(v_i)\right) \leftarrow A\left(v_{\text{down}}(v_i)\right) + A(v_i)$$

The elevation reduction $\Delta z_{\text{incise}}(v_i)$ during channel incision is calculated as:

$$\Delta z_{\text{incise}}(v_i) = C_{\text{incise}} \cdot \left( \frac{A(v_i)}{A_{\text{ref}}} \right)^m \cdot \left( S_{\max}(v_i) \right)^n$$

$$z_{\text{new}}(v_i) = z(v_i) - \Delta z_{\text{incise}}(v_i)$$

#### Specification of Terms

* $A(v_i)$: Total accumulated catchment area / runoff volume at vertex $v_i$ ($\text{km}^2$ or vertex count). Initialized to local precipitation $P(v_i)$ (default $1.0$).
* $A_{\text{ref}}$: Reference normalization area ($\text{km}^2$), setting the scale threshold for major continental river trunks.
* $m$: Flow exponent (dimensionless, typical range $0.3 \le m \le 0.8$). Lower values carve headwater tributaries deeper; higher values emphasize major trunks.
* $n$: Slope exponent (dimensionless, default $1.0$). Scales incision depth with local terrain steepness.
* $C_{\text{incise}}$: Incision rate multiplier ($\text{m}/\text{pass}$), controlling overall carving intensity per iteration.

### 2.3 Precipiton Non-Linear Diffusion

Precipiton erosion smooths sharp channel walls and redistributes fine sediment into valley floors using a distance-weighted Laplacian operator:

$$\Delta z_{\text{precip}}(v_i) = k_{\text{precip}} \sum_{v_j \in N(v_i)} \frac{1}{d(v_i, v_j)} \left( z(v_j) - z(v_i) \right)$$

#### Specification of Terms

* $k_{\text{precip}}$: Precipiton diffusion coefficient ($\text{km}/\text{pass}$), controlling sediment transport rate.
* $d(v_i, v_j)$: Great-circle distance between $v_i$ and neighbor $v_j$ ($\text{km}$).

### 2.4 Thermal Weathering (Slope Repose Enforcement)

When pairwise gradient $S_{ij}$ exceeds the critical slope threshold $S_{\text{repose}} = \tan(\theta_{\text{repose}})$, excess material collapses to enforce mass wasting limits.

The height displacement $\Delta z_{\text{thermal}}$ transferred from $v_i$ to $v_j$ is:

$$\Delta z_{\text{thermal}} = 0.5 \cdot \left( z(v_i) - z(v_j) - d(v_i, v_j) \cdot \tan(\theta_{\text{repose}}) \right)$$

#### Specification of Terms

* $\theta_{\text{repose}}$: Angle of repose (typically $30.0^\circ \le \theta_{\text{repose}} \le 40.0^\circ$).
* $S_{\text{repose}}$: Critical slope tangent threshold ($\tan(\theta_{\text{repose}})$).

---

## 3. Glacial Erosion Engine (Alpine & Ice-Sheet Dynamics)

Glacial erosion models ice mass balance, U-shaped valley carving, alpine landform development, moraine till deposition, and flexural crustal rebound.

### 3.1 Ice Accumulation & Mass Balance

Ice thickness $H_{\text{ice}}(v_i)$ accumulates above the Equilibrium Line Altitude $z_{\text{ELA}}(\text{Lat}_i)$:

$$z_{\text{surface}}(v_i) = z(v_i) + H_{\text{ice}}(v_i)$$

$$\Delta H_{\text{ice}}(v_i) = a_{\text{accum}} \cdot \max\left(0.0, z_{\text{surface}}(v_i) - z_{\text{ELA}}(\text{Lat}_i)\right) - a_{\text{ablate}} \cdot \max\left(0.0, z_{\text{ELA}}(\text{Lat}_i) - z_{\text{surface}}(v_i)\right)$$

#### Specification of Terms

* $z_{\text{surface}}(v_i)$: Total surface elevation ($\text{m}$), equal to bedrock height plus ice thickness.
* $H_{\text{ice}}(v_i)$: Glacial ice column thickness ($\text{m}$).
* $z_{\text{ELA}}(\text{Lat}_i)$: Altitude of the equilibrium snowline ($\text{m}$) as a function of latitude.
* $a_{\text{accum}}$: Annual snow accumulation rate ($\text{m}/\text{m}_{\text{above ELA}}\cdot\text{yr}$).
* $a_{\text{ablate}}$: Annual ice ablation/melt rate ($\text{m}/\text{m}_{\text{below ELA}}\cdot\text{yr}$).

### 3.2 Viscous Surface Slope & Glacial Carving

Ice moves under gravity along the surface gradient $S_{ij}^{\text{ice}}$ across all lower 1-ring neighbors (multi-receiver viscous flow):

$$S_{ij}^{\text{ice}} = \frac{z_{\text{surface}}(v_i) - z_{\text{surface}}(v_j)}{d(v_i, v_j)}$$

The glacial bedrock erosion rate $E_{\text{glacial}}(v_i)$ is formulated as:

$$E_{\text{glacial}}(v_i) = K_g \cdot \left( H_{\text{ice}}(v_i) \right)^a \cdot \left( V_{\text{ice}}(v_i) \right)^b$$

Where sliding velocity $V_{\text{ice}}(v_i)$ follows Glen's Flow Law derivative:

$$V_{\text{ice}}(v_i) = B_g \cdot H_{\text{ice}}(v_i) \cdot \left( S_{\max}^{\text{ice}}(v_i) \right)^3$$

#### Specification of Terms

* $E_{\text{glacial}}(v_i)$: Annual bedrock erosion depth ($\text{m}/\text{yr}$).
* $K_g$: Glacial erodibility coefficient ($\text{m}^{1-a}\cdot\text{yr}^{b-1}/\text{m}^b$).
* $V_{\text{ice}}(v_i)$: Basal sliding velocity ($\text{m}/\text{yr}$).
* $B_g$: Ice viscosity factor ($\text{m}^{-2}\cdot\text{yr}^{-1}$).
* $a, b$: Empirical exponents (typically $a \approx 1.0$, $b \approx 2.0$).

### 3.3 U-Shaped Parabolic Profile & Isostatic Rebound

To transform V-shaped valleys into U-shaped troughs, bedrock erosion is distributed across orthogonal neighbors using a parabolic weighting function $W_{\text{para}}(r_{\perp})$:

$$W_{\text{para}}(r_{\perp}) = \max\left(0.0, 1.0 - \left( \frac{r_{\perp}}{W_{\text{trough}}} \right)^2\right)$$

Bedrock depression $\Delta z_{\text{sub}}$ under glacial load and subsequent post-glacial rebound follow hydrostatic crustal equilibrium:

$$\Delta z_{\text{sub}}(v_i) = -\frac{\rho_{\text{ice}}}{\rho_{\text{mantle}}} \cdot H_{\text{ice}}(v_i)$$

#### Specification of Terms

* $r_{\perp}$: Orthogonal distance ($\text{km}$) from the central glacial flow channel.
* $W_{\text{trough}}$: Target half-width ($\text{km}$) of the U-shaped trough.
* $\rho_{\text{ice}}$: Density of glacial ice ($917\text{ kg}/\text{m}^3$).
* $\rho_{\text{mantle}}$: Density of upper mantle rock ($3,300\text{ kg}/\text{m}^3$).
* $\Delta z_{\text{sub}}$: Bedrock crustal depression ($\text{m}$).

---

## 4. Cratonic Ice-Scouring Engine (Shield Terrain / "Knock & Lochan")

Continental ice sheets overriding flat, crystalline basement rock (e.g., Canadian Shield) strip regolith, truncate high topography, and carve joint lineaments into shallow lochan basins and roche moutonnée knobs.

### 4.1 Structural Lineaments & Directional Grooving

Bedrock erodibility $W(v_i) \in [0.0, 1.0]$ is generated by combining multi-scale 3D Voronoi $F_2 - F_1$ border noise with structural fracture orientations. 

For an ice sheet moving along unit vector field $\mathbf{D}_{\text{ice}}(v_i)$, the relative displacement vector $\boldsymbol{\Delta}_{ij} = \mathbf{p}_j - \mathbf{p}_i$ of neighbor $v_j$ is decomposed into parallel and perpendicular components:

$$d_{\parallel} = \boldsymbol{\Delta}_{ij} \cdot \mathbf{D}_{\text{ice}}(v_i)$$

$$d_{\perp} = \left\| \boldsymbol{\Delta}_{ij} - d_{\parallel} \mathbf{D}_{\text{ice}}(v_i) \right\|$$

Anisotropic grooving depth $\Delta z_{\text{groove}}(v_i)$ along fracture lines is computed as:

$$\Delta z_{\text{groove}}(v_i) = -K_{\text{groove}} \cdot W(v_i) \cdot \left( 1.0 + \gamma \cdot |d_{\parallel}| \right)$$

#### Specification of Terms

* $W(v_i)$: Dimensionless erodibility weight ($0.0 = \text{hard granite}$, $1.0 = \text{highly fractured fault}$).
* $\mathbf{D}_{\text{ice}}(v_i)$: Unit vector ($\mathbb{R}^3$) representing regional continental ice flow direction.
* $d_{\parallel}$: Parallel spatial component along ice flow direction ($\text{km}$).
* $d_{\perp}$: Perpendicular spatial component transverse to ice flow ($\text{km}$).
* $K_{\text{groove}}$: Base grooving incision scale ($\text{m}$).
* $\gamma$: Anisotropy alignment multiplier (dimensionless).

### 4.2 Roche Moutonnée Asymmetry & Regional Planation

Asymmetrical knobs are carved by evaluating the dot product $\alpha(v_i)$ between local bedrock gradient $\nabla z(v_i)$ and ice flow vector $\mathbf{D}_{\text{ice}}(v_i)$:

$$\alpha(v_i) = \nabla z(v_i) \cdot \mathbf{D}_{\text{ice}}(v_i)$$

$$\Delta z_{\text{scour}}(v_i) = 
\begin{cases} 
+k_{\text{stoss}} \cdot \sum \Delta z_{\text{Laplacian}}, & \text{if } \alpha(v_i) > 0 \text{ (Stoss Ramp: Abrasion)} \\
-K_{\text{pluck}} \cdot W(v_i) \cdot |\alpha(v_i)|, & \text{if } \alpha(v_i) < 0 \text{ (Lee Cliff: Plucking)} 
\end{cases}$$

Regional bedrock planation truncates elevated cratonic caps toward a baseline $z_{\text{craton}}$:

$$z_{\text{planed}}(v_i) = z_{\text{craton}} + \frac{z(v_i) - z_{\text{craton}}}{1.0 + \lambda \cdot \max\left(0.0, z(v_i) - z_{\text{craton}}\right)}$$

#### Specification of Terms

* $\alpha(v_i)$: Slope aspect alignment with ice flow (dimensionless).
* $k_{\text{stoss}}$: Abrasion smoothing coefficient (dimensionless).
* $K_{\text{pluck}}$: Quarreling/plucking rate multiplier ($\text{m}$).
* $z_{\text{craton}}$: Mean target elevation of the cratonic shield ($\text{m}$).
* $\lambda$: Planation attenuation factor ($\text{m}^{-1}$).

---

## 5. C++ Data Architecture & Execution Pipeline

### 5.1 Vertex Data Layout

Attributes are allocated in contiguous 1D arrays to maximize CPU L3 cache locality during graph traversals.

```cpp
// Contiguous Memory Layout for C++20 Spherical Fibonacci-Delaunay Mesh
struct VertexErosionData {
    glm::vec3 position;       // 3D Unit Vector on S^2 (12 bytes)
    float elevation;          // Current Bedrock Height in meters (4 bytes)
    float sediment;           // Suspended Sediment Volume (4 bytes)
    float waterAccumulation;  // Fluvial Drainage Area A(v_i) (4 bytes)
    float iceThickness;       // Glacial Ice Height H_ice in meters (4 bytes)
    float erodibility;        // Cratonic Fracture Mask W(v_i) (4 bytes)
    uint32_t flags;           // Sink/Endorheic Bitmask (4 bytes)
}; // 36 bytes per vertex
```

### 5.2 Multi-Pass Pipeline Execution Flow

1. **Phase 1: Structural Initialization**
   - Generate Voronoi fracture lineament field $W(v_i)$.
   - Initialize regional ice velocity vector field $\mathbf{D}_{\text{ice}}(v_i)$.
2. **Phase 2: Glacial & Cratonic Scouring Pass**
   - Truncate high-standing peaks via regional planation ($z_{\text{craton}}, \lambda$).
   - Apply anisotropic grooving along fracture lineaments parallel to $\mathbf{D}_{\text{ice}}$.
   - Compute stoss-side abrasion and lee-side plucking across bedrock knobs.
   - Accumulate ice mass $H_{\text{ice}}$ and apply flexural crustal depression $\Delta z_{\text{sub}}$.
3. **Phase 3: Fluvial Macro-Cycle (Wilbur Protocol)**
   - Execute Spherical Fill Basins via priority queue.
   - Perform topological sort by elevation and compute catchment areas $A(v_i)$.
   - Apply Stream Power Incise Flow channel carving.
   - Execute Precipiton non-linear sediment diffusion across 1-ring neighbors.
   - Apply Thermal Weathering slope repose enforcement.

---

## 6. Summary Specification Table

The functional properties and parameters of all three erosion regimes are summarized below:

| Erosion Regime | Primary Driven Landforms | Governing Formula / Operator | Key Parameters & Inputs |
| :--- | :--- | :--- | :--- |
| **Fluvial (Wilbur)** | V-shaped valleys, dendritic networks, floodplains | $\Delta z = C_{\text{incise}} \left(\frac{A}{A_{\text{ref}}}\right)^m S_{\max}^n$ | $A(v_i)$ (Flow area), $m$ ($0.5$), $n$ ($1.0$), $C_{\text{incise}}$ |
| **Glacial (Alpine)** | U-shaped troughs, cirques, tarns, moraines | $E_g = K_g (H_{\text{ice}})^a (V_{\text{ice}})^b$ | $H_{\text{ice}}$ (Ice depth), $z_{\text{ELA}}$ (Snowline), $K_g$, $V_{\text{ice}}$ |
| **Cratonic Scour** | Knock & lochan, roches moutonnées, linear grooves | $\Delta z = -K_{\text{pluck}} W(v_i) \|\alpha\|$ | $W(v_i)$ (Joint mask), $\mathbf{D}_{\text{ice}}$ (Ice vector), $z_{\text{craton}}$ |
