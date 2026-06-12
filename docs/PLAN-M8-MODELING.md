# Forge M8 — Modeling & Output Features Plan

**Self-contained plan**: written for a fresh session with no prior context. Read this top-to-bottom before implementing.
**Date:** 2026-06-12. Research below was verified by sub-agents against the live codebase and current library versions.

---

## 0. Current state of the engine (what already exists)

Forge: beginner-focused 3D editor. C++20, OpenGL 4.6, MinGW-w64 GCC 13.2, CMake+Ninja, all deps via FetchContent (NO vcpkg). GPU: Intel Arc Pro (NOT NVIDIA — keep everything vendor-neutral GL).

**Build & verify workflow:**
- Build: `cmake --build --preset mingw-release` (configure: `cmake --preset mingw-release`). Run: `.\run.ps1`.
- Kill leftover editors before relinking: `Get-Process ForgeEditor | Stop-Process -Force`.
- Visual verification: `& .\tools\capture-editor.ps1 -AppArgs @("--rt", "model.glb") -WaitSeconds 8 -OutFile x.png` then Read the png. MUST be invoked in-session with `&` — `powershell -File` silently EATS `--` arguments.
- App CLI: `ForgeEditor.exe [--rt] [--hdri path.hdr] [model.glb ...]`. Logs flush immediately; use -RedirectStandardOutput.

**Shipped features:** primitives (cube/sphere/cylinder/cone/torus/plane + high-res sculpt sphere/terrain), glTF/OBJ import (multi-part → group), PBR raster (HDR RGBA16F + bloom + ACES via PostProcess), shadow map, HDRI environment + IBL (Environment.cpp; equirect-based), point lights + emissive, GPU path tracer (pathtrace.comp: BVH, sun+point NEE, env mip-blur for diffuse, firefly clamp, 1-4 spp/pass, 75% render scale, ground plane, geometric-normal offsets, 0.3s BVH-rebuild settle delay with raster fallback), parent-child hierarchy + groups (Ctrl+G) + multi-select, sculpt mode (Draw/Smooth/Grab/Inflate, weld-map seam handling, COW mesh clone, sparse-diff stroke undo), Unity-style camera (Alt+LMB orbit, F frame, 1/3/7 views, F1-F4 bookmarks), themed ImGui UI (Theme.h, Segoe UI fonts, Tools/Scene/Inspector panels, viewport gizmo toolbar + status bar).

**Key architecture facts (verified, load-bearing):**
- `Mesh` (engine/src/forge/renderer/Mesh.h): Vertex{pos,normal,uv} + uint32 indices; CPU copies retained; `UploadVertices()` exists but **indices are immutable** → all topology ops must build new vectors and swap `entity->mesh = make_shared<Mesh>(...)`. Mesh has `Version()` counter mixed into the RT scene hash.
- `MeshTopology` (engine/src/forge/geometry/MeshEdit.h): weld groups (co-located verts across UV seams), group adjacency, `RecomputeNormalsWelded`. Primitives have duplicate seam vertices — topologically open, geometrically closed.
- `Scene` (engine/src/forge/scene/Scene.h): flat entity vector, `parent` UUID, `WorldTransform(id)` walks chain, `Raycast` returns `RaycastHit{entity, distance, worldPos, worldNormal, triIndex}` + per-entity overload.
- Undo (editor/src/CommandStack.h): whole-Entity snapshots + `SculptStrokeCommand` (sparse vertex diff) + `CompositeCommand`. **New pattern needed for topology ops:** `MeshSwapCommand{UUID, shared_ptr<Mesh> before, after}` — O(1), safe because of existing COW discipline.
- SculptTool COW-clones shared meshes on Enter (`use_count() > 1`); any topology change must force sculpt Exit/re-Enter.
- `PathTracer::Upload` bakes world transforms, skips light-gizmo entities, appends optional ground plane.
- tinygltf's source dir bundles `stb_image_write.h` (already on include path). `stb_truetype.h` NOT present. `FileDialog.h` has Open only — `SaveFileDialog` (GetSaveFileNameA mirror, ~25 LOC) is a shared prerequisite for exports.
- `EditorApp::UpdateRayTracer` reset condition compares viewProj/sun/bounces/env — ANY new RT parameter (aperture, focus, denoise, ortho) must join it.

**Existing pending tasks (task list)**: #1 save/load, #2 drag-drop, #3 render-to-PNG, #4 glb export, #5 snapping, #10 material presets, #11 normal maps, #12 starter scenes, #13 P2 backlog. This plan ADDS the M8 features below; interleave at will (save/load #1 remains highest product priority).

---

## 1. M8 feature set & build order

