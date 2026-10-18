# Custom DX12 Ray-Tracing Engine

A Windows C++20 Direct3D 12/DXR editor and renderer for the Sponza atrium, with a separate Jolt-driven ocean demonstration. 

The renderer favors a fast direct-lighting mode for interaction and provides a slower progressive path-tracing mode for indirect lighting. O3DE Atom DX12 acceleration-structure code is credited below where its BLAS/TLAS organization informed the local implementation.

## Implemented Features

| Feature | Status in this engine | Scope and validation |
| --- | --- | --- |
| Direct lighting and shadows | Implemented | PBR material response, one directional sun, analytic sky, DXR shadow rays, adjustable soft-shadow sampling. Exercised by Sponza capture stages 2–3. |
| DXR specular reflections | Implemented | Editor toggle in **Graphics & GI Settings**; a GGX visible-normal-sampled reflection query for eligible glossy primary surfaces, traced on a half-rate sample schedule with inverse-probability compensation. It is exercised on Sponza, an official Khronos glTF metal/roughness sample, and the ocean. The bundled Sponza's missing roughness/metalness maps use conservative dielectric fallbacks; verified stone floor and pillar materials receive documented polished-stone fallbacks so their DXR reflections are visible. |
| Path-traced global illumination | Implemented | Progressive diffuse path tracing with a configurable 1–4 secondary-bounce limit, cosine sampling, and low-discrepancy per-pixel Sobol/Owen sampling. GI is substantially slower than direct mode. |
| Denoising | Implemented when built with NRD | NVIDIA NRD REBLUR diffuse denoising is available in the default build. Without NRD, samples are progressively accumulated and a depth/luminance-aware 9-tap filter is applied to display radiance only; the raw estimator remains in history, so filtering does not recursively blur later frames. The Sponza capture suite passes in both configurations. |
| Temporal AA and reprojection | Implemented | Depth-aware temporal accumulation and camera reprojection are used by the renderer; the moving-camera capture exercises this path. The moving ocean disables temporal accumulation to avoid trails. |
| Texture filtering | Implemented | Sponza texture arrays have linear-light mip chains; DXR hit shaders estimate ray-cone mip levels for base color and alpha-cutout textures. |
| Post-processing | Partial | Exposure and ACES, Reinhard, or linear tone mapping are available. NRD and temporal AA are the temporal image-quality features. There is no bloom, depth of field, motion blur, or general post-effect stack. |
| Ocean and buoyancy | Demonstration | Six Jolt bodies sample analytic waves, radial entry wakes, and neighboring-body displacement. A persistent volume-balanced depression/ridge follows each submerged body's position; vertical support accounts for orientation. Water shading adds multiscale detail, foam, Fresnel, GGX glints, and one-bounce environment/object reflection. Closest-hit rays receive three bounded corrections against the analytic height field inside the flat DXR proxy; this is not a displaced mesh or full fluid-pressure solve. This scene is separate from Sponza. |
| ECS scene selector and Inspector | Implemented, bounded scope | Name/ID filtering, checked O(1) entity-ID lookup, source-node path grouping for glTF primitives, viewport ray picking against transformed world-space mesh bounds, transform editing, and per-entity material overrides. The picker uses AABBs, so it can select empty space inside a mesh's bounds. Storage remains a bundled `SceneEntity` plus parallel render arrays; there are no generic component pools, entity destruction/generation handles, or editable parent-transform graph. |
| RTXDI/ReSTIR, RTXGI, RTXPT | Reference sources only | Source trees are preserved under `vendor/nvidia/` with their license files. These SDKs are not linked into the running renderer. In particular, ReSTIR needs reservoir storage and temporal/spatial reuse passes that this renderer does not implement. |
| RTXMG, RTXMU, RTXNTC | Not present or integrated | These SDKs are not dependencies of the current executable and are not included in the checked-in vendor source tree. |
| Skeletal animation and full O3DE Atom systems | Not implemented | The current scene path renders static mesh instances. It does not include an animation playback/skinning system, Atom material pipeline, or full O3DE lighting/post-processing stack. |

## Render Captures



<img width="1907" height="1011" alt="Screenshot 2026-10-18 181634" src="https://github.com/user-attachments/assets/641ac5f7-64fd-4953-96b9-2112e9b18981" />
<img width="1907" height="1011" alt="Screenshot 2026-10-18 182159" src="https://github.com/user-attachments/assets/d8ac3306-370f-4c17-b044-8332b51953a4" />


(docs/media/sponza-rt-reflections.png)

