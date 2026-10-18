#pragma once
#include <DirectXMath.h>
#include <cstddef>
#include <cstdint>

namespace Engine {

using namespace DirectX;

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 uv;
};

struct MaterialData {
    XMFLOAT4 baseColor;
    float metallic;
    float roughness;
    float emissive;
    int   textureId;
    int   metallicRoughnessTextureId;
    float alphaCutoff;
    float pad[2];
};
static_assert(sizeof(MaterialData) == 48, "MaterialData must stay byte-compatible with HLSL StructuredBuffer layout.");

struct InstanceData {
    uint32_t materialId;
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint32_t pad;
};

struct SceneCB {
    XMFLOAT4X4 viewProjInv;
    XMFLOAT3   cameraPos;
    uint32_t   accumFrameCount; // Frame count for temporal accumulation
    XMFLOAT3   sunDirection;
    float      sunIntensity;
    XMFLOAT4   sunColor;
    XMFLOAT4   skyZenithColor;
    XMFLOAT4   skyHorizonColor;
    uint32_t   maxBounces;      // Diffuse secondary-bounce limit (0 = direct only; shader clamps to 4)
    uint32_t   frameCount;      // stochastic random noise seed
    uint32_t   renderMode;      // 0 = Path Traced GI, 1 = Direct + RT Shadows, 2 = Normals, 3 = Albedo
    float      exposure;
    float      roughnessOverride;
    float      metallicOverride;
    uint32_t   toneMappingMode; // 0 = ACES Filmic, 1 = Reinhard, 2 = Linear
    float      shadowSoftness;
    XMFLOAT3   camForward;
    float      fovY;
    XMFLOAT3   camRight;
    float      aspectRatio;
    XMFLOAT3   camUp;
    float      cameraMoved;     // 1.0f if camera moved this frame, 0.0f if stationary
    // Previous-camera data used by this renderer's depth-checked reprojection.
    XMFLOAT3   prevCameraPos;
    uint32_t   enableShadows;   // Reuses the existing 16-byte cbuffer padding slot.
    XMFLOAT3   prevCamForward;
    float      padPrev1;
    XMFLOAT3   prevCamRight;
    float      enableRayTracedReflections;
    XMFLOAT3   prevCamUp;
    float      padPrev3;
    XMFLOAT2   jitterOffset;    // Halton (2,3) sub-pixel camera jitter
    uint32_t   enableTAA;       // 1 = depth-aware temporal AA enabled, 0 = disabled
    float      taaBlendWeight;  // Current frame weight during motion (default: 0.12f)
    uint32_t   enableNRD;       // NVIDIA REBLUR diffuse denoising for path-traced Sponza.
    uint32_t   renderHeight;    // Internal viewport height for DXR ray-cone texture LOD.
    uint32_t   oceanBodyCount;
    float      oceanPadAlignment;
    float      oceanPad[3];
    float      oceanPadAlignment2;
    XMFLOAT4   oceanBodyPositionRadius[8]; // xyz world position, w horizontal wake radius.
    XMFLOAT4   oceanBodyVelocityImpact[8]; // x/y horizontal velocity, z impact time, w impact strength.
    XMFLOAT4   oceanBodyImpactPositionRadius[8]; // xyz pinned impact origin, w body wake radius.
    XMFLOAT4   oceanBodyVerticalState[8]; // x vertical velocity, y vertical support half-extent, z immersion fraction.
};

static_assert(offsetof(SceneCB, oceanBodyPositionRadius) % 16 == 0,
    "Ocean body data must begin on an HLSL constant-buffer register boundary.");
static_assert(offsetof(SceneCB, oceanBodyVelocityImpact) % 16 == 0 &&
              offsetof(SceneCB, oceanBodyImpactPositionRadius) % 16 == 0 &&
              offsetof(SceneCB, oceanBodyVerticalState) % 16 == 0,
    "Ocean event and vertical-state arrays must begin on HLSL register boundaries.");
static_assert(sizeof(SceneCB) % 16 == 0,
    "SceneCB must remain a 16-byte multiple for HLSL constant-buffer compatibility.");

} // namespace Engine