| # | Feature | Effort | Depends on |
|---|---|---|---|
| M8.1 | Depth of field (path tracer) | S | — |
| M8.2 | Orthographic camera toggle | S | — |
| M8.3 | STL export + watertight check | S | SaveFileDialog |
| M8.4 | Sculpt brushes: Flatten + Pinch | S | — |
| M8.5 | Mirror bake | S | MeshSwapCommand |
| M8.6 | Loop subdivision (+ "keep shape" midpoint mode) | M | MeshSwapCommand, edge map |
| M8.7 | Boolean ops (Manifold library) | M | MeshSwapCommand |
| M8.8 | RT denoiser (à-trous) | M | — |
| M8.9 | Face extrude (push-pull) | M | edge map |
| M8.10 | Turntable GIF export | M | SaveFileDialog, ideally M8.8 |
| M8.11 | Text → 3D mesh | M | earcut.hpp + stb_truetype |
| M8.12 | Voxel remesh | M(large) | do LAST |

Shared prerequisites to build first: **SaveFileDialog** (FileDialog.{h,cpp}), **MeshSwapCommand** (CommandStack.h), **group-space edge map** (`edgeTris: map<(gMin,gMax) -> tris>` in MeshEdit.h — keyed on weld groups NOT raw indices, else UV seams create false boundaries).

---

## 2. Feature specs (distilled from verified research)

### M8.1 Depth of field — pathtrace.comp, ~25 LOC total
Thin-lens after the existing primary-ray unproject:
```glsl
if (u_Aperture > 0.0) {
    vec3 focalPoint = ro + rd * u_FocusDist;
    float r = u_Aperture * sqrt(rand(seed));
    float phi = 6.28318530718 * rand(seed);
    ro += u_CamRight * (r * cos(phi)) + u_CamUp * (r * sin(phi));
    rd = normalize(focalPoint - ro);
}
```
C++: right/up from view matrix columns (`view[0][0],view[1][0],view[2][0]` / `[0][1],[1][1],[2][1]`). UI: Aperture slider 0–0.3 (0=off) + Focus distance; default focus = `length(camera.FocalPoint() - camera.Position())`; "Focus on target" button = one line. BOTH params join the accumulation-reset condition. Disable when ortho (M8.2).

### M8.2 Orthographic toggle — EditorCamera + 1-line gizmo fix
Verified: the invViewProj-based ray gen in pathtrace.comp AND `ViewportRay` picking handle ortho matrices automatically (near/far unproject → parallel rays). Changes:
- EditorCamera: `bool m_Orthographic`, factor `RecalculateProjection()`; ortho extents: `halfH = m_Distance * tan(radians(m_FOV)*0.5)`, `glm::ortho(-halfH*aspect, halfH*aspect, -halfH, halfH, near, far)`. **Gotcha: Zoom()/Focus()/ApplyBookmark() change m_Distance → must also RecalculateProjection() in ortho mode.**
- EditorApp gizmo: `ImGuizmo::SetOrthographic(m_Camera.IsOrthographic())` (currently hardcoded false).
- UI: toggle in Display section; optionally auto-ortho with 1/3/7 presets (Blender style). Skydome slightly wrong under ortho — acceptable; verify visually.

### M8.3 STL export — new engine/src/forge/assets/StlExporter.{h,cpp}, ~200 LOC
Binary STL: 80-byte header (MUST NOT start with "solid"), uint32 count, 50-byte tris (float3 normal + 3×float3 verts + uint16 attr=0). Write field-by-field (struct is 52 bytes due to alignment) or #pragma pack(2). File size must equal 84+50·count.
- Units: add "Scale (mm/unit)" field, default 100. Y-up → Z-up: rotation (x,y,z)→(x,−z,y) — proper rotation, winding preserved. Negative-determinant world transforms: swap v1/v2.
- Watertight check: weld by position (reuse MeshTopology grouping) → drop degenerate tris → undirected edge map → every edge exactly 2 tris (1=hole, >2=fin) + the two directed edges opposite (else flipped face). Report "watertight / N open edges / N flipped"; export anyway with warning.
- Bake `scene.WorldTransform(e.id)` like PathTracer::Upload; skip light entities; selected-subtree (`SubtreeOf`) or whole scene.