The latest water preview is captured from the 60 Hz Jolt simulation at 30 frames per second. The DXR quad remains flat broad-phase geometry, while the closest-hit shader corrects the primary ray position and depth against the analytic water height field. On the ocean, the reflection toggle traces a single scene-geometry ray from the wave-corrected water hit and blends the returned object/environment radiance with dielectric Fresnel; when disabled, the analytic-sky reflection remains as the fallback.

![Jolt water entry and rebound sequence captured at 30 FPS](docs/media/water-bounce-30fps.gif)

The **DXR specular reflections** checkbox controls the same reflection path in Sponza, the Khronos material lab, and the ocean. It skips reflection traversal while disabled. Eligible primary materials trace at most one secondary hit; reflected-hit shading does not recurse. The bundled Sponza package has 25 external base-color images but none of its 24 referenced metallic/roughness images. Materials that actually reference missing metallic/roughness maps use a conservative dielectric fallback (`metallic=0`, `roughness=0.65`). The verified stone floor and pillar base/shaft materials use localized polished-stone fallback roughness `0.18`. These are asset-specific fallbacks, not restored authored maps. At 1920×1080 on an NVIDIA GeForce RTX 3050 Laptop GPU, the latest full reflection suite measured Sponza at 104.34/152.19 FPS on/off (68.6% retained), with 1.438 mean channel delta / 8.68% materially changed pixels overall, 5.119 / 34.57% on the floor ROI, and 0.453 / 2.61% averaged across the pillar ROIs. Khronos Metal/Rough Spheres measured 82.76/104.01 FPS, with 3.011 mean / 5.46% changed. Restored captures were within 0.0002 mean error and 0.001% materially changed pixels in Sponza and within 0.0000 / 0.001% in Khronos. The runner requires a reported hardware DXR tier and `DispatchRays`, checks the selected-scene marker, repeats matching 64-sample sequences, analyzes the Sponza floor/pillar ROIs, and preserves separate `sponza_reflections_*.bmp` and `khronos_reflections_*.bmp` captures. Short-window FPS varies with GPU scheduling. On the latest 688×448 ocean A/B, enabling DXR changed 3.29% of full-frame pixels (0.880 mean channel delta) and changed 5.47% of 108,382 baseline blue-water pixels (1.878 mean delta; a pixel counts when summed BGR delta exceeds three code values); the three captures measured about 445–449 FPS. `ocean_capture_runner.py --compare-reflections` now checks the water-only mask and preserves on/off/restored BMPs under ignored `captures/`; `python reflection_capture_runner.py --scene all --native-resolution` repeats the Sponza/Khronos checks.

### Reflection estimator

For isotropic GGX roughness `r`, the shader uses `alpha = max(r², 0.0025)` and samples the visible-normal distribution (VNDF). With normalized surface normal `N`, view direction `V`, reflected direction `L`, and half-vector `H`, the VNDF reflected-direction density is `p(L|V) = D(H) G₁(V) / (4 N·V)`. Cook–Torrance GGX is `fᵣ = D(H) G₁(V) G₁(L) F(V,H) / (4 (N·V)(N·L))`. Multiplying by `N·L / p(L|V)` cancels `D`, `G₁(V)`, and the geometric denominator, leaving the path weight `F(V,H) G₁(L)`. The shader traces exactly one of each consecutive eligible sample pair; a hashed per-pixel phase chooses which complementary Owen-scrambled Sobol subsequence is used. Accepted contributions are multiplied by `1/0.5 = 2`, preserving the estimator mean across the two complementary phases while halving eligible DXR reflection queries over an even sample count. The CTest math target checks the uncancelled PDF/BRDF ratio against the simplified weight over roughness, cosine, and F0 grids, and checks that complementary 64-sample sequences with this weight reproduce the full-sample mean. The image runner treats one 8-bit code-value difference as output quantization for its changed-pixel counter; mean absolute channel error still includes every code-value difference.

## Requirements

- Windows 10/11 x64, Visual Studio 2022 C++ tools, Windows SDK, CMake, and Python 3.
- A Direct3D 12 GPU with hardware DXR Tier 1.0 or newer. The shader library is compiled as HLSL 6.5.
- DXC (`dxc.exe`) discoverable from the installed Windows SDK.
- MSYS2 MinGW64 Assimp and MSYS2 UCRT64 Jolt. Defaults are `C:/msys64/mingw64` and `C:/msys64/ucrt64`; override them at configure time if installed elsewhere.
- The Jolt bridge is compiled with AVX2 and related x64 CPU extensions. The target CPU must support AVX2. The build stages Jolt and its UCRT64 runtime beside the executable.
- CMake 3.30 or newer for the default NRD-enabled build. NRD/NRI are NVIDIA's denoising/integration SDKs; this option can be disabled to build without them.
- Pillow is optional and only needed to rebuild the curated 30 FPS README GIF from the dense BMP sequence (`python -m pip install Pillow`).

