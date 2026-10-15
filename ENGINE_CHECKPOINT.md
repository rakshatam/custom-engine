# Engine Rendering Checkpoint

**Scope:** `engine/` is the editable project. The adjacent `o3de-development (...)` folder is a read-only reference and is not modified.
**Checkpoint started:** 2026-10-15 (Asia/Calcutta)

## Baseline findings

- User screenshot: 1920x1080 viewport, grainy image, reported 6.4 FPS / 144.97 ms, accumulated samples = 4.
- Existing `engine_run_log.txt` identifies an NVIDIA GeForce RTX 3050 Laptop GPU and a 25-instance Sponza scene (262,267 triangles).
- Existing `stdout.txt` records 287.22 ms / 3.48 FPS at test frame 160.
- The renderer is a DXR path tracer with PBR materials, direct lighting and ray traced shadows, progressive accumulation, a TAA/reprojection path, and tone mapping. Several of these controls are already present; the current implementation needs correctness and performance work.
- The renderer currently dispatches full viewport resolution, performs a GPU readback/copy path in the main loop, and presents with VSync. I am measuring which of these dominate before changing scheduling or quality defaults.
- The source HLSL is paired with an embedded bytecode header (`shaders/RayTracingPBR_Bytecode.h`); any shader edit must regenerate and validate that bytecode before it can affect the executable.

## Checkpoint log

### 2026-10-15 — Initial inspection

- Confirmed user intent: improve image quality and frame rate, use O3DE as a reference, test the engine, remove only clearly redundant project-local artifacts, and document checkpoints/faults/fixes here.
- Confirmed that the initial test captures and build tree contain generated outputs plus several old diagnostic executables. Cleanup scope will be verified against the active CMake project before removal.
- No source changes or deletions made yet.

## Faults and changes

| State | Finding / change | Evidence |
|---|---|---|
| Open | High GPU cost and noise at low sample count | Screenshot telemetry: 4 spp, 6.4 FPS; existing test log: 3.48 FPS at frame 160 |
| Investigating | Whether temporal history and frame scheduling are correct | Shader has both accumulation and history code; DXR dispatch copies accumulation to history each frame |
| Open | Embedded shader bytecode may be stale relative to source | CMake currently has no shader compilation rule |
| Open | Build contains stale diagnostic targets/artifacts | `build/Release` includes multiple non-engine test executables not in the current CMake target list |

## Verification

- Pending: clean rebuild, engine capture run, image/telemetry review, and regression check.

### 2026-10-15 — Root causes narrowed

- Confirmed the viewport is ray traced at a hard-coded 1920x1080 regardless of the much smaller editor viewport. The screenshot shows that full-HD buffer is being stretched into the center panel, wasting most of the 3050 Laptop GPU's ray budget. Existing relative-scale presets only take effect after a manual settings change.
- The camera shader aspect is hard-coded to 16:9 in `Scene::updateGpuBuffers`; it should follow the actual viewport aspect.
- Default GI sets `maxBounces = 2`, while the shader loops from bounce 0 through that value (three surface hits), each of which also computes direct light/shadow work.
- The embedded DXR shader binary is manually checked into the source tree, but a Windows SDK `dxc.exe` is installed, so shader fixes can be compiled into the actual executable.
- Read-only O3DE reference check: Atom TAA uses motion vectors, depth-aware reprojection, neighborhood filtering, and anti-flicker feedback. The local shader approximates temporal blending but lacks depth rejection; I’ll add a lightweight depth validity check rather than claim feature parity.
- Test cleanup candidates confirmed: `check_lion.py` and `check_mesh11.cpp` are unreferenced one-off diagnostics; generated `build/` also contains stale diagnostic targets absent from the active `CMakeLists.txt`. I will preserve Sponza assets and the existing capture suite as test evidence.

### Planned fix set

1. Default rendering to a dynamic 75% viewport scale and keep the other relative/fixed presets useful; start with a 720p internal buffer while docking settles.
2. Match camera aspect to the actual viewport and reduce the default GI path depth by one bounce while preserving the editor controls for higher-quality settings.
3. Validate temporal history by comparing reprojected scene depth, regenerate the embedded DXR library, and update the checkpoint after the implementation/build milestones.
4. Rebuild, run the capture test, review captured output/telemetry, then remove only confirmed redundant diagnostics/stale build artifacts.

### 2026-10-15 — Implementation and first clean build

- Changed the default to a viewport-relative 75% render scale; dynamic presets now track viewport resizing, fixed presets remain available, and the displayed image is aspect-fitted instead of stretched.
- The camera and view-projection now use the render target's aspect ratio. GPU target resize also resets accumulation.
- Reduced default path depth to one indirect bounce (higher bounce counts remain selectable); raised TAA jitter to the intended one-pixel Halton sequence and added linear-depth rejection for stale history at disocclusions.
- Removed the low-SPP unsharp mask that was sharpening Monte Carlo noise. Temporal blend slider limits now match the shader's supported range.
- Fixed first-frame history use, delayed UI accumulation resets, and a descriptor-index sentinel bug that allocated new descriptors on every resize.
- Replaced the wall-clock desktop screenshot script with a deterministic test runner that waits for the engine's own frame-driven captures and validates all seven BMPs.
- Added DXC shader compilation and bytecode embedding to CMake. A fresh Release build in `build_validation/` succeeded; the compiler generated a 23,360-byte DXIL library from the edited HLSL. No compiler errors or warnings were reported.

## Current verification status

- Complete: Python syntax check for the capture runner and DXIL embedding helper.
- Complete: Fresh Visual Studio 2022 / Windows SDK Release build.
- Pending: Run the seven-capture integration sequence, inspect the image and FPS line, then clean obsolete diagnostics and the stale build tree.

### 2026-10-15 — Clean-launch packaging fix

- The first clean executable exited with Windows status `0xC0000135` before the test loop because its Assimp runtime dependency was missing. The previous output folder had hidden this dependency by carrying copied DLLs from an older build.
- Added explicit, configurable staging for Assimp's MinGW runtime DLL set as a CMake post-build step. A clean Release rebuild now compiles the shader and stages all seven required runtime DLLs beside `CustomEngine.exe`.
- Replaced an overly broad runtime dependency scanner after it spent several seconds traversing the MSYS DLL tree; the deterministic list is based on the actual import chain inspected with `dumpbin`.
- Pending: launch the freshly staged executable, verify all frame-driven captures, check the new FPS measurement, and inspect the render.

### 2026-10-15 — First runtime verification

- The freshly staged Release executable launched and completed all seven frame-driven capture milestones. The runner validated each BMP header and confirmed the test suite's success line.
- Frame 160 measured **46.43 FPS** (about 13.3× the prior 3.48 FPS test result); the full-editor capture displayed about **50 FPS / 20.32 ms** on the RTX 3050 Laptop GPU at 688×448 internal resolution.
- Visual review confirms the large grain reduction from eliminating the noise-sharpening pass and cutting the traced pixel count, but low-sample shadow/path noise remains visible at 26 SPP. I’m adding a small depth- and luminance-aware cross filter over accumulated history, then will rerun the exact same capture test and compare performance.

### 2026-10-15 — Temporal cleanup and converged capture

- Added a five-tap cross-bilateral history filter for stationary GI. It weights neighbors by prior-frame depth and luminance, so it suppresses low-sample noise while rejecting depth edges and strong color changes.
- Moved the full-editor capture from frame 170 to frame 230 so it records a stationary, more converged view after the camera-motion test. The same seven-capture test passes; frame 160 measured **49.64 FPS** and the UI capture showed **46.3 FPS** while displaying the 688×448 internal render.
- The inspected full-editor capture is substantially less speckled and has stable temporal output. The 75% viewport scale trades some fine texture sharpness for the roughly 50 FPS target; 100% native viewport and supersampling presets remain available.
- Updated UI text and comments to identify the renderer as custom and O3DE-inspired instead of presenting it as the O3DE Atom pipeline itself.

### Final cleanup plan

- Rebuild the final executable into the normal `build/` output using the new CMake DXC and runtime-staging rules.
- Remove the temporary validation build, stale generated diagnostic targets, unreferenced one-off mesh check scripts, old test desktop capture, and Python bytecode cache. Keep current Sponza assets and all seven verified capture images.

## Final verification — 2026-10-15

- The normal `build/` Release target was configured and rebuilt from the current source. DXC regenerated and embedded the DXR library; the post-build step staged all seven Assimp runtime DLLs.
- `python capture_runner.py` exited successfully against `build/Release/CustomEngine.exe`. All **7/7 milestones** and **7/7 BMP captures** passed validation.
- Final frame-160 measurement: **48.99 FPS / 20.41 ms** on the RTX 3050 Laptop GPU. Baseline in the prior test log was **3.48 FPS / 287.22 ms**. The screenshot telemetry was **6.4 FPS**, so the measured test result is about 7.7× the reported screenshot rate.
- The final UI capture is `captures/06_full_editor_ui.bmp`; it is taken after 230 test frames to show stationary GI accumulation after the camera-motion test. Internal ray-traced resolution is viewport-relative (688×448 in this run); the displayed viewport has less grain at the cost of some fine-detail sharpness. Higher native-resolution and supersampling presets remain available.
- Removed the unreferenced `check_lion.py` and `check_mesh11.cpp` diagnostics, the stale checked-in DXIL dump header, and `stdout.txt`. Kept the Sponza source assets, current capture suite, run log, and user screenshots. O3DE remained read-only.
- **Generated-output cleanup limitation:** the execution policy rejected PowerShell deletion of the preexisting stale build outputs (`blocked by policy`). As a result, old diagnostic `.exe`/Visual Studio project files under `build/`, the temporary `build_validation/` tree, and Python cache artifacts remain. The active Release executable and seven current captures are verified.