### M8.4 Sculpt brushes Flatten + Pinch — SculptTool.cpp ApplyDab
Needs a per-dab pre-pass over the region computed BEFORE moving (add region-gather at top of ApplyDab).
- **Flatten**: weighted centroid `cbar = Σ w·rep/Σw`, plane normal `nbar = normalize(Σ w·normal)`; `newPos = rep − nbar·dot(rep−cbar, nbar)·clamp(strength·w,0,1)`. Compute plane once per dab from pre-move positions (recomputing per substep jitters). Sign-gate variants: d>0 only = shave peaks.
- **Pinch**: `tang = (localPos − rep) − n·dot(localPos−rep, n)`; `newPos = rep + tang·clamp(strength·w·0.5, 0, 0.5)` — the 0.5 clamp prevents collapse into zero-area pleats. Ctrl-invert = spread.
Brush grid becomes 2×3. Mirror path works unchanged (second ApplyDab call mirrors).

### M8.5 Mirror bake — MeshEdit.cpp, S
Across object-space x=0: verts with |x|<1e-4 snap to x=0 and map to themselves (index-shared seam = watertight, no epsilon merge); others duplicated with x,normal.x negated; mirrored tris get REVERSED winding (non-negotiable); skip tris fully in the plane. UV unchanged (optional flip toggle). Then new Mesh + topology rebuild + RecomputeNormalsWelded + MeshSwapCommand. Skip clipping of geometry crossing x<0 in v1 (warn only).