## Build

Run these commands from this project directory in a Visual Studio Developer PowerShell. Adjust the MSYS2 roots if needed.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DASSIMP_ROOT=C:/msys64/mingw64 `
  -DJOLT_ROOT=C:/msys64/ucrt64 `
  -DENGINE_ENABLE_NRD=ON
cmake --build build --config Release --parallel
```

The normal build stages both `build/Release/CustomEngine.exe` and `build/Release/WaterDemo.exe`. To rebuild/stage just the water entry point explicitly, use `cmake --build build --config Release --target WaterDemo`.

To build without NRD/NRI, configure a separate build directory with the same generator and `-DENGINE_ENABLE_NRD=OFF`, then build that directory. The NRD-off Release build was also compiled and passed the Sponza capture suite. The default build places the executable at `build/Release/CustomEngine.exe` and stages assets and runtime DLLs beside it. Launch from the project directory so relative asset paths resolve:

```powershell
build\Release\CustomEngine.exe
```

The editor opens in direct PBR + DXR shadows mode for faster interaction. Use **Graphics & GI Settings** to enable path tracing/GI, toggle NRD, adjust bounce count, shadows, sun/sky, exposure, tone mapping, TAA, and render resolution. The `Adaptive (target 60 FPS)` preset adjusts the viewport render scale between 50% and 100%; 1080p/4K fixed modes can be much slower, especially with path tracing.

`build/Release/WaterDemo.exe` starts directly in the interactive Jolt ocean scene. It is staged beside `CustomEngine.exe` and uses the same editor, renderer, assets, and runtime DLLs. From the project root, run `build\\Release\\WaterDemo.exe`; use the editor's **Scene** menu to switch between the ocean, Sponza, and the Khronos reflection lab. `CustomEngine.exe` continues to start in Sponza. The automated `--ocean-test` command remains a separate fixed-duration capture mode.

Camera controls are shown in **Help**: WASD to move, Space/E to ascend, Ctrl/Q to descend, Shift to sprint, hold right mouse to look, and left-click the viewport to select an entity.

## Automated Checks

The runners launch the executable from this directory and refresh generated images in `captures/`.

```powershell
python capture_runner.py
python capture_runner.py --native-resolution
python capture_runner.py --no-nrd
python ocean_capture_runner.py
python ocean_capture_runner.py --no-reflections
python ocean_capture_runner.py --compare-reflections
python ocean_capture_runner.py --gif-30fps
python tools/build_ocean_gif.py
python reflection_capture_runner.py --scene all
```

The Sponza runner validates eight fresh captures: path-traced GI, direct DXR shadows, golden-hour lighting/three-bounce GI, Reinhard exposure, normals, the full editor UI, moving-camera TAA/reprojection, and a material-upload regression image. That last stage edits entities through the scene material API, requires a GPU upload log entry, and requires at least 10% of sampled pixels in the capture to show the test color. It also checks BMP structure, dimensions, freshness, and nonblank image samples.

The ocean runner checks a fresh 688×448 capture, hardware DXR dispatch, six Jolt bodies, at least 5.9 seconds of simulation, measured vertical body movement, final body centers near the water, and blue-water pixels. `--compare-body-displacement` compares common water pixels near the six props with their persistent body field enabled and disabled. `--sequence` and `--gif-30fps` also validate all six initial entries, per-body rise/fall motion, and temporal water change. The bodies remain partly submerged (the current run records zero full-body exits), so the test does not expect a second discrete entry splash. `--gif-30fps` captures every second 60 Hz simulation step (166 BMPs); `tools/build_ocean_gif.py` combines those into the curated README preview. Reflection on/off/on checks the hardware DXR/dispatch log, overall frame delta, and a water-only mask classified from the reflection-disabled capture; it saves on/off/restored evidence under ignored `captures/` and leaves the reflection-enabled image in place.

The reflection runner uses fixed camera/sample settings and compares fresh on/off/restored captures. It checks the requested scene marker, hardware DXR tier and `DispatchRays` log, archives unique captures per scene, requires enabled/restored repeatability within 0.01 mean channel level and 0.1% materially changed pixels (per-channel difference greater than one 8-bit value), validates floor and pillar ROIs, and rejects an on-state FPS below 55% of the off-state measurement. Latest 1920×1080 results: Sponza 1.438 mean / 8.68% materially changed at 104.34/152.19 FPS on/off; floor ROI 5.119 / 34.57%; pillar ROIs 0.453 / 2.61%. Khronos Metal/Rough Spheres measured 3.011 / 5.46% at 82.76/104.01 FPS. Enabled/restored error was 0.0002 mean / 0.001% materially changed for Sponza and 0.0000 / 0.000% for Khronos. These short-window averages vary with GPU scheduling.

## Measured Performance

Latest Release test measurements on an NVIDIA GeForce RTX 3050 Laptop GPU (Windows, 2026-10-18):

| Test mode | Render size | Measured average |
| --- | ---: | ---: |
| Direct + DXR shadows, NRD build | 1920×1080 | 114.3 FPS |
| Path-traced GI with NRD | 1920×1080 | 9.54 FPS |
| Sponza direct + DXR reflections | 1920×1080 | 104.3 FPS on / 152.2 FPS off |
| Khronos material lab + DXR reflections | 1920×1080 | 82.8 FPS on / 104.0 FPS off |
| Ocean + Jolt, DXR reflection enabled | 688×448 | ~449 FPS |

These values are averages over the runner's designated steady-state frames, not guaranteed game frame rates. They vary with driver state and hardware load. Native-resolution GI remains the main performance bottleneck; use direct mode or adaptive/lower resolution for interactive work.

## Project Layout and Generated Files

- `src/` — scene/ECS, DX12/DXR renderer, denoiser bridge, editor, Jolt bridge interface, and tested analytic water-wave math.
- `shaders/` — DXR path tracing, water, and NRD composition shaders.
- `assets/sponza/` — Sponza glTF scene and textures.
- `tools/` — DXIL embedding, runtime staging, Jolt C ABI bridge build source, and optional water GIF encoder.
- `vendor/` — ImGui, cgltf, stb, and NVIDIA source snapshots with their license/notice files.
- `captures/` — test output BMPs; ignored by Git because the runners regenerate them.
- `docs/media/` — curated screenshots and the 30 FPS water GIF embedded in this README; these files are intentionally not ignored.

The root `.gitignore` excludes build/validation directories, Python caches, generated captures, local ImGui state, test bitmaps, and local logs. It does not ignore engine source, vendored source/licenses, assets, test runners, or curated `docs/media/` assets.

## Renderer Architecture

```text
Win32 window + ImGui editor
        |
        +-- scene selection / hierarchy / inspector / graphics controls
        |
        v
