# Custom Engine: A Beginner’s Code and Math Guide

This guide introduces the engine to readers who have never programmed before. It explains the repository’s own C++ and HLSL code, the way the compiler reads it, the rendering and physics data flow, and the mathematics used by the renderer. It gives line-by-line explanations for the teaching examples and walks through every engine-owned subsystem using exact source locations.

> **Scope:** The tutorial explains code maintained in `src/`, `shaders/`, `tools/`, and `tests/`. It does not annotate every line of the separately vendored ImGui, cgltf, NVIDIA NRD/NRI, stb, or Jolt libraries, nor generated shader bytecode. Those projects are dependencies with their own documentation and licenses. A useful code review can check how this engine calls a library; it cannot rewrite a vendor’s thousands of lines into an accurate beginner tutorial here.

## Part I: Engine orientation

## 1. What is a program?

A program is a list of instructions a computer follows. A C++ source file is text for people. The C++ compiler translates that text into machine instructions. The graphics card has its own programs, called **shaders**; this engine writes those in HLSL and compiles them to DXIL.

Think of a 3D engine as a factory:

```text
assets on disk
     |
     v
importer -----> scene entities and GPU geometry/material buffers
                         |
keyboard/mouse -> editor + camera
                         |
                         v
                 frame constants
                         |
                         v
 CPU records DXR commands ---> GPU ray-generation shader
                                   |       |       |
                                   |       |       +--> shadows / reflections
                                   |       +----------> triangle intersection
                                   +------------------> path-traced lighting
                         |
                         v
             accumulation / TAA / NRD
                         |
                         v
                  swap-chain image
                         |
                         v
                    monitor
```

The **CPU** is the general-purpose processor. It reads files, updates the editor, simulates Jolt physics, uploads data, and tells the GPU which work to do. The **GPU** processes many pixels or rays in parallel. A **frame** is one image displayed by the program.

## 2. How to read the source tree

| Location | Beginner description |
| --- | --- |
| `src/main.cpp` | Starts Windows, creates the device/editor/scene, and runs the frame loop. |
| `src/core/` | Small building blocks: vectors, matrices, window, clock, and image capture. |
| `src/ecs/` | Scene data, transforms, asset import, entity lookup, and ocean equations. |
| `src/physics/` and `tools/jolt_bridge.cpp` | C++ interface to a separately built Jolt DLL and the physics simulation inside that DLL. |
| `src/rhi/` | Rendering Hardware Interface: the Direct3D 12 device, ray tracing pipeline, acceleration structures, GPU buffers, and optional NRD bridge. |
| `src/editor/` | ImGui panels, input, viewport display, and graphics settings. |
| `shaders/RayTracingPBR.hlsl` | GPU ray-generation, hit, miss, lighting, water, accumulation, and tone mapping code. |
| `shaders/NrdComposite.hlsl` | Presents the denoiser’s result and applies output mapping. |
| `tests/` | Deterministic math and glTF accessor/import checks. |
| `capture_runner.py`, `ocean_capture_runner.py`, `reflection_capture_runner.py` | Launch the program in repeatable modes and inspect generated images/logs. |
| `vendor/` | External libraries; consult their own documentation and notices. |

The repository is a Windows Direct3D 12/DXR engine. Its build also expects MSYS2 installations for Assimp and Jolt. The paths are CMake cache options, so a different computer can point them at its own installs.

## 3. C++ punctuation and ideas, with a line-by-line example

Here is a small real-style C++ function. The right column describes every line in plain language.

```cpp
float clampUnit(float value) {
    if (!std::isfinite(value)) return 0.0f;
    return std::clamp(value, 0.0f, 1.0f);
}
```

1. `float` says the function returns one single-precision decimal number.
2. `clampUnit` is the function’s name. A function is a named set of steps.
3. `(float value)` says the caller must provide one decimal number. Inside this function, that number is named `value`.
4. `{` begins the function body. The matching `}` ends it.
5. `if` means “only run the following statement if this condition is true.”
6. `std::isfinite(value)` asks the C++ standard library whether the number is ordinary finite data. It rejects NaN (“not a number”) and positive or negative infinity.
7. `!` means “not.” So `!std::isfinite(value)` is true for invalid non-finite values.
8. `return 0.0f;` immediately exits the function and gives the caller zero. The `f` suffix makes the literal a `float`.
9. `std::clamp(value, 0.0f, 1.0f)` returns `value` if it is between zero and one, zero if it is smaller, and one if it is larger.
10. `return std::clamp(value, 0.0f, 1.0f);` sends the bounded input back to the caller. Inside it, the compiler calls the standard-library `clamp` function with three arguments. The semicolon `;` ends the return statement.
11. `}` closes the block that began at the function declaration.

The engine uses these same ideas throughout. `const` means a name cannot be reassigned after initialization. `auto` asks the compiler to infer the type from the right side. `unsigned` means an integer that cannot represent negative values; `uint32_t` is an explicitly 32-bit unsigned integer. `&` in a parameter such as `const Vec3& point` passes a reference to an existing object rather than copying it. `*` in `float* pixels` means a pointer: an address where some data lives. A pointer can be null, which is why robust code checks it before use.

### Declarations and definitions

A **declaration** tells the compiler a name and type exist. A **definition** provides the actual code or storage. Header files (`.h`) commonly declare types/functions so multiple `.cpp` files can use them. `#include "Thing.h"` asks the preprocessor to insert that header’s declarations before compilation. `#pragma once` prevents a header from being inserted twice in one translation unit.

The preprocessor also handles `#if`, `#define`, and platform headers. For example `ENGINE_ENABLE_NRD` selects compile-time code paths. This differs from an editor checkbox: a compile-time option decides what exists in the executable, while a run-time setting chooses what the already-built executable does now.

### Types, objects, and ownership

`struct Vec3 { float x, y, z; };` defines a type with three named fields. `Vec3 cameraPosition{0, 2, -5};` creates an object of that type. `std::vector<T>` owns a variable-sized sequence of `T` objects. `std::unique_ptr<T>` represents exclusive ownership: when it is destroyed, the pointed-to resource is released. Direct3D interfaces use COM reference counting; `Microsoft::WRL::ComPtr<T>` is the smart pointer used here to call `Release()` automatically.

`HRESULT` is a Windows result code. `SUCCEEDED(result)` tests if an API operation worked. The engine must check such results: GPU resource creation or mapping can fail, and continuing with a null or invalid resource may create a crash much later.

## 4. Build instructions and what the build steps mean

`CMakeLists.txt` is a recipe that asks CMake to generate a native build project. It sets C++20, finds Python, optionally configures NVIDIA NRD/NRI, finds Assimp and Jolt in MSYS2, compiles HLSL with DXC, embeds compiled shader bytes, and links `CustomEngine.exe` plus test executables.

From a Visual Studio developer PowerShell prompt, with the dependencies installed and CMake able to reach any configured downloads:

```powershell
cmake -S . -B build -A x64 -DENGINE_ENABLE_NRD=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

To check the built-in renderer path without NRD, use a separate build directory and configure with `-DENGINE_ENABLE_NRD=OFF`. Never reuse one build directory for both configurations: CMake caches its options there.

`cmake -S . -B build` means “read the recipe in the current source folder and write generated project files in `build`.” `cmake --build` compiles. `ctest` runs tests. `--output-on-failure` keeps successful test output quiet and prints details when a test fails. CMake downloads some NVIDIA build dependencies into the selected build directory; the engine’s source tree should remain clean of those generated files.

## 5. Starting the app and following one frame

`WinMain` in `src/main.cpp` is the Windows GUI program entry point. Read it from top to bottom. It creates the window and D3D12 device, initializes editor and renderer objects, loads a level, then repeatedly:

1. reads Windows events;
2. measures elapsed time with the timer;
3. updates camera and Jolt scene state;
4. records GPU commands for the current scene/frame;
5. draws the editor panels and viewport;
6. presents the finished swap-chain image;
7. waits or advances to the next frame until the window closes.

An `if` handles optional paths such as a failed device initialization. A `while` loop repeats while the application is alive. The `deltaTime` value is elapsed seconds since the previous frame; camera movement uses distance = speed × time so movement depends less on frame rate.

The GPU is asynchronous. The CPU can prepare frame 12 while the GPU is still processing frame 11. A **fence** is a counter the CPU can wait on when it must know GPU work has finished, such as before reusing memory or destroying a resource. A **command list** is a recorded set of GPU instructions. A **resource barrier** tells D3D12 when a texture or buffer changes role (for example, from a ray-tracing output to a shader-readable image). Omitting a required barrier is a synchronization bug even if the code compiles.

## 6. Scene objects, entities, and selection

`src/ecs/Entity.h` gives an entity a stable ID. `src/ecs/Components.h` describes names, transforms, materials, meshes, lights, and cameras. In this engine, a `SceneEntity` is a convenient bundle of these data; the implementation is not a general-purpose ECS with independent component pools.

```text
stable entity ID ---> EntityIndex ---> dense render-array index
      1027                 |                    4
                           |                    |
                           +--------------------+--> SceneEntity + GPU instance data
