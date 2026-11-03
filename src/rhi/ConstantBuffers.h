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
static_assert(sizeof(Vertex) == 32 && offsetof(Vertex, normal) == 12 && offsetof(Vertex, uv) == 24,
    "Vertex must match the 32-byte HLSL StructuredBuffer layout.");

struct MaterialData {
    XMFLOAT4 baseColor;
    float metallic;
    float roughness;
    float emissive;
    int   textureId = -1;
    int   metallicRoughnessTextureId = -1;
    float alphaCutoff = 0.0f;
    int   normalTextureId = -1;
    int   emissiveTextureId = -1;
    XMFLOAT4 emissiveColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 0.0f);
    // dielectric = (specularTransmission, indexOfRefraction, reserved, reserved)
    XMFLOAT4 dielectric = XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f);
    XMFLOAT4 volumeAbsorption = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
};
static_assert(sizeof(MaterialData) == 96, "MaterialData must stay byte-compatible with HLSL StructuredBuffer layout.");
static_assert(offsetof(MaterialData, baseColor) == 0 &&
              offsetof(MaterialData, metallic) == 16 &&
              offsetof(MaterialData, roughness) == 20 &&
              offsetof(MaterialData, emissive) == 24 &&
              offsetof(MaterialData, textureId) == 28 &&
              offsetof(MaterialData, metallicRoughnessTextureId) == 32 &&
              offsetof(MaterialData, alphaCutoff) == 36 &&
              offsetof(MaterialData, normalTextureId) == 40 &&
              offsetof(MaterialData, emissiveTextureId) == 44 &&
              offsetof(MaterialData, emissiveColor) == 48 &&
              offsetof(MaterialData, dielectric) == 64 &&
              offsetof(MaterialData, volumeAbsorption) == 80,
    "MaterialData field offsets must match the HLSL StructuredBuffer layout.");

struct InstanceData {
    uint32_t materialId;
    uint32_t vertexOffset;
    uint32_t indexOffset;
};
static_assert(sizeof(InstanceData) == 12 && offsetof(InstanceData, indexOffset) == 8,
    "InstanceData must match the 12-byte HLSL StructuredBuffer layout.");

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
    float      prevJitterX;
    XMFLOAT3   prevCamRight;
    float      enableRayTracedReflections;
    XMFLOAT3   prevCamUp;
    float      prevJitterY;
    XMFLOAT2   jitterOffset;    // Halton (2,3) sub-pixel camera jitter
    uint32_t   enableTAA;       // 1 = depth-aware temporal AA enabled, 0 = disabled
    float      taaBlendWeight;  // Current frame weight during motion (default: 0.12f)
    uint32_t   enableNRD;       // NVIDIA REBLUR diffuse + specular denoising for path-traced Sponza.
    uint32_t   renderHeight;    // Internal viewport height for DXR ray-cone texture LOD.
    uint32_t   oceanBodyCount;
    float      waterReflectionStrength; // Artistic multiplier for water's bounded Fresnel reflection blend.
    uint32_t   enableDLSS;
    uint32_t   enableDLSSRR;
    uint32_t   dlssReset;
    uint32_t   textureArrayResolution; // Texture layer size used for ray-cone mip selection.
    XMFLOAT2   cameraClipPlanes; // Standard Direct3D LH projection near/far planes for HW depth guides.
    float      oceanTime;
    float      ambientMultiplier;
    XMFLOAT4   oceanBodyPositionRadius[8]; // xyz world position, w horizontal wake radius.
    XMFLOAT4   oceanBodyVelocityImpact[8]; // x/y horizontal velocity, z impact time, w impact strength.
    XMFLOAT4   oceanBodyImpactPositionRadius[8]; // xyz pinned impact origin, w body wake radius.
    XMFLOAT4   oceanBodyVerticalState[8]; // x vertical velocity, y vertical support half-extent, z immersion fraction.
    float      environmentIntensity;
    uint32_t   environmentMapEnabled;
    float      environmentMapMaxMip;
    uint32_t   environmentPadding;
};

static_assert(offsetof(SceneCB, oceanBodyPositionRadius) % 16 == 0,
    "Ocean body data must begin on an HLSL constant-buffer register boundary.");
static_assert(offsetof(SceneCB, oceanBodyVelocityImpact) % 16 == 0 &&
              offsetof(SceneCB, oceanBodyImpactPositionRadius) % 16 == 0 &&
              offsetof(SceneCB, oceanBodyVerticalState) % 16 == 0,
    "Ocean event and vertical-state arrays must begin on HLSL register boundaries.");
static_assert(sizeof(SceneCB) % 16 == 0,
    "SceneCB must remain a 16-byte multiple for HLSL constant-buffer compatibility.");
static_assert(offsetof(SceneCB, viewProjInv) == 0 &&
              offsetof(SceneCB, cameraPos) == 64 && offsetof(SceneCB, accumFrameCount) == 76 &&
              offsetof(SceneCB, sunDirection) == 80 && offsetof(SceneCB, sunIntensity) == 92 &&
              offsetof(SceneCB, maxBounces) == 144 && offsetof(SceneCB, shadowSoftness) == 172 &&
              offsetof(SceneCB, camForward) == 176 && offsetof(SceneCB, fovY) == 188 &&
              offsetof(SceneCB, prevCameraPos) == 224 && offsetof(SceneCB, prevCamForward) == 240 &&
              offsetof(SceneCB, prevCamRight) == 256 && offsetof(SceneCB, enableRayTracedReflections) == 268 &&
              offsetof(SceneCB, prevCamUp) == 272 && offsetof(SceneCB, jitterOffset) == 288 &&
              offsetof(SceneCB, enableTAA) == 296 && offsetof(SceneCB, enableNRD) == 304 &&
              offsetof(SceneCB, enableDLSS) == 320 && offsetof(SceneCB, cameraClipPlanes) == 336 &&
              offsetof(SceneCB, oceanBodyPositionRadius) == 352 &&
              offsetof(SceneCB, oceanBodyVelocityImpact) == 480 &&
              offsetof(SceneCB, oceanBodyImpactPositionRadius) == 608 &&
              offsetof(SceneCB, oceanBodyVerticalState) == 736 &&
              offsetof(SceneCB, environmentIntensity) == 864 && sizeof(SceneCB) == 880,
    "SceneCB offsets/size must match the register-packed HLSL SceneCB declaration.");

} // namespace Engine