Assimp glTF import ---------------------> cgltf fallback
        |                                      |
        +------ mesh/material/texture data <---+
        |     (CPU arrays, mip chain, material/instance tables)
        v
Per-mesh BLAS  ---> per-frame instance descriptors ---> TLAS
                                                        |
SceneCB + TLAS + vertex/index/material/texture SRVs -----+
                                                        v
DXR state object + shader binding table -> DispatchRays(W,H,1)
        |
        +-- RayGen: camera ray, sample stream, bounce loop
        |      +-- TraceRay(primary)
        |      |      +-- AnyHit: alpha-cutout texture test
        |      |      +-- ClosestHit: PBR, optional shadow TraceRay
        |      +-- cosine-sampled indirect rays (GI mode)
        |      +-- one bounded reflection TraceRay (eligible glossy primary hits)
        |
        +-- direct/debug output OR progressive history
        +-- optional NRD REBLUR diffuse -> local composition pass
        +-- output texture -> ImGui viewport -> swap-chain present

Ocean scene only:
Jolt C++ bridge -> fixed-step wave buoyancy -> ECS transforms -> updated TLAS
```

Sponza loads through Assimp first, then cgltf if Assimp rejects the asset. Imported submeshes are flattened into shared CPU vertex/index/material arrays, converted to GPU buffers, and associated with BLAS instances. The texture path creates a texture array and mip chain; DXR hit shaders estimate an explicit mip from triangle world area, UV area, hit distance, camera FOV, and render height. The top-level structure carries per-entity transforms and instance indices used to look up material data.

The hardware path is capability-gated. Device startup enumerates non-software adapters, creates an `ID3D12Device5`, checks `D3D12_FEATURE_D3D12_OPTIONS5::RaytracingTier`, and requires Tier 1.0 or newer. Pipeline startup creates a DXR state object and shader table. The frame loop binds that state object with `SetPipelineState1` and records `DispatchRays`; HLSL contains primary, shadow, GI, water-reflection, and PBR-reflection `TraceRay` paths. The editor checkbox writes the scene constant buffer that gates the reflection queries. The primary-material path only traces for sufficiently metallic/polished materials, and uses a single secondary hit whose material shader is marked to prevent further reflection recursion. Test logs report the selected adapter tier and first dispatch; capture runners also compare images with the toggle on and off.

## Lighting and Sampling Equations

For each opaque surface the shader uses a Cook–Torrance form with GGX normal distribution, Smith masking-shadowing, and Schlick Fresnel:

```text
Lo = (kd * albedo / pi + D_GGX * G_Smith * F / max(4 * NoV * NoL, epsilon))
     * sunRadiance * NoL * shadowVisibility + emissive + skyAmbient