## Build and test commands

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target CustomEngine
python capture_runner.py
```

Use `-DASSIMP_ROOT=<path>` at configure time if Assimp is installed somewhere other than the current default `C:/msys64/mingw64`.

## Follow-up checkpoint — editor controls and frame pacing

- Kept the O3DE development checkout read-only. This project already has a custom DXR path-tracing pipeline with RT shadows, one-to-four GI bounces, depth-aware temporal AA, ACES/Reinhard tone mapping, and sun/sky controls.
- Removed the unconditional 60 Hz presentation cap: the swap chain now detects tearing support, creates the appropriate flag, and the editor exposes a VSync toggle (off by default). Resize preserves the swap-chain flags.
- Added a direct path-tracing/GI switch. Turning it off selects direct PBR plus DXR shadows; debug-normal and albedo views remain separately selectable.
- Added an adaptive resolution preset targeting roughly 60 FPS. It adjusts internal viewport scale in 5%/2.5% steps between 50% and 100%, waits 3 seconds before quality reductions, and waits 8 seconds before recovery; fixed presets remain available. It is opt-in to avoid automatic target recreation during normal editing.
- Improved the scene selector with name-or-ID search, per-row triangle counts and stable IDs, result count, previous/next navigation, and clear selection.
- First adaptive test passed all captures but exposed a quality fault: 0.75-second scale changes repeatedly recreated render targets and restarted temporal accumulation (final capture fell to 4 SPP). The controller is now slower and opt-in; a follow-up capture returned to 86 SPP with visibly reduced noise.
- The editor now starts in direct PBR plus DXR shadows for responsive editing; test mode explicitly enters GI before the path-tracing captures so those checks remain valid. Added a direct-mode average FPS measurement to the capture run.
- Updated the capture runner to report direct-lighting and path-traced GI FPS independently instead of accidentally labeling the first measurement as frame 160.

### Follow-up verification

- Release rebuild completed successfully after the editor/RHI changes.
- Capture suite completed with **7/7 milestones** and **7/7 valid BMPs**. Latest measured direct PBR + DXR-shadow average: **296.14 FPS**; path-traced GI at frame 160: **49.52 FPS**. These are in-engine scene measurements and will vary with GPU load and render scale.
- Full editor capture (`captures/06_full_editor_ui.bmp`) shows the path-traced quality mode at 688×448 internal resolution, 86 accumulated samples, and about 42 FPS at capture time; visual noise is substantially reduced relative to the supplied screenshot.
- The local O3DE reference contains Atom and PhysX gems; a Jolt gem/source integration was not found. This app has no physics-world/collider lifecycle or imported animation playback system, so I did not add an unused third-party physics dependency or claim animation support. A safe Jolt/animation integration requires a real component and asset/runtime design, rather than copying O3DE modules into this standalone DXR renderer.
- O3DE reference remained read-only.

### Latest checkpoint — explicit shadow control

- Added the editor's ray-traced shadow checkbox. It disables shadow rays while retaining direct sun/sky lighting; the shader consumes an existing constant-buffer padding word, so the CPU/GPU buffer layout size is unchanged.
- Extended test mode to toggle shadows off and back on, and retained the test runner's distinct direct/GI FPS reports.
- Latest DXC + Release build succeeded; capture runner passed all **7/7** images/milestones. In this run direct PBR + DXR shadows averaged **252.23 FPS** and GI measured **39.49 FPS** at frame 160 (other runs varied; GPU load and temporal state affect these figures).
- Normal editor startup now uses direct lighting with ray-traced shadows, VSync off, and a stable 75% internal resolution. Path-traced GI, shadow disable, VSync, and adaptive 60-FPS resolution are editor controls.

## Water scene request — initial investigation

- Correction to the prior Jolt note: the installed package is `mingw-w64-ucrt-x86_64-jolt-physics` **5.3.0**, under `C:/msys64/ucrt64` (not `mingw64`). Its headers, shared DLL, and import library are present. The earlier check searched the wrong triplet.
- The renderer already builds per-scene BLAS/TLAS and supports rebuilding instance transforms. The ocean will use a separate `Scene` instance and editor selection so the Sponza assets, materials, camera setup, and capture tests remain intact.
- Checking compiler/runtime compatibility and Jolt API before connecting it to the MSVC renderer. Plan: isolate Jolt behind a small C ABI bridge, use Jolt rigid bodies plus buoyancy forces, add animated ocean shading and reflected-ray shading to the water material, and verify switching back to Sponza.

### 2026-10-15 — Ocean scene implementation checkpoint

- Added an independent Ocean Buoyancy scene with a broad ocean surface and six colored floating props. It is exposed through a Scene menu and initialized only when selected, so Sponza's entities, assets, camera, and render resources remain separate.
- Integrated MSYS2 UCRT64 Jolt 5.3 through a small C ABI bridge. The engine loads Jolt dynamically, stages its runtime under `jolt_runtime/` to avoid overwriting Assimp's MinGW64 runtime, runs fixed 60 Hz physics steps, and syncs floating transforms into the scene TLAS.
- Added analytic animated ocean shading, wave normals, Fresnel reflections traced through DXR, a sun glint, and an ocean-specific non-temporal mode to prevent moving objects from leaving temporal trails. Existing Sponza materials have no water flag and stay on their original shader path.
- Enabled one extra DXR recursion level only to support water reflection rays that hit shaded objects and then cast their normal shadow ray.
- Pending: compile bridge/engine, resolve any compiler/runtime issues, run the untouched Sponza capture suite and a dedicated Ocean capture test, then review the generated images and record performance.

### 2026-10-15 — Ocean integration faults and fixes

- The first live Ocean attempt exposed two real integration faults: per-frame TLAS rebuilds reused upload data unsafely and stalled/crashed; unbounded reflected-hit shading triggered D3D12 device removal. TLAS now uses update-capable builds with per-frame instance upload buffers; water reflection rays stop after one reflected hit and run at recursion depth two.
- Physics simulation and render timing are separated in `--ocean-test`: 360 frames advance a fixed 6 seconds of Jolt simulation while the runner reports wall-clock FPS. This avoids a fast uncapped render loop advancing too little physics time.
- Ocean-only visual fixes lower the flat ray-hit slab under the analytic wave mean so the floating props remain visible, brighten open-sky fill for shaded object faces, and keep temporal history off for moving bodies. Sponza still uses its prior material/shader settings.
- Verification so far: the full Sponza capture suite passes **7/7**, and the Ocean runner validates a **688×448** capture with **six Jolt bodies** and about **350–425 FPS** across recent runs. Continuing final image review and rerunning both suites after the latest glint adjustment.

### Final Ocean / Sponza verification — 2026-10-15

- Updated the editor scene switch to reset path-tracing/TAA accumulation on scene changes, preventing the shared viewport target from showing the previous scene's history.
- Water reflections default on and can be switched off under Ocean graphics settings. A single ray reflects the sky and objects; reflected objects use a compact unshadowed response, keeping recursion bounded and avoiding D3D12 device hangs. The original path-tracing, direct PBR, and DXR shadow controls still apply.
- Release build succeeded. `capture_runner.py` passed all **7/7 Sponza captures** after the shader and acceleration-structure changes; latest test readings were **258.33 FPS direct + DXR shadows** and **47.17 FPS path-traced GI** at the existing internal test resolution.
- `ocean_capture_runner.py` passed: **688×448 BMP**, six live Jolt bodies, **5.983 seconds** simulated, and **354.57 FPS** measured from wall time. The water test also passed with ray-traced reflections disabled as a performance/control check. The reviewed Ocean image shows the blue animated surface, colored buoyant props, their traced reflections, and sky/sun response.
- The older note that Jolt was not integrated is superseded: UCRT64 Jolt 5.3 is loaded through the isolated C ABI bridge. O3DE remains reference-only.
- Existing stale `build_validation/` and diagnostic build outputs are not engine source or runtime dependencies; the prior PowerShell cleanup attempt was rejected by the execution environment, so they remain. The new Ocean capture and runner are retained as regression assets.

### Verification update — 2026-10-15

- Final rebuild and rerun: Sponza **7/7 captures passed**, direct + DXR shadows averaged **265.35 FPS**, and path-traced GI measured **37.99 FPS** at frame 160. Run-to-run GPU load and temporal workload change these figures.
- Ocean reflections-on capture passed at **688×448**, **six Jolt bodies**, **5.983 seconds** simulated, **352.28 FPS**; the reflection-off control also passed at **376.74 FPS**. The final retained capture is `captures/08_ocean_buoyancy.bmp`.
- Confirmed the editor scene switch clears accumulated history on each switch; Ocean-only graphics settings expose the ray-traced reflection toggle.

### 2026-10-15 — NVIDIA SDK integration checkpoint

- Verified the requested names against NVIDIA's official GitHub organization: RTXPT, RTXDI, RTXGI, RTXMG, RTXMU, NRD, and RTXNTC are separate projects. “RTXMGRTXMU” refers to RTX Mega Geometry and RTX Memory Utility; no combined SDK exists.
- Hardware here is an RTX 3050 Laptop GPU with 4 GiB VRAM and DXR. RTXMG's current sample requires 10 GiB VRAM, so adopting its sample/large-geometry path is a poor fit. RTXNTC would help texture storage/streaming only after adding an NTC asset-conversion/inference pipeline; it does not denoise lighting or correct saturated material colors. RTXMU is a memory utility rather than a renderer and requires a memory-allocation integration.
- NRD directly targets the visible low-sample path-tracing noise. Its official integration expects motion vectors, packed normals/roughness, view depth, noisy lighting signals, per-frame dispatches, and persistent history. This engine currently outputs only combined radiance plus depth/color history, so I am checking the smallest valid D3D12 NRD path before wiring it in; feeding NRD incomplete guides would make temporal artifacts worse.
- RTXDI, RTXGI, and RTXPT each assume broader sampling/radiance-cache/path-tracing pipelines. I am checking their required device features and host/resource assumptions against this custom DXR 1.1 engine before selecting any parts. The reference O3DE checkout remains untouched.
- Current known baseline remains: RTX 3050 direct+DXR shadow path around 250–300 FPS; accumulated GI around 38–50 FPS at the reduced 688×448 test render size. New SDK work is not yet integrated or tested.

#### SDK checkpoint — deeper compatibility review

- Cloned the official NVIDIA `NRD` source into `vendor/nvidia/NRD` (v4.18.0) for a real SDK integration, not a reimplementation. The sample's recommended NRD workflow wants guides (`IN_NORMAL_ROUGHNESS`, `IN_VIEWZ`, motion vectors) and NRD-packed radiance/hit distance; the current shader lacks those explicit outputs and its current cross-filter is only a quick screen-space approximation.
- Verified two separate image-quality faults to fix alongside NRD: glTF base-color textures are stored in sRGB bytes but the engine's texture-array SRV samples them as linear UNORM, and tonemapping applies a hard-coded 2.2 gamma instead of the standard piecewise sRGB transfer function. These are likely contributors to the dull/incorrect color response.
- NRD 4.18's official CMake can build its D3D12 DXIL denoiser and NRI wrapper, but the engine's existing command/resource lifecycle still needs guide UAVs, a noisy signal, denoised output composition, and per-frame NRD history/settings. I am using the official sample's `simplex` workflow to avoid a half-wired SDK pass.
- RTXDI requires deep light-sampling integration and renderer callbacks; RTXGI SHaRC requires multiple world-space radiance-cache buffers and resolve/update passes. RTXPT is a full path-tracing sample, not a drop-in D3D12 library for this engine. RTXMU and RTXNTC target memory/texture pipelines and do not directly remove Monte Carlo light noise. I'll leave these integrations out unless they can be connected to real engine resources without increasing GPU cost or breaking Sponza.

#### SDK integration checkpoint — NRD pass in progress

- Added official NVIDIA NRD v4.18.0 and NRI v190 source snapshots under `vendor/nvidia/`; CMake is configured to build the D3D12-only NRD path and DXIL shaders, avoiding unrelated NRI backends.
- Added a Sponza-only NRD REBLUR diffuse path. The DXR pass exports a demodulated indirect radiance/hit-distance signal plus packed world normal/roughness, linear view depth, zero world-space motion for static Sponza, and albedo. A D3D12 NRI bridge dispatches the official denoiser; a small composition compute pass restores albedo, adds unfiltered direct lighting, tone maps, and outputs sRGB.
- Added an editor toggle for NRD; the Ocean scene leaves it off and keeps its existing reflection/physics path. If the SDK fails to initialize, the toggle is disabled and the current renderer fallback remains selected.
- Fixed the identified color pipeline issue: glTF texture-array resources are typeless with sRGB SRVs, and output now uses the piecewise sRGB transfer function after tone mapping.
- Not yet compiled or visually verified. Remaining faults to check in first build: NRD/NRI CMake dependency compatibility, HLSL packing/guide formats, NRD matrix conventions, D3D12 transitions around NRD/composition, and capture performance.

#### Build checkpoint — first SDK configure fault

- The clean Visual Studio configure reached the official NRI project, then failed because its default `NRI_ENABLE_NVTX_SUPPORT` option tried to fetch optional NVTX and MSYS2's CMake/curl could not validate its configured CA bundle. No C++ or shader compile has run yet.
- Fix in progress: turn off optional NVTX for this D3D12 renderer and identify whether the CMake TLS bundle is malformed or misconfigured. I will keep certificate verification enabled; if required official source downloads cannot be verified, I will report that constraint rather than bypass TLS.

#### Build checkpoint — secure SDK configure recovered

- The failure was a stale MSYS2 CMake CA path pointing at a nonexistent UCRT bundle, not a repository or SDK issue. Reconfigured CMake to use the installed Git CA bundle while preserving TLS certificate verification, and explicitly disabled optional NVTX so NRI does not fetch it.
- Official NRI v190 and NRD v4.18.0 now configure successfully. CMake found the local Windows SDK/DXC and downloaded the required D3D12MA, MathLib, ShaderMake, and DXC dependencies through verified TLS.
- Release build is underway; no GPU runtime result yet.

#### Build checkpoint — NVIDIA SDK compiler faults isolated

- The first Release build reached the official NRI D3D12 backend and NRD shader compiler. NRI's compatibility probe missed opacity-micromap structs already supplied by this Windows SDK; the project now defines the SDK alignment macro for NRI's D3D12 target to skip duplicate fallback declarations.
- NRD was initially configured for RGBA16 normals, but its generated temporal material-ID variants require its recommended packed R10G10B10A2 encoding. Switched the build and external input view to that supported encoding.
- No app source has compiled yet; restarting the build with these SDK compatibility corrections.

#### Build checkpoint — NRD/NRI and engine compile

- Fixed NRI's Windows SDK OMM feature-detection collision with a target-local compatibility define. Switched NRD to its recommended R10G10B10A2 normal/material-ID format.
- All 159 NRD DXIL shader tasks compiled. NRI D3D12, NRD 4.18.0, the engine DXR library, and output-composite shader built, and `CustomEngine.exe` linked in Release.
- Next validation is the full seven-stage Sponza capture suite with NRD enabled, followed by visual capture review and the separate Ocean/Jolt regression suite.

#### Validation checkpoint — capture suite exposed stale-image risk

- The Release app launched and completed its frame sequence, but all seven Sponza capture calls returned failure; the BMPs in `captures/` kept their old timestamps. They are stale baseline images, so they do not prove anything about the NRD build.
- Existing test tooling validated BMP headers without checking modification times. I am adding capture failure details to the run log and will make the runner reject stale captures so success requires fresh GPU output.
- The test HUD image still shows the old built-in depth-aware TAA path, suggesting NRD may not be available at runtime. Added explicit runtime status logging to pinpoint the reason before evaluating image quality.

#### Runtime checkpoint — NRD resource format mismatch found

- Added a reliable runtime status line and per-capture Map diagnostics to the engine log. NRD REBLUR initialized, but its first capture returned DXGI device-removed (`0x887A0005`, reason `0x887A0001`, device hung). Existing BMPs were confirmed stale.
- Root cause found: NRD was compiled for the recommended R10G10B10A2 normal/material-ID encoding and NRI was told that format, but the engine still allocated and created a UAV view over an R16G16B16A16 guide resource. Corrected the resource and UAV format to match NRD/NRI exactly.
- Rebuilding before another full Sponza capture; the full editor and Ocean/Jolt suite must both complete with fresh outputs before considering this fixed.

#### Sponza capture checkpoint — GPU recovery verified

- Rebuilt with matching R10G10B10A2 allocation, UAV, NRD configuration, and NRI resource snapshot. NRD reports available and the GPU no longer hangs.
- The seven-step Sponza runner passed; every BMP timestamp is fresh. Direct+DXR shadows averaged 267.2 FPS. Path-traced GI at the final stationary sample checkpoint measured 44.9 FPS on the RTX 3050 Laptop GPU.
- Fresh captures show the corrected saturated glTF color response and a stable full-editor frame, but the low-SPP GI capture remains visibly grainy. Next: validate the Ocean/Jolt path, then tune and re-capture NRD guides/temporal settings; current visual quality is not yet accepted.

#### NRD quality checkpoint — temporal guides and direct-light noise

- Official NRD guidance confirms world-space normals, non-jittered matrices, zero world-space motion for static geometry, material demodulation before the denoiser, and pixel-space camera jitter in the `[-0.5, 0.5]` range. The integration was mistakenly sending normalized-by-resolution jitter, weakening temporal reprojection.
- The DXR pass also wrote NRD guides as UAVs and immediately handed them to compute without explicit UAV ordering barriers; added barriers for the radiance, normal/roughness, view-depth, motion, and albedo guides.
- The signal previously denoised only later-bounce GI while adding raw one-sample direct lighting over the whole image. Switched the Sponza diffuse signal to demodulated primary diffuse radiance (direct plus indirect), kept sky misses separate, used real primary roughness in both guides, and bounded input HDR values. This should remove soft-shadow grain as well as GI grain.
- Corrected telemetry to identify NRD when it is actually active. Next validation captures will assess whether the change reduces visible noise without destabilizing the Ocean/Jolt path.

#### NRD comparison test checkpoint

- After aligning pixel-space jitter, adding same-command-list guide UAV barriers, denoising the complete diffuse primary radiance, and reporting NRD accurately in telemetry, the Sponza suite still captures successfully. Direct+DXR measured 238.4 FPS; stationary GI measured 45.7 FPS.
- The denoised capture is still grainier than desired on thin/high-frequency Sponza surfaces. To distinguish NRD input quality from intrinsic texture/undersampling, added a test-only `--no-nrd` override and matching capture-runner option. I will capture the same deterministic frame sequence with and without NRD, compare images, then tune based on the measured difference.

#### Texture quality diagnosis and mip-chain implementation

- The controlled comparison confirms NRD preserves the texture detail while the non-NRD path mainly reduces grain by blurring its temporal history. Inspection found the actual Sponza texture array was hard-coded to a single mip level; `MaxLOD` and anisotropic filtering therefore sampled only the full-resolution 1024² image, so thin cloth/stone detail aliased at editor scale.
- Replacing that shortcut with a full mip chain: source images are bilinearly resized to array size, each mip is box-filtered in linear light for sRGB correctness, and all array/mip subresources are uploaded with D3D12 copyable footprints. The existing sRGB SRV and anisotropic sampler now have real mip data to select.
- This changes the asset upload path rather than softening the denoised output. Rebuild and capture comparison will verify texture clarity and ensure the additional texture mips remain within the RTX 3050 laptop GPU memory budget.

#### Mip-chain Sponza run

- Build succeeded after resolving a C++ lambda capture issue in the sRGB lookup helper.
- The Sponza suite passes all seven current captures with NRD enabled and textures mipmapped. Direct+DXR shadows: 235.3 FPS; GI: 45.7 FPS. No D3D12 device removal occurred.
- The texture upload adds 10 mip levels per 1024² layer but keeps GPU cost flat; current GI benchmark is unchanged within capture-run variation. Visual review and Ocean/Jolt regression are pending.

#### Ray texture LOD correction

- Review after the first mip-chain build found both DXR hit shaders explicitly called `SampleLevel(..., 0)`. This forced every primary/secondary hit and alpha mask to the full-resolution image, so the new mip levels were not used.
- Added a shared ray-cone LOD estimate from hit distance, internal render height, triangle UV area, and transformed world area. Base color and alpha-cutout samples now choose an explicit mip suitable for the ray footprint; the GPU anisotropic sampler still handles directional filtering.
- Added the internal render height to the scene constant buffer. Rebuilding and comparing the next Sponza captures for both less texture aliasing and correct foliage cutouts.

#### Explicit ray-footprint mip run

- Added an HLSL ray-cone LOD estimator and provided the actual internal render height to the DXR scene constants. The estimate uses each hit triangle's transformed world area and UV area; both base-color and alpha-cutout shaders now sample the mip selected by the ray footprint.
- Release build and all seven fresh Sponza capture milestones pass; NRD initializes and no device removal is logged. Direct+DXR: 313.0 FPS; path-traced GI: 53.6 FPS in this run, an improvement over the previous 235/45.7 FPS runs.
- Inspecting the refreshed render now; then rerun Ocean/Jolt and run syntax/build checks. Performance values are capture-run measurements and vary with scene/test state.

#### Sponza/Ocean end-to-end verification

- Fresh post-LOD Sponza captures show significantly fewer fine-texture sparkles and retain cloth/stone detail. Telemetry correctly reports `NVIDIA NRD REBLUR` active in GI mode.
- All seven Sponza milestones pass with fresh capture timestamps; current measured performance is 313 FPS direct+DXR and 53.6 FPS path-traced GI at the 688x448 editor render target. The 45.7 FPS pre-LOD run is retained in earlier checkpoint history for comparison.
- The Ocean capture runner also passes on the new build: six Jolt bodies advanced 5.983 seconds, fresh 688x448 capture, 450.3 FPS. Sponza remains loaded first, then the Ocean scene is created separately.
- Configured the canonical `build` directory with NRD 4.18/NRI v190 and verified TLS; compiling this default executable before cleanup.

#### Canonical build verification

- Reconfigured the repository's normal `build/` directory with official NRD 4.18.0/NRI v190 and built `build/Release/CustomEngine.exe` successfully, including all 159 NRD DXIL shaders and the ray-tracing/composite shaders.
- The default Sponza capture runner passed all seven milestones with fresh BMPs: 295.4 FPS Direct+DXR and 53.0 FPS path-traced GI. The default Ocean runner passed with six Jolt bodies, 5.983 seconds simulated, and a fresh capture at 444.5 FPS.
- Next: verify the NRD-off fallback in the canonical executable, restore the NRD capture set, run syntax checks, and remove only the now-redundant generated validation trees/cache files.

#### Final test and cleanup checkpoint — 2026-10-15

- Canonical Release build (`build/Release/CustomEngine.exe`) passes all seven Sponza capture milestones with NRD enabled and all six Jolt Ocean bodies simulated for 5.983 seconds. Latest runs: Sponza 265.3 FPS Direct+DXR / 53.0 FPS GI; Ocean 444.5 FPS. The NRD-off Sponza fallback also passes (292.2 FPS Direct+DXR / 50.6 FPS GI).
- Final captures in `captures/` are refreshed: seven Sponza stages show NRD on; `08_ocean_buoyancy.bmp` shows the separate water/Jolt scene. `engine_run_log.txt` currently contains the last successful NRD-enabled Sponza run.
- Verified the editor exposes a path-tracing/GI checkbox, an NRD toggle for Sponza GI, and an Ocean ray-traced reflection toggle. The viewport defaults to direct PBR + DXR shadows for interactive speed.
- Redundant generated candidates identified: `build_validation/` (old 15:01 build), `build_nrd_validation/` (superseded by tested canonical build), `__pycache__/`, and unreferenced `test_desktop.bmp`. Removal was requested and target paths were checked inside this workspace, but the environment's automatic command review blocked both the guarded PowerShell cleanup and a single-file `Remove-Item` with `blocked by policy`. I left all four intact rather than trying another deletion mechanism. The working `build/`, test scripts, captures, source screenshots, NVIDIA sources, and this checkpoint are preserved.
- NVIDIA source scope: NRD 4.18.0 and its NRI v190 D3D12 integration are built and used. RTXDI, RTXGI, RTXPT, RTXMG, RTXMU, and RTXNTC were evaluated but are not wired into this renderer; they require distinct light sampling/radiance-cache/path-tracing/memory/texture pipelines. RTXMG's sample has a 10 GiB-class memory requirement, beyond this RTX 3050 Laptop GPU's 4 GiB. See official repos in the final report.

#### Native-resolution benchmark and final binary regression

- Added an opt-in `--native-resolution` mode to `capture_runner.py`. It pins the Sponza render target to 1920x1080 for a direct comparison with the original screenshot while leaving the normal 75% viewport-scale runner intact. The runner validates the target size and reports it.
- Replaced the one-frame GI timing sample with an average over steady-state frames 150-169; the direct-lighting + DXR measurement already averages frames 22-39.
- Rebuilt Release successfully; Python runner syntax passed. The native-resolution runner passed all seven fresh captures at 1920x1080: 150.302 FPS direct + DXR shadows and 9.36247 FPS path-traced GI (NRD enabled). This compares more directly with the original screenshot's 6.4 FPS at 1920x1080, though render settings and capture timing are not identical.
- Re-ran the Ocean/Jolt suite against this binary: six bodies, 5.98336 simulated seconds, 445.434 FPS, fresh 688x448 capture. Sponza and Ocean run in isolated scene setup; the Sponza scene is not replaced by the ocean setup.
- Current Sponza render-target captures (`01`–`05`, `07`) are 1920x1080. The full-editor UI capture (`06`) is the 1540x845 client backbuffer with the 1920x1080 viewport fitted inside it. Ocean capture `08` remains the 688x448 run. Re-run `python capture_runner.py --native-resolution` to refresh the Sponza captures.
- Final post-Ocean restoration run again passed all seven native-resolution Sponza captures; the final repeat measured 150.881 FPS direct + DXR and 9.09581 FPS GI. `engine_run_log.txt` now reflects this final NRD-enabled Sponza run, while the successful Ocean capture remains preserved as `08_ocean_buoyancy.bmp`.

#### Noise investigation follow-up — 2026-10-15

- Imported shallow, official NVIDIA source trees for RTXDI, RTXGI, and RTXPT under `vendor/nvidia/` (without their unrelated sample assets and nested application dependencies); fetched RTXDI's runtime library into its SDK submodule location.
- NVIDIA's ReSTIR PT guide confirms path reservoirs are formed by a path tracer context and resampled in temporal and spatial stages, then denoised. ReSTIR can reduce path-sampling variance; it does not replace NRD and requires G-buffer/history resources and several app-specific shader/host bridges. RTXPT is a reference renderer with these subsystems integrated, rather than a small drop-in module.
- Current code review found Sponza reset progressive/NRD history on camera movement and NRD received identical current and previous view matrices. The camera-history and reprojection fixes are recorded above; native-resolution recapture results are recorded in the low-noise checkpoint below.

#### Low-noise sampling checkpoint — 2026-10-15

- Verified against the official NVIDIA RTXDI/RTXPT docs: ReSTIR PT is a multi-pass path-reservoir pipeline with G-buffer/history reprojection and spatial plus temporal resampling. This renderer currently has no stored reservoirs/motion-vector pass, and its direct-light set is one sun, so claiming a drop-in ReSTIR integration would be incorrect. RTXDI sources remain available under `vendor/nvidia/` for a future full integration.
- NRD's official guidance recommends Heitz Owen-scrambled Sobol low-discrepancy sampling to lower residual noise and boiling. Replacing the engine's frame-hashed PCG sequence with an independently implemented, pixel-scrambled Sobol sequence in the path tracer; this targets the current one-sample-per-pixel GI input while avoiding use of RTXPT's separately licensed helper source.
- The NRD-on/off capture comparison showed that disabling NRD softens details rather than removing the source of the residual noise. The sampler compile, native-resolution Sponza captures, visual review and Ocean/Jolt regression are now complete; results are recorded below.
- Implemented the per-pixel Owen-scrambled Sobol sampler in the DXR path shader. GI uses accumulation sample index; direct mode uses frame index. Cosine GI bounces, stochastic sun-disk visibility and optional stochastic pixel jitter share the deterministic stream. No proprietary RTXPT shader files are included by this change.
- First shader compile caught HLSL's `inout` requirement for the existing PCG hash helper; added a value-hash wrapper and the Release build now completes (38,540-byte DXIL). Native-resolution render and performance validation is running next.
- Initial runtime capture caught that the added payload fields exceeded the pipeline's 64-byte DXR payload configuration, so state-object initialization failed. Packed the 24-bit sample index, 7-bit dimension, and reflection flag into one word, retained the existing payload size, rebuilt (38,444-byte DXIL), and restarted the capture suite.
- Native 1920x1080 Sponza passed all 7 capture milestones with NRD available. The full-editor capture reports 116 accumulated samples; visual inspection shows stable detail without the severe raw 4-SPP grain from the original screenshot. First run: 144.0 FPS direct + DXR / 9.12 FPS GI.
- Trimmed the Sobol direction evaluation to the 24 bits actually carried in the packed sequence state. Final Release rebuild and recapture passed all 7 milestones: 149.3 FPS direct + DXR / 9.46 FPS GI at 1920x1080. This is close to the pre-sampler 150-151 / 9.6 FPS measurements and adds no per-ray payload bytes.
- Final DXIL library: 37,308 bytes. The 24-bit sequence index wraps after 16,777,216 accumulated samples; this is far beyond a normal editor session.
- Re-ran the final build's Ocean/Jolt capture after the Sponza run: six bodies, 5.98336 simulated seconds, valid 688x448 capture, 446.156 FPS. Ocean reflections remain enabled and Sponza captures remain intact.
- Scope note: this change implements low-discrepancy sampling recommended by NRD and demonstrated in RTXPT; it does not claim RTXDI ReSTIR is active. ReSTIR PT needs path-tracer-context recording, reservoir buffers, motion reprojection, temporal/spatial shift mapping and several pass/host bridges that this single-pass renderer does not have. ReSTIR DI primarily reuses direct-light candidates; this scene exposes one sun, so it would not target the dominant indirect GI noise as effectively. Current 1080p GI is still about 9.5 FPS on the RTX 3050 Laptop GPU; the tested fast interactive path is Direct + DXR shadows at about 149 FPS.
- Official references: [NRD low-discrepancy sampling guidance](https://github.com/NVIDIA-RTX/NRD#interaction-with-low-discrepancy-samplers-blue-noise), [RTXDI ReSTIR PT guide](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirPT.md), and [RTXDI integration guide](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md).

#### Feature audit and regression coverage — 2026-10-15 (in progress)

- Source audit confirmed the Inspector changed only the CPU `MaterialComponent`; DXR read the immutable `m_materialBuffer` built during scene upload. Shared imported materials also meant a direct update would unintentionally recolor every mesh using that glTF material.
- Added per-entity GPU material overrides: shared material data is cloned into preallocated material-buffer capacity, only the selected instance is remapped, and original texture/alpha/cutout metadata is preserved. GPU writes wait for queued prior frames and update the instance buffer when the material ID changes.
- Fixed viewport picking to transform all eight local AABB corners into world space before testing the world-space click ray. Hierarchy Previous/Next now cycles through the filtered entities rather than the full scene.
- Added a Sponza capture regression that drives the same material edit API across entities, checks the GPU upload log, and measures visible magenta pixels in a fresh BMP. Capture validation now checks 24-bit BMP layout, dimensions, nonblank sampled pixels, and output color for that specific material test.
- Release build succeeds. The strengthened Sponza runner passes all eight milestones at 960x544 (264.34 FPS direct+DXR, 48.31 FPS GI), then at native 1920x1080 (150.68 FPS direct+DXR, 9.43 FPS GI); NRD-off fallback also passes (288.41 / 51.84 FPS at 960x544).
- The new material capture passes its image-content check: 73.1% magenta sampled pixels at 960x544, 71.7% at 1920x1080, and 73.2% with NRD disabled. The log explicitly confirms 25 entity edits were uploaded to the GPU. The full-editor capture is taken before this deliberate material-test override.
- Ocean/Jolt runner now requires measured body movement. It passes with six bodies, 5.98336 simulated seconds, up to 2.60089 m measured vertical displacement, and 444.80 FPS; fresh 688x448 ocean capture is valid.
- Ocean reflection comparison is now automated: enabled/disabled/enabled runs produce identical Jolt displacement (2.60089 m) and 4.03% changed pixels (0.972 mean channel delta), then restore the normal enabled capture.
- Built a separate Visual Studio Release configuration with `ENGINE_ENABLE_NRD=OFF`; it passes all eight Sponza checks at 960x544 and validates the per-entity GPU material upload (73.2% sampled magenta pixels). The first configure attempt inherited Ninja while passing the Visual Studio-only `-A x64`; README now pins the VS 2022 generator explicitly.
- Audited remaining controls and corrected ray-bounce semantics in comments and telemetry: direct mode no longer reports GI rays, and estimates explicitly exclude shadow/reflection rays. Removed an inaccurate DXR 1.1 label; adapter selection accepts DXR Tier 1.0+, and the UI now reports that baseline accurately.
- Fixed CMake shader dependencies so NRD HLSL includes trigger DXR shader recompilation. Both NRD-enabled and NRD-disabled shader libraries build successfully.
- Added detailed `README.md` and a scoped `.gitignore`. Ignore rules cover generated build trees/captures/logs/caches but preserve assets, vendor files/licenses, source, scripts, and user screenshots. `git check-ignore` verified both the ignored/generated set and the preserved source/reference set.

#### Final audit closeout — 2026-10-15

- Final native 1920x1080 Release Sponza run passes all eight fresh capture milestones. Direct + DXR shadow mode measures 151.281 FPS; path-traced GI with NRD measures 9.657 FPS. The material test image contains 71.7% sampled magenta pixels after the GPU material/instance-buffer update. The 06 editor screenshot is captured before the intentional material override.
- The 960x544 default-target run also passes all eight milestones at 272.924 FPS direct + DXR and 48.343 FPS GI with NRD. A separate build configured with `ENGINE_ENABLE_NRD=OFF` compiles and passes the same suite at 301.533 FPS direct and 52.278 FPS GI. Performance values vary with GPU load and are documented as measured results, not guarantees.
- Ocean runner verifies fresh 688x448 blue-water output, six live Jolt bodies, 5.98336 seconds of simulation, max vertical displacement 2.60089 m, and final body-center range -0.33842..0.02781 m. The reflection comparison changes 4.03% of image pixels (mean channel delta 0.972) and restores the enabled capture. Ocean test average is about 444 FPS.
- Visual review of the native Sponza GI and editor captures shows the old severe 4-SPP grain is absent after the 116-sample denoised checkpoint; banners and stone retain saturated color and texture detail. Native GI remains slow at about 9.7 FPS; direct mode is the interactive high-FPS path.
- Final feature inventory: implemented/tested direct PBR, sun/sky, DXR shadows, progressive diffuse GI, NRD (optional at build/runtime), TAA/reprojection, texture mips/ray-cone LOD, Inspector material uploads, filtered ECS navigation, transformed-AABB picking, Jolt ocean buoyancy, and an ocean-only reflection toggle. RTXDI/ReSTIR, RTXGI, RTXPT runtime integration, RTXMG/RTXMU/RTXNTC, skeletal animation, multi-light support, and a full O3DE Atom post-effect stack are not implemented; `README.md` identifies these limits instead of claiming feature parity.
- O3DE reference content was not edited. Superseded `build_validation/`, `build_nrd_validation/`, failed-config `build_no_nrd_validation/`, the verified `build_no_nrd_validation_vs/`, `__pycache__/`, and unused `test_desktop.bmp` remain on disk. A bounded cleanup request for those exact in-workspace targets was rejected by automatic command review with the generic reason `blocked by policy`; no alternate deletion method was attempted. The `.gitignore` excludes generated build trees, test captures, caches, local UI state, and logs without hiding source, licenses, assets, or screenshots.

#### Path-tracing, DXR, ocean, and math audit — 2026-10-15 (in progress)

- Confirmed the active hardware ray path from adapter selection through `ID3D12Device5`, DXR state-object creation, `SetPipelineState1`, `DispatchRays`, and HLSL `TraceRay` calls. Added selected DXR-tier and first-dispatch log records; both capture runners now require them. The app previously only showed a hardcoded UI claim, so this makes the runtime evidence checkable.
- Source audit ruled out a suspected repeated Sobol jitter dimension: `sampleSobol2D` advances its packed dimension on each call. That was not the cause of the residual noise.
- Found a real NRD signal-classification fault: primary specular highlights and emissive radiance were included in the REBLUR diffuse signal. Added a separate payload field and route those primary non-diffuse terms around REBLUR; direct diffuse plus diffuse-bounced lighting remains demodulated by base color. Increased REBLUR's stable history from 30 to 48 frames (NRD's documented maximum is 63) and explicitly retained its anti-firefly filter.
- Ocean baseline capture showed a short, visibly finite quad, gradient-only sky, smooth low-frequency normals, and bright cyan water. Replaced the wave field with five directional components; matched height, spatial derivatives, and vertical velocity in HLSL and the Jolt C ABI bridge. Added procedural clouds/horizon haze only to the ocean sky, darker water, crest foam and sun glints, moved the view toward a level horizon, and extended the quad boundary to 2 km.
- Removed the O3DE-inspired label from the viewport and rewrote nearby comments to describe the local implementation. `AccelerationStructure.h` identifies its BLAS/TLAS patterns as adapted from O3DE Atom; README will map this attribution to the relevant module. No O3DE checkout files have been written.
- Fixed the AABB ray test's near-zero direction handling: substituting a positive epsilon for negative/parallel directions could select boxes on the wrong side. Added finite/degenerate guards to vector division/normalization, perspective parameters, scene camera inversion, ray-picking dimensions, and the shader's normal/direction normalization.
- Pending: build both NRD configurations, run Sponza captures with NRD on/off and the ocean/Jolt runner, inspect images/performance, and finish the technical README. No test or generated build output has been deleted; cleanup is limited to verified code/doc issues because earlier environment review rejected deletion of generated trees.
- First Release build after the DXR/NRD/math changes succeeded; Sponza passed 8/8 captures with NRD (241.2 FPS direct, 47.0 FPS GI at 960x544) and without NRD (239.5 / 47.1 FPS). Both runs verified the hardware-tier and DispatchRays log checks.
- Ocean/Jolt first visual check passed the old blue-water/body-motion checks (444.7 FPS), but showed overly broad white foam from the new wave field. Tightened the crest mask, added projected-pixel filtering for short waves, and lowered the sun toward the horizon before the final water capture.
- Second water capture still reflected too much of the bright analytic sky and showed distant sine-wave aliasing. Attenuated all five wave bands by projected pixel footprint, applied a dark blue ocean reflection response, narrowed sun glints, reduced foam, and increased cloud coverage across the sky. Current 688x448 capture passes at 431.6 FPS; 92.2% of sampled lower-half pixels classify as blue. Reviewing final cloud variation and reflection comparison next.
- Rewrote README with the rendering/physics workflow, formulas, dependency setup, test commands, limitations, and O3DE Atom DX12 BLAS/TLAS source/license attribution. Updated the matching source comments and removed the O3DE label from the viewport.
- Added `tests/MathTests.cpp` and a CTest target for AABB parallel/inside rays, finite hit distances, zero rays, invalid vector division/normalization, and projection clamping. Fresh CMake reconfigure/build succeeded (DXR library 48,692 bytes; NRD composite 5,464 bytes); `ctest --test-dir build -C Release --output-on-failure` passes 1/1 test containing seven assertions.

#### Path history, water spectrum, and release verification — 2026-10-16 (completed)

- Visual review of the no-NRD 86-sample editor capture exposed a real temporal-history defect: the 9-tap spatially filtered result was written back as the next progressive estimate, so the filter compounded every frame and washed out Sponza textures. Split raw accumulation history from the display-only cross-bilateral result; the temporal estimator is no longer filtered recursively. The low-sample filter uses depth and luminance weights and fades out over 128 accumulated samples.
- The no-NRD 86-sample editor capture is now sharp, with the Sponza banners and stone texture preserved. Its 18-sample automated GI capture still has some expected Monte Carlo residual noise; the corresponding NRD capture is visibly clean. Both exact Release executables pass all eight Sponza milestones and the Inspector-to-GPU material upload test.
- Made Russian-roulette survival explicit as `q = clamp(max(throughput), 0.05, 0.95)`; surviving paths divide throughput by `q`, preserving the estimator. Removed the unused `indirectRadiance` shader accumulator and a duplicate payload assignment.
- Replaced the overly aligned ocean wave vectors/amplitudes with a less repetitive five-band spectrum and matched the analytic height, X/Z derivatives, and vertical velocity in HLSL/Jolt. Reduced sharp direct-sun peaks, added filtered render-only micro-normal variation, and tightened projected-footprint filtering to soften distant normal bands. The normal, height, and buoyancy equations remain finite; the micro-normal affects shading only.
- Moved the CPU wave evaluation into `src/ecs/OceanWave.h`, used by the Jolt bridge, with explicit phase/result finite guards. CTest now checks analytic X/Z slopes and vertical velocity against central finite differences, plus non-finite and overflowing input fallback. Both NRD build variants pass all 12 assertions.
- Final current-source NRD-enabled 960x544 Sponza suite: 8/8 milestones, 273.761 FPS direct + DXR shadows, 47.3879 FPS path-traced GI. Final NRD-disabled 960x544 suite: 8/8, 281.034 FPS direct, 46.9565 FPS GI. Both runs verify DXR tier/dispatch telemetry and the GPU material-upload regression.
- Final current-source native 1920x1080 Sponza suite: 8/8 milestones, 144.822 FPS direct and 8.97878 FPS path-traced GI. All three ocean/reflection variants also pass on the final source: six Jolt bodies, 5.98336 s, 2.74067 m maximum vertical displacement, final center range -0.177309..0.313717 m, and 9.68% changed pixels / 3.218 mean absolute channel delta. The restored ocean render measures 439.411 FPS in the final run; results vary with GPU load.
- The README records the measured feature status, current benchmark values, renderer workflow, exact wave coefficients/units, denoiser math and history behavior, dependency setup, test commands, known limits, and O3DE Atom source/license attribution. O3DE remains read-only. RTXDI/ReSTIR, RTXGI, and RTXPT were not integrated because the existing DXR path is active and the remaining noise was traced to NRD signal classification and recursive fallback filtering.
- Generated validation/build folders remain ignored by `.gitignore`. A prior cleanup deletion request was rejected by automatic command review (`blocked by policy`), so no generated build/capture/reference directory was deleted or removed through another path. Useful math and capture tests remain in the repository.

#### General DXR reflections and visual regression — 2026-10-16 (in progress)

- Starting from the previous final audit without redoing its tests. Current inspection shows the shader's DXR reflection ray is only reachable from the ocean material branch; Sponza's imported PBR materials never trace reflections. The editor checkbox is also hidden outside Ocean, so the UI does not expose general ray-traced reflections.
- Existing pipeline recursion is depth 2 and the reflection payload marker already bounds a single secondary hit; this can support one explicit reflection query without adding recursive bounce cost.
- Chosen reflection design: a scene-wide editor toggle, with tracing skipped entirely when off; one roughness-aware stochastic GGX reflection ray on visible eligible glossy pixels when on; Schlick Fresnel weights the traced radiance against the existing local material response. Keep ocean's Fresnel/environment response on the same toggle.
- Khronos glTF Sample Assets lists `MetalRoughSpheresNoTextures` as a CC0 sample designed to vary metal/roughness. It is suitable for import and a deterministic reflection regression; its GLB is available from the official Khronos asset repository.
- No changes made yet in this request. The previous Sponza and Ocean visual quality limitations remain until the code changes and fresh captures are complete.
- Follow-up audit found Sponza's glTF metallic-roughness maps were not imported by the active Assimp path, and the only texture SRV is sRGB. A general reflections toggle alone would therefore ignore authored per-pixel roughness/metalness and make Sponza's reflected response inaccurate.
- Expanding the material texture representation to include packed metallic-roughness layers and using an UNORM texture-array SRV with explicit shader sRGB decoding for base-color layers; data-texture mips will be averaged in linear channel space. This preserves base-color behavior while making glTF G=roughness / B=metallic samples mathematically meaningful.
- Implemented a unified linear UNORM material texture array: base-color RGB is decoded from sRGB in HLSL, packed metallic-roughness data remains linear, and CPU mip generation averages each texture semantic in its authored domain. The Assimp path now imports glTF metallic/roughness factors and the packed texture layer IDs; the material GPU structure is expanded in lockstep with HLSL.
- Added scene-wide one-bounce GGX reflection evaluation to primary direct/PBR and GI hits, Fresnel-weighted with `f * cos / pdf`; Sobol state now dedicates two high bits to primary-reflection eligibility and reflected-hit shading, so diffuse GI bounces do not pay reflection traversal. Ocean uses the same editor-controlled DXR toggle.
- Added a Khronos Metal-Rough Spheres (textureless, CC0) asset and a camera-framed reflection-lab scene entry. Added the general editor checkbox; Sponza and ocean now start with DXR reflections enabled, and the UI can disable them to recover the faster path.
- Added fixed-scene reflection comparison mode (`--reflection-compare`, plus `--reflection-khronos`) that captures DXR-on / DXR-off / restored-on frames and reports enabled/disabled frame time. Existing ocean compare mode is retained separately for the animated Jolt water.
- Remaining implementation checks: finish editor/scene selection compile integration, validate C++/HLSL material layouts and Assimp property access, build NRD on/off configurations, then inspect captures and enforce meaningful on/off image deltas without disproportionate frame-time cost.
- First live Sponza toggle comparison confirmed that the new TraceRay path changes 99.52% of sampled pixels (mean channel delta 20.26), proving reflections execute, but the broad `roughness < 0.72` gate regressed direct mode from 263.6 FPS with reflections off to 83.1 FPS on this laptop GPU. This is outside the requested performance budget.
- Narrowing reflection traversal to visually significant polished/metallic surfaces; I will keep the estimator/Fresnel weighting intact, rerun image-delta and FPS measurements, and adjust only the eligibility/cost strategy if it remains too expensive.
- The Khronos first capture exposed a validation/setup fault: the asset did import (99 entities / 1,040,409 triangles) but its source-space diameter is only about 0.01 units, so the automatic camera framed an almost subpixel model and showed only the sky. Sponza's 0.974 mean-channel / 58.45% pixel toggle delta and near-zero measured FPS cost (272.1 on / 266.2 off) pass the revised reflection gate.
- The Khronos test scene now needs a uniform bounds-centered import scale before its TLAS is first updated; this is restricted to the reflection lab and does not mutate Sponza or ocean geometry.
- Khronos image still failed its visible-reflection delta after rescaling: the active Assimp path retained geometry but did not preserve the sample's per-material PBR factor grid. The Khronos lab now loads directly through the in-tree cgltf path instead of Assimp, reads metallic/roughness factors from the glTF source, and builds its TLAS once after normalization. Sponza remains on its current Assimp path and material-map handling.
- Reworked reflection sampling into a private Sobol dimension pair so it cannot shift primary shadow or diffuse-GI samples. Reflection validation now freezes the per-pixel sequence seed and disables temporal jitter, producing deterministic on/off captures; this removes stochastic shadow changes from the image-difference proof.
- Visual review of the latest Jolt capture still showed near-horizon black reflection bands, under-detailed normal variation, and hard-edged box props. Updated the water shader to avoid tracing downward through the flat water slab (use its analytic horizon response there), add three footprint-filtered high-frequency analytic normal bands, reduce pixel-noise micro-normal strength, and retain more blue-tinted reflected radiance.
- Replaced the water-scene's six flat-shaded box meshes with subdivided rounded-box buoy meshes; Jolt collision remains the original robust box collider. Pending visual/performance retest.

#### Water/reflection final-pass work — 2026-10-16 (in progress)

- Rebuilt the NRD-enabled Release targets after the first water-detail pass; `CustomEngine` and `EngineMathTests` compile successfully. Python capture-runner syntax and the registered `EngineMathChecks` CTest (12 assertions) pass.
- The fresh ocean on/off/on comparison completed with six Jolt bodies and 5.98336 seconds of simulation, 430.913 FPS enabled / 445.006 FPS disabled on one sampled pair, 92.9% blue lower-half pixels, and a 0.894 mean-channel / 4.85% changed-pixel reflection delta. Water DXR is active and visible, but image inspection still showed overly coherent bands and a camera-centered sun streak; that visual result is not considered polished yet.
- Added an analytic smooth-value-noise gradient helper to HLSL and replaced finite-difference micro-normal taps with three screen-footprint-filtered ripple scales. Reduced overly coherent sine-normal amplitudes, removed arbitrary RGB attenuation from reflected radiance, and broadened the direct sun lobe to avoid a hard linear streak.
- Moved the ocean-scene sun off the camera centerline and raised it, increased rounded-crate bevel tessellation/radius, and increased buoy mesh tessellation. These visual edits do not change Jolt's box colliders or the shared CPU/GPU buoyancy wave coefficients. Rebuild, image inspection, performance comparison, Sponza/Khronos tests, and documentation corrections remain pending.
- The resulting capture confirmed more fine-scale surface variation, but exposed a lighting regression: removing the old reflection attenuation made the near-horizon surface reflect an overbright cloud field and the same solar disk already represented by the direct GGX water lobe. The pixel classifier dropped to 51.3% blue lower-half pixels from 92.9%; FPS remained 439.4 enabled in the on/off/on sequence and the toggle changed 4.99% of pixels (mean delta 1.632). This is a visual failure to correct, not a passing quality result.
- Replaced the arbitrary RGB reflection multiplier with a separate reflected-miss sky path that omits the solar disk (direct sun is already represented by the water GGX lobe), reduced procedural cloud radiance, and reduced only the ocean-scene directional light from 5.0 to 3.5. These changes aim to restore a dark blue ocean while retaining Fresnel-weighted environment/geometry reflections. Fresh capture/build validation is pending.
- The reduced sky/sun build compiles and CTest/Python checks still pass. Its on/off/on ocean run is 444.634 FPS reflection-on with a 1.398 mean pixel delta and 5.02% changed pixels; buoyancy measurements are unchanged. Visual inspection improved the diffuse water, but point samples show a few near-white water pixels (e.g. 233/238/239), identifying an overly sharp direct GGX sun lobe (`roughness=0.20`, peak GGX D about 198 at alignment) as the remaining hot-streak source. Increased the unresolved-water roughness to 0.34 to reduce its peak by roughly an order of magnitude while retaining the micro-wave normals. This needs a fresh capture before acceptance.
- Fresh 0.34-roughness capture still showed white glint points despite the broader lobe; the brightest point is the water's direct-sun GGX term, not the DXR object-reflection toggle (the non-DXR comparison looks similar). Added a 0.22 finite-solar-disk radiance approximation to the water-only direct highlight. This is an explicit water-scene artistic approximation; generic PBR surfaces and Sponza keep their original direct-light energy. New capture is pending.
- Code-path audit identified a likely source of both stripe artifacts and highlight instability: the ocean intersects a flat DXR proxy, but uses a wave shading normal and previously flipped the entire normal to face the camera. That can turn the water normal downward on steep proxy-vs-shading mismatches and drive Fresnel to invalid values. Replaced the flip with a bounded correction toward the upward geometric hemisphere for invalid/backfacing water normals. This remains a shading approximation until the ocean uses a genuinely displaced intersection surface; visual/performance verification pending.
- The final ocean reflection-on/off/on comparison passes after the upper-hemisphere correction: 432.822/444.196 FPS, 1.512 mean channel delta, 5.47% changed pixels, 64.7% blue lower-half samples, and unchanged Jolt results (6 bodies, 5.98336 s simulated, 2.74067 m maximum measured vertical movement). The 688x448 image shows the DXR-reflected props and fine multi-scale ripples; a flat proxy still limits the silhouette/crest realism.
- `reflection_capture_runner.py --scene all` passes with active hardware Tier/DispatchRays telemetry. Sponza: 0.217 mean delta / 29.82% changed pixels at 266.73/183.59 FPS on/off; Khronos Metal/Rough Spheres: 2.447 / 59.73% at 181.50/153.36 FPS. Paired short-window timings vary materially with GPU scheduling, so treat the 45% test budget as a regression guard, not a stable benchmark.
- Full Sponza runner passes all 8/8 capture milestones in the NRD-enabled build (960x544, 287.759 FPS direct + DXR, 45.201 FPS GI) and a rebuilt NRD-disabled Visual Studio Release configuration (298.175 FPS direct, 47.533 FPS GI). Both report fresh material-upload image evidence. The NRD-enabled `EngineMathChecks` CTest passes 1/1; Python validators compile successfully.
- Updated `README.md` to describe the scene-wide editor reflection toggle, GGX sampling PDF/estimator weight, Khronos asset, water proxy/normal limitations, actual test commands, and latest measurements. `.gitignore` already scopes ignored generated build/capture/log/Python-cache output without hiding sources/assets/licenses/tests; no source folders or tests were removed because the audited validation and vendor files are in active use.
- Remaining scope limits are documented rather than presented as complete features: the ocean uses a flat DXR proxy rather than displaced geometry, full-resolution GI remains expensive, and RTXDI/RTXGI/RTXPT and a full post-effect/animation stack are not integrated. No edits were made to the sibling O3DE reference checkout.
- Final verification after the last shader/documentation edit: both NRD-enabled and NRD-disabled Visual Studio Release builds complete; `EngineMathChecks` passes 1/1 in both build trees; all three Python runners pass `py_compile`. The last HLSL edit only clarified the calibrated water highlight comment, so previously captured imagery/tests correspond to the same shader behavior. No compiler diagnostics were emitted in these builds.

### Sponza reflection floor follow-up — 2026-10-16

- Re-ran the Sponza-only reflection capture after the user observed that the prior yellow-sphere image was not Sponza. The on/off files show the atrium, and their pixel difference confirms the Sponza DXR toggle is active, although the floor reflection was visually too subtle to confidently claim from the prior image alone.
- Audited `assets/sponza/Sponza.gltf`: all 25 referenced base-color images exist, while 0 of 24 referenced metallic/roughness images exist. The absent M/R files had been silently represented by white texture-array layers, turning glTF's default 1/1 factors into fully metallic, fully rough materials.
- `Scene::loadAssimp` now detects missing M/R maps and uses a conservative dielectric fallback (`metallic=0`, `roughness=0.65`). For the bundled anonymous material slot 9, source geometry inspection found the wide horizontal atrium floor (y=-2.506, upward normals); it uses `roughness=0.30` to enable the existing bounded DXR specular query there. No extra SDK or recursive reflection path was introduced.
- A broad experiment allowing rays on every fallback surface measured 99.40 FPS on / 285.14 FPS off and failed the 45% cost guard. That threshold change was reverted. The final code keeps the original glossy-material eligibility threshold and applies polished fallback only to the floor material; this passed at 247.39/190.24 FPS on/off (timings are noisy short-window measurements).
- Final Sponza reflection delta: 0.407 mean channel delta and 35.11% changed pixels at 688x448. This confirms a measurable but subtle floor/surface response; it is not a mirror-like result, and authored M/R maps remain unavailable. `python capture_runner.py` passes 8/8 milestones/captures (960x544; 296.778 FPS direct and 46.6125 FPS GI); `ctest --test-dir build -C Release --output-on-failure` passes 1/1.
- Updated README reflection/asset-fallback notes and capture metrics. No files were changed in the sibling O3DE reference folder.

### Water-entry displacement and native Sponza captures — 2026-10-16 (completed)

- User reported floating props have no visible contact/wake and requested a video-like water test plus real 1920x1080 Sponza reflection on/off evidence.
- Confirmed the Jolt surface is at mean Y=0 while the DXR water quad was offset to Y=-0.45. Confirmed the quad is flat and the prior shader only changed normals; it had no body state or impact data. Corrected the proxy to Y=0.
- Added a Jolt C ABI getter for linear velocity, a 16-byte-aligned SceneCB payload for up to eight body positions/radii and velocities/impact data, bottom-crossing detection, and analytic contact, expanding impact-ring, foam, and trailing-motion wake terms in the water shader. These terms change the analytic shading height, gradients/normals and foam; the reflection-ray origin is kept above the flat proxy to avoid self-intersection. This is not a full displaced mesh or fluid solver.
- Raised ocean demo objects to begin above the surface and added optional captures at frames 30, 45, 60, 75, 90, 105, 120, 150, 180, 240, 300 and 360. The final capture reports the number of detected water-entry impacts. The ocean runner can validate the image sequence.
- Added `--native-resolution` to the reflection runner; it now asks for and verifies exact 1920x1080 Sponza on/off/restored frames.
- The build, sequence/impact checks, visual inspection, animated preview, native Sponza reflection comparisons and regressions were completed in the repeated-bounce follow-up below.
- First integrated Release build succeeded after correcting C++/HLSL cbuffer array alignment; compile-time `offsetof` and 16-byte-size checks now pass. The HLSL DXIL compiled and the Jolt bridge/runtime DLLs staged.
- `python ocean_capture_runner.py --sequence` passed with 12 fresh frames, 8 detected impact crossings, six Jolt bodies, 5.98336 simulated seconds, 4.90488 m maximum vertical movement, 71.0% blue lower-half samples, and 389.892 FPS in the first build. After fixing water reflection self-intersection (keep reflection ray origin above proxy) and restricting contact depressions to submerged props, the repeated sequence passed at 420.968 FPS and 63.7% blue-water samples. It reported six bodies ending with center Y range -0.159101..0.43655 m; the bodies are partially submerged as expected for these collisions, though the flat proxy still hides parts that fall below its plane.
- Inspected frames 30, 45, 60, 75, 90 and 360. The new impact rings and moving normal/reflection pattern are visible without the earlier black reflection bands. The water intersection remains flat geometry, so the measured wake is analytic shading height/normal/foam, not a physically displaced DXR silhouette.
- Generated `captures/ocean_drop_wake.gif` from the 12 time-stamped BMPs. Sponza comparison at native 1920x1080 passed: hardware DXR on/off, 0.487 overall mean-channel delta, 0.8098 over the broad floor region, and 123.22 FPS on / 131.03 FPS off (short timing window, roughly 6% measured cost). The on/off capture dimensions are both confirmed as 1920x1080; image inspection shows full-resolution detail, with a restrained floor reflection rather than a mirror.
- Final validation passed: `ctest --test-dir build -C Release --output-on-failure` reports `EngineMathChecks` 1/1; `python -m py_compile ocean_capture_runner.py reflection_capture_runner.py capture_runner.py` passes; `python capture_runner.py` passes all 8/8 milestones at 960x544 (272.067 FPS direct + DXR shadows, 42.3806 FPS path-traced GI). Native Sponza reflection comparison and the Jolt ocean sequence passed separately as recorded above.
- No O3DE reference files were changed. No project test or vendor directories were removed: the existing tests and vendored SDKs remain part of the validated engine build. The new BMP/GIF/PNG captures are covered by `.gitignore` as generated evidence.

### Repeated-bounce water interaction follow-up — 2026-10-16 (completed)

- User reviewed the water sequence and correctly pointed out that later cube rebounds did not produce a clearly anchored, fresh response. The first pass counted entries but detected them after rendered-frame physics and centered each expanding ring on the cube's live position, causing older rings to move with the body.
- Changed entry detection to run after every fixed Jolt substep. Each event now stores its world-space impact point, event time, and speed-derived strength. The shader evaluates the expanding ring and foam around that pinned point, while live center position remains reserved for contact depression and directional wakes.
- Added the same analytic impact waves to the CPU water-surface sample used by Jolt on subsequent fixed steps, so previous entries influence their source and nearby bodies' buoyancy. Added vertical body speed to the shader payload and an analytic bob-driven wave/gradient for submerged bodies between impacts.
- C++ and HLSL SceneCB layouts were extended in matching order with static 16-byte size/alignment requirements. Bridge argument validation and finite-value checks remain enabled. Release build passed and the Jolt bridge/runtime DLLs staged successfully.
- Final `python ocean_capture_runner.py --sequence` passed with 12 fresh 688x448 captures. It logs 6 entries by frame 90 and 10 by frame 180. The validator compares blue-water pixels classified as water in both frames 90 and 180: 89,561 common water pixels, 35.59 mean channel levels of temporal change, and 87.6% changed materially. Latest sequence average was 419.188 FPS; an immediate repeat measured 426.752 FPS, illustrating short-window timing variation.
- Regenerated `captures/ocean_drop_wake.gif` and the contact sheet `captures/ocean_sequence_contact.png`. Visual review shows rings remain at impact origins after their source moves, with fresh wave response around later rebounds. The bridge retains the latest event per body; CPU and GPU use matching radial wave coefficients, and Jolt buoyancy includes that event on subsequent steps. Crossing detection excludes self events to prevent the generated oscillation from faking new entries.
- Water DXR reflection on/off/on validation passes with a 1.247 mean-channel difference and 4.75% changed pixels. The ocean reflection-on/off timing pair measured 442.607/448.372 FPS at 688x448.
- Engine regressions pass: `ctest` 1/1; Python runner syntax checks; `python capture_runner.py` 8/8 captures at 960x544 (248.506 FPS direct + DXR shadows and 44.7057 FPS GI); native Sponza reflection comparison at exactly 1920x1080 (0.200 mean channel delta, 21.40% changed pixels, 120.59/135.69 FPS on/off).
- README mechanics, sequence assertions and measured result were updated. The water remains a flat DXR quad with analytic shading height, not displaced ray-hit geometry or a complete fluid pressure solve; submerged-geometry clipping and limited physical displacement remain known constraints. No files in the O3DE reference folder were changed.

### README media and repository hygiene pass — 2026-10-16 (completed)

- Located the two newly added root screenshots, verified their editor overlays, and moved them into `docs/media/` with descriptive names for the README gallery.
- The existing water GIF was assembled from only 12 unevenly spaced diagnostic frames, so it could not honestly be described as 30 FPS. Added an opt-in capture path that records every second 60 Hz simulation frame (166 captures) and a palette-stable GIF encoder for the resulting sequence.
- Expanded `.gitignore` to ignore all generated `captures/` output while keeping curated `docs/media/` content trackable; added CTest output and old root scratch screenshot ignores.
- Dense capture passes all 166 freshness checks, including repeated impacts (6 by frame 90, 10 by frame 360) and the common-water temporal-difference check. The encoder verified 166 frames at 516x336 and an average 30.02 FPS; curated GIF size is 11,306,232 bytes.
- Added the two supplied editor screenshots to the README gallery: path tracing at 1920x1080 and direct PBR/DXR at 3840x2160. Added the 30 FPS water GIF and documented that its visible height is analytic shading over a flat ray-hit surface. Documented the optional Pillow requirement for regenerating it.
- Final verification after the media and capture-mode edits: Release CMake build passed; CTest passed 1/1; all four Python scripts compile; the dense ocean runner passed 166 images and temporal-water checks; the GIF encoder output verified at 166 frames / 516x336 / 30.02 FPS; `capture_runner.py` passed 8/8 at 960x544 (265.69 FPS direct and 48.0546 FPS GI); the exact 1920x1080 Sponza reflection comparison passed (0.544 mean delta, 48.50% changed, 129.59/128.53 FPS on/off). The curated media is not ignored; generated capture frames and uncurated root screenshots are ignored as intended.
- README measurements now match the final engine regression run. Remaining known scope limits are unchanged: the ocean uses a flat intersection proxy and analytic wave coupling rather than dynamically displaced DXR geometry or a full fluid solver. No O3DE reference files were touched.

### Standalone water executable — 2026-10-16 (completed)

- Added a staged `WaterDemo.exe` copy beside `CustomEngine.exe`; both use the same compiled renderer, assets, and runtime dependency staging.
- Startup detects the `WaterDemo.exe` image name (or an explicit `--water-demo` argument) and initializes the ocean scene in normal interactive mode. It does not enable the fixed-step capture/auto-exit behavior of `--ocean-test`.
- The window title identifies the water demo, and the editor Scene menu can still switch between ocean, Sponza, and the Khronos reflection lab.
- The Release build passed, and CTest passed `EngineMathChecks` (1/1). `WaterDemo.exe` is staged byte-for-byte from `CustomEngine.exe`; it shares the assets and staged Assimp/Jolt runtime DLLs beside it.
- Runtime smoke test passed: the executable logged `TestMode: false`, initialized six Jolt bodies in the interactive ocean scene, recorded DXR `DispatchRays` at 960x544, and remained running after startup. The test then closed its own process.
- Added a `WaterDemo` CMake target for explicitly building/staging the water executable; its target-specific build also passed.
- Staging checks confirmed matching SHA-256 hashes for both executable entry points and verified Sponza assets plus `JoltBridge.dll` are present beside the water executable. `python ocean_capture_runner.py` passed with six Jolt bodies, 5.98336 simulated seconds, blue-water coverage, and 443.184 FPS in that capture run.
- Re-ran Sponza regression via `python capture_runner.py`: all 8 captures/milestones passed at 960x544, measuring 296.322 FPS direct + DXR shadows and 44.229 FPS path-traced GI. These short run averages vary by GPU load; README's latest table reflects this run.