### M8.6 Loop subdivision — MeshEdit.cpp, M
Loop (NOT Catmull-Clark — that's for quads). Split topology trick: **positions in weld-group space, connectivity/UVs on raw indices** — both seam copies look up the same group-edge position (no cracks) while keeping their own UVs (seam preserved).
- Odd (edge) vert, interior: `3/8(P0+P1) + 1/8(PL+PR)`; boundary edge (1 adjacent tri in group space): `1/2(P0+P1)`.
- Even vert, valence k: `(1−kβ)v + βΣnbrs`, Warren's `β = 3/(8k)` (k>3), `3/16` (k=3); boundary: `3/4 v + 1/8(b0+b1)`.
- New UVs = raw midpoints; emit 4 tris per tri. Non-manifold edges → fall back to midpoint. Drop degenerate tris first.
- **Shrinkage is expected** — add a "keep shape" toggle = plain midpoint subdivision (no smoothing; adds sculpt resolution).
- Cap: disable button when `4·triCount > 100000`. Growth V'≈4V. Force sculpt Exit before swap.

### M8.7 Booleans — Manifold v3.5.1, M (~1.5-2 days)
Winner: Apache-2.0, active (v3.5.1, June 2026), zero deps with right flags, MinGW-proven (OpenSCAD MXE builds). MCUT = LGPL/commercial, Cork dead, CGAL = GPL+GMP pain, hand-rolled BSP = coplanar-face disaster (do NOT).
```cmake
set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_JSBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CROSS_SECTION OFF CACHE BOOL "" FORCE)  # drops Clipper2
set(MANIFOLD_PAR OFF CACHE BOOL "" FORCE)            # no TBB
set(MANIFOLD_DOWNLOADS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)       # required on Windows
FetchContent_Declare(manifold GIT_REPOSITORY https://github.com/elalish/manifold.git GIT_TAG v3.5.1 GIT_SHALLOW TRUE)
```
Link `manifold::manifold`. Flow: bake world transform into positions (flip winding if `determinant(mat3(world)) < 0`) → MeshGL numProp=3 → **`m.Merge()` (CRITICAL: closes UV-seam duplicate topology; our primitives FAIL NotManifold without it)** → check `Status() != NoError` → `a - b` / `a + b` / `a ^ b` → `IsEmpty()` check → GetMeshGL → recompute normals (uv=0; cut faces have no meaningful UVs). Result entity gets identity transform (world-space baked). UI: two selected entities + op buttons (Union/Subtract/Intersect) in a "Modify" section; subtract order = primary minus secondary; undo = Composite(delete sources or keep+hide, add result). User-facing errors: "Mesh is not closed — booleans need watertight meshes" / "Result is empty". New file: engine/src/forge/geometry/MeshBoolean.{h,cpp}.

### M8.8 RT denoiser — à-trous wavelet, M (~150 LOC GLSL + 130 C++)
Rejected: bilateral-on-display (vaseline), OIDN (ISPC+TBB on MinGW = gamble, 50MB, CPU readback). Plan:
- Buffers (in PathTracer::Resize): m_AlbedoTex RGBA16F (first-hit albedo — exact, materials are flat factors), m_NormalDepthTex RGBA16F (xyz=first-hit normal, w=primary t, 1e30 sky), ping/pong RGBA16F.
- pathtrace.comp: write guides on s==0; REMOVE display/tonemap write (moves to resolve.comp).
- atrous.comp ×4 iterations (step 1,2,4,8): 5×5 B3-spline kernel `(3/8, 1/4, 1/16)`, edge weights: `wN = pow(max(dot(cN,qN),0), 128)`, `wD = exp(-|cD-qD|/(cD+1e-3))`, `wL = exp(-|lumDiff|/4)` (widen by 1/sqrt(spp)), `wA = albedo diff < 0.05 ? 1 : 0`. Sky pixels pass through.
- resolve.comp: `strength = clamp(k/sqrt(spp)·8, 0, 1)`, `mix(rawMean, filtered, strength)`, ACES+gamma → display. Skip à-trous past ~2048 spp. UI: Denoise checkbox + strength.

### M8.9 Face extrude (push-pull) — M, hardest UX
- Coplanar region flood fill from `hit.triIndex/3` across the group-space edge map: accept iff `dot(n, n0) > cos(2°)` AND `|dot(n0, centroid) − d0| < eps·meshScale` — both vs the SEED (prevents drift around smooth bends). Abort on >2-tri edges.
- Boundary = region edges with exactly 1 region tri; keep directed (region on left); chain-walk into loops (multiple loops legal).
- Build once at click: duplicate region verts (top cap), remap region tris; per directed boundary edge make 4 fresh wall verts + 2 tris ordered (bottomA, bottomB, topB, topA) = outward for +n0. Drag: closest point between mouse ray and line `hitPos + s·n0` (formula in research; degenerate when ray ∥ n → keep last s); move cap+wall-top verts, RecomputeNormalsWelded + UploadVertices per frame; MeshSwap undo at release.
- Known/accepted: single-tri spike on curved surfaces, smeared rim shading from welded normals, self-intersection when pushing through. Negative offset → flip wall winding at stroke end.

### M8.10 Turntable GIF — M
gif-h (github.com/charlietangora/gif-h, public domain) — RGBA8 API matches the display texture exactly. Amortized job state machine in EditorApp (NOT a blocking loop): per frame i of N=48: save camera bookmark, `SetOrbit(pitch, baseYaw + 2πi/N)` around selection/scene center, drive PathTracer directly (ResetAccumulation + repeated Dispatch ~8spp/UI-frame until M=64–256 spp; bypass settle logic), `glGetTexImage` RGBA8 + CPU row flip (GL bottom-up), GifWriteFrame (delay=4 → 25fps). Progress modal; restore bookmark. PNG-sequence radio option = free via stb_image_write. MP4 deferred (minih264+minimp4 exist if ever needed).

### M8.11 Text → 3D — M (~400 LOC), new MeshFactory::Text(text, fontPath, depth)
Deps: stb_truetype.h (public domain; NOT yet in tree) + earcut.hpp (mapbox, ISC; handles holes natively via ring lists — ring 0 outer, rest holes). Pipeline per glyph: `stbtt_GetCodepointShape` (vmove/vline/vcurve; quadratic bezier → 8–12 segments; reject CFF cubics) → contour classification by signed area + point-in-polygon containment (don't trust font winding; handles 'i', '%', 'O', 'B') → earcut front cap; back cap = same indices reversed, z=−depth; sides = quad strips per contour with duplicated verts for hard edges → kern advance (`stbtt_GetCodepointKernAdvance`) → normalize height to ~1, center. Watertight by construction (STL synergy: printed nameplates). Skip bevels. UI: popup with text field + depth slider, spawn like a primitive. Use C:/Windows/Fonts/segoeui.ttf default.

### M8.12 Voxel remesh — M(large), LAST
Scope-pinned version only: watertight input assumed (true for our meshes), narrow-band unsigned distance (point-tri within ±2 cells) + **outside flood-fill for sign** (BFS from grid boundary; no winding numbers, no BVH) → marching cubes (public-domain 256-case tables, ~300 LOC) → weld via Quantize-hash → **Taubin smooth 5–15 iters (λ=0.5, μ=−0.53)** — mandatory or output looks worse than input → normals. Grid ≈100–150 cells on longest axis ("detail" slider); UVs destroyed (box-project or zero); force sculpt re-enter; MeshSwap undo. If scope creeps toward winding numbers/adaptivity → STOP, not worth it here.

---

## 3. UI placement
New "Modify" section in the Tools panel (between Sculpt and Lighting & Sky): Boolean Union/Subtract/Intersect (enabled when exactly 2 selected), Subdivide Smooth + Keep-Shape toggle, Mirror X, Extrude (or viewport push-pull tool button), Remesh + detail slider, Add 3D Text. Display section: Ortho toggle. Ray Tracing section: Aperture/Focus/Denoise. Inspector or Tools: Export STL. Tools: Turntable GIF button.

All destructive ops push undo commands; all mesh swaps bump the RT hash automatically (mesh pointer changes).