F  = F0 + (1 - F0) * (1 - VoH)^5
kd = (1 - F) * (1 - metallic)
```

The sun is directional. A hard shadow uses one DXR visibility ray; nonzero softness jitters that ray over a disk, so it trades extra Monte Carlo variance for a softer penumbra. The ray payload includes a per-pixel Owen-scrambled Sobol stream shared by stochastic bounce and shadow sampling. Every sample call advances the packed sequence dimension. Cosine-hemisphere sampling uses `pdf(w) = max(N·w, 0) / pi`; for a Lambertian bounce, `(albedo/pi * N·w) / pdf(w) = albedo`, so path throughput is multiplied by base albedo.

For eligible glossy primary surfaces, the DXR reflection ray samples a GGX NDF half-vector. With `NoV = N·V`, `NoH = N·H`, `VoH = V·H`, GGX distribution `D`, Smith term `G`, and Schlick Fresnel `F`, the NDF half-vector density is `pH = D(NoH) NoH`. Reflection's Jacobian gives `pL = pH / (4 |VoH|)`. The code applies the Cook–Torrance estimator weight `F * G * VoH / max(NoV * NoH, epsilon)` to traced radiance. It traces no more than one reflected hit, leaves shadow/GI Sobol dimensions unchanged, and avoids traversal when the editor toggle is off or the material is outside its glossy/metal eligibility range. The ocean uses exact dielectric `F0=0.02` Schlick Fresnel for its view response, one enabled/disabled DXR geometry query, and a procedural-sky fallback.

```text
L = 0, throughput = 1
for each surface hit k up to maxBounces:
    L += throughput * (emission + directSun + skyAmbient)
    if another diffuse bounce is permitted:
        direction ~ cosineHemisphere(N)
        throughput *= albedo
        q = clamp(max(throughput), 0.05, 0.95)
        terminate if xi > q; otherwise throughput /= q