```

The stable ID stays associated with the object. The dense index is the object's position in render arrays. This avoids searching all entities when an Inspector edit arrives. `src/ecs/EntityIndex.h` builds an `unordered_map` from stable ID to array index. Its `rebuild()` builds a temporary map and only swaps it into place after validating every ID. That is a **transactional update**: a duplicate or null ID rejects the new table without destroying the previous good table.

[`EntityIndex::rebuild()`](../src/ecs/EntityIndex.h#L13) in simplified form:

```cpp
bool rebuild(const std::vector<EntityID>& ids) {
    std::unordered_map<EntityID, std::size_t> next;
    next.reserve(ids.size());
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const EntityID id = ids[index];
        if (id == NULL_ENTITY || !next.emplace(id, index).second) return false;
    }
    m_indexById.swap(next);
    return true;
}
```

Line-by-line:

1. `bool` means the function returns true or false. `rebuild` is the function name.
2. `const std::vector<EntityID>& ids` is a read-only reference to the new list of IDs. The `&` avoids copying the entire list.
3. `next` is a temporary hash map. It is private to this call and starts empty.
4. `reserve(ids.size())` asks the map to allocate enough buckets for the expected entries, reducing reallocations.
5. The loop index begins at zero, compares with the vector’s length, and increments by one per ID.
6. `const EntityID id = ids[index];` copies one ID to a local, unchangeable name so the rest of the loop reads clearly.
7. `id == NULL_ENTITY` rejects the reserved “no entity” value. `emplace(id,index)` inserts the key/value only if the key is new and returns a pair. `.second` is true only when insertion succeeded. `||` means logical OR; either invalid condition returns false immediately.
8. `m_indexById.swap(next)` commits the completed map in one operation. It is reached only if every input ID was valid and unique.
9. `return true` tells the caller the new mapping is ready. The closing brace ends the function.

Because the active map is untouched until `swap`, every early failure leaves the old index valid. This prevents a half-built lookup table from silently breaking selection.

The selector in `src/editor/EditorUI.cpp` filters visible names/IDs, supports next/previous selection within the filtered list, and passes selection to `src/ecs/Scene.cpp`. Viewport picking converts mouse pixel coordinates into a camera ray and tests world-space axis-aligned bounding boxes (AABBs). An AABB is a box aligned with world X/Y/Z. It is fast, but a click may select empty space inside a mesh’s box; it is not an exact triangle test.

### Camera ray conversion

The mouse position is first normalized to the viewport:

```text
ndcX =  2 * mouseX / viewportWidth  - 1
ndcY =  1 - 2 * mouseY / viewportHeight
```

`NDC` means Normalized Device Coordinates. Its horizontal and vertical coordinates normally range from -1 to +1. The Y expression is flipped because screen pixels count downward while the camera convention counts upward. The code constructs a view-projection matrix, inverts it, unprojects near/far points, and normalizes the difference to obtain a ray. It must reject a zero-sized viewport and a non-finite or near-zero matrix determinant; otherwise inversion may create invalid values.

## 7. Vectors, matrices, and transforms

`src/core/Math.h` defines small `Vec2`, `Vec3`, `Vec4`, `Mat4`, and `AABB` types. A vector is a tuple of numbers used as a position, direction, color, or normal. The same numbers can mean different things depending on their job: positions translate, directions do not, and normals must stay perpendicular to a surface.

### Vector formulas

For vectors `a=(ax,ay,az)` and `b=(bx,by,bz)`:

```text
a + b = (ax+bx, ay+by, az+bz)
a · b = ax*bx + ay*by + az*bz                 dot product
|a|   = sqrt(a · a)                           length
a/|a|                                             normalized direction
a × b = (ay*bz-az*by, az*bx-ax*bz, ax*by-ay*bx) cross product
```

The dot product measures alignment. A dot product of 1 means same direction for unit vectors; 0 means perpendicular; -1 means opposite. The cross product returns a perpendicular vector. Normalizing a zero-length vector would divide by zero, so `Vec3::normalized()` returns a zero vector for tiny lengths and throws for non-finite length. `Vec3::operator/` rejects zero/tiny and non-finite divisors.

### Matrices and order

A 4×4 matrix can rotate, scale, and translate a point. The extra fourth coordinate is `w=1` for positions and `w=0` for directions. A transform is composed from scale `S`, rotation `R`, and translation `T`:

```text
local point --S--> scaled point --R--> rotated point --T--> world point
```

DirectXMath uses row vectors in the engine's common matrix path, so `S * R * T` means scale first, then rotate, then translate. Matrix memory layout and algebraic multiplication order are separate concepts; this is why the code has explicit transposes and copies for the D3D12 instance transform. The `D3D12_RAYTRACING_INSTANCE_DESC` transform is a 3×4 row-major affine matrix. Its 3 rows encode rotation/scale plus translation; the absent last row is implicitly `(0,0,0,1)`.

When debugging a transform, check three things separately: the vector convention (row or column), the order of matrix multiplication, and the memory layout expected by the GPU API. A transposed matrix can compile and still put the mesh in the wrong place.

The editor also maps its UI coordinates into clip space. If its top-left display origin is `(L,T)` and its size is `(W,H)`, the scales are `sx=2/W`, `sy=-2/H`; the translations are `tx=-1-2L/W`, `ty=1+2T/H`. The negative Y scale flips screen-down coordinates into clip-space-up coordinates. [`makeUiProjection()`](../src/editor/ImGuiProjectionMath.h#L10) evaluates these values in `double`, rejects zero/non-finite extents, then narrows to GPU `float` constants only after checking that they remain finite. It first builds a temporary `candidate` matrix and copies it to `out` only when every calculated value is representable, so a failed call leaves the caller’s output untouched. This is algebraically the same as forming right/bottom edges and dividing by `(R-L)` / `(B-T)`, but avoids adding two large screen coordinates before dividing. Three math tests compare non-zero and zero-origin rectangles against expected clip-space values and check invalid-input behavior.

#### A real matrix bug found during this review

The DXR shader had been taking the transposed `ObjectToWorld4x3()` matrix and multiplying it as if it were the ordinary object-to-world matrix. This was wrong for rotated objects and non-uniform scales. The official HLSL reference says `ObjectToWorld4x3()` is the transpose of `ObjectToWorld3x4()`, and HLSL `mul(x,y)` treats a vector on the right as a column vector. See Microsoft’s [ObjectToWorld4x3 reference](https://learn.microsoft.com/en-us/windows/win32/direct3d12/objecttoworld4x3), [WorldToObject4x3 reference](https://learn.microsoft.com/en-us/windows/win32/direct3d12/worldtoobject4x3), and [HLSL `mul` reference](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-mul).

The corrected expressions in `shaders/RayTracingPBR.hlsl` are:

```hlsl
float3 worldEdge = mul((float3x3)ObjectToWorld3x4(), objectEdge);
float3 worldNormal = normalize(mul((float3x3)WorldToObject4x3(), objectNormal));
```

Line-by-line:

1. `float3` means “three floating-point numbers.” The result represents a direction in world space.
2. `ObjectToWorld3x4()` asks DXR for the affine transform from BLAS/object coordinates into world coordinates. Casting to `float3x3` discards translation, which is correct for an edge because translating both endpoints does not change the edge direction.
3. `mul(matrix, vector)` applies the 3×3 linear transform to a column direction. This gives the correct world-space edge used to estimate triangle footprint and texture mip level.
4. A normal must use the inverse transpose. `WorldToObject4x3()` is the transpose of the world-to-object matrix, so its upper 3×3 is the inverse-transpose needed by this column-vector multiplication.
5. `normalize` makes the result unit length. The shader’s `safeNormalize` version additionally returns a fallback direction if the input is invalid or nearly zero.

The equation behind the second line is `n_world = (A⁻¹)ᵀ n_object`, where `A` is the 3×3 linear part of the object-to-world transform. It guarantees that the transformed normal remains perpendicular to every transformed surface tangent. `tests/MathTests.cpp` now checks that perpendicularity under a scale of 2 on X and 1 on Y. The shader is also compiled in the Release build. A CPU math check plus shader compilation is evidence for the formula; it is not a substitute for rendering a specially scaled test mesh and inspecting its lighting.

### AABB ray test

`AABB::rayIntersect()` uses the **slab method**. Imagine the box as three pairs of parallel walls. For each axis, calculate the ray times at the two walls, sort them, and intersect the three time intervals. If the intervals do not overlap, the ray misses. If a ray direction is nearly zero on one axis, it is parallel to that pair of walls: it misses if its origin is outside that slab and otherwise imposes no limit. If the ray begins inside the box, the method returns the positive exit distance instead of a negative entry distance.

The function validates finite origins/directions/bounds, rejects inverted bounds, avoids division for parallel axes, rejects rays pointing away, and writes `outT` only for a finite hit. The caller should provide a direction in the units it expects for the returned distance; the editor normalizes its pick ray.

## 8. Importing glTF and interpreting mesh data

The glTF importer is in `src/ecs/Scene.cpp`; the project uses cgltf to parse the file and Assimp for supported import paths. glTF stores node transforms in a 16-number, column-major array. The helper in `src/ecs/GltfImportMath.h` applies that matrix to positions and normals.

For a position `(x,y,z,1)`, the helper computes:

```text
w' = m3*x + m7*y + m11*z + m15
x' = (m0*x + m4*y + m8*z + m12) / w'
y' = (m1*x + m5*y + m9*z + m13) / w'
z' = (m2*x + m6*y + m10*z + m14) / w'
```

The division by `w'` is the homogeneous-coordinate rule. An invalid or nearly zero `w'` is rejected. Positions are points, so translation affects them. Normals are directions, so translation must not affect them.

### Why normals use inverse transpose

For nonuniform scale, transforming a normal with the position matrix does not keep it perpendicular to the transformed surface. If `A` is the upper-left 3×3 transform, a normal must be transformed by `(A⁻¹)ᵀ`, then normalized. `transformGltfNormal()` forms the cofactor matrix, which equals `det(A) * (A⁻¹)ᵀ`. Normalization removes the determinant’s magnitude; the code restores its sign so a mirrored transform keeps orientation consistent. If the determinant is zero, the transform collapses a dimension and has no inverse, so the helper returns failure.

The importer helper is [`transformGltfNormal()`](../src/ecs/GltfImportMath.h#L19). Its important local names mean:

| Name | Meaning |
| --- | --- |
| `a` through `i` | The 3×3 linear transform, unpacked by row. These are not arbitrary letters; together they are `A=[[a,b,c],[d,e,f],[g,h,i]]`. |
| `c00` through `c22` | Nine cofactors that form the cofactor matrix. Each is a signed 2×2 determinant. |
| `determinant` | `det(A)`, a signed measure of volume scaling. Zero means the matrix squashes 3D space into a plane/line and cannot be inverted. |
| `nx, ny, nz` | The input normal multiplied by the cofactor matrix. |
| `lengthSquared` | `nx²+ny²+nz²`; squaring avoids a square root until the code has checked it is finite and nonzero. |
| `inverseLength` | Reciprocal length, including the determinant sign. Multiplying by it normalizes the resulting normal. |

The routine checks pointers and input components first, computes cofactors/determinant, rejects a nearly singular matrix, transforms, checks the squared magnitude, normalizes, and checks the final components. Returning `false` means “the output must not be used.” It does not throw. The importer treats that failure as an invalid primitive instead of uploading a corrupt normal.

The importer validates accessors and rejects malformed/non-finite values before emitting geometry. `tests/GltfImportTests.cpp` has focused checks for interleaved, normalized, and sparse accessors, plus inspection of the Khronos sample asset. A parser passing these checks does not prove that every glTF extension or every external asset is supported.

### Vertex and index buffers

A **vertex** stores attributes such as position, normal, and texture coordinates. An **index** is an integer selecting a vertex. Three indices form one triangle. Reusing vertices with indices saves memory. `vertexOffset` and `indexOffset` locate a mesh’s data inside larger shared GPU buffers. `indexCount` should be divisible by three for triangle lists; all offsets and counts must remain within their allocated buffers.

The importer creates mesh bounds and material records, uploads combined arrays, and builds a BLAS for each mesh. A BLAS (Bottom-Level Acceleration Structure) organizes triangles for fast ray/triangle intersection. A TLAS (Top-Level Acceleration Structure) organizes instances of those meshes with their transforms.

## 9. Ocean math and Jolt interaction

`src/ecs/OceanWave.h` is the CPU implementation of the analytic water surface. `shaders/RayTracingPBR.hlsl` contains a matching GPU implementation for visual shading and ray-hit correction. Jolt is a rigid-body physics library; the wave functions provide water height/velocity and approximate buoyant forces, while Jolt calculates the motion of the floating objects. The ocean is a demonstration, not a general fluid solver.

### Five base waves

For wave `i`, position `(x,z)`, and time `t`:

```text
phase_i = kx_i*x + kz_i*z + omega_i*t
h(x,z,t) = Σ A_i sin(phase_i)
∂h/∂x = Σ A_i*kx_i*cos(phase_i)
∂h/∂z = Σ A_i*kz_i*cos(phase_i)
∂h/∂t = Σ A_i*omega_i*cos(phase_i)
```

`A` is wave height in world units, `kx/kz` describe spatial frequency/direction, and `omega` is angular speed in radians per second. The surface slope is a derivative, not an angle. A geometric normal can be built from `(-∂h/∂x, 1, -∂h/∂z)` and normalized. These analytic derivatives are faster and more accurate than repeatedly sampling nearby points; the test suite nevertheless compares them to finite differences as an independent check.

#### The source loop, one line at a time

This excerpt is from [`evaluateOceanWave()`](../src/ecs/OceanWave.h#L19):

```cpp
for (std::size_t i = 0; i < amplitude.size(); ++i) {
    const float phase = waveX[i] * x + waveZ[i] * z + angularFrequency[i] * timeSeconds;
    if (!std::isfinite(phase)) return {};
    const float phaseSin = std::sin(phase);
    const float phaseCos = std::cos(phase);
    if (!std::isfinite(phaseSin) || !std::isfinite(phaseCos)) return {};
    result.height += amplitude[i] * phaseSin;
    result.slopeX += amplitude[i] * waveX[i] * phaseCos;
    result.slopeZ += amplitude[i] * waveZ[i] * phaseCos;
    result.verticalVelocity += amplitude[i] * angularFrequency[i] * phaseCos;
}
```

1. `for` starts a loop. `std::size_t i = 0` creates the array index at its first element.
2. `i < amplitude.size()` asks whether the index is still inside the array. The loop has five passes because the coefficient arrays each have five elements.
3. `++i` increases the index by one after each pass.
4. `const float phase` creates a single-precision number that cannot be reassigned. `waveX[i]`, `waveZ[i]`, and `angularFrequency[i]` select the three coefficients belonging to this wave.
5. `if` checks the phase before evaluating trigonometry. `!` means “not”; `std::isfinite` is false for NaN/infinity. `return {}` exits immediately with all result fields set to zero.
6. `std::sin(phase)` evaluates the wave height shape in radians and stores it in `phaseSin`.
7. `std::cos(phase)` is reused because the derivative of sine is cosine.
8. The next `if` guards against invalid math-library results before they reach the accumulated output.
9. `result.height += ...` adds this wave’s height to the sum. `+=` means “replace the left value with left plus right.”
10. `result.slopeX += ...` adds the derivative along X. The factor `waveX[i]` is the derivative of `phase` with respect to X.
11. `result.slopeZ += ...` does the same for Z.
12. `result.verticalVelocity += ...` computes the derivative with respect to time; angular frequency is the rate at which phase changes over time.
13. `}` closes the repeated part. Once all five waves are added, the function checks all accumulated fields for finite values and returns the structure.

The guard before this excerpt rejects non-finite `x`, `z`, and `timeSeconds`. The arrays above it are `constexpr std::array<float, 5>` values: `constexpr` makes them compile-time constants, `std::array` stores a fixed number of same-typed values, and `<float, 5>` says five floating-point entries. Keeping equal array lengths makes each `[i]` refer to a matching wave. The GPU copy [`evaluateOceanSurface()`](../shaders/RayTracingPBR.hlsl#L248) repeats the coefficients; changing one side without changing the other makes the visible water disagree with Jolt’s sampled water.

### Impact ring

An impact stores center `(x0,z0)`, start time `t0`, and normalized strength `s`. To avoid the undefined radial direction at exactly the center, the code uses a smoothed radius:

```text
r = sqrt((x-x0)² + (z-z0)² + epsilon²) - epsilon
age = t - t0
phase = 4.6*r - 7.2*age
amplitude = 0.23*s*exp(-0.27*r - 0.92*age)
impactHeight = amplitude*sin(phase)
```

The derivatives use the product rule: the amplitude changes with distance/time, and the sine changes with phase. `evaluateOceanImpactWave()` rejects future, expired, non-finite, and negligible-strength events. The CPU and GPU coefficients must agree; a mismatch makes physics and rendered waves disagree.

### Body depression and volume-balancing ridge

Submersion is estimated from the water surface `H`, body center height `yb`, and vertical half-size `hy`:

```text
rawSubmersion = (H + hy - yb) / (2*hy)
submersion = clamp(rawSubmersion, 0, 1)
```

Zero means the body is fully above the water; one means it is at least as submerged as this simple vertical model represents. For body horizontal center `(xb,zb)` and radius `R`, define `u=((x-xb)²+(z-zb)²)/R²`. The profile is:

```text
F(u) = -exp(-u) + w*exp(-((u-2.25)/2)²)
D(x,z) = min(0.45*R, 0.45)*submersion*F(u)
```

The first Gaussian makes a depression under the body. The second makes a broader raised ring. The ridge weight `w≈0.2987677926` is chosen to balance the radial height integral. Since `u=r²/R²`, the area integral is proportional to `∫F(u)du`; choosing `w` as the ratio of the depression integral to the ridge integral makes the net height integral zero over the infinite plane. The numerical integration test checks this property over a wide finite domain.

The profile derivative is the chain rule. Because `du/dx=2(x-xb)/R²`, its X slope is:

```text
∂D/∂x = amplitude * (x-xb)/R² * [2 exp(-u) - 2*w*exp(-q²)*q]
q = (u-2.25)/2
```

The Z derivative is the same with `(z-zb)`. Its time derivative includes both changing submersion and the body moving horizontally across its own radial field:

```text
∂D/∂t = (∂amplitude/∂t)*F - amplitude*(∂F/∂x*vx + ∂F/∂z*vz)
```

This keeps the water field attached to the moving body and supplies vertical water velocity to nearby buoyancy samples. It is still a compact height-field approximation; it does not solve pressure or conserve a finite water volume inside a bounded ocean.

### Rotated body support and entry detection

An oriented box does not always extend vertically by its local Y half-height. The world vertical half-extent is:

```text
supportY = |R_yx|*halfX + |R_yy|*halfY + |R_yz|*halfZ
```

An oriented ellipsoid uses the scaled-sphere support formula:

```text
supportY = sqrt((rx*R_yx)² + (ry*R_yy)² + (rz*R_yz)²)
```

`R_yx`, `R_yy`, and `R_yz` are the vertical components of the object's rotated local axes. The quaternion is normalized first; a non-finite or zero quaternion is rejected. This support is used to estimate the lowest point of a body for entry and buoyancy checks.

`Scene::updateOceanPhysics()` in `src/ecs/Scene.cpp` owns the fixed `1/60` second schedule. It accumulates accepted frame time, calls the `JoltPhysicsWorld::step()` wrapper, which calls the `JoltBridge_Step` C ABI function in `tools/jolt_bridge.cpp`, and limits catch-up to four steps per rendered frame. A fixed step makes simulation behavior less dependent on whether a rendered frame took 10 ms or 30 ms. If a frame stalls, the engine intentionally records and drops excess elapsed time rather than simulate an unbounded backlog (the “spiral of death”). This is a stability tradeoff, not exact wall-clock conservation.

Jolt uses collision shapes, mass, velocity, gravity, constraints, and collision layers. The bridge exports a small C ABI (functions with plain C-compatible arguments) so the engine can call the Jolt DLL without exposing Jolt C++ types across the DLL boundary. Every pointer/count pair needs validation: a pointer to the first item plus a count tells the function which memory range is allowed.

## 10. DirectX Raytracing (DXR) in this engine

**Ray tracing** asks what geometry a line intersects. A ray is:

```text
P(t) = origin + t * direction,  where t >= 0
```

The GPU first tests a TLAS, then a BLAS, and the shader reports a hit or miss. `src/rhi/AccelerationStructure.cpp` builds and manages BLAS/TLAS resources. `src/rhi/RayTracingPipeline.cpp` creates the DXR state object, shader tables, descriptors, and dispatch. `src/rhi/D3D12Device.cpp` owns the D3D12 device, queue, command allocators/lists, descriptor heaps, swap chain, and synchronization.

### Shader roles

| HLSL entry point | What it does |
| --- | --- |
| `RayGenShader` | Runs once per output pixel, makes camera rays, traces scene rays, estimates lighting, handles accumulation/reprojection, writes color/history. |
| `AnyHitShader` | Can reject a triangle hit, for example when a texture says the material is transparent at that UV. |
| `ClosestHitShader` | Runs for the nearest accepted triangle and computes surface/material data, direct illumination, reflection/secondary-hit information, or corrected analytic water hit. |
| `MissShader` | Supplies sky radiance when a regular ray hits no geometry. |
| `ShadowMissShader` | Marks a shadow ray as unoccluded when it hits nothing. |

`TraceRay()` asks DXR to traverse the acceleration structures. The payload is the small record passed between ray generation, hit, and miss shaders. Keep payload size bounded because the GPU may need storage for many active rays.

### Correctness and resource lifetime

DXR buffers must be large enough for their declared element counts, aligned as required by D3D12, and left alive until the GPU fence says work using them has completed. A CPU pointer returned by `Map()` is only valid according to the resource’s mapping lifetime. A barrier must put the output resource in the state expected by the next operation. Errors from `CreateCommittedResource`, state-object creation, command-list close, and queue submission must not be silently ignored.

#### A real C++ status path, line by line

`D3D12Device::beginFrame()` now reports whether it actually opened a command list. Simplified from `src/rhi/D3D12Device.cpp`:

```cpp
bool beginFrame() {
    if (m_frameRecording || m_frameIndex >= FRAME_COUNT || !m_commandList) return false;
    HRESULT hr = m_commandAllocators[m_frameIndex]->Reset();
    if (FAILED(hr)) return false;
    hr = m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);
    if (FAILED(hr)) return false;
    m_frameRecording = true;
    return true;
}
```

1. `bool` is the function’s result type: it can return `true` or `false`.
2. The `if` combines four “bad state?” questions. `||` means OR, so any bad condition exits with failure. `m_frameRecording` prevents opening a second command list over one already being recorded. `m_frameIndex` chooses which back-buffer allocator to reset. `FRAME_COUNT` is the number of swap-chain buffers. `!m_commandList` checks whether the smart pointer is empty.
3. `HRESULT hr` declares a Windows API result variable. `m_commandAllocators[m_frameIndex]` selects the allocator for this frame. `->` calls a method through a pointer-like COM object. `Reset()` tells D3D12 this allocator’s old recording can be reused; the frame fence must already prove the GPU finished with it.
4. `FAILED(hr)` is a Windows macro that tests the result code. If it failed, `return false` exits now, so the caller cannot record commands into an unusable list.
5. The second `Reset(...)` opens the graphics command list and associates it with the chosen allocator. `.Get()` gives D3D12 the raw COM pointer owned by `ComPtr`. `nullptr` means “use no initial pipeline state.”
6. Only after both API calls succeed does the function set `m_frameRecording = true`. `=` assigns the Boolean value; it is not a comparison.
7. `return true` tells the caller it can record commands. The caller checks that result before doing rendering work.

In the full function, the method also checks all required objects and binds the shader-visible descriptor heap. The same explicit status pattern now covers `endFrame()`, queue/fence synchronization, resize waits, and capture waits. `FAILED` is preferable to checking `hr != S_OK`: Windows APIs may return a successful status other than exactly `S_OK`.

`src/rhi/AccelerationStructure.cpp` uses checked alignment/range helpers. Alignment to `a` is usually `ceil(n/a)*a`, expressed with integer operations; it is only safe when `a != 0` and the addition does not overflow. Buffer bounds are checked as `offset <= size && requested <= size-offset`; this subtraction form avoids overflowing `offset + requested`.

## 11. Lighting and material math

The shader implements a metallic/roughness PBR (physically based rendering) model. “Physically based” means the equations are shaped around measured light/material behavior; it does not mean every material value or every lighting approximation is physically exact.

### Lambert diffuse and direct light

For a surface normal `N` and direction to a light `L`, `max(N·L,0)` reduces light on grazing or back-facing surfaces. Lambert diffuse is:

```text
f_diffuse = baseColor / π
directDiffuse ∝ f_diffuse * max(N·L,0) * lightRadiance
```

`π` appears because Lambertian light is spread evenly over a hemisphere; integrating the cosine-weighted hemisphere gives `π`. Shadows multiply direct light by zero when blocked and one when visible, or by a sampled fractional visibility for soft shadows.

### GGX specular and Fresnel

GGX describes how many tiny surface facets face the half vector `H=normalize(V+L)`. The distribution is controlled by roughness. The code squares roughness to form `alpha`, then evaluates the normal distribution `D`, the masking term `G`, and Fresnel reflectance `F`:

```text
f_specular = D(N,H) * G(N,V,L) * F(V,H) / (4 * (N·V) * (N·L))
F(cosθ) = F0 + (1-F0)*(1-cosθ)^5
```

`F0` is reflectance looking straight at the surface. For ordinary dielectric materials it is around 0.04; water is roughly 0.02. At grazing angles, Schlick’s approximation raises Fresnel toward one. Every denominator needs a nonzero guard, and dot products are clamped to their valid cosine range.

### Sampling a glossy ray

The reflection shader samples a GGX **visible normal distribution** (VNDF), which favors facet normals visible from the current view. Importance sampling chooses directions that contribute more often, reducing wasted samples. For reflected direction `L`, the VNDF density is:

```text
p(L|V) = D(H) * G1(V) / (4 * (N·V))
```

The Monte Carlo path weight is `BRDF * max(N·L,0) / PDF`. Substituting the GGX BRDF above cancels the matching factors and gives:

```text
weight = F(V,H) * G1(L)
```

The shader traces eligible reflections at half rate. If selection probability is `p=0.5`, an accepted contribution is multiplied by `1/p=2`. This preserves the expected average, but individual pixels become noisier because fewer rays were taken. This is a performance/variance tradeoff. The automated algebra check verifies the cancellation on a grid; it cannot prove a GPU shader implementation matches the reference for every input, so shader compilation and image A/B tests are also needed.

### Diffuse path tracing

Cosine-weighted hemisphere sampling chooses a bounce direction with probability `PDF=cosθ/π`. Lambert BRDF is `albedo/π`; therefore `(BRDF*cosθ)/PDF = albedo`. The throughput (the multiplier carried along the path) can be updated by multiplying by the surface albedo. The ray then gathers light if it hits geometry or sky.

Russian roulette sometimes ends a long path. If a path continues with probability `q`, divide its throughput by `q`; otherwise its expected energy is reduced. The shader bounds the survival probability to avoid unstable tiny denominators and imposes a maximum bounce count. This reduces average cost while keeping an unbiased expectation for the randomized continuation, subject to the implementation’s clamps and termination policy.

## 12. Anti-aliasing, history, denoising, and tone mapping

One ray per pixel creates aliasing and Monte Carlo noise. The renderer varies sample positions and accumulates results over time. A running average can be updated without storing every old sample:

```text
average_n = average_(n-1) + (sample_n - average_(n-1)) / n
```

The shader stores radiance and depth history. Camera movement reprojects a world position into the previous frame and compares previous depth; a mismatch means the prior pixel likely belongs to another surface, so history is rejected. Static scenes can become much cleaner over time, but motion, disocclusion, and invalid history can cause trails or flicker.

NRD is NVIDIA’s denoising library. This project’s optional NRD integration passes the required noisy signals and camera/material guides to a denoiser, then composites the returned image. Without NRD, the shader has its own limited display filter. A filter can smooth noise but may blur details; it is not a replacement for enough correct samples.

Path tracing produces **HDR** (high dynamic range) values that may be greater than one. Tone mapping compresses those values for the display. ACES, Reinhard, and linear output are selectable. The final output should be converted to the display’s sRGB encoding. Applying gamma twice or mixing sRGB texture values directly into linear lighting causes muted or over-bright colors.

## 13. Variable-by-variable tour of the rendering path

This section is a reading map for the long functions. It names the data a beginner will meet and the direction that data travels. C++ comments, braces, and expressions in the linked files should be read in order; the tables explain the variables that carry the important state.

### Start-up and the frame loop

Open [`WinMain`](../src/main.cpp#L23). Its important names have these jobs:

| Variable | Job |
| --- | --- |
| `window` | Owns the Win32 window and collects mouse/keyboard/resize events. |
| `device` | Owns D3D12/DXR objects, the GPU command queue, swap chain, descriptors, and fences. |
| `pipeline` | Owns ray tracing state, output/history textures, shader binding table, and denoising/composite objects. |
| `scene` / `activeScene` | The currently selected level and its mesh/material buffers, camera, and TLAS. |
| `editorUI` / `imguiRenderer` | The controls and the code that submits their draw commands. |
| `wallDt` | Real seconds elapsed since the previous frame. |
| `dt` | Simulation update interval; in the water capture it is fixed to 1/60 second. |
| `frameNum` | Test-mode frame counter. It schedules reproducible mode changes and captures. |
| `needCapture` | Zero for normal frames, otherwise identifies a requested test screenshot. |
| `viewMatrix`, `projectionMatrix` | Camera transforms used to build rays and project history into screen space. |

The loop’s order matters: process events; switch/update scene; upload changed scene state; update camera matrices; open a command list; dispatch DXR; start the ImGui frame and build controls; draw ImGui; close/submit commands and present; wait only when a test capture needs pixels on the CPU. Reordering these steps can cause the UI to display one frame behind, CPU capture to read unfinished GPU work, or descriptor memory to be overwritten too early.

### CPU-to-GPU scene records

Open [`src/rhi/ConstantBuffers.h`](../src/rhi/ConstantBuffers.h) beside the top of [`RayTracingPBR.hlsl`](../shaders/RayTracingPBR.hlsl#L11). `Vertex`, `MaterialData`, `InstanceData`, and `SceneCB` are mirrored on both sides. A **structured buffer** is an array of same-size records; HLSL uses the C++ field offsets to read each record. The compile-time `static_assert` lines check important C++ sizes and 16-byte packing boundaries. A changed field order or padding can cause the shader to read a color as an index without any type error at run time.

| Record/field | Meaning |
| --- | --- |
| `Vertex.position` | Object-space XYZ point used by triangle intersection and ray-hit shading. |
| `Vertex.normal` | Object-space unit-ish normal used for lighting. |
| `Vertex.uv` | 2D texture address interpolated over the triangle. |
| `MaterialData.baseColor` | Linear material tint and alpha. |
| `MaterialData.metallic`, `roughness`, `emissive` | PBR response controls. |
| `textureId`, `metallicRoughnessTextureId` | Array-layer indices; negative means no texture. |
| `InstanceData.vertexOffset`, `indexOffset` | Base positions of this mesh’s vertices/indices in shared arrays. |
| `InstanceData.materialId` | Material-table row selected by this instance. |
| `SceneCB.cameraPos`, camera basis, `fovY`, `aspectRatio` | Values needed to generate a primary ray. |
| `SceneCB.prev*` | Prior frame camera values used to reproject history. |
| `renderMode`, `maxBounces`, feature toggles | Select direct shading/path tracing and optional effects. |
| `oceanBody*` arrays | Fixed-capacity GPU snapshot of Jolt body positions, sizes, velocity, and impacts. |

The CPU may have extra fields for editor/bookkeeping. Only the GPU-shared structs must match byte-for-byte. Comments and `static_assert` protect the boundary; changing one side requires changing and recompiling the other.

### `RayGenShader()` data path

`RayGenShader()` in [`RayTracingPBR.hlsl`](../shaders/RayTracingPBR.hlsl#L550) is the first shader for each pixel. Key variables:

| Variable | Meaning |
| --- | --- |
| `launchIndex` | Pixel coordinates (X, Y) of this GPU shader invocation. |
| `launchDim` | Width/height of the dispatched image. |
| `pixelIndex` | One-dimensional pixel number, `y*width+x`, used in hashes/history addressing. |
| `sampleSeed`, `sampleStream`, `sampleIndex` | Deterministic/randomized sequence state. Different pixels and frames should not share the same sample stream. |
| `jitterX`, `jitterY` | Subpixel camera offset for anti-aliasing. |
| `tanHalfFov`, `aspect` | Convert normalized screen coordinates to view-ray slopes. |
| `ray` | DXR ray origin, direction, near bound, and far bound. |
| `payload` | Mutable return record filled by closest-hit or miss shader. |
| `totalRadiance` | Running radiance estimate contributed by the primary and subsequent path segments. |
| `throughput` | Fraction of path energy that remains after each bounce. |
| `maxBounces` | Upper limit on path length, clamped to the supported range. |
| `primaryHitPos`, `primaryHitT` | First surface position/distance, used for depth and reprojection. |
| `accumFrameCount` | Number of samples in the current history run; reset when camera or settings invalidate history. |

The control flow is: calculate pixel/sample; generate a camera ray; call `TraceRay`; read the payload; optionally choose a secondary diffuse bounce or accept radiance; stop at a bounce limit or Russian roulette; reject non-finite radiance; accumulate; validate/reproject old history; denoise/composite if enabled; store the current history and write the output pixel. The `if` statements in this sequence are not just optimizations: they select different estimators and history behavior.

### `ClosestHitShader()` data path

`ClosestHitShader()` in [`RayTracingPBR.hlsl`](../shaders/RayTracingPBR.hlsl#L922) runs for the nearest accepted triangle:

| Variable | Meaning |
| --- | --- |
| `instanceIndex`, `primIndex` | Which instance and triangle DXR hit. |
| `inst`, `mat` | Per-instance offsets/IDs and the corresponding material. |
| `i0`, `i1`, `i2` | Three global vertex indices for this triangle. |
| `v0`, `v1`, `v2` | The triangle’s three vertex records. |
| `attribs.barycentrics` | Two barycentric weights; the third is `1-x-y`. They interpolate vertex values across the triangle. |
| `localNormal`, `worldNormal` | Interpolated normal in mesh coordinates, then correctly inverse-transpose transformed to world coordinates. |
| `uv`, `texColor` | Interpolated texture coordinate and sampled base-color texel. |
| `hitPos`, `V` | World hit point and direction from the hit toward the camera. |
| `albedo` | Texture color converted from sRGB to linear then multiplied by the material tint. |
| `F0`, `H`, `NdotL`, `NdotV` | Specular reflectance at normal incidence, half-vector, and clamped light/view cosines. |
| `reflectionRay`, `shadowRay` | Secondary DXR queries starting just above the surface to reduce self-intersections. |

The shader first fetches/interpolates the triangle, transforms its normal, and samples its material. It has early-return branches for a reflected hit and the water material. Ordinary opaque surfaces then evaluate material response, direct light, optional shadow visibility, optional reflected radiance, and optional path-tracing signals. `payload` is the output: the closest-hit shader writes it and returns to the shader that launched the ray.

### GPU resource lifetime

In [`D3D12Device.cpp`](../src/rhi/D3D12Device.cpp#L20), `m_frameIndex` chooses the swap-chain image and command allocator for the current frame. `m_fenceValues[frame]` records the GPU fence count associated with that allocator. The CPU must wait until that count has completed before resetting the allocator, or it may overwrite command memory the GPU is still reading. The shader-visible descriptor heap stores handles that the HLSL `register(t#)` declarations refer to. Descriptor indices must stay below capacity; the engine’s allocator returns a sentinel on exhaustion.

In [`RayTracingPipeline.cpp`](../src/rhi/RayTracingPipeline.cpp#L25), `m_stateObject` contains compiled shaders and hit groups; `m_shaderBindingTable` tells DXR which shader records and local data to use; `m_outputTexture` receives the current image; `m_accumulationTexture` stores progressive history; `m_historyTexture` supplies prior-frame data. Resource state transitions and UAV barriers express ordering to D3D12. They do not block the CPU; they order operations in the GPU command stream.

### Import and Jolt code paths

`Scene::loadGltf()` in [`Scene.cpp`](../src/ecs/Scene.cpp#L1152) owns parsed data with `std::unique_ptr<cgltf_data, decltype(&cgltf_free)>`: when the function exits, `cgltf_free` releases the parser object even on an early return. It builds `texturePaths`, then converts each primitive’s accessor data into `localVertices` and `localIndices`, validates bounds/material IDs, and appends the accepted primitive to shared arrays. `worldMatrix` is the accumulated node transform. `sourcePosition` and `sourceNormal` are temporary accessor values; `transformedPosition` and `transformedNormal` are the baked world-space values. `localBounds` is recomputed from those positions. If one vertex/accessor is invalid, `primitiveValid` becomes false and that primitive is skipped; if no valid geometry remains, the whole import fails.

`World` and `FloatBody` in [`tools/jolt_bridge.cpp`](../tools/jolt_bridge.cpp#L64) hold the physics system and per-body handles/state. `WaterImpact` stores a bounded event record. `sampleWaterSurface()` combines the base wave, retained impact rings, and other bodies’ displacement. `excludedBody` prevents a body from lifting itself using its own depression field. Each exported `JoltBridge_*` function checks the opaque world handle before use and copies plain floats/integers across the C ABI. The `JoltPhysicsWorld` C++ wrapper checks that the function pointers were resolved before calling them.

## 14. Testing: what each test can and cannot prove

`tests/MathTests.cpp` tests the vector/camera guards, entity index, AABB slab test, ocean base-wave/impact/body derivatives, volume-balanced field, water ray-height Newton solve, quaternion support extents, glTF transforms, and reference reflection math. For a derivative check, it estimates:

```text
f'(x) ≈ [f(x+ε)-f(x-ε)] / (2ε)
```

This central finite difference is an independent approximation to the derivative formula. It catches sign and coefficient mistakes at tested points. It does not prove correctness for every point, so edge cases (center, grazing direction, expired impact, NaN, infinity, overflow) are tested separately.

`tests/GltfImportTests.cpp` checks parser/accessor cases: interleaved data, normalized integers, sparse replacements, and a real sample asset. `capture_runner.py` checks visible Sponza modes and expected log markers. `reflection_capture_runner.py` compares identical scenes with reflections on/off/restored and verifies DXR telemetry and image deltas. `ocean_capture_runner.py` checks Jolt motion, water masks, displacement/reflection A/Bs, and generated sequences. These capture tests require the Windows app, a supported DXR GPU, assets, and the staged DLLs; a unit test passing does not mean a GPU integration test ran.

Reproduce the core checks with:

```powershell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
python -m py_compile capture_runner.py ocean_capture_runner.py reflection_capture_runner.py tools\build_ocean_gif.py tools\embed_dxil.py
python capture_runner.py --native-resolution
python ocean_capture_runner.py --compare-reflections
python reflection_capture_runner.py --scene all --native-resolution
```

These capture commands launch the built Windows executable, take time, and require a supported DXR GPU. Do not run multiple at the same time because they write the same log and several shared BMP names. Their command-line parsing is implemented in each file’s `main()` function near the end; this project’s runners do not implement `--help`, and would otherwise interpret that word as an executable path. Generated images/logs live under `captures/`, which `.gitignore` excludes.

## 15. A careful reading route for a new programmer

Read in this order and try changing only one small thing at a time:

1. `src/core/Math.h`: vector fields, constructors, operators, normalization, and AABB ray test.
2. `src/ecs/Components.h`: transform and camera; trace how position/yaw/pitch become a view ray.
3. `src/ecs/Entity.h`, `EntityIndex.h`, `Scene.h`: stable IDs and scene ownership.
4. `src/ecs/OceanWave.h`: begin at `evaluateOceanWave()`, then impact, body displacement, support.
5. Matching HLSL helpers in `shaders/RayTracingPBR.hlsl`; compare each coefficient with the CPU helper.
6. `src/ecs/Scene.cpp`: follow asset loading first, then GPU buffer creation, then per-frame update and picking.
7. `src/rhi/D3D12Device.cpp`, `AccelerationStructure.cpp`, `RayTracingPipeline.cpp`: follow ownership from device to TLAS to dispatch.
8. `shaders/RayTracingPBR.hlsl`: start at `RayGenShader()`, then `TraceRay`, `ClosestHitShader`, `MissShader`.
9. `src/editor/EditorUI.cpp`, `src/main.cpp`: follow a UI setting to its constant buffer and next GPU frame.
10. Read tests alongside each function. A test is executable documentation of a case the author expects to keep working.

For each function, ask five questions: What are its inputs? What does it promise as output? Can any input be invalid? Does it divide, index, allocate, map, or launch GPU work? How does a test observe success or failure?

## 16. Review notes and important limits

- No test can establish that *every* possible floating-point input is safe. Tests sample mathematical identities and adversarial edge cases; code review must still inspect preconditions and error paths.
- DXR performance and image quality depend on GPU, driver, resolution, scene, and power limits. README numbers are measurements from one machine, not promises.
- Path-traced global illumination is materially slower than direct rendering. Denoising reduces visible noise but does not add missing samples or fix a biased estimator.
- Ocean displacement is an analytic height field. The collision proxy is not a finite fluid volume, and the engine does not solve Navier–Stokes fluid motion.
- The scene storage is a bounded ECS-like arrangement, not a fully general ECS.
- The picker uses bounds instead of a triangle-accurate selection structure.
- External library implementations are not rewritten here. Their licenses and notices remain with those source trees.

## Glossary

| Term | Plain-language meaning |
| --- | --- |
| ABI | Rules for how compiled functions pass arguments and values between binary modules. |
| AABB | Axis-aligned bounding box, a world-aligned box around geometry. |
| Acceleration structure | GPU-built spatial index that makes ray/triangle queries faster. |
| Accumulator | Stored running result, such as the average of many noisy samples. |
| API | A defined set of functions/types used to communicate with another system. |
| BLAS / TLAS | Bottom-level mesh index / top-level instance index for ray tracing. |
| BRDF | Formula describing how a surface reflects incoming light. |
| Buffer | A block of GPU or CPU memory containing typed records or raw bytes. |
| C ABI | A stable, plain-function interface usable across separately compiled DLLs. |
| Constant buffer | Small GPU-readable block of per-scene or per-frame values. |
| DXIL / DXC | DirectX shader bytecode / the compiler that creates it from HLSL. |
| DXR | DirectX Raytracing, the Windows GPU ray-tracing API. |
| ECS | Entity/component/system architecture for organizing game objects. |
| Fence | GPU/CPU synchronization counter used to know when submitted work completes. |
| Fresnel | Effect where reflective strength increases at grazing viewing angles. |
| GI | Global illumination: light that bounces between surfaces. |
| HLSL | High-Level Shading Language, the GPU programming language used here. |
| HDR | Color values with brightness range larger than the display’s final range. |
| Jolt | External rigid-body physics and collision library. |
| NRD / NRI | NVIDIA Real-time Denoisers / NVIDIA Rendering Interface. |
| PBR | Physically based rendering, a family of material/light models. |
| PDF | Probability density function that describes how a random sample was selected. |
| Reprojection | Finding where a current pixel’s surface was located in a prior frame. |
| sRGB | Display-oriented nonlinear encoding for ordinary color images. |
| TLAS instance | One use of a mesh BLAS with a transform and instance metadata. |
| VNDF | Visible-normal distribution function, a sampling method for glossy reflections. |

# Part II — Source-reading lesson cards

The cards below form a second, slower pass through the engine. Each four-line C++ sample is a **teaching miniature**, not a claim that the engine uses exactly those four lines. The “Source to inspect” line points to the engine implementation that demonstrates the same idea. Read the miniature, read each line note, then open the real source and find the corresponding data flow. Every example is deliberately small enough to reason about by hand.

## Part II: C++ and engine systems, step by step

## 1. Translation units: how C++ source becomes a program

Before Windows runs the engine, a preprocessor, compiler, and linker interpret different parts of the repository.

**Source to inspect:** CMakeLists.txt, src/main.cpp, and headers under src/.

Teaching miniature:
```cpp
#include "Thing.h"
int add(int a, int b) { return a + b; }
int main() { return add(2, 3); }
// The linker joins compiled object files.
```

### Read each line

1. Line 1 copies declarations from a header before the compiler checks normal C++ types.
2. Line 2 defines a function with an integer result and two integer inputs.
3. Line 3 is the entry function in this tiny example and calls add.
4. Line 4 is a comment; the real linker resolves function references across object files.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `translation unit` | C++ source and included headers after preprocessing |
| `declaration` | Promise that a name and type exist |
| `definition` | Implementation or storage that fulfills a declaration |
| `linker` | Build stage that joins object files and libraries |

### Rules to keep in mind

- A header declaration and source definition must agree on parameters, return type, and namespace.
- #include is handled before type checking; it does not execute at run time.
- CMake generates the build graph; MSBuild and the compiler perform compilation.

### Common mistakes

- An unresolved external usually means a definition was not linked, not that the header is missing.
- Repeated includes need #pragma once or guards to prevent duplicate declarations.
- Cached dependencies can make a build succeed on one computer and fail on another.

### Practice

- Find the CMake target containing src/main.cpp.
- Locate one header included by main.cpp and its matching definition.
- Identify one compiled source and the final executable in a build log.

### Check your understanding

- Why does preprocessing happen before C++ compilation?
- Why can many files include a class declaration without making duplicate objects?
- What does the linker do?

**Take-away:** The executable is the result of text transformations and linking.

## 2. Names, types, and declarations

A type tells the compiler how to interpret bits; a variable name gives those bits meaning in one scope.

**Source to inspect:** src/rhi/ConstantBuffers.h and src/ecs/Components.h.

Teaching miniature:
```cpp
uint32_t width = 1280;
float exposure = 1.0f;
const bool ready = true;
struct Size { uint32_t w, h; };
```

### Read each line

1. Line 1 creates an unsigned 32-bit integer and initializes it immediately.
2. Line 2 creates a floating-point value; f selects float rather than double.
3. Line 3 creates a Boolean that cannot be assigned a different value later.
4. Line 4 declares a new type with two named fields.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `uint32_t` | Fixed-width unsigned integer |
| `float` | Single-precision floating-point number |
| `bool` | Logical true or false |
| `const` | Rule preventing reassignment through this name |

### Rules to keep in mind

- A type controls arithmetic, overload selection, memory layout, and shader interpretation.
- Initialize local variables before reading them; an uninitialized scalar may be indeterminate.
- A const reference can observe an object without copying it or allowing edits.

### Common mistakes

- Mixing signed and unsigned integers can turn a negative value into a huge positive value.
- A decimal literal without f is generally double and may change overloads or precision.
- C++ and HLSL structures are not automatically layout-compatible.

### Practice

- Explain why uint32_t cannot be negative.
- Compare SceneCB in C++ and HLSL and match its fields.
- Find a float literal that should have an f suffix.

### Check your understanding

- Which line creates a new type?
- Which listed values represent negative numbers?
- Why is a field name insufficient to determine its byte size?

**Take-away:** Correct type choices turn intended meanings into compiler-enforced checks.

## 3. Initialization and safe defaults

Initialization gives an object valid starting state before another function can observe it.

**Source to inspect:** src/rhi/ConstantBuffers.h and src/ecs/Scene.cpp.

Teaching miniature:
```cpp
struct Range { float low = 0.0f;
 float high = 1.0f; };
Range r{};
if (r.low <= r.high) { /* usable */ }
```

### Read each line

1. The first line begins a structure declaration and gives low a default.
2. The second line gives high a default; the brace and semicolon finish the type.
3. Range r{} creates an object using its declared defaults and value-initialization.
4. The final condition checks that the interval is not inverted.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `default member initializer` | Value applied when an object is created without an override |
| `value initialization` | Syntax that zero-initializes applicable fields |
| `invariant` | Fact that should remain true when the object is used |
| `valid state` | State satisfying later assumptions |

### Rules to keep in mind

- Defaults matter because different scenes and tests initialize different subsets of SceneCB.
- A default of zero is not always safe: zero dimensions, divisors, and GPU addresses differ.
- Review every initialization path after adding a field.

### Common mistakes

- A missing initialization can cause unstable values when an object is not value-initialized.
- Changing a default can alter every scene relying on it.
- Clamping later can hide a missing initializer.

### Practice

- Inspect Scene::init, Scene::initOcean, and Scene::initReflectionLab.
- Choose a safe reflection strength default and explain why HLSL also clamps it.
- List each creation path that must set a new member.

### Check your understanding

- What should always be true about low and high?
- Why can scene-specific initialization be safer than depending on load order?
- What test could expose an absent default?

**Take-away:** Defaults are part of engine behavior and must be checked in every initialization path.

## 4. Expressions, precedence, and conversions

Expressions combine values and operators; explicit grouping and conversions make intent easier to audit.

**Source to inspect:** src/ecs/Scene.cpp, src/editor/ImGuiProjectionMath.h, and shaders.

Teaching miniature:
```cpp
float ratio = float(numerator) / denominator;
float squared = value * value;
bool valid = denominator != 0;
float bounded = std::clamp(ratio, 0.0f, 1.0f);
```

### Read each line

1. The first line converts before division so integer truncation cannot remove a fraction.
2. The second line multiplies value by itself; * is not an exponent operator.
3. The third line checks for a zero denominator and produces a Boolean.
4. The fourth line bounds ratio between inclusive limits.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `/` | Division; integer operands use integer division |
| `*` | Multiplication, or pointer syntax in a declaration |
| `!=` | Comparison true when operands differ |
| `std::clamp` | Standard-library interval bound function |

### Rules to keep in mind

- Multiplication has higher precedence than addition unless parentheses change grouping.
- A conversion can lose range or precision; float cannot exactly store all large integers.
- Check divisors and floating-point inputs for zero, NaN, and infinity where required.

### Common mistakes

- 1 / 2 with integer literals evaluates to zero before assignment to float.
- std::clamp needs a lower bound no greater than its upper bound.
- Parentheses make formulas easier to audit.

### Practice

- Calculate 5 / 2 and float(5) / 2 by hand.
- Rewrite a formula with named intermediates and units.
- Find a division in a math helper and identify its denominator check.

### Check your understanding

- Why can assigning integer division to float not recover a fraction?
- Why is NaN < 1 a poor validation test by itself?
- When could clamping hide an upstream bug?

**Take-away:** Readable, checked arithmetic prevents visual artifacts before they reach a shader.

## 5. Conditions and early-return guards

A guard rejects an invalid case close to detection, before that state causes a later failure.

**Source to inspect:** src/core/ImageCapture.h, src/editor/ImGuiProjectionMath.h, and src/rhi/.

Teaching miniature:
```cpp
bool project(float x, float w, float& out) {
 if (!std::isfinite(x) || w <= 0) return false;
 out = x / w;
 return std::isfinite(out);
}
```

### Read each line

1. Line 1 returns a success flag and receives output through a reference.
2. Line 2 uses logical OR: invalid x or nonpositive w rejects the input.
3. Line 3 divides only after validating the denominator and writes to out.
4. Line 4 reports whether the computed output is finite.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `if` | Conditionally run a statement |
| `||` | Logical OR with short-circuit evaluation |
| `!` | Logical NOT that reverses a Boolean |
| `output reference` | Parameter used to return extra data |

### Rules to keep in mind

- A guard should state a real precondition: finite values, valid dimensions, or non-null resource.
- Short-circuiting makes ptr && ptr->value safe when ptr is null.
- The caller must check failure and stop dependent work.

### Common mistakes

- Checking only after division is too late to avoid invalid intermediate values.
- A fallback zero can hide the difference between zero and failure.
- Logging an error does not prevent use of invalid state.

### Practice

- Find a guard and write one failing input and one passing input.
- Trace what the caller does when project returns false.
- Add an input check and exercise its rejection path.

### Check your understanding

- Why validate w before division?
- What does && short-circuit?
- How can you tell whether failure is propagated?

**Take-away:** A guard makes the contract explicit where the contract matters.

## 6. Loops, indices, and bounds

A loop repeats code while changing a counter; it needs a start, stop condition, and progress step.

**Source to inspect:** src/ecs/Scene.cpp, src/rhi/AccelerationStructure.cpp, and tests/.

Teaching miniature:
```cpp
for (size_t i = 0; i < items.size(); ++i) {
 const Item& item = items[i];
 process(item);
}
// i stops at size(), before indexing it.
```

### Read each line

1. Line 1 starts at zero, tests against element count, then increments after each pass.
2. Line 2 binds a read-only reference to the current element instead of copying it.
3. Line 3 processes the element; the closing brace ends the loop body.
4. The comment records the boundary: valid indices end at size minus one.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `size_t` | Unsigned type used for sizes and indices |
| `i` | Loop counter selecting one element |
| `items.size()` | Element count, not the last valid index |
| `++i` | Increment the counter by one |

### Rules to keep in mind

- For N elements, i < N visits exactly N indices when starting at zero.
- An empty container has size zero, so its loop body runs zero times.
- Changing the container during a loop can invalidate references and iterators.

### Common mistakes

- Using i <= items.size() indexes one past the final element.
- A negative signed index converted to size_t becomes a huge positive value.
- Erasing a vector item can shift later elements and invalidate references.

### Practice

- Trace the loop for zero, one, and three items.
- Find nested importer loops and estimate their complexity.
- Check whether a loop changes its own container.

### Check your understanding

- Which values are valid for a vector of length four?
- How many iterations run when the vector is empty?
- Why can reallocation invalidate a reference?

**Take-away:** Most index bugs are boundary mistakes that small hand-traced examples expose.
## 7. Functions and contracts

A function packages a task behind a named interface; parameters, return value, and side effects form its contract.

**Source to inspect:** src/ecs/Scene.h, src/ecs/Scene.cpp, and src/rhi/RayTracingPipeline.h.

Teaching miniature:
```cpp
bool loadScene(const Path& path) {
 if (path.empty()) return false;
 // Read, validate, install scene.
 return true;
}
```

### Read each line

1. Line 1 returns success or failure and reads the path through a const reference.
2. Line 2 rejects an empty path before file operations begin.
3. Line 3 is a comment; it has no run-time effect.
4. Line 4 reports success only after the work completes; the brace closes the function.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `return type` | Type sent back to caller |
| `parameter` | Named input listed in signature |
| `side effect` | Observable change such as writing or uploading |
| `const Path&` | Read-only reference that avoids copying |

### Rules to keep in mind

- The caller must check a bool result and stop work that depends on success.
- A function that mutates shared scene state should make that side effect clear.
- A void function still has a contract even though it returns no value.

### Common mistakes

- Returning true too early reports false success.
- A function combining import, physics, and UI work is difficult to isolate.
- A non-const reference parameter can be changed by the function.

### Practice

- Find a declaration in Scene.h and its definition in Scene.cpp.
- List a function's inputs, return value, and members it modifies.
- Trace its caller and record the failure response.

### Check your understanding

- What does a signature tell you before reading the function body?
- Why is bool better than silently continuing after allocation failure?
- What evidence shows the caller checked the result?

**Take-away:** A clear function contract makes behavior easier to test and failure easier to locate.

## 8. References, pointers, and null addresses

References name an existing object; pointers store an address that may be absent and must be checked before dereference.

**Source to inspect:** src/rhi/, src/ecs/Scene.cpp, and src/editor/.

Teaching miniature:
```cpp
void read(const Item& item) { /* observe */ }
Item* findItem(int id);
Item* p = findItem(7);
if (p != nullptr) read(*p);
```

### Read each line

1. Line 1 receives an existing item by const reference and prevents changes through this name.
2. Line 2 declares a function that may return an item's address or null.
3. Line 3 stores that address in pointer p.
4. Line 4 checks the address before *p accesses the object at that location.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `& in a type` | Reference declaration syntax |
| `* in a type` | Pointer declaration syntax |
| `nullptr` | Explicit value meaning no object address |
| `dereference` | Access the object at the stored address |

### Rules to keep in mind

- A reference must refer to an object; a pointer can represent absence.
- The target object must outlive every use of a raw pointer to it.
- ComPtr manages COM interface references but does not make every raw pointer safe.

### Common mistakes

- Dereferencing nullptr is undefined behavior and often crashes.
- Returning a pointer into a vector and then growing it can leave a dangling pointer.
- A non-null pointer may still point to an object whose lifetime ended.

### Practice

- Find Scene::getEntity and inspect its null case.
- Check how long a returned pointer's owner remains alive.
- Explain the difference between const T* and T* const.

### Check your understanding

- What must be true before *p is evaluated?
- Who owns the object returned by a lookup?
- What does a smart pointer help with, and what does it not guarantee?

**Take-away:** Pointer safety depends on both existence and lifetime; null checks alone do not prevent dangling pointers.

## 9. Scope, construction, and object lifetime

An object's lifetime begins at construction and ends at destruction; scope often makes cleanup automatic.

**Source to inspect:** src/rhi/D3D12Device.h, src/rhi/RayTracingPipeline.h, and src/ecs/Scene.h.

Teaching miniature:
```cpp
{
 std::vector<int> values{1, 2, 3};
 values.push_back(4);
} // values is destroyed here
```

### Read each line

1. Line 1 opens a smaller scope inside the surrounding function.
2. Line 2 constructs a vector and initializes it with three integers.
3. Line 3 appends an integer; the vector may allocate more memory.
4. Line 4 ends the scope, so C++ invokes the vector destructor.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `scope` | Region where a name can be used |
| `constructor` | Special function that initializes a new object |
| `destructor` | Special function that releases owned resources |
| `RAII` | Ownership follows object lifetime |

### Rules to keep in mind

- ComPtr releases its COM interface when destroyed.
- Members are destroyed in reverse declaration order after the containing destructor body.
- A raw pointer does not extend its target's lifetime.

### Common mistakes

- A pointer to a local becomes invalid when its scope ends.
- Destroying a GPU resource while submitted commands use it can be unsafe.
- A cycle of shared_ptr objects can keep them alive forever.

### Practice

- Find a ComPtr member and identify the containing object's lifetime.
- Sketch the destruction order for three local objects.
- Find a pointer returned from a function and ensure it is not local.

### Check your understanding

- What event destroys a local vector?
- Why does CPU lifetime alone not prove that GPU use has finished?
- Why can member declaration order matter?

**Take-away:** RAII automates cleanup, but asynchronous GPU work adds a second lifetime boundary.

## 10. Vectors, size, and capacity

A vector is a resizable contiguous sequence; size counts live elements while capacity describes reserved storage.

**Source to inspect:** src/ecs/Scene.cpp, src/ecs/EntityIndex.h, and importer code.

Teaching miniature:
```cpp
std::vector<int> ids;
ids.reserve(100);
ids.push_back(42);
size_t count = ids.size();
```

### Read each line

1. Line 1 creates an empty vector that owns a dynamic array of integers.
2. Line 2 requests capacity for 100 integers but does not create 100 live elements.
3. Line 3 appends one integer, making the size equal one.
4. Line 4 reads the number of live values.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `std::vector<T>` | Owning dynamic array of T objects |
| `size` | Count of currently constructed elements |
| `capacity` | Elements the current allocation can hold before growth |
| `reallocation` | Move or copy elements to a new allocation |

### Rules to keep in mind

- After reallocation, pointers, references, and iterators to elements may be invalid.
- reserve can reduce repeated allocations when a useful upper bound is known.
- The vector owns its elements; do not delete memory returned by data().

### Common mistakes

- reserve(100) does not make size() equal 100.
- operator[] outside size is invalid.
- Appending to m_entities can invalidate a SceneEntity reference.

### Practice

- Print size and capacity after each example operation.
- Find where entities are appended and check which references survive.
- Use at() in a debug example to observe a bounds exception.

### Check your understanding

- What does reserve guarantee, and what does it not create?
- When can an element reference become invalid?
- Why validate an index against size rather than capacity?

**Take-away:** Vector safety depends on understanding when its contiguous storage can move.

## 11. Results, errors, and propagation

An error is manageable when each layer checks the result and passes failure to a layer able to respond.

**Source to inspect:** src/main.cpp, src/rhi/D3D12Device.cpp, and src/core/ImageCapture.h.

Teaching miniature:
```cpp
bool upload() {
 HRESULT hr = createResource();
 if (FAILED(hr)) return false;
 return mapAndCopy();
}
```

### Read each line

1. Line 1 declares an operation whose caller receives a Boolean result.
2. Line 2 stores a Windows result instead of discarding it.
3. Line 3 exits before using a resource that failed to be created.
4. Line 4 passes the next operation's result to the caller.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `HRESULT` | Windows result code |
| `FAILED(hr)` | Macro testing whether an HRESULT represents failure |
| `error propagation` | Pass failure to the next layer |
| `diagnostic` | Information that identifies what failed and where |

### Rules to keep in mind

- A failure message should identify the operation and relevant resource or frame.
- An early return may need to release resources already allocated in earlier steps.
- The application boundary chooses whether to retry, disable a feature, or stop.

### Common mistakes

- Ignoring Map failure and then using its output pointer is unsafe.
- Logging an error and returning success misleads the caller.
- A generic initialization error is hard to diagnose without component context.

### Practice

- Find a checked D3D12 call and trace its failure to main.cpp.
- List operations that depend on a successful resource creation.
- Add context to a teaching error message.

### Check your understanding

- What does FAILED(hr) inspect?
- Why is logging not a substitute for returning failure?
- Which layer can decide whether to exit the application?

**Take-away:** Errors are part of function behavior, not a log-only afterthought.

## 12. Headers, namespaces, and include paths

Headers share declarations; namespaces group names so libraries can use similar short names safely.

**Source to inspect:** Headers under src/ and namespace Engine.

Teaching miniature:
```cpp
namespace Engine {
 struct Camera { float fov; };
}
Engine::Camera camera{1.0f};
```

### Read each line

1. Line 1 opens the Engine namespace.
2. Line 2 declares Camera inside that namespace.
3. Line 3 closes the namespace; it does not destroy the type.
4. Line 4 uses :: to name that exact Camera type.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `namespace` | Named compile-time scope for declarations |
| `Engine::Camera` | Camera type in the Engine namespace |
| `header` | File commonly sharing declarations |
| `include path` | Directory searched for a requested header |

### Rules to keep in mind

- A header should include what its own declarations need instead of relying on include order.
- Quoted includes commonly search local paths first; angle brackets commonly search system paths.
- Namespaces organize names at compile time and do not allocate objects.

### Common mistakes

- A missing include can appear to compile only because another header included it first.
- A using namespace directive in a widely included header pollutes callers.
- Same short names in separate namespaces can refer to different types.

### Practice

- Write the fully qualified name for Scene.
- List the types declared by one engine header.
- Remove an include in a tiny program and predict the compile error.

### Check your understanding

- What does :: select?
- Why should a header not rely on accidental include order?
- Does a namespace allocate run-time memory?

**Take-away:** Headers and namespaces define the language-level boundaries between engine modules.
## 13. Program entry and startup order

Startup builds dependencies in an order where each subsystem exists before a later subsystem uses it.

**Source to inspect:** src/main.cpp.

Teaching miniature:
```cpp
Window window;
if (!window.create(...)) return -1;
D3D12Device device;
if (!device.init(window.handle(), ...)) return -1;
```

### Read each line

1. Line 1 creates a window object whose lifetime spans startup and the main loop.
2. Line 2 checks its creation result; ! means return early on failure.
3. Line 3 declares the GPU device after the native window exists.
4. Line 4 passes the window handle to device setup, stopping if D3D12 initialization fails.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `WinMain` | Windows GUI program entry function |
| `HWND` | Opaque handle to a native window |
| `dependency` | Subsystem that must exist before another initializes |
| `startup` | Ordered creation of program systems |

### Rules to keep in mind

- The swap chain depends on a valid HWND because it presents into that window.
- Scene loading follows enough GPU setup to upload geometry and build acceleration structures.
- Each initialization call should have a clear failure result and diagnostic.

### Common mistakes

- Ignoring window creation failure passes an invalid handle to D3D12.
- A successful window does not prove that DXR, shaders, assets, Jolt, or NRD initialized.
- Constructing systems in the wrong order can leave dangling references.

### Practice

- Write startup order from window creation to the first captured image.
- Mark which steps create GPU resources.
- Choose an initialization error and trace the return path.

### Check your understanding

- Why does a swap chain need a window?
- When does scene data become GPU data?
- How does main.cpp know startup succeeded?

**Take-away:** Startup is a dependency graph expressed as checked sequential operations.

## 14. Frame loop and asynchronous work

A frame coordinates events, simulation, command recording, submission, and presentation while GPU execution remains asynchronous.

**Source to inspect:** src/main.cpp and src/rhi/D3D12Device.cpp.

Teaching miniature:
```cpp
while (running) {
 pollEvents();
 update(dt);
 render();
 present();
}
```

### Read each line

1. Line 1 repeats until the application decides to stop.
2. Line 2 handles window events such as input, resizing, and close.
3. Line 3 updates the scene before commands use its new state.
4. Lines 4 and 5 record or submit rendering, then present the finished swap-chain image.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `frame` | One iteration producing an image |
| `event pump` | Processing queued window-system events |
| `command recording` | Writing GPU commands to a command list |
| `present` | Make a swap-chain image available for display |

### Rules to keep in mind

- Recording commands does not mean the GPU has finished executing them.
- Simulation should use elapsed time rather than assuming a fixed frame rate.
- Vsync and presentation pacing can limit measured FPS independently of shader cost.

### Common mistakes

- An unhandled close event can leave a process running after its window closes.
- CPU timing around command recording is not automatically GPU execution time.
- Updating camera state after recording can make a frame respond one iteration late.

### Practice

- Find the frame index selecting the current swap-chain resource.
- Compare Scene::update, render, and present ordering in main.cpp.
- Explain how the CPU can prepare another frame while the GPU renders one.

### Check your understanding

- What event terminates the loop?
- Why does present not mean all GPU work is finished?
- What can cap FPS at monitor refresh?

**Take-away:** A frame loop coordinates several asynchronous timelines, not just drawing calls.

## 15. Time deltas and stable simulation

Elapsed time dt lets motion scale with seconds rather than the number of loop iterations.

**Source to inspect:** src/core/Timer.h, src/core/Timer.cpp, and src/main.cpp.

Teaching miniature:
```cpp
float dt = timer.tickSeconds();
if (!std::isfinite(dt)) dt = 0.0f;
dt = std::clamp(dt, 0.0f, 0.1f);
position += velocity * dt;
```

### Read each line

1. Line 1 reads elapsed seconds since the previous timer tick.
2. Line 2 replaces NaN or infinity with zero so invalid time does not contaminate state.
3. Line 3 limits a long pause so simulation cannot jump an enormous distance.
4. Line 4 applies displacement equals velocity times elapsed time.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `dt` | Elapsed time between updates, in seconds |
| `velocity` | Position change per second |
| `clamp` | Limit a value to an interval |
| `fixed substep` | Simulation step of a constant duration |

### Rules to keep in mind

- If velocity is meters per second and dt is seconds, their product is meters.
- Variable time steps can destabilize rigid-body simulation; fixed substeps help.
- A very small dt still requires care if code divides by dt.

### Common mistakes

- Moving one meter each frame changes speed when FPS changes.
- A huge dt can tunnel a body through collision surfaces.
- A negative or NaN dt can reverse or corrupt updates.

### Practice

- Calculate distance at 2 m/s for 0.25 seconds.
- Find the maximum dt accepted by the main loop and ocean update.
- Compare total movement at 30 and 120 FPS over one second.

### Check your understanding

- What units does dt use?
- Why is velocity times dt dimensionally consistent?
- Why can a fixed substep improve Jolt stability?

**Take-away:** Time needs units, a validated range, and a pause policy.

## 16. Input and camera movement

Input changes intent; the camera turns that intent into position and orientation before view rays are generated.

**Source to inspect:** src/core/Window.cpp, src/editor/EditorUI.cpp, and camera code in src/ecs/Scene.cpp.

Teaching miniature:
```cpp
if (keyDown('W')) move += forward;
if (keyDown('S')) move -= forward;
if (length(move) > 0)
 position += normalize(move) * speed * dt;
```

### Read each line

1. Line 1 adds the forward vector when the forward key is pressed.
2. Line 2 subtracts it for reverse movement.
3. Line 3 avoids normalizing an exactly zero direction.
4. Line 4 scales a unit direction by speed and elapsed time, then updates position.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `movement intent` | Direction assembled from input states |
| `forward` | Camera-facing direction vector |
| `normalize` | Divide a vector by its length |
| `camera position` | World-space origin of primary rays |

### Rules to keep in mind

- Normalizing diagonal motion avoids extra speed from combining perpendicular directions.
- A zero vector cannot safely be normalized without a fallback or length check.
- Input routing should avoid camera motion when the editor UI owns keyboard focus.

### Common mistakes

- Without normalization, diagonal movement is about sqrt(2) times faster.
- A nearly zero vector can still be unstable when the threshold is too small.
- Camera motion should invalidate temporal histories tied to the previous view.

### Practice

- Test forward, reverse, diagonal, and no-input cases.
- Trace how a mouse delta changes the camera basis.
- Find where camera movement resets accumulation.

### Check your understanding

- Why check length before normalization?
- How does multiplying by dt remove FPS dependence?
- Why should camera motion invalidate history?

**Take-away:** Input, camera basis, and elapsed time together define every primary ray.

## 17. Vectors, dot products, and coordinate spaces

A three-component vector may represent a point, direction, color, or normal; its meaning comes from usage and coordinate space.

**Source to inspect:** src/core/Math.h, src/ecs/OceanWave.h, and HLSL helpers.

Teaching miniature:
```cpp
float3 a = float3(1, 0, 0);
float3 b = float3(0, 1, 0);
float d = dot(a, b);
float3 c = cross(a, b);
```

### Read each line

1. Line 1 points along X; line 2 points along Y.
2. Line 3 computes the dot product, which is zero for perpendicular unit vectors.
3. Line 4 computes a perpendicular vector, positive Z in this right-handed example.
4. The operand order of cross product affects the resulting direction.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `point` | Location relative to a coordinate origin |
| `direction` | Orientation and magnitude, not location |
| `dot product` | Pairwise products added together; measures alignment |
| `cross product` | Perpendicular vector whose direction follows handedness |

### Rules to keep in mind

- For unit vectors, dot(a,b) equals cosine of their angle.
- World, object, view, and tangent-space vectors must not be mixed casually.
- A normalized normal makes lighting dot products represent cosines.

### Common mistakes

- Using a point where a direction is expected adds an origin offset.
- A non-unit normal changes lighting magnitude.
- Reversing cross operands flips the result.

### Practice

- Calculate dot((1,0,0),(0,1,0)) and cross((1,0,0),(0,1,0)).
- Label vectors in one lighting equation by coordinate space.
- Find a dot product used as NdotV and explain its clamp.

### Check your understanding

- What does a dot product of one mean for unit vectors?
- Why is a normal usually normalized?
- How can code distinguish a point from a direction?

**Take-away:** Vector algebra is simple only after each quantity and coordinate space are named.

## 18. Matrices, points, and transforms

A matrix transforms coordinates; multiplication order depends on the engine's row/column vector convention.

**Source to inspect:** src/core/Math.h, src/ecs/Components.h, and src/ecs/Scene.cpp.

Teaching miniature:
```cpp
float4 p = float4(local, 1.0f);
float4 world = mul(objectToWorld, p);
float4 clip = mul(viewProjection, world);
float3 ndc = clip.xyz / clip.w;
```

### Read each line

1. Line 1 adds homogeneous w=1 so p is a point affected by translation.
2. Line 2 transforms object coordinates into world space.
3. Line 3 applies the view and projection transformation.
4. Line 4 divides by clip w to produce normalized device coordinates.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `homogeneous point` | Four-component location with w equal to one |
| `homogeneous direction` | Four-component vector with w equal to zero |
| `clip space` | Coordinates before perspective divide |
| `NDC` | Normalized device coordinates after division |

### Rules to keep in mind

- Matrix storage order and multiplication order are separate conventions.
- Perspective division needs nonzero suitable clip.w.
- CPU XMFLOAT4X4 and HLSL float4x4 must agree on transpose convention.

### Common mistakes

- Applying translation to a direction with w=1 incorrectly moves it.
- Swapping matrix order changes the result because matrix multiplication is not commutative.
- A point behind the camera may have invalid or negative clip w.

### Practice

- Transform a local origin by a translation matrix.
- Trace matrix order in the ray-generation shader.
- Compare CPU and HLSL conventions around mul calls.

### Check your understanding

- Why does w distinguish points from directions?
- What does perspective division calculate?
- Why can matrix multiplication order not be rearranged?

**Take-away:** Transform math requires coordinate-space labels and one consistent convention.

## 19. Bounding boxes and ray picking

A ray-box test selects likely objects quickly by checking a coarse volume before expensive geometry work.

**Source to inspect:** src/core/Math.h and Scene::rayPick in src/ecs/Scene.cpp.

Teaching miniature:
```cpp
for (int axis = 0; axis < 3; ++axis) {
 float t0 = (lo[axis] - origin[axis]) / dir[axis];
 float t1 = (hi[axis] - origin[axis]) / dir[axis];
 swapIfNeeded(t0, t1);
}
```

### Read each line

1. Line 1 examines X, Y, and Z intervals in turn.
2. Line 2 calculates where the ray crosses the low plane for one axis.
3. Line 3 calculates where it crosses the high plane.
4. Line 4 orders the values so entry precedes exit even if the ray points in the negative direction.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `ray origin` | Starting point of a parametric ray |
| `ray direction` | Direction multiplied by parameter t |
| `AABB` | Axis-aligned box described by low and high corners |
| `slab interval` | Ray-parameter range inside one pair of box planes |

### Rules to keep in mind

- A ray point is origin + t times direction; t is distance if direction is normalized.
- A hit requires a common nonempty interval across all three axes.
- A near-zero direction component needs an explicit parallel-ray case.

### Common mistakes

- Dividing by zero can produce infinity or NaN.
- An unconstrained t may select geometry behind the camera.
- AABB picking can select empty space inside a large box.

### Practice

- Draw the X interval for a ray pointing in positive X.
- Test a ray parallel to a slab and one inside a box.
- Compare approximate bounds selection with triangle-accurate selection.

### Check your understanding

- What does parameter t represent?
- Why sort entry and exit?
- Why does a box hit not prove a triangle hit?

**Take-away:** Coarse picking trades precision for speed and still needs exact interval rules.
## 20. Reading glTF accessor bytes

A glTF accessor describes typed elements in a binary buffer; offsets, strides, and lengths must be validated before reading.

**Source to inspect:** src/ecs/GltfImporter.cpp and importer tests.

Teaching miniature:
```cpp
size_t address = viewOffset + offset;
if (index >= count) return false;
if (address + size > bytes.size()) return false;
readElement(bytes.data() + address);
```

### Read each line

1. Line 1 combines the buffer-view and accessor offsets.
2. Line 2 rejects an element number beyond the accessor's logical count.
3. Line 3 checks that the requested bytes are inside the file buffer.
4. Line 4 passes the validated address to a type-aware decoder.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `buffer view` | Byte range containing accessors |
| `accessor` | Typed element count, offset, and optional stride description |
| `stride` | Byte distance between neighboring elements |
| `element size` | Bytes needed for one decoded element |

### Rules to keep in mind

- Offsets and lengths come from external files and may overflow during addition or multiplication.
- A stride may exceed packed element size but still must fit the buffer.
- Component type and normalized metadata determine how bytes become numbers.

### Common mistakes

- Checking index<count does not prove the required bytes exist.
- Multiplying an untrusted count by element size before overflow checks can wrap.
- Reinterpreting unaligned bytes as a C++ struct can violate alignment rules.

### Practice

- Trace a position accessor from file metadata to Vertex.position.
- Compute offsets for three 12-byte values with a 16-byte stride.
- Run importer tests for truncated buffers and invalid accessors.

### Check your understanding

- What is the difference between count and byte length?
- Why check multiplication overflow before allocating?
- What does normalized integer data mean?

**Take-away:** Asset parsing is decoding and validation because files are external input.

## 21. Textures, materials, and color spaces

Color textures and numeric data textures must be decoded differently before physically based lighting uses them.

**Source to inspect:** src/ecs/GltfImporter.cpp, src/ecs/Scene.cpp, and material shader code.

Teaching miniature:
```cpp
float3 albedo = sampleColorTexture(uv);
float3 normalData = sampleDataTexture(uv);
float3 tangentNormal = normalData * 2.0f - 1.0f;
float3 baseColor = albedo * factor;
```

### Read each line

1. Line 1 samples color and converts an sRGB image to linear light.
2. Line 2 samples numeric data such as normal-map channels without sRGB decoding.
3. Line 3 remaps channels from zero-to-one into signed vector components.
4. Line 4 multiplies sampled color by the material base-color factor.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `UV` | 2D texture coordinates |
| `sRGB` | Nonlinear encoding for display-oriented color |
| `linear color` | Values proportional to light intensity for rendering math |
| `normal map` | Texture storing surface direction rather than a photograph |

### Rules to keep in mind

- Lighting and blending math should operate on linear values.
- Normal maps require a tangent basis before they become world-space normals.
- Base-color alpha may represent mask behavior rather than ordinary transparency.

### Common mistakes

- Treating sRGB albedo as linear makes midtones too dark.
- Treating roughness as sRGB changes its numeric value.
- A missing texture index must not index outside the descriptor array.

### Practice

- Compare approximate linear values for sRGB 0.5 and linear 0.5.
- Find how importer distinguishes color and data texture usage.
- Inspect the shader's material texture ID checks.

### Check your understanding

- Why is roughness stored as linear data?
- What does remapping normal RGB to [-1,1] accomplish?
- Why can one bitmap format need different sampling modes?

**Take-away:** A material channel is meaningful only when its encoding and physical role are correct.

## 22. Transforming positions and normals

Positions use the object transform; normals use inverse transpose so nonuniform scale preserves perpendicular surface directions.

**Source to inspect:** src/ecs/GltfImporter.cpp, shaders/RayTracingPBR.hlsl, and math tests.

Teaching miniature:
```cpp
float3 pW = mul(objectToWorld, float4(p, 1)).xyz;
float3x3 nToW = transpose(inverse(linearPart));
float3 nW = mul(nToW, nLocal);
nW = normalize(nW);
```

### Read each line

1. Line 1 treats p as a point using homogeneous w=1 and extracts transformed xyz.
2. Line 2 computes the inverse-transpose of the transform's rotation-and-scale part.
3. Line 3 applies that matrix to the local surface normal.
4. Line 4 restores unit length for dot-product lighting.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `object-to-world` | Matrix placing mesh-local coordinates in the scene |
| `linear part` | Rotation and scale without translation |
| `inverse transpose` | Transform preserving normal/tangent perpendicularity |
| `unit normal` | Normal vector with length one |

### Rules to keep in mind

- For transformed tangent t'=M*t, the correct normal satisfies n' dot t'=0.
- The inverse fails for singular transforms such as zero scale.
- Normals do not receive translation because it does not change directions.

### Common mistakes

- Using the point matrix for normals gives wrong lighting under nonuniform scale.
- Skipping normalization changes BRDF strength and bias calculations.
- Using wrong row/column conventions effectively transposes the intended result.

### Practice

- Apply scale (2,1,0.5) and check a transformed normal remains perpendicular.
- Find ObjectToWorld3x4 and WorldToObject4x3 in the hit shader.
- Run importer math tests for nonuniform scale.

### Check your understanding

- What equation proves perpendicularity is retained?
- Why is translation excluded?
- What should happen if a matrix inverse cannot be computed?

**Take-away:** Normal transforms follow geometry constraints, not guesswork.

## 23. Scene data and GPU uploads

The scene owns editable CPU data and uploads consistent copies into GPU resources used by ray tracing.

**Source to inspect:** src/ecs/Scene.cpp, especially uploadGpuScene and updateGpuBuffers.

Teaching miniature:
```cpp
if (transformsDirty) {
 uploadInstanceMatrices();
 rebuildTopLevelAS();
 transformsDirty = false;
}
```

### Read each line

1. Line 1 checks whether cached GPU instance data is stale.
2. Line 2 uploads changed transforms into a GPU-visible buffer.
3. Line 3 rebuilds or updates the TLAS so rays use the new placements.
4. Line 4 clears the dirty flag only after successful dependent work.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `CPU state` | Authoritative values in system memory |
| `GPU snapshot` | Uploaded values commands will read |
| `dirty flag` | Marker that a cached value no longer matches its source |
| `TLAS` | Top-level structure over transformed instances |

### Rules to keep in mind

- A transform edit affects instance matrices and may require TLAS update.
- A material edit changes material data but does not necessarily require a geometry rebuild.
- Resources must remain alive until submitted commands finish reading them.

### Common mistakes

- Clearing dirty before a failed upload loses the pending change.
- Uploading before CPU update sends the previous transform for a frame.
- Writing a resource still read by the GPU may require fences or per-frame copies.

### Practice

- Trace one inspector transform edit through updateGpuBuffers.
- Find where transformsDirty is set and cleared.
- List which GPU buffers contain vertices, indices, materials, and instances.

### Check your understanding

- What does a dirty flag mean?
- Why can frame buffers contain different snapshots?
- When is a CPU write safe for a GPU-read resource?

**Take-away:** Scene synchronization makes editable CPU state visible to the ray-tracing GPU.

## 24. Building BLAS and TLAS

DXR indexes a mesh in a BLAS, then indexes transformed instances in a TLAS for scene-wide ray queries.

**Source to inspect:** src/rhi/AccelerationStructure.cpp and src/rhi/AccelerationStructure.h.

Teaching miniature:
```cpp
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
in.Type = BOTTOM_LEVEL;
in.NumDescs = geometryCount;
BuildRaytracingAccelerationStructure(&in);
```

### Read each line

1. Line 1 initializes the descriptor that tells D3D12 what to build.
2. Line 2 declares a bottom-level geometry build.
3. Line 3 gives the number of mesh geometry descriptions.
4. Line 4 records the command; GPU execution happens after submission.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `BLAS` | Bottom-level acceleration structure over triangles |
| `TLAS` | Top-level structure over instances and transforms |
| `instance` | One mesh use with transform and metadata |
| `scratch buffer` | Temporary GPU memory used during a build |

### Rules to keep in mind

- A BLAS usually changes when geometry changes; TLAS changes when instance transforms change.
- GPU addresses and build sizes must meet D3D12 requirements.
- Build resources must stay valid until asynchronous execution finishes.

### Common mistakes

- A stale GPU address makes the build description invalid.
- Rebuilding every static BLAS each frame wastes work.
- Releasing scratch before build completion is unsafe.

### Practice

- Trace a mesh vertex buffer to its BLAS input address.
- Find the transform copied into the TLAS instance descriptor.
- Identify a barrier ordering build and trace commands.

### Check your understanding

- Which structure contains triangle geometry?
- Which structure changes when moving an object?
- Why does recording a build not mean it already finished?

**Take-away:** BLAS/TLAS separate stable geometry from movable placements.

## 25. Constant-buffer binary layout

CPU and HLSL constant-buffer declarations are a binary contract: matching fields must occupy matching byte offsets.

**Source to inspect:** src/rhi/ConstantBuffers.h and shaders/RayTracingPBR.hlsl.

Teaching miniature:
```cpp
struct ExampleCB {
 XMFLOAT3 camera;
 float time;
};
static_assert(sizeof(ExampleCB) == 16);
```

### Read each line

1. Line 1 begins the CPU structure copied to the GPU.
2. Line 2 stores three 32-bit floats for position.
3. Line 3 uses the remaining four bytes in the 16-byte block for time.
4. Line 4 asks the compiler to reject an unexpected layout size.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `constant buffer` | Small read-mostly block of shader values |
| `member offset` | Byte position of a field |
| `register boundary` | Packing boundary used by HLSL |
| `static_assert` | Compile-time condition checked by C++ |

### Rules to keep in mind

- Change CPU and HLSL field order together whenever the layout changes.
- A float3 plus scalar can share a 16-byte register when both sides agree.
- D3D12 CBV allocation sizes align to 256 bytes even if the structure is smaller.

### Common mistakes

- Adding a field to one side shifts later shader inputs silently.
- C++ bool and HLSL bool need not have matching byte layout.
- Allocation stride and copied payload size are separate values.

### Practice

- Compare every SceneCB CPU field with the HLSL declaration.
- Check sizeof(SceneCB) and important offsetof values after an edit.
- Build shaders and render a scene using a newly added field.

### Check your understanding

- Why can layout mismatch compile yet shade incorrectly?
- What does static_assert prevent?
- Why pad the CBV allocation separately from structure size?

**Take-away:** CPU-to-shader structures are an ABI and should be reviewed like a binary format.

## 26. Command recording and ray dispatch

The CPU binds resources and records dispatch dimensions; each GPU thread then creates one primary ray for its pixel.

**Source to inspect:** src/rhi/RayTracingPipeline.cpp and src/main.cpp.

Teaching miniature:
```cpp
D3D12_DISPATCH_RAYS_DESC d{};
d.Width = width;
d.Height = height;
d.Depth = 1;
cmdList->DispatchRays(&d);
```

### Read each line

1. Line 1 initializes the descriptor that defines the ray-generation launch.
2. Line 2 sets horizontal thread count, normally matching render pixels.
3. Line 3 sets vertical thread count.
4. Line 4 launches one 2D layer of GPU work.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `dispatch` | Launch of shader work over a dimension |
| `width and height` | Number of ray-generation indices |
| `thread index` | GPU invocation's coordinate |
| `root table` | Bound resource descriptors visible to a shader |

### Rules to keep in mind

- Every dispatch dimension must match valid output and accumulation texture bounds.
- Root descriptors must be bound before the dispatch command.
- GPU resources need correct states and lifetime through execution.

### Common mistakes

- Zero dimensions may produce no output and look like a frozen viewport.
- Dispatching beyond output bounds causes shader out-of-range access unless guarded.
- A missing descriptor can bind an unrelated or null GPU resource.

### Practice

- Compare viewport dimensions to dispatch dimensions.
- Trace the output UAV binding through descriptor setup.
- Inspect the shader guard against width and height.

### Check your understanding

- How many primary invocations occur for width W and height H?
- Why does the dispatch need depth one?
- Who executes the shader after command recording?

**Take-away:** Dispatch describes parallel work; it does not execute on the CPU.
## 27. Primary rays and pixel coordinates

The ray-generation shader maps each pixel to a direction through the camera, then traces from the camera position.

**Source to inspect:** shaders/RayTracingPBR.hlsl.

Teaching miniature:
```cpp
float2 ndc = pixelToNdc(pixel, size);
float3 rayDir = normalize(forward + right * ndc.x + up * ndc.y);
Ray ray{cameraPosition, rayDir};
traceScene(ray, payload);
```

### Read each line

1. Line 1 maps a pixel coordinate into a normalized camera-plane coordinate.
2. Line 2 combines camera basis vectors and normalizes the result.
3. Line 3 creates a ray starting at the camera.
4. Line 4 asks DXR to find the closest relevant hit or miss.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `pixel coordinate` | Integer location in the render target |
| `NDC` | Normalized device coordinate near the view plane |
| `camera basis` | Forward, right, and up directions |
| `Ray` | Origin and direction defining a parametric line |

### Rules to keep in mind

- A ray point is origin plus t times direction.
- Aspect ratio scales horizontal view extent relative to vertical extent.
- Normalizing direction makes hit distance and ray limits easier to interpret.

### Common mistakes

- Using viewport instead of render resolution distorts the ray mapping.
- A zero or invalid camera basis can create NaN directions.
- Forgetting aspect ratio stretches the image horizontally.

### Practice

- Trace corner and center pixel coordinates through pixelToNdc.
- Check how FOV changes the ray direction.
- Compare camera basis with mouse-look updates.

### Check your understanding

- Which pixel should point nearly forward?
- Why normalize rayDir?
- What does aspect ratio change?

**Take-away:** Primary-ray math maps the screen grid into the camera's 3D viewing cone.

## 28. DXR payloads and shader communication

The payload is a small data record carried by a ray and filled by the miss or closest-hit shader.

**Source to inspect:** shaders/RayTracingPBR.hlsl and src/rhi/RayTracingPipeline.cpp.

Teaching miniature:
```cpp
struct Payload { float3 color; float hitT; };
Payload p{};
TraceRay(scene, flags, mask, ray, p);
writePixel(pixel, p.color);
```

### Read each line

1. Line 1 defines the values that can travel with this teaching ray.
2. Line 2 initializes the outgoing payload to known values.
3. Line 3 launches ray traversal and lets hit/miss shaders update p.
4. Line 4 uses the returned color after traversal finishes.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `payload` | User data passed through ray traversal |
| `color` | Radiance or shaded result returned for a hit |
| `hitT` | Ray parameter at intersection, or sentinel for a miss |
| `sentinel` | Special value representing a distinct state |

### Rules to keep in mind

- The C++ root signature, HLSL payload declaration, and shader export must agree on size and semantics.
- Initialize every payload member before TraceRay reads or modifies it.
- A miss and a hit should set enough state for the caller to distinguish them.

### Common mistakes

- Leaving fields uninitialized can make a miss appear as random light.
- An oversized payload increases traversal state and can cost performance.
- Using one sentinel inconsistently makes later control flow ambiguous.

### Practice

- Find RayPayload and list every field written by each shader stage.
- Trace a primary miss and a reflected-ray hit separately.
- Check payload initialization before every TraceRay call.

### Check your understanding

- Where does the payload get filled?
- Why initialize the payload before a ray?
- How does the caller know whether a hit occurred?

**Take-away:** The payload is the explicit communication channel between ray stages.

## 29. Closest-hit geometry and barycentric coordinates

DXR reports which triangle was hit and barycentric weights; the shader uses them to interpolate per-vertex data.

**Source to inspect:** shaders/RayTracingPBR.hlsl.

Teaching miniature:
```cpp
float w = 1.0f - bary.x - bary.y;
float3 p = p0 * w + p1 * bary.x + p2 * bary.y;
float3 n = normalize(n0 * w + n1 * bary.x + n2 * bary.y);
shade(p, n);
```

### Read each line

1. Line 1 computes the third triangle weight from the first two.
2. Line 2 forms a weighted average of the three vertex positions.
3. Line 3 applies the same weights to vertex normals and normalizes the result.
4. Line 4 shades the interpolated surface point.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `triangle index` | Index selecting one triangle's three vertices |
| `barycentric weights` | Three coefficients expressing location inside a triangle |
| `interpolation` | Weighted combination of vertex attributes |
| `closest hit` | Nearest intersection accepted for a ray |

### Rules to keep in mind

- For an interior point, weights are nonnegative and sum to one within tolerance.
- The hit distance and barycentrics are supplied by DXR traversal.
- Normals transform with inverse transpose, while positions use object-to-world.

### Common mistakes

- Using the wrong triangle indices reads unrelated vertices.
- Incorrect barycentric ordering smears textures and normals.
- Normalizing a zero interpolated normal needs a fallback.

### Practice

- Check that three barycentric weights sum to one.
- Walk from primitive index to index buffer to vertex buffer.
- Inspect ObjectToWorld3x4 for positions and WorldToObject4x3 for normals.

### Check your understanding

- What do the barycentric weights sum to?
- How does the shader find the triangle's vertices?
- Why do position and normal transforms differ?

**Take-away:** A closest hit combines acceleration-structure results with interpolated scene attributes.

## 30. Fresnel, reflection, and energy balance

Fresnel estimates the reflected portion of light; blending uses a bounded weight so reflected and transmitted/base responses do not exceed a unit partition.

**Source to inspect:** shaders/RayTracingPBR.hlsl.

Teaching miniature:
```cpp
float f0 = 0.02f;
float fresnel = f0 + (1.0f - f0) * pow(1.0f - nDotV, 5.0f);
float weight = saturate(fresnel * artStrength);
float3 color = lerp(deepWater, reflectedLight, weight);
```

### Read each line

1. Line 1 sets approximate normal-incidence reflectance for a dielectric water interface.
2. Line 2 applies Schlick's fifth-power approximation as view angle becomes grazing.
3. Line 3 applies a user art-direction multiplier and clamps the result to [0,1].
4. Line 4 partitions the output between water body and reflected radiance.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `F0` | Reflectance at normal incidence |
| `NdotV` | Cosine between normalized surface normal and view direction |
| `saturate` | Clamp to zero through one |
| `lerp` | Linear interpolation between two endpoints |

### Rules to keep in mind

- Schlick's formula approaches F0 at normal view and one at grazing view.
- The art multiplier changes effective reflectance but does not claim exact water IOR.
- Clamping ensures the blend weight stays in [0,1], keeping this mix bounded.

### Common mistakes

- Adding the reflected term on top of full deep-water color double-counts energy.
- A factor above one is art direction, not physical Fresnel data.
- A normal pointing away from the viewer can create an incorrect grazing response.

### Practice

- Evaluate Fresnel at NdotV=1 and NdotV=0.
- Move the ocean slider and compare fixed on/off captures.
- Keep the physical F0 distinct from the art multiplier in documentation.

### Check your understanding

- What value does Fresnel approach at normal incidence?
- Why clamp after applying art strength?
- Does a 4x multiplier represent pure water's measured refractive index?

**Take-away:** A controllable stylized blend can be bounded and explicit without mislabeling its physical meaning.

## 31. Path-tracing averages, variance, and denoising

A path tracer estimates light with random samples; averaging reduces noise but costs rays, while denoisers trade some detail for smoother images.

**Source to inspect:** shaders/RayTracingPBR.hlsl and NRD integration in src/rhi/.

Teaching miniature:
```cpp
float3 sum = 0.0f;
for (uint i = 0; i < sampleCount; ++i)
 sum += tracePath(randomRay(i));
float3 estimate = sum / float(sampleCount);
```

### Read each line

1. Line 1 starts an accumulated radiance sum at zero.
2. Line 2 repeats the path estimate for each sample.
3. Line 3 adds one independent path result into the sum.
4. Line 4 divides by sample count to produce the Monte Carlo average.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `sample` | One randomized path estimate |
| `variance` | Spread of estimates around the expected value |
| `accumulation` | Running average over samples or frames |
| `denoiser` | Filter using temporal/spatial evidence to reduce visible variance |

### Rules to keep in mind

- For independent samples, standard error usually falls proportionally to 1/sqrt(N).
- A denoiser reduces visible noise but cannot create missing unbiased samples.
- Temporal history needs motion, depth, and stable sampling information to avoid ghosting.

### Common mistakes

- Dividing by zero samples is invalid.
- Reusing correlated random values can leave structured patterns.
- Blurring too strongly removes thin geometry and high-frequency texture detail.

### Practice

- Compare images at sample counts 1, 4, 16, and 64.
- Inspect accumulation reset when the camera or material changes.
- Compare built-in temporal filtering with NRD enabled and disabled.

### Check your understanding

- Why does quadrupling samples not usually cut noise in half?
- Can a denoiser recover detail never sampled?
- Why reset accumulation after a scene change?

**Take-away:** Sampling, variance, and temporal reuse are separate parts of the path-tracing quality problem.

## 32. Ocean waves, Jolt buoyancy, and water displacement

The demo couples rigid-body motion to an analytic water height field and produces shader ripples from body impacts; it is not a full fluid solver.

**Source to inspect:** src/ecs/OceanWave.h, src/ecs/Scene.cpp, and tools/jolt_bridge.cpp.

Teaching miniature:
```cpp
float height = baseHeight(x, z, time);
float depth = height - bodyBottom;
float buoyancy = density * gravity * submergedVolume;
applyForce(body, buoyancy);
```

### Read each line

1. Line 1 evaluates the base ocean surface at a world position and time.
2. Line 2 estimates how far the body extends below that surface.
3. Line 3 applies Archimedes' displacement principle using submerged volume.
4. Line 4 sends the upward force into the rigid-body world.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `height field` | Function returning water height for horizontal coordinates and time |
| `submerged volume` | Body volume below the water surface |
| `buoyancy` | Upward force balancing displaced fluid weight |
| `wake` | Surface disturbance attached to body motion or impact |

### Rules to keep in mind

- Buoyancy is approximately displaced-fluid density times gravity times submerged volume.
- The renderer evaluates analytic wave height and normals at pixel positions.
- Impact rings use event position, time, speed, and decay; they are a visual wave model.

### Common mistakes

- Applying buoyancy without tracking immersion can make objects float at a fixed line.
- Using a different surface equation for rendering and buoyancy makes cubes appear detached.
- An unbounded impact queue or bad time can create persistent or exploding waves.

### Practice

- Trace Jolt body position into water sampling and back to force integration.
- Compare water around a floating body with body displacement disabled.
- Check that impact energy decays with age and distance.

### Check your understanding

- What physical quantity does buoyancy depend on?
- Why must the shader and physics query the same wave model?
- Is this solver simulating Navier-Stokes fluid flow?

**Take-away:** The current scene couples Jolt rigid bodies to a shared analytic height model, not volumetric fluid dynamics.

## 33. Captures, A/B tests, and repeatability

A capture test compares controlled runs and preserves images; image evidence and timing evidence should be reported separately.

**Source to inspect:** ocean_capture_runner.py, reflection_capture_runner.py, and capture_runner.py.

Teaching miniature:
```cpp
on = run_engine(reflections=True);
off = run_engine(reflections=False);
delta = compareWaterPixels(on, off);
assert delta.mean > minimum;
```

### Read each line

1. Line 1 launches a controlled render with the feature enabled.
2. Line 2 launches the comparison render with the feature disabled.
3. Line 3 measures differences only within the water region identified by the test.
4. Line 4 fails the test if the feature makes no measurable image change.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `A/B comparison` | Two runs differing by one chosen setting |
| `ROI` | Region of interest selected for image analysis |
| `mean delta` | Average absolute channel difference over sampled pixels |
| `repeatability` | Ability to reproduce the same result under controlled conditions |

### Rules to keep in mind

- Keep scene, resolution, camera, build, and simulation time the same between variants.
- Measure GPU time with GPU timing tools; process wall time is not shader time.
- A threshold detects absent contribution, not whether reflections look beautiful.

### Common mistakes

- Different random seeds or body positions can change the image mask between runs.
- Counting pixels above a threshold can understate broad subtle differences.
- A one-run FPS sample is noisy and depends on clock, temperature, and window state.

### Practice

- Run the water toggle test at 2.5x and 4x and preserve all outputs.
- Inspect the difference image near the reflected props.
- Repeat one mode to estimate natural capture variation.

### Check your understanding

- What does an A/B test isolate?
- Why report mean image delta and changed-pixel ratio together?
- Why should FPS be measured separately from screenshot wall time?

**Take-away:** Good rendering evidence combines visual inspection, image metrics, repeatability, and honest performance limits.
## Part III: Focused rendering and simulation follow-up

## 34. Pixel rays, camera basis, and ray generation

The ray-generation program maps one GPU invocation to one pixel and traces a camera ray through that pixel.

**Source to inspect:** shaders/RayTracingPBR.hlsl.

Teaching miniature:
```cpp
float2 ndc = pixelToNdc(pixel, size);
float3 dir = normalize(forward + right * ndc.x + up * ndc.y);
Ray ray{cameraPosition, dir};
TraceRay(scene, ray, payload);
```

### Read each line

1. Line 1 maps pixel coordinates to a camera-plane coordinate.
2. Line 2 combines the camera basis and normalizes the viewing direction.
3. Line 3 starts a ray at cameraPosition.
4. Line 4 asks DXR to traverse the scene and fill payload data.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `pixel coordinate` | Integer location in render target |
| `NDC` | Normalized device coordinate on the view plane |
| `camera basis` | Forward, right, and up directions |
| `ray` | Origin plus direction used to query the scene |

### Rules to keep in mind

- The aspect ratio scales horizontal field of view relative to vertical field of view.
- A normalized direction makes t behave like distance in scene units.
- The camera basis vectors should be orthogonal and finite.

### Common mistakes

- Using UI viewport size instead of render-target dimensions distorts ray directions.
- Forgetting aspect ratio stretches the image horizontally.
- A degenerate camera basis can produce invalid rays.

### Practice

- Trace the center and corner pixels through pixelToNdc.
- Check how FOV changes the direction.
- Compare the camera basis with the mouse-look update.

### Check your understanding

- Which pixel should point almost forward?
- Why normalize dir?
- What does aspect ratio change?

**Take-away:** Primary-ray math maps a screen grid into the camera's three-dimensional view cone.

## 35. DXR payload and closest-hit interpolation

The payload carries values through traversal; a triangle hit supplies an index and barycentric weights used to interpolate scene data.

**Source to inspect:** shaders/RayTracingPBR.hlsl and src/rhi/RayTracingPipeline.cpp.

Teaching miniature:
```cpp
struct Payload { float3 color; float hitT; };
Payload p{};
float w = 1.0f - bary.x - bary.y;
float3 pos = p0*w + p1*bary.x + p2*bary.y;
```

### Read each line

1. Line 1 declares two pieces of information carried by a teaching ray.
2. Line 2 initializes the outgoing payload to known values.
3. Line 3 calculates the third barycentric triangle weight.
4. Line 4 interpolates the triangle's vertex positions at the hit location.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `payload` | User data carried by a ray |
| `hitT` | Ray parameter at intersection or a miss sentinel |
| `barycentric weights` | Three coefficients locating a point in a triangle |
| `closest hit` | Nearest accepted surface intersection |

### Rules to keep in mind

- C++ root signature and HLSL payload declaration must agree on size and use.
- Interior barycentric weights sum to one within numeric tolerance.
- Normals transform with inverse transpose while positions use object-to-world.

### Common mistakes

- An uninitialized payload field can make a miss look like random light.
- Wrong triangle index ordering reads unrelated vertices.
- Incorrect barycentric ordering smears UVs and normals.

### Practice

- Find RayPayload and list which shader stages write every member.
- Trace primitive index to index-buffer offsets to vertex data.
- Test a known point with weights 0.25, 0.25, and 0.5.

### Check your understanding

- Where does DXR fill payload data?
- What do barycentric weights sum to?
- Why use different transforms for positions and normals?

**Take-away:** The hit shader joins acceleration-structure results to interpolated scene attributes.

## 36. Ray-traced reflections and bounded water reflectance

The water ray traces scene geometry in the reflected direction; an art control scales its Fresnel share but keeps the blend bounded.

**Source to inspect:** shaders/RayTracingPBR.hlsl, src/ecs/Scene.cpp, and src/editor/EditorUI.cpp.

Teaching miniature:
```cpp
float F0 = 0.02f;
float F = F0 + (1-F0) * pow(1-NdotV, 5);
float weight = saturate(F * strength);
float3 outColor = lerp(waterBody, reflected, weight);
```

### Read each line

1. Line 1 sets approximate normal-incidence Fresnel reflectance for water.
2. Line 2 applies Schlick's approximation as viewing angle becomes grazing.
3. Line 3 applies the editor's reflection multiplier and clamps it into [0,1].
4. Line 4 partitions the response between deep water and reflected scene radiance.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `F0` | Normal-incidence reflectance estimate |
| `NdotV` | Cosine of angle between surface normal and viewer |
| `strength` | Art-directed reflectance multiplier exposed by editor |
| `lerp` | Linear interpolation of two endpoint colors |

### Rules to keep in mind

- Schlick approaches F0 at a head-on view and one at a grazing view.
- Saturating the weight preserves a bounded reflection/deep-water partition.
- The art multiplier is not the same as measured water refractive index.

### Common mistakes

- Adding reflections on top of the full deep-water term double counts energy.
- Calling a 4x control pure physically correct water Fresnel would be inaccurate.
- Some reflected rays see scene objects; rays missing geometry use the procedural sky.

### Practice

- Capture enabled and disabled images at strength 2.5 and 4.
- Inspect the mirrored colored silhouettes beneath floating props.
- Keep image delta, changed-pixel ratio, and FPS as separate measurements.

### Check your understanding

- Why is the reflection weight clamped?
- Does 4x represent pure water's physical F0?
- Which geometry appears in the reflected capture?

**Take-away:** The reflection toggle proves DXR contribution; the slider art-directs its bounded strength.

## 37. Path tracing, sample averages, and NRD

Path tracing estimates lighting with randomized paths; accumulation lowers variance over time, while denoising filters incomplete evidence.

**Source to inspect:** shaders/RayTracingPBR.hlsl and the NRD bridge under src/rhi/.

Teaching miniature:
```cpp
float3 sum = float3(0,0,0);
for (uint i=0; i<count; ++i)
 sum += tracePath(randomRay(i));
float3 estimate = sum / float(count);
```

### Read each line

1. Line 1 begins a radiance sum at zero.
2. Line 2 traces one path per iteration.
3. Line 3 adds each path's light estimate.
4. Line 4 divides by sample count to make the average.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `sample` | One randomized path estimate |
| `variance` | Spread of estimates around their expected value |
| `accumulation` | Average over multiple samples or frames |
| `denoiser` | Filter using temporal or spatial evidence |

### Rules to keep in mind

- For independent samples, standard error generally falls like one over square root of sample count.
- NRD reduces visible variance but cannot create an unsampled unbiased estimate.
- Temporal reuse needs valid motion, depth, and history correspondence.

### Common mistakes

- Dividing by zero samples is invalid.
- Correlated random values can create structured noise.
- Excessive filtering removes thin geometry and fine texture.

### Practice

- Compare images at 1, 4, 16, and 64 samples.
- Reset accumulation after changing camera or materials.
- Compare NRD enabled and disabled with the same camera and seed.

### Check your understanding

- Why does 4x samples not usually halve noise?
- Can a denoiser recover details never sampled?
- Why reset accumulation after scene edits?

**Take-away:** Sampling, accumulation, and denoising solve different parts of the noise problem.

## 38. Water waves, gradients, and surface normals

The analytic height function supplies displacement; its partial derivatives determine the normal used for lighting and reflection.

**Source to inspect:** src/ecs/OceanWave.h and the ocean branch of shaders/RayTracingPBR.hlsl.

Teaching miniature:
```cpp
float h = height(x, z, time);
float dx = derivativeX(x, z, time);
float dz = derivativeZ(x, z, time);
float3 N = normalize(float3(-dx, 1, -dz));
```

### Read each line

1. Line 1 evaluates surface height at a horizontal point and time.
2. Line 2 evaluates slope along X.
3. Line 3 evaluates slope along Z.
4. Line 4 builds a normal perpendicular to tangent directions and normalizes it.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `height` | Vertical surface coordinate at horizontal x,z |
| `partial derivative` | Slope along one axis while other inputs remain fixed |
| `surface normal` | Direction perpendicular to the local surface |
| `pixel footprint` | Approximate size of one screen pixel in world units |

### Rules to keep in mind

- The analytic normal follows the negative height gradient for this surface parameterization.
- High-frequency detail fades as pixel footprint grows to avoid aliasing.
- Physics and shader displacement should use compatible wave functions.

### Common mistakes

- A wrong derivative sign tilts highlights and reflections opposite to the wave.
- Keeping all small waves at far distance creates shimmer.
- Unfiltered normal noise makes the image grainy at low resolution.

### Practice

- Compare the analytic derivative against a finite difference at one point.
- Lower resolution and check that high-frequency ripple amplitude fades.
- View the normal debug mode while waves move.

### Check your understanding

- What does derivativeX measure?
- Why use (-dx,1,-dz) before normalization?
- Why fade detail with pixel footprint?

**Take-away:** Wave shape, surface slope, and screen-space filtering all affect water reflection quality.

## 39. Jolt fixed steps, buoyancy, and impact events

The engine steps Jolt in bounded increments, samples the shared water height for buoyancy, and sends impact events to shader-side wake equations.

**Source to inspect:** src/ecs/Scene.cpp, src/physics/JoltPhysicsWorld.cpp, and tools/jolt_bridge.cpp.

Teaching miniature:
```cpp
accumulator += dt;
while (accumulator >= fixedStep) {
 physics.step(fixedStep);
 accumulator -= fixedStep;
}
```

### Read each line

1. Line 1 accumulates elapsed time that has not yet been simulated.
2. Line 2 executes while at least one full fixed step remains.
3. Line 3 advances Jolt by one stable time interval.
4. Line 4 removes exactly the time just simulated.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `accumulator` | Unsimulated elapsed seconds |
| `fixedStep` | Chosen constant duration of each Jolt update |
| `buoyancy` | Upward force related to displaced fluid weight |
| `impact event` | Position, strength, speed, and time for a new wave disturbance |

### Rules to keep in mind

- Fixed substeps reduce physics instability when render FPS varies.
- Buoyancy approximates density times gravity times submerged volume.
- The renderer uses an analytic height field, not a volume-mesh fluid solver.

### Common mistakes

- Dropping excess dt without reporting it can desynchronize render and physics.
- Using different wave heights in physics and shader makes bodies look detached.
- A wake with no decay can persist forever or become numerically unstable.

### Practice

- Trace a body's height through water sampling, force calculation, and Jolt step.
- Compare body-neighborhood water captures with displacement enabled and disabled.
- Check the six body-entry and displacement assertions in the runner.

### Check your understanding

- Why does the accumulator loop use a fixed duration?
- What controls buoyant force?
- Is this a full two-way fluid pressure simulation?

**Take-away:** Jolt controls rigid bodies; the analytic ocean supplies forces and visual disturbances.

## 40. Test evidence, repeatability, and FPS claims

Tests should isolate a setting, verify valid output images, and keep image quality measurements separate from variable frame timings.

**Source to inspect:** ocean_capture_runner.py, reflection_capture_runner.py, and capture_runner.py.

Teaching miniature:
```cpp
on = capture(feature=True);
off = capture(feature=False);
delta = compareRegion(on, off);
assert delta.mean > minimum;
```

### Read each line

1. Line 1 captures with the selected feature enabled.
2. Line 2 captures the same scenario with only that feature disabled.
3. Line 3 measures a region-of-interest difference between images.
4. Line 4 rejects a test where the effect is absent or too small.

### Names and meanings

| Name | Meaning in this example |
| --- | --- |
| `A/B` | Two runs differing by one selected setting |
| `ROI` | Region of interest for a targeted comparison |
| `mean delta` | Average absolute pixel-channel difference |
| `repeatability` | Ability to reproduce the measurement under controlled inputs |

### Rules to keep in mind

- Keep scene, camera, resolution, sample count, and simulation time fixed.
- A threshold proves measurable contribution, not visual quality by itself.
- GPU frame time and screenshot wall time are different measurements.

### Common mistakes

- Random seeds or moving props can change capture masks between runs.
- A pixel threshold can miss many subtle changes or overemphasize a few bright ones.
- One FPS sample depends on GPU clocks, temperature, window size, and background work.

### Practice

- Run Sponza path tracing, then the reflections on/off/restored suite.
- Review the actual water and Sponza captures instead of trusting only numbers.
- Record build mode, resolution, GPU, and test command with measurements.

### Check your understanding

- What does an A/B comparison isolate?
- Why report mean delta and changed-pixel ratio together?
- Why should FPS be measured repeatedly?

**Take-away:** Reliable graphics claims combine image inspection, metrics, repeatability, and stated limits.