```

The engine accumulates one stochastic path sample per pixel per rendered GI frame. A deterministic sample index and pixel hash make the Sobol/Owen sequence repeatable. In the no-NRD path, stationary samples are progressively averaged; the optional temporal reprojection branch validates reprojected depth and clamps history in YCoCg. NRD REBLUR receives a diffuse-only radiance signal demodulated by primary albedo, world normal/linear roughness, positive linear view depth, zero world-space motion for static Sponza, and first diffuse-bounce hit distance. Primary direct specular and emissive terms bypass the diffuse denoiser so REBLUR does not blur those features. The composite re-modulates denoised diffuse by albedo, adds those preserved terms, then applies exposure, tone mapping, and sRGB encoding.

Without NRD, the progressive estimator uses `alpha = 1/N` through 256 samples, then a fixed `alpha = 1/256` exponential update. The 9-tap fallback filters previous-frame color for display, gated by depth and luminance; it never writes filtered color back to the estimator. Its strength is `0.90 * (1 - saturate((N - 1) / 128))`, preventing repeated spatial blur while helping the first few samples. This is a modest fallback rather than a substitute for NRD.

At low sample counts, path tracing is inherently noisy; NRD and temporal accumulation reduce variance but do not create missing samples. The no-NRD fallback applies a single display-only filter pass and keeps its raw progressive estimator, avoiding the old feedback loop that blurred history repeatedly. Raising bounce count or render resolution raises cost. The editor defaults to direct PBR plus DXR shadows; path tracing/GI is an explicit graphics setting. Adaptive resolution scales the viewport ray dispatch between 50% and 100% of its fitted display size.

## Ocean Wave and Buoyancy Model

The ocean uses five directional traveling waves. With horizontal position `x = (x,z)`, phase `phi_i = k_i dot x + omega_i t`, amplitude `A_i` in meters, wave vector `k_i` in radians per meter, and angular frequency `omega_i` in radians per second:

```text
H(x,t) = sum_i A_i sin(k_i dot x + omega_i t)
gradH = sum_i A_i k_i cos(k_i dot x + omega_i t)
N = normalize((-dH/dx, 1, -dH/dz))
verticalVelocity = dH/dt = sum_i A_i omega_i cos(k_i dot x + omega_i t)
```

The shared CPU/GPU wave parameters are:

| Band | `A_i` (m) | `k_i` (rad/m) | `omega_i` (rad/s) |
| ---: | ---: | ---: | ---: |
| 0 | 0.320 | (0.28, 0.62) | 0.92 |
| 1 | 0.190 | (-0.74, 0.41) | -1.17 |
| 2 | 0.105 | (1.03, -0.78) | 1.75 |
| 3 | 0.045 | (-1.65, -0.18) | -2.35 |
| 4 | 0.014 | (1.25, 1.82) | 3.20 |

`evaluateOceanSurface()` in `shaders/RayTracingPBR.hlsl` mirrors the coefficients in the tested CPU helper `src/ecs/OceanWave.h`. At full detail they use the same base-wave height and analytic derivatives. Rendering attenuates each wave band from its projected pixel footprint and adds smooth-value-noise normal gradients at three scales; those gradients do not change the base-wave height used to compute per-body immersion. Each body also contributes a persistent radial height field to CPU water samples and GPU water shading:

```text
s = saturate((H0(xb, zb, t) + hv - yb) / (2 hv))
u = ((x - xb)^2 + (z - zb)^2) / R^2
a = min(0.45 R, 0.45 m)
u0 = 2.25, q = 2.0
w = 1 / (q sqrt(pi) / 2 * (1 + erf(u0/q))) = 0.2987677926
D(x,z,t) = a s (-exp(-u) + w exp(-((u - u0) / q)^2))
```

Here `(xb,yb,zb)` is the body center, `hv` is its orientation-aware vertical support, and `R = sqrt(hx hz)` is the approximate horizontal footprint radius. `w` is the reciprocal integral of the Gaussian crest over `u∈[0,∞)`, making the depression-plus-crest height integral zero on an infinite plane. Expressing the crest in `u=r²` also gives a smooth, zero gradient at the body center. Height, spatial gradients, and body-motion time derivative are analytic and finite-difference tested; the radial integral is checked numerically. Jolt buoyancy samples radial entry wakes and other-body fields, while body-field immersion is computed against the common base ocean wave for stable CPU/GPU agreement.

For a primary ray `P(t)=O+t d`, the water closest-hit shader solves `F(t)=O_y+t d_y-H(O_x+t d_x,O_z+t d_z)=0`. Its derivative is `F'(t)=d_y-H_x d_x-H_z d_z`; three bounded Newton updates use a derivative guard and finite-value checks. The flat DXR quad supplies the broad-phase hit, and the corrected `t` supplies water shading and depth. This corrects the local visible height field without rebuilding a mesh BLAS. The proxy still bounds where water rays can start, does not clip submerged props as a true displaced surface would, and does not solve fluid pressure or volume transfer among bodies.

Jolt applies buoyant impulses and advances at a 1/60 s fixed step, with at most four catch-up steps admitted per rendered frame; excess elapsed time is counted as dropped simulation time to avoid an unbounded spiral of catch-up work. Water uses dielectric Schlick Fresnel, a GGX sun lobe with a calibrated water-only intensity scale, and one secondary DXR ray for scene geometry; reflected misses use the analytic atmosphere without counting the solar disk twice. The upper-hemisphere shading correction and directional-sun highlight scale are explicit proxy-scene approximations. This is not a spectral ocean, displaced water mesh, full multi-bounce reflection system, or general fluid solver.

Water reflectance uses Schlick Fresnel, a dark blue body color, one finite-intensity GGX directional glint, and a restrained wave-crest foam term. The large quad still defines the distant broad-phase boundary; local primary-ray hit height is corrected analytically, but the surface is not a tessellated or dynamically displaced mesh. Capture tests verify blue-water coverage, Jolt body motion, a local water response around resting props with the persistent body field toggled, and an image delta when DXR geometry reflections are enabled.

## ECS, Selection, and Math Guards

The scene selector filters entity names, IDs, and imported source-node paths; Previous/Next cycle the filtered result set. A transactional ID index maps stable IDs to dense render-array indices and rejects null/duplicate IDs. Imported glTF primitives keep their source ancestry for editor grouping, but parent-node world transforms are still baked into vertices; these groups are not editable transform parents. Inspector material edits clone shared material data into preallocated GPU capacity and remap only the selected instance, preserving shared textures and alpha-cutout metadata for other meshes. Picking transforms all eight local AABB corners by the entity world transform and uses a robust ray/slab test; this selects the correct transformed bounds but can still select empty space inside an AABB. The project still uses a bundled `SceneEntity` record aligned to parallel render arrays; it is not a generic component-pool ECS and has no entity destruction, generation handles, or animation/skinning. Embedded buffer-view/data-URI images currently use a documented factor fallback, and `BLEND` materials are warned and treated as opaque.

Camera projection inputs are finite/clamped before matrix creation. Inverse view-projection paths check the determinant before use. Vector division rejects non-finite or near-zero divisors, normalization reports non-finite lengths, ray picking rejects invalid viewport/ray data, and AABB slabs explicitly handle parallel axes rather than dividing by a fabricated epsilon. The Jolt bridge validates body/step/sample inputs, oriented support extents reject invalid quaternions, and BLAS/TLAS setup checks ranges, finite transforms, determinant, GPU addresses, prebuild sizes, resource allocations, and mapping results. Render-target resize stages replacement resources before swapping them in, and SBT creation checks mapping and shader identifiers. Shader vector normalization has finite fallbacks, and NRD radiance is sanitized before packing. `EngineMathChecks` runs 42 checks, including wave/body derivatives, volume integral, Newton ray/height roots and degeneracy guards, entity indexing, and glTF transform equations. `EngineGltfImportChecks` adds four checks for interleaved, normalized, and sparse accessors and validates all active-scene geometry in the bundled Khronos GLB. Run both with CTest as shown below.

## Build and Dependency Details

The project targets Windows x64 and the Visual Studio 2022 generator. Dependencies are:

| Dependency | Use | Configuration |
| --- | --- | --- |
| Direct3D 12 / DXR | Device, acceleration structures, ray pipeline, swap chain | Windows SDK; hardware DXR Tier 1.0+ |
| DXC | HLSL `lib_6_5` DXIL and NRD composite shader | Discovered from installed Windows SDK; CMake embeds generated DXIL in the executable |
| Assimp | Primary Sponza glTF importer | MSYS2 MinGW64 headers/import library; configured by `ASSIMP_ROOT`; required MinGW runtime DLLs staged post-build |
| cgltf | glTF fallback/scene importer | Source under `vendor/cgltf`; active-scene traversal, accessor stride/sparse decoding, node transforms, and external image URI paths are implemented; embedded image payloads and BLEND transparency remain unsupported |
| Khronos glTF Sample Assets | Reflection validation scene | `assets/khronos/MetalRoughSpheresNoTextures.glb`, CC0 sample; see its adjacent attribution/metadata |
| stb_image | Image decode and texture upload | Source under `vendor/stb`; compiled into the application |
| Jolt Physics 5.3 | Ocean rigid bodies and buoyancy | MSYS2 UCRT64 headers and `libJolt.dll.a`; compiled through `tools/jolt_bridge.cpp` |
| NVIDIA NRD 4.18 / NRI v190 | Optional REBLUR diffuse denoiser on D3D12 | Source under `vendor/nvidia`; default CMake build enables it; requires CMake 3.30+ |
| Dear ImGui | Editor UI and panels | Source under `vendor/imgui` |

Jolt is built as a small C ABI DLL by the configured UCRT64 `g++.exe`, then dynamically loaded by the MSVC application. This isolates MSYS2's C++ ABI from the main executable. The bridge currently enables AVX/AVX2 and related CPU flags, so the target CPU must support AVX2. CMake stages `JoltBridge.dll`, `libJolt.dll`, and the required UCRT64 runtime files beside the executable. Assimp is linked with the MinGW import library; its required MinGW runtime DLLs are also staged. Keep those runtime DLL families paired with their respective MSYS2 installations.

NRD can be disabled at configure time with `-DENGINE_ENABLE_NRD=OFF`. The runtime `--no-nrd` switch exercises the built-in accumulation path without requiring a second binary. RTXDI/ReSTIR, RTXGI, RTXPT, RTXMG, RTXMU, and RTXNTC are not linked or called by the renderer; the first three source snapshots are reference material only and are not CMake dependencies. Animation/skinning, multi-light clustering, a general post-effect graph, bloom, depth of field, and full Atom systems are outside the current feature set.

## Repeatable Capture Tests

```powershell
python -m py_compile capture_runner.py ocean_capture_runner.py reflection_capture_runner.py tools/build_ocean_gif.py
ctest --test-dir build -C Release --output-on-failure
python capture_runner.py                         # 960x544 viewport-scale Sponza
python capture_runner.py --native-resolution     # 1920x1080 Sponza
python capture_runner.py --no-nrd                # built-in temporal fallback
python ocean_capture_runner.py                   # Jolt ocean + water capture
python ocean_capture_runner.py --compare-body-displacement # resting-prop neighborhood regression
python ocean_capture_runner.py --compare-reflections
```

CTest runs 45 math assertions, including ocean-water Fresnel/reflection direction and the GGX reflection estimator reference checks, plus four cgltf/import validation checks. The Sponza runner checks eight fresh BMP captures and their dimensions/content, all frame milestones, Inspector-to-GPU material uploads, selected hardware DXR tier, and `DispatchRays` telemetry. The ocean runner checks active hardware DXR, six Jolt bodies, simulation duration, measured body displacement, final body heights, and blue-water image content. Its body-displacement regression compares shared blue-water pixels only in fixed neighborhoods around the six resting props with the persistent submersion field on/off; its reflection comparison captures with/without/restored reflections and checks a meaningful water-only pixel delta. Captures and logs are generated evidence, not source assets, and are ignored by `.gitignore`.

Performance is measured over fixed capture frames and varies by driver, power mode, window state, and machine load. Use the exact output from the local runner when comparing builds; do not interpret a single average as a universal target. Full-resolution path-traced GI is the costliest mode, while direct PBR plus DXR shadows is intended for interactive editing.

## O3DE Atom Attribution

The BLAS/TLAS wrapper organization and acceleration-structure lifecycle in `src/rhi/AccelerationStructure.h/.cpp` were informed by O3DE Atom's DX12 RHI `RayTracingBlas` and `RayTracingTlas` implementations: [RayTracingBlas.cpp](https://github.com/o3de/o3de/blob/development/Gems/Atom/RHI/DX12/Code/Source/RHI/RayTracingBlas.cpp) and [RayTracingTlas.cpp](https://github.com/o3de/o3de/blob/development/Gems/Atom/RHI/DX12/Code/Source/RHI/RayTracingTlas.cpp). Local comments mark the adapted area. This engine uses its own Direct3D 12 resources, state object, shaders, scene system, and editor; it does not link O3DE libraries or ship Atom code. The referenced O3DE files carry `SPDX-License-Identifier: Apache-2.0 OR MIT`; see the [O3DE license](https://github.com/o3de/o3de/blob/development/LICENSE.txt) and the upstream repository for their terms.

The engine source and project documentation are licensed under the MIT License in [`LICENSE`](LICENSE). Vendored third-party projects, sample assets, and other external content retain their own licenses and notices; see the applicable files under `vendor/` and `assets/`. The O3DE files cited above carry `SPDX-License-Identifier: Apache-2.0 OR MIT`; their attribution and upstream license remain applicable to any O3DE-derived material.

### Ocean body interaction and capture verification

The ocean demo keeps Jolt buoyancy and the visible water at the same mean height. At each 1/60-second physics substep, the scene checks each body's bottom clearance against the CPU analytic wave and descending velocity. An entry stores a world-space origin, substep time and speed-derived strength. A repeat entry replaces that body's previous event, which keeps CPU and GPU wave state consistent and bounds the event set to the number of floating bodies.

The CPU water evaluator adds each active radial impact wave's height, slope and vertical velocity to the shared analytic ocean sample. Jolt uses that sample for the next buoyancy impulses, including the source body's wake and nearby props, so a body reacts to the wave it generated. The body's persistent displacement is excluded from its own buoyancy sample to avoid a self-lifting force, but its prior impact wake is included. Crossing detection compares oriented bottom support against the combined water sample. The shader pins the expanding ring and foam to the stored impact origin, uses live center position for contact depression and horizontal wake, and adds the persistent depression/ridge height and gradient field. The radial, body-field, and time derivatives are analytic; these are bounded wave approximations, not a fluid solver.

Run `python ocean_capture_runner.py --sequence` to build and validate the 12-frame diagnostic sequence under `captures/ocean_sequence/`. For the README animation, `python ocean_capture_runner.py --gif-30fps` captures frames 30, 32, …, 360 from the 60 Hz simulation under `captures/ocean_30fps_sequence/`; with Pillow installed, `python tools/build_ocean_gif.py` encodes these as a 30 FPS GIF at `docs/media/water-bounce-30fps.gif`. The current GIF contains 166 frames at 516×336, averages 30.02 FPS, and is 11,520,710 bytes. The runner verifies six initial impacts, per-body rise/fall motion, and temporal color change among pixels classified as water in both frames 90 and 180. Bodies remain partially submerged in this six-second capture, so no later full-surface crossing is recorded.

Interaction terms modify the analytic hit position/depth, normals, foam, and wakes inside a flat DXR broad-phase quad. They avoid per-frame BLAS rebuilding, but do not deform the quad's outer boundary, clip partially submerged geometry as a real mesh would, or provide full two-way fluid pressure. Run `python reflection_capture_runner.py --scene sponza --native-resolution` to capture DXR reflections on, off, and restored at exactly 1920x1080; compare `captures/sponza_reflections_on.bmp` and `captures/sponza_reflections_off.bmp`.



