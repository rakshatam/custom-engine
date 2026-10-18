// RayTracingPBR.hlsl - DirectX 12 DXR hardware path tracing and GI shader.
// Temporal reprojection and ray-tracing architecture are implemented locally;
// see README.md for the O3DE Atom reference modules consulted during development.

#define PI 3.14159265358979323846f

#if ENGINE_ENABLE_NRD
#include "NRD.hlsli"
#endif

struct Vertex {
    float3 position;
    float3 normal;
    float2 uv;
};

struct MaterialData {
    float4 baseColor;
    float  metallic;
    float  roughness;
    float  emissive;
    int    textureId;
    int    metallicRoughnessTextureId;
    float  alphaCutoff;
    float2 pad;
};

struct InstanceData {
    uint materialId;
    uint vertexOffset;
    uint indexOffset;
    uint pad;
};

struct SceneCB {
    float4x4 viewProjInv;
    float3   cameraPos;
    uint     accumFrameCount; // Frame count for temporal accumulation (SPP)
    float3   sunDirection;
    float    sunIntensity;
    float4   sunColor;
    float4   skyZenithColor;
    float4   skyHorizonColor;
    uint     maxBounces;
    uint     frameCount;
    uint     renderMode;
    float    exposure;
    float    roughnessOverride;
    float    metallicOverride;
    uint     toneMappingMode;
    float    shadowSoftness;
    float3   camForward;
    float    fovY;
    float3   camRight;
    float    aspectRatio;
    float3   camUp;
    float    cameraMoved;
    // Previous-camera data for analytic temporal reprojection.
    float3   prevCameraPos;
    uint     enableShadows;
    float3   prevCamForward;
    float    padPrev1;
    float3   prevCamRight;
    float    enableRayTracedReflections;
    float3   prevCamUp;
    float    padPrev3;
    float2   jitterOffset;
    uint     enableTAA;
    float    taaBlendWeight;
    uint     enableNRD;
    uint     renderHeight;
    uint     oceanBodyCount;
    float    oceanPadAlignment;
    float3   oceanPad;
    float    oceanPadAlignment2;
    float4   oceanBodyPositionRadius[8];
    float4   oceanBodyVelocityImpact[8];
    float4   oceanBodyImpactPositionRadius[8];
    float4   oceanBodyVerticalState[8];
};

// Global Resources
RaytracingAccelerationStructure g_Scene          : register(t0);
RWTexture2D<float4>             g_Output         : register(u0);
RWTexture2D<float4>             g_Accumulation   : register(u1);
RWTexture2D<float4>             g_History        : register(u2);
#if ENGINE_ENABLE_NRD
RWTexture2D<float4>             g_NrdSignal      : register(u3);
RWTexture2D<float4>             g_NrdNormalRoughness : register(u4);
RWTexture2D<float>              g_NrdViewZ       : register(u5);
RWTexture2D<float4>             g_NrdMotion      : register(u6);
RWTexture2D<float4>             g_NrdAlbedo      : register(u7);
#endif
ConstantBuffer<SceneCB>         g_SceneCB        : register(b0);
StructuredBuffer<Vertex>        g_Vertices       : register(t1);
StructuredBuffer<uint>          g_Indices        : register(t2);
StructuredBuffer<MaterialData>  g_Materials      : register(t3);
StructuredBuffer<InstanceData>  g_Instances      : register(t4);
Texture2DArray<float4>          g_Textures       : register(t5);
SamplerState                    g_LinearSampler  : register(s0);

// Payload structures
struct RayPayload {
    float3 color;
    float3 albedo;
    float3 normal;
    float3 hitPos;
    float3 nonDiffuseRadiance;
    float  hitT;
    float  roughness;
    // Packed 32-bit stream: 24-bit sample index, 7-bit dimension, 1-bit
    // reflection marker. The separate non-diffuse output keeps NRD's input typed.
    uint   sampleSeed;
    uint   sampleStream;
};

struct ShadowPayload {
    float visibility;
};

// PCG Random Number Generator
uint pcgHash(inout uint state) {
    uint prev = state;
    state = prev * 747796405u + 2891336453u;
    uint word = ((prev >> ((prev >> 28u) + 4u)) ^ prev) * 277803737u;
    return (word >> 22u) ^ word;
}

uint hashValue(uint value) {
    uint state = value;
    return pcgHash(state);
}

float3 safeNormalize(float3 value, float3 fallback) {
    const float lengthSquared = dot(value, value);
    if (!isfinite(lengthSquared) || lengthSquared <= 1.0e-20f) return fallback;
    const float3 result = value * rsqrt(lengthSquared);
    return all(isfinite(result)) ? result : fallback;
}

// Hash-based Owen scrambling following the published method by Brent Burley
// (JCGT 2020). A per-pixel key randomizes the pattern spatially while retaining
// Sobol's low-discrepancy distribution across accumulated frames.
uint owenScramble(uint value, uint seed) {
    value = reversebits(value);
    value ^= value * 0x3d20adeau;
    value += seed;
    value *= (seed >> 16u) | 1u;
    value ^= value * 0x05526c56u;
    value ^= value * 0x53a22864u;
    return reversebits(value);
}

uint sobolSecondDimension(uint index) {
    // Canonical direction numbers for Sobol dimension 2.
    static const uint directions[24] = {
        0x80000000u, 0xc0000000u, 0x60000000u, 0x90000000u,
        0xe8000000u, 0x5c000000u, 0x8e000000u, 0xc5000000u,
        0x68800000u, 0x9cc00000u, 0xee600000u, 0x55900000u,
        0x80680000u, 0xc09c0000u, 0x60ee0000u, 0x90550000u,
        0xe8808000u, 0x5cc0c000u, 0x8e606000u, 0xc5909000u,
        0x6868e800u, 0x9c9c5c00u, 0xeeee8e00u, 0x5555c500u
    };

    uint result = 0u;
    [unroll]
    for (uint bit = 0u; bit < 24u; ++bit) {
        result ^= ((index >> bit) & 1u) * directions[bit];
    }
    return result;
}

float sampleSobol2D(inout uint sampleStream, uint pixelSeed) {
    const uint sampleIndex = sampleStream & 0x00ffffffu;
    const uint dimension = (sampleStream >> 24u) & 0x3fu;
    const uint component = dimension & 1u;
    const uint pairIndex = dimension >> 1u;
    sampleStream = (sampleStream & 0xc0ffffffu) | (((dimension + 1u) & 0x3fu) << 24u);

    uint pairSeed = hashValue(pixelSeed ^ (pairIndex * 0x9e3779b9u));
    uint shuffledIndex = owenScramble(sampleIndex, hashValue(pairSeed ^ 0x68bc21ebu));
    uint bits = component == 0u ? reversebits(shuffledIndex) : sobolSecondDimension(shuffledIndex);
    bits = owenScramble(bits, hashValue(pairSeed ^ (component + 1u) * 0x967a889bu));
    return float(bits >> 8u) * (1.0f / 16777216.0f);
}

// Cosine-weighted hemisphere sample
float3 getCosineHemisphereSample(float3 N, inout uint sampleStream, uint pixelSeed) {
    float u1 = sampleSobol2D(sampleStream, pixelSeed);
    float u2 = sampleSobol2D(sampleStream, pixelSeed);
    float r = sqrt(u1);
    float theta = 2.0f * PI * u2;
    float x = r * cos(theta);
    float z = r * sin(theta);
    float y = sqrt(max(0.0f, 1.0f - u1));

    float3 up = abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = safeNormalize(cross(up, N), float3(1.0f, 0.0f, 0.0f));
    float3 bitangent = cross(N, tangent);

    return safeNormalize(tangent * x + N * y + bitangent * z, N);
}

float oceanValueNoise(float2 position) {
    const float2 cell = floor(position);
    const float2 fraction = frac(position);
    const float2 blend = fraction * fraction * (3.0f - 2.0f * fraction);
    const uint2 lattice = (uint2)(int2)cell;
    const float a = float(hashValue(lattice.x * 0x8da6b343u ^ lattice.y * 0xd8163841u) >> 8u) * (1.0f / 16777216.0f);
    const float b = float(hashValue((lattice.x + 1u) * 0x8da6b343u ^ lattice.y * 0xd8163841u) >> 8u) * (1.0f / 16777216.0f);
    const float c = float(hashValue(lattice.x * 0x8da6b343u ^ (lattice.y + 1u) * 0xd8163841u) >> 8u) * (1.0f / 16777216.0f);
    const float d = float(hashValue((lattice.x + 1u) * 0x8da6b343u ^ (lattice.y + 1u) * 0xd8163841u) >> 8u) * (1.0f / 16777216.0f);
    return lerp(lerp(a, b, blend.x), lerp(c, d, blend.x), blend.y);
}

// Analytic derivative of the smooth value-noise interpolant. This avoids
// finite-difference texture taps while supplying stable render-only ripples.
float2 oceanValueNoiseGradient(float2 position) {
    const float2 cell = floor(position);
    const float2 fraction = frac(position);
    const float2 blend = fraction * fraction * (3.0f - 2.0f * fraction);
    const float2 blendDerivative = 6.0f * fraction * (1.0f - fraction);
    const uint2 lattice = (uint2)(int2)cell;
    const float a = float(hashValue(lattice.x * 0x8da6b343u ^ lattice.y * 0xd8163841u) >> 8u) * (1.0f / 16777216.0f);
    const float b = float(hashValue((lattice.x + 1u) * 0x8da6b343u ^ lattice.y * 0xd8163841u) >> 8u) * (1.0f / 16777216.0f);
    const float c = float(hashValue(lattice.x * 0x8da6b343u ^ (lattice.y + 1u) * 0xd8163841u) >> 8u) * (1.0f / 16777216.0f);
    const float d = float(hashValue((lattice.x + 1u) * 0x8da6b343u ^ (lattice.y + 1u) * 0xd8163841u) >> 8u) * (1.0f / 16777216.0f);
    const float derivativeX0 = (b - a) * blendDerivative.x;
    const float derivativeX1 = (d - c) * blendDerivative.x;
    const float derivativeY0 = c - a;
    const float derivativeY1 = d - b;
    return float2(lerp(derivativeX0, derivativeX1, blend.y),
                  lerp(derivativeY0, derivativeY1, blend.x) * blendDerivative.y);
}

float oceanCloudFbm(float2 position) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    [unroll]
    for (uint octave = 0u; octave < 4u; ++octave) {
        sum += amplitude * oceanValueNoise(position);
        position = position * 2.03f + float2(17.13f, -9.71f);
        amplitude *= 0.5f;
    }
    return sum * (1.0f / 0.9375f);
}

void evaluateOceanSurface(float x, float z, float time, float pixelFootprint,
                          out float height, out float2 gradient) {
    const float p0 = 0.28f * x + 0.62f * z + 0.92f * time;
    const float p1 = -0.74f * x + 0.41f * z - 1.17f * time;
    const float p2 = 1.03f * x - 0.78f * z + 1.75f * time;
    const float p3 = -1.65f * x - 0.18f * z - 2.35f * time;
    const float p4 = 1.25f * x + 1.82f * z + 3.20f * time;
    if (!all(isfinite(float4(p0, p1, p2, p3))) || !isfinite(p4)) {
        height = 0.0f;
        gradient = 0.0f;
        return;
    }
    const float fade0 = 1.0f - smoothstep(0.06f, 0.15f, pixelFootprint);
    const float fade1 = 1.0f - smoothstep(0.04f, 0.12f, pixelFootprint);
    const float fade2 = 1.0f - smoothstep(0.025f, 0.08f, pixelFootprint);
    const float fade3 = 1.0f - smoothstep(0.015f, 0.05f, pixelFootprint);
    const float fade4 = 1.0f - smoothstep(0.008f, 0.03f, pixelFootprint);
    const float a0 = 0.32f * fade0;
    const float a1 = 0.19f * fade1;
    const float a2 = 0.105f * fade2;
    const float a3 = 0.045f * fade3;
    const float a4 = 0.014f * fade4;
    height = a0 * sin(p0) + a1 * sin(p1) + a2 * sin(p2) + a3 * sin(p3) + a4 * sin(p4);
    gradient.x = a0 * 0.28f * cos(p0) - a1 * 0.74f * cos(p1)
               + a2 * 1.03f * cos(p2) - a3 * 1.65f * cos(p3) + a4 * 1.25f * cos(p4);
    gradient.y = a0 * 0.62f * cos(p0) + a1 * 0.41f * cos(p1)
               - a2 * 0.78f * cos(p2) - a3 * 0.18f * cos(p3) + a4 * 1.82f * cos(p4);
}

// Shared analytic interaction height used both to intersect the displaced
// water surface and to shade it. The flat quad in the BLAS is only a broad
// phase; this function supplies the actual primary-ray height field.
void evaluateOceanInteractionSurface(float x, float z, float time, float pixelFootprint,
                                     out float height, out float2 gradient, out float foam) {
    evaluateOceanSurface(x, z, time, pixelFootprint, height, gradient);
    foam = 0.0f;
    const uint bodyCount = min(g_SceneCB.oceanBodyCount, 8u);
    [loop]
    for (uint bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
        const float4 body = g_SceneCB.oceanBodyPositionRadius[bodyIndex];
        const float4 motion = g_SceneCB.oceanBodyVelocityImpact[bodyIndex];
        const float4 impactOrigin = g_SceneCB.oceanBodyImpactPositionRadius[bodyIndex];
        const float verticalVelocity = g_SceneCB.oceanBodyVerticalState[bodyIndex].x;
        const float2 delta = float2(x - body.x, z - body.z);
        const float distanceToBody = length(delta);
        const float radius = max(body.w, 0.25f);
        const float submersion = saturate(g_SceneCB.oceanBodyVerticalState[bodyIndex].z);
        const float radiusSquared = radius * radius;
        const float radialSquared = dot(delta, delta) / radiusSquared;
        const float normalizedRadius = sqrt(radialSquared);
        const float innerDisplacement = exp(-radialSquared);
        const float ridgeCoordinate = (radialSquared - 2.25f) / 2.0f;
        const float ridgeDisplacement = exp(-ridgeCoordinate * ridgeCoordinate);
        const float displacementProfile = -innerDisplacement + 0.2987677926f * ridgeDisplacement;
        const float displacementAmplitude = min(0.45f * radius, 0.45f) * submersion;
        height += displacementAmplitude * displacementProfile;
        const float ridgeSlopeFactor = 2.0f * 0.2987677926f * ridgeDisplacement * ridgeCoordinate;
        gradient += displacementAmplitude * (delta / radiusSquared)
            * (2.0f * innerDisplacement - ridgeSlopeFactor);
        if (submersion > 0.01f) {
            const float bodyRim = abs(distanceToBody - 1.36f * radius);
            foam = max(foam, 0.22f * submersion
                * (1.0f - smoothstep(0.08f, 0.34f, bodyRim)));
        }

        const float impactAge = time - motion.z;
        const float impactStrength = saturate(motion.w);
        if (impactStrength > 0.001f && impactAge >= 0.0f && impactAge < 4.5f) {
            const float2 impactDelta = float2(x - impactOrigin.x, z - impactOrigin.z);
            const float impactDistanceRoot = sqrt(max(dot(impactDelta, impactDelta), 0.0f) + 0.0004f);
            const float impactDistance = impactDistanceRoot - 0.02f;
            const float phase = 4.6f * impactDistance - 7.2f * impactAge;
            const float decay = exp(-0.27f * impactDistance - 0.92f * impactAge);
            const float amplitude = 0.23f * impactStrength * decay;
            height += amplitude * sin(phase);
            const float radialDerivative = amplitude
                * (4.6f * cos(phase) - 0.27f * sin(phase));
            gradient += radialDerivative * (impactDelta / impactDistanceRoot);
            const float ringDistance = abs(impactDistance - radius * 0.8f - 1.8f * impactAge);
            foam = max(foam, impactStrength * exp(-2.0f * impactAge)
                * (1.0f - smoothstep(0.04f, 0.22f, ringDistance)));
        }

        const float bodyDistanceRoot = sqrt(max(dot(delta, delta), 0.0f) + 0.0004f);
        const float smoothBodyDistance = bodyDistanceRoot - 0.02f;
        const float bobEnergy = submersion * saturate(abs(verticalVelocity) * 0.25f);
        if (bobEnergy > 0.01f) {
            const float bobPhase = 4.0f * smoothBodyDistance - 2.8f * time
                + (verticalVelocity < 0.0f ? 0.0f : 3.14159265f);
            const float bobEnvelope = exp(-0.5f * smoothBodyDistance);
            const float bobAmplitude = 0.10f * bobEnergy * bobEnvelope;
            height += bobAmplitude * sin(bobPhase);
            const float radialBobDerivative = bobAmplitude
                * (4.0f * cos(bobPhase) - 0.5f * sin(bobPhase));
            gradient += radialBobDerivative * (delta / bodyDistanceRoot);
        }

        const float2 horizontalVelocity = motion.xy;
        const float horizontalSpeed = length(horizontalVelocity);
        if (horizontalSpeed > 0.08f) {
            const float2 forward = horizontalVelocity / horizontalSpeed;
            const float2 side = float2(-forward.y, forward.x);
            const float along = dot(delta, forward);
            const float across = dot(delta, side);
            const float smoothAbsAcross = sqrt(across * across + 0.0004f) - 0.02f;
            const float wakeEnvelope = exp(-max(-along, 0.0f) * 0.22f - smoothAbsAcross * 0.48f);
            const float wakePhase = 1.45f * along + 2.8f * across - 1.3f * time;
            const float wakeAmplitude = 0.10f * saturate(horizontalSpeed * 0.16f);
            height += wakeAmplitude * wakeEnvelope * sin(wakePhase);
            const float envelopeAlongDerivative = along < 0.0f ? 0.22f : 0.0f;
            const float envelopeAcrossDerivative = -0.48f * across / sqrt(across * across + 0.0004f);
            const float derivativeAlong = wakeAmplitude * wakeEnvelope
                * (envelopeAlongDerivative * sin(wakePhase) + 1.45f * cos(wakePhase));
            const float derivativeAcross = wakeAmplitude * wakeEnvelope
                * (envelopeAcrossDerivative * sin(wakePhase) + 2.8f * cos(wakePhase));
            gradient += derivativeAlong * forward + derivativeAcross * side;
        }
    }
}

// Analytic atmosphere with a low-cost procedural cloud layer in the ocean scene.
float3 evaluateSky(float3 rayDir, bool includeSolarDisk) {
    rayDir = safeNormalize(rayDir, float3(0.0f, 1.0f, 0.0f));
    float t = saturate((rayDir.y + 0.1f) / 1.1f);
    float3 sky = lerp(g_SceneCB.skyHorizonColor.rgb, g_SceneCB.skyZenithColor.rgb, pow(t, 0.55f));

    if (g_SceneCB.padPrev3 > 2.0f) {
        const float horizonHaze = exp(-abs(rayDir.y) * 24.0f);
        sky = lerp(sky, float3(0.43f, 0.53f, 0.59f), horizonHaze * 0.34f);
        const float2 cloudUv = rayDir.xz / max(rayDir.y + 0.20f, 0.075f);
        const float cloudShape = oceanCloudFbm(cloudUv * 1.45f + float2(g_SceneCB.padPrev1 * 0.004f, 0.0f));
        const float cloudDetail = oceanCloudFbm(cloudUv * 3.6f - float2(0.0f, g_SceneCB.padPrev1 * 0.009f));
        const float cloudCoverage = smoothstep(0.40f, 0.56f, cloudShape);
        const float cloudLayer = smoothstep(-0.045f, 0.08f, rayDir.y)
                               * (1.0f - 0.35f * smoothstep(0.55f, 0.95f, rayDir.y));
        const float cloudShade = saturate(0.22f + cloudDetail * 0.78f + max(rayDir.y, 0.0f) * 0.22f);
        const float3 cloudColor = lerp(float3(0.17f, 0.24f, 0.29f), float3(0.58f, 0.66f, 0.70f), cloudShade);
        sky = lerp(sky, cloudColor, cloudCoverage * cloudLayer * 0.92f);
    }
    
    // Sun disk
    if (includeSolarDisk) {
        const float sunDot = dot(rayDir, -g_SceneCB.sunDirection);
        if (sunDot > 0.99997f) {
            sky += g_SceneCB.sunColor.rgb * g_SceneCB.sunIntensity * 3.0f;
        } else if (sunDot > 0.997f) {
            const float glow = pow(saturate((sunDot - 0.997f) / 0.003f), 4.0f);
            sky += g_SceneCB.sunColor.rgb * glow * g_SceneCB.sunIntensity * 0.32f;
        }
    }
    return sky;
}

// Microfacet Cook-Torrance GGX PBR Functions
float distributionGGX(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = PI * denom * denom;
    return num / max(denom, 0.0000001f);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0f);
    float k = (r * r) / 8.0f;
    float num = NdotV;
    float denom = NdotV * (1.0f - k) + k;
    return num / max(denom, 0.0000001f);
}

float geometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float3 sampleGGXVisibleNormal(float3 N, float3 V, float alpha, float2 u) {
    // Heitz's isotropic GGX VNDF sampler. Stretch the view into the GGX
    // configuration, sample the projected visible disk, then unstretch H.
    const float3 up = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f)
                                         : float3(1.0f, 0.0f, 0.0f);
    const float3 T = safeNormalize(cross(up, N), float3(1.0f, 0.0f, 0.0f));
    const float3 B = cross(N, T);
    const float3 localV = float3(dot(V, T), dot(V, B), max(dot(V, N), 1.0e-5f));
    const float3 stretchedV = safeNormalize(float3(alpha * localV.x, alpha * localV.y, localV.z),
                                             float3(0.0f, 0.0f, 1.0f));
    const float lensq = stretchedV.x * stretchedV.x + stretchedV.y * stretchedV.y;
    const float3 diskT1 = lensq > 1.0e-8f
        ? float3(-stretchedV.y, stretchedV.x, 0.0f) * rsqrt(lensq)
        : float3(1.0f, 0.0f, 0.0f);
    const float3 diskT2 = cross(stretchedV, diskT1);
    const float radius = sqrt(saturate(u.x));
    const float phi = 2.0f * PI * saturate(u.y);
    const float diskX = radius * cos(phi);
    float diskY = radius * sin(phi);
    const float blend = 0.5f * (1.0f + stretchedV.z);
    diskY = (1.0f - blend) * sqrt(max(1.0f - diskX * diskX, 0.0f)) + blend * diskY;
    const float normalZ = sqrt(max(1.0f - diskX * diskX - diskY * diskY, 0.0f));
    const float3 stretchedH = diskX * diskT1 + diskY * diskT2 + normalZ * stretchedV;
    const float3 localH = safeNormalize(float3(alpha * stretchedH.x, alpha * stretchedH.y,
                                                max(stretchedH.z, 0.0f)),
                                          float3(0.0f, 0.0f, 1.0f));
    return safeNormalize(T * localH.x + B * localH.y + N * localH.z, N);
}

float smithGGXG1(float NdotDirection, float alpha) {
    const float nDotV = saturate(NdotDirection);
    if (nDotV <= 1.0e-6f) return 0.0f;
    const float alpha2 = alpha * alpha;
    return (2.0f * nDotV) /
        max(nDotV + sqrt(max(alpha2 + (1.0f - alpha2) * nDotV * nDotV, 0.0f)), 1.0e-6f);
}

float3 fresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float3 srgbToLinear(float3 color) {
    const float3 low = color / 12.92f;
    const float3 high = pow(max((color + 0.055f) / 1.055f, 0.0f), 2.4f);
    return lerp(low, high, step(0.04045f, color));
}

// Tone Mapping Functions
float3 applyACESFilmic(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 applyReinhard(float3 x) {
    return x / (1.0f + x);
}

float3 applyToneMapping(float3 hdrColor) {
    float3 exposed = hdrColor * g_SceneCB.exposure;
    float3 mapped;
    if (g_SceneCB.toneMappingMode == 0) {
        mapped = applyACESFilmic(exposed);
    } else if (g_SceneCB.toneMappingMode == 1) {
        mapped = applyReinhard(exposed);
    } else {
        mapped = saturate(exposed);
    }
    // The swap chain and editor viewport use UNORM, so encode standard sRGB.
    const float3 low = mapped * 12.92f;
    const float3 high = 1.055f * pow(max(mapped, 0.0f), 1.0f / 2.4f) - 0.055f;
    return lerp(low, high, step(0.0031308f, mapped));
}

// -------------------------------------------------------------
// Temporal color transforms and perceptual utilities used by this renderer.
// -------------------------------------------------------------
float3 RgbToYCoCg(float3 rgb) {
    return float3(
         0.25f * rgb.r + 0.50f * rgb.g + 0.25f * rgb.b,
         0.50f * rgb.r + 0.00f * rgb.g - 0.50f * rgb.b,
        -0.25f * rgb.r + 0.50f * rgb.g - 0.25f * rgb.b
    );
}

float3 YCoCgToRgb(float3 ycocg) {
    return float3(
        ycocg.x + ycocg.y - ycocg.z,
        ycocg.x           + ycocg.z,
        ycocg.x - ycocg.y - ycocg.z
    );
}

float getLuminance(float3 color) {
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// Ray tracing shaders have no reliable pixel derivatives. Estimate the
// footprint from the ray cone and triangle UV/world area, then select the
// corresponding mip explicitly instead of forcing every texture hit to LOD 0.
float estimateTextureLod(Vertex v0, Vertex v1, Vertex v2, float hitDistance) {
    const float3 edge1 = mul((float3x3)ObjectToWorld4x3(), v1.position - v0.position);
    const float3 edge2 = mul((float3x3)ObjectToWorld4x3(), v2.position - v0.position);
    const float worldArea = 0.5f * length(cross(edge1, edge2));
    const float2 uv1 = v1.uv - v0.uv;
    const float2 uv2 = v2.uv - v0.uv;
    const float uvArea = 0.5f * abs(uv1.x * uv2.y - uv1.y * uv2.x);
    const float uvDensity = sqrt(uvArea / max(worldArea, 1.0e-8f));
    const float pixelConeWidth = max(hitDistance, 0.0f) * 2.0f * tan(g_SceneCB.fovY * 0.5f)
                               / max((float)g_SceneCB.renderHeight, 1.0f);
    const float footprintInTexels = pixelConeWidth * uvDensity * 1024.0f;
    return clamp(log2(max(footprintInTexels, 1.0f)), 0.0f, 10.0f);
}

// -------------------------------------------------------------
// Ray generation shader (Monte Carlo path tracing with temporal accumulation)
// -------------------------------------------------------------
[shader("raygeneration")]
void RayGenShader() {
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    const uint pixelIndex = launchIndex.y * launchDim.x + launchIndex.x;
    uint sampleSeed = hashValue(pixelIndex ^ 0xa511e9b3u);
    const uint sampleIndex = g_SceneCB.renderMode == 0u
        ? max(g_SceneCB.accumFrameCount, 1u) - 1u
        : g_SceneCB.frameCount;
    uint sampleStream = sampleIndex & 0x00ffffffu;

    // Sub-pixel jitter: Halton (2,3) low-discrepancy sequence for temporal antialiasing.
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    if (g_SceneCB.enableTAA != 0) {
        jitterX = g_SceneCB.jitterOffset.x;
        jitterY = g_SceneCB.jitterOffset.y;
    } else if (g_SceneCB.accumFrameCount > 1u) {
        jitterX = (sampleSobol2D(sampleStream, sampleSeed) - 0.5f) * 0.20f;
        jitterY = (sampleSobol2D(sampleStream, sampleSeed) - 0.5f) * 0.20f;
    }

    float2 pixelCenter = float2(launchIndex) + float2(0.5f + jitterX, 0.5f + jitterY);
    float2 inUV = pixelCenter / float2(launchDim);
    float2 ndc = inUV * 2.0f - 1.0f;
    ndc.y = -ndc.y; // DX inverted Y

    float tanHalfFov = tan(g_SceneCB.fovY * 0.5f);
    float3 rayDir = safeNormalize(g_SceneCB.camForward +
                                  g_SceneCB.camRight * (ndc.x * tanHalfFov * max(g_SceneCB.aspectRatio, 1.0e-4f)) +
                                  g_SceneCB.camUp * (ndc.y * tanHalfFov),
                                  safeNormalize(g_SceneCB.camForward, float3(0.0f, 0.0f, 1.0f)));

    RayDesc ray;
    ray.Origin = g_SceneCB.cameraPos;
    ray.Direction = rayDir;
    ray.TMin = 0.001f;
    ray.TMax = 10000.0f;

    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float3 totalRadiance = float3(0.0f, 0.0f, 0.0f);

    float3 primaryHitPos = float3(0.0f, 0.0f, 0.0f);
    float  primaryHitT   = -1.0f;
    float3 primaryDirectRadiance = float3(0.0f, 0.0f, 0.0f);
    float3 primaryNonDiffuseRadiance = float3(0.0f, 0.0f, 0.0f);
    float  firstIndirectHitDistance = 0.0f;
    float  primaryRoughness = 1.0f;
    float3 primaryNormal = float3(0.0f, 1.0f, 0.0f);
    float3 primaryAlbedo = float3(1.0f, 1.0f, 1.0f);

    // Maximum depth for path tracing:
    // renderMode: 0 = Path Traced GI, 1 = Direct + RT Shadows, 2 = Normals, 3 = Albedo
    uint maxBounces = (g_SceneCB.renderMode == 0) ? min(g_SceneCB.maxBounces, 4u) : 0u;

    for (uint bounce = 0; bounce <= maxBounces; ++bounce) {
        RayPayload payload;
        payload.color = float3(0.0f, 0.0f, 0.0f);
        payload.albedo = float3(0.0f, 0.0f, 0.0f);
        payload.normal = float3(0.0f, 0.0f, 0.0f);
        payload.hitPos = float3(0.0f, 0.0f, 0.0f);
        payload.nonDiffuseRadiance = float3(0.0f, 0.0f, 0.0f);
        payload.hitT = -1.0f;
        payload.roughness = 1.0f;
        payload.sampleSeed = sampleSeed;
        payload.sampleStream = sampleStream | (bounce == 0u ? 0x40000000u : 0u);

        TraceRay(
            g_Scene,
            RAY_FLAG_NONE,
            0xFF,
            0, 0, 0,
            ray,
            payload
        );
        sampleStream = payload.sampleStream & 0x3fffffffu;

        if (bounce == 0) {
            primaryHitPos = payload.hitPos;
            primaryHitT   = payload.hitT;
            primaryRoughness = payload.roughness;
            primaryNormal = payload.normal;
            primaryAlbedo = payload.albedo;
            primaryNonDiffuseRadiance = payload.nonDiffuseRadiance;
        }

        // If ray missed geometry, add environment / sky light and terminate path
        if (payload.hitT < 0.0f) {
            totalRadiance += throughput * payload.color;
            if (bounce == 0) primaryDirectRadiance = payload.color;
            break;
        }

        // Direct debug visualization modes on primary bounce
        if (bounce == 0 && (g_SceneCB.renderMode == 2 || g_SceneCB.renderMode == 3)) {
            totalRadiance = payload.color;
            break;
        }

        // Add emitted + direct reflected radiance from this hit point
        float3 hitRadiance = payload.color;
        if (bounce > 0) {
            // Secondary bounce GI: clamp fireflies to avoid extreme outlier grain
            hitRadiance = min(hitRadiance, float3(3.0f, 3.0f, 3.0f)) * 0.5f;
        }
        totalRadiance += throughput * hitRadiance;
        if (bounce == 0) {
            primaryDirectRadiance = hitRadiance;
        } else {
            if (bounce == 1) firstIndirectHitDistance = max(payload.hitT, 0.0f);
        }

        // Stop if maximum bounce reached
        if (bounce >= maxBounces) {
            break;
        }

        // Stochastic diffuse GI bounce via Cosine-Weighted Hemisphere Sampling
        float3 bounceDir = getCosineHemisphereSample(payload.normal, sampleStream, sampleSeed);
        float NdotL = saturate(dot(payload.normal, bounceDir));
        if (NdotL <= 0.0001f) {
            break;
        }

        // Throughput update: (BRDF * dot(N, L)) / PDF
        // For diffuse Lambertian: BRDF = albedo / PI, PDF = dot(N, L) / PI -> Factor = albedo
        throughput *= payload.albedo;

        // Russian Roulette path termination after 1st bounce
        if (bounce > 0) {
            const float survivalProbability = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05f, 0.95f);
            if (sampleSobol2D(sampleStream, sampleSeed) > survivalProbability) {
                break;
            }
            throughput /= survivalProbability;
        }

        // Setup ray for next bounce
        ray.Origin = payload.hitPos + payload.normal * 0.003f;
        ray.Direction = bounceDir;
        ray.TMin = 0.001f;
        ray.TMax = 10000.0f;
    }

    if (g_SceneCB.renderMode == 2 || g_SceneCB.renderMode == 3) {
        g_Output[launchIndex] = float4(totalRadiance, 1.0f);
        return;
    }

    // Sanitize radiance to avoid NaNs / fireflies
    if (isnan(totalRadiance.r) || isnan(totalRadiance.g) || isnan(totalRadiance.b) ||
        isinf(totalRadiance.r) || isinf(totalRadiance.g) || isinf(totalRadiance.b)) {
        totalRadiance = float3(0.0f, 0.0f, 0.0f);
    }
    totalRadiance = min(totalRadiance, float3(10.0f, 10.0f, 10.0f));
    if (!all(isfinite(primaryNonDiffuseRadiance))) primaryNonDiffuseRadiance = 0.0f;
    primaryNonDiffuseRadiance = clamp(primaryNonDiffuseRadiance, 0.0f, 10.0f);

#if ENGINE_ENABLE_NRD
    // NRD owns temporal history for this branch. Its diffuse signal contains
    // direct diffuse plus diffuse-bounced lighting, demodulated by base color.
    // Keep primary specular and emissive radiance outside REBLUR to preserve
    // sharp highlights and emissive detail.
    if (g_SceneCB.enableNRD != 0 && g_SceneCB.renderMode == 0) {
        const float viewZ = primaryHitT > 0.0f
            ? max(dot(primaryHitPos - g_SceneCB.cameraPos, g_SceneCB.camForward), 0.001f)
            : NRD_INF;
        const float3 materialFloor = max(primaryAlbedo, float3(0.02f, 0.02f, 0.02f));
        // REBLUR's diffuse signal should include the primary diffuse direct
        // term as well as later bounces; leaving direct light outside the
        // denoiser preserves one-sample shadow grain across the entire image.
        const float3 primaryDiffuseAndIndirect = max(totalRadiance - primaryNonDiffuseRadiance, 0.0f);
        const float3 denoisedDiffuseRadiance = min(primaryDiffuseAndIndirect / materialFloor, 250.0f);
        const float normalizedHitDistance = REBLUR_FrontEnd_GetNormHitDist(
            firstIndirectHitDistance, viewZ, float3(3.0f, 0.1f, 20.0f), primaryRoughness);
        g_NrdSignal[launchIndex] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
            denoisedDiffuseRadiance, normalizedHitDistance, true);
        g_NrdNormalRoughness[launchIndex] = NRD_FrontEnd_PackNormalAndRoughness(
            primaryNormal, primaryRoughness, 0.0f);
        g_NrdViewZ[launchIndex] = viewZ;
        g_NrdMotion[launchIndex] = 0.0f;
        g_NrdAlbedo[launchIndex] = float4(primaryAlbedo, 1.0f);
        // Miss pixels are not denoised; preserve their sky radiance separately.
        g_Accumulation[launchIndex] = float4(primaryHitT > 0.0f ? primaryNonDiffuseRadiance : primaryDirectRadiance, viewZ);
        g_Output[launchIndex] = float4(applyToneMapping(totalRadiance), 1.0f);
        return;
    }
#endif

    // -------------------------------------------------------------
    // Temporal antialiasing and progressive accumulation.
    // -------------------------------------------------------------
    float3 accumulatedRadiance;
    const float3 previousRadiance = g_History[launchIndex].rgb;
    float3 filteredHistory = previousRadiance;
    float denoiseStrength = 0.0f;

    // A small cross-bilateral pass suppresses low-SPP Monte Carlo noise while
    // rejecting depth edges and large luminance changes to protect material detail.
    if (g_SceneCB.renderMode <= 1 && g_SceneCB.cameraMoved <= 0.5f &&
        g_SceneCB.accumFrameCount > 1u && g_SceneCB.accumFrameCount <= 128u && primaryHitT > 0.0f) {
        float centerDepth = dot(primaryHitPos - g_SceneCB.cameraPos, g_SceneCB.camForward);
        float centerLuminance = getLuminance(previousRadiance);
        float depthTolerance = max(0.02f, centerDepth * 0.005f);
        float3 filteredColor = previousRadiance;
        float totalWeight = 1.0f;
        int2 tapOffsets[8] = {
            int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
            int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1)
        };
        int2 maxCoord = int2(launchDim) - 1;

        [unroll]
        for (uint tap = 0; tap < 8; ++tap) {
            int2 tapCoord = clamp(int2(launchIndex) + tapOffsets[tap], int2(0, 0), maxCoord);
            float4 tapHistory = g_History[tapCoord];
            float depthDifference = abs(tapHistory.a - centerDepth);
            if (tapHistory.a > 0.0f && depthDifference <= depthTolerance) {
                float luminanceDifference = abs(getLuminance(tapHistory.rgb) - centerLuminance);
                // Permit noisy same-surface samples to contribute. A very sharp
                // luminance gate treats Monte Carlo outliers as real edges and
                // leaves isolated fireflies untouched.
                float colorWeight = rcp(1.0f + luminanceDifference * 1.5f);
                float depthWeight = saturate(1.0f - depthDifference / depthTolerance);
                float tapWeight = colorWeight * depthWeight;
                filteredColor += tapHistory.rgb * tapWeight;
                totalWeight += tapWeight;
            }
        }

        filteredHistory = filteredColor / totalWeight;
        // Stronger at low sample counts, then rapidly relax to preserve texture
        // once temporal accumulation has reduced Monte Carlo variance.
        denoiseStrength = 0.90f * (1.0f - saturate(float(g_SceneCB.accumFrameCount - 1u) / 128.0f));
    }

    if (g_SceneCB.enableTAA != 0) {
        if (g_SceneCB.cameraMoved > 0.5f && primaryHitT > 0.0f) {
            // Camera is moving: Analytic reprojection into previous frame's camera coordinates
            float3 diff = primaryHitPos - g_SceneCB.prevCameraPos;
            float zPrev = dot(diff, g_SceneCB.prevCamForward);

            if (zPrev > 0.05f) {
                float tanHalfFov = tan(g_SceneCB.fovY * 0.5f);
                float ndcX = dot(diff, g_SceneCB.prevCamRight) / (zPrev * tanHalfFov * g_SceneCB.aspectRatio);
                float ndcY = dot(diff, g_SceneCB.prevCamUp) / (zPrev * tanHalfFov);
                float2 prevUV = float2(ndcX * 0.5f + 0.5f, -ndcY * 0.5f + 0.5f);

                bool isInside = (prevUV.x >= 0.001f && prevUV.x <= 0.999f && prevUV.y >= 0.001f && prevUV.y <= 0.999f);
                int2 depthCoord = clamp(int2(round(prevUV * float2(launchDim) - 0.5f)), int2(0, 0), int2(launchDim) - 1);
                float historyDepth = g_History[depthCoord].a;
                float depthTolerance = max(0.04f, zPrev * 0.02f);
                bool depthMatches = historyDepth > 0.0f && abs(historyDepth - zPrev) <= depthTolerance;

                if (isInside && depthMatches) {
                    // Bilinear history filtering for silky-smooth motion without nearest-neighbor jitter
                    float2 fCoord = prevUV * float2(launchDim) - 0.5f;
                    int2 iCoord = int2(floor(fCoord));
                    float2 fracCoord = frac(fCoord);

                    int2 c00 = clamp(iCoord, int2(0, 0), int2(launchDim) - 1);
                    int2 c10 = clamp(iCoord + int2(1, 0), int2(0, 0), int2(launchDim) - 1);
                    int2 c01 = clamp(iCoord + int2(0, 1), int2(0, 0), int2(launchDim) - 1);
                    int2 c11 = clamp(iCoord + int2(1, 1), int2(0, 0), int2(launchDim) - 1);

                    float3 s00 = g_History[c00].rgb;
                    float3 s10 = g_History[c10].rgb;
                    float3 s01 = g_History[c01].rgb;
                    float3 s11 = g_History[c11].rgb;

                    float3 historyColor = lerp(lerp(s00, s10, fracCoord.x), lerp(s01, s11, fracCoord.x), fracCoord.y);

                    // YCoCg neighborhood clipping inspired by Salvi's temporal AA method.
                    float3 currentYCoCg = RgbToYCoCg(totalRadiance);
                    float3 historyYCoCg = RgbToYCoCg(historyColor);

                    float3 aabbExtent = float3(0.35f, 0.25f, 0.25f) + abs(currentYCoCg) * 0.30f;
                    float3 minColor = currentYCoCg - aabbExtent;
                    float3 maxColor = currentYCoCg + aabbExtent;
                    historyYCoCg = clamp(historyYCoCg, minColor, maxColor);
                    float3 historyClamped = YCoCgToRgb(historyYCoCg);

                    // Perceptual tonemapped blend, following the Karis approach.
                    float lumCurrent = getLuminance(totalRadiance);
                    float lumHistory = getLuminance(historyClamped);
                    float3 tonemappedCurrent = totalRadiance / (1.0f + lumCurrent);
                    float3 tonemappedHistory = historyClamped / (1.0f + lumHistory);

                    float alpha = clamp(g_SceneCB.taaBlendWeight, 0.05f, 0.35f);
                    float3 blended = lerp(tonemappedHistory, tonemappedCurrent, alpha);

                    // Restore the approximate linear range after perceptual blending.
                    accumulatedRadiance = blended * (1.0f + lumCurrent);
                    accumulatedRadiance = max(accumulatedRadiance, float3(0.0f, 0.0f, 0.0f));
                } else {
                    accumulatedRadiance = totalRadiance;
                }
            } else {
                accumulatedRadiance = totalRadiance;
            }
        } else {
            // Camera stationary: Progressive Monte Carlo accumulation (increasing SPP)
            float3 prevRadiance = previousRadiance;
            if (g_SceneCB.accumFrameCount <= 1u) {
                accumulatedRadiance = totalRadiance;
            } else {
                float weight = 1.0f / float(min(g_SceneCB.accumFrameCount, 256u));
                accumulatedRadiance = lerp(prevRadiance, totalRadiance, weight);
            }
        }
    } else {
        // TAA disabled: standard temporal accumulation
        float3 prevRadiance = previousRadiance;
        if (g_SceneCB.accumFrameCount <= 1u) {
            accumulatedRadiance = totalRadiance;
        } else {
            float weight = 1.0f / float(min(g_SceneCB.accumFrameCount, 256u));
            accumulatedRadiance = lerp(prevRadiance, totalRadiance, weight);
        }
    }

    float accumulatedDepth = primaryHitT > 0.0f
        ? dot(primaryHitPos - g_SceneCB.cameraPos, g_SceneCB.camForward)
        : -1.0f;
    g_Accumulation[launchIndex] = float4(accumulatedRadiance, accumulatedDepth);

    // Filter only the display radiance. The unfiltered estimator is retained in
    // g_Accumulation so spatial smoothing never compounds into temporal history.
    float3 displayRadiance = accumulatedRadiance;
    if (g_SceneCB.renderMode <= 1 && g_SceneCB.cameraMoved <= 0.5f &&
        g_SceneCB.accumFrameCount > 1u && primaryHitT > 0.0f) {
        displayRadiance = lerp(accumulatedRadiance, filteredHistory, denoiseStrength);
    }
    float3 finalColor = applyToneMapping(displayRadiance);
    g_Output[launchIndex] = float4(finalColor, 1.0f);
}

// -------------------------------------------------------------
// Any Hit Shader (Alpha Cutout for Foliage / Chains)
// -------------------------------------------------------------
[shader("anyhit")]
void AnyHitShader(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    uint instanceIndex = InstanceID();
    uint primIndex = PrimitiveIndex();

    InstanceData inst = g_Instances[instanceIndex];
    MaterialData mat = g_Materials[inst.materialId];

    if (mat.alphaCutoff > 0.0f && mat.textureId >= 0) {
        uint i0 = g_Indices[inst.indexOffset + primIndex * 3 + 0] + inst.vertexOffset;
        uint i1 = g_Indices[inst.indexOffset + primIndex * 3 + 1] + inst.vertexOffset;
        uint i2 = g_Indices[inst.indexOffset + primIndex * 3 + 2] + inst.vertexOffset;

        Vertex v0 = g_Vertices[i0];
        Vertex v1 = g_Vertices[i1];
        Vertex v2 = g_Vertices[i2];

        float3 barycentrics = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
        float2 uv = v0.uv * barycentrics.x + v1.uv * barycentrics.y + v2.uv * barycentrics.z;

        const float textureLod = estimateTextureLod(v0, v1, v2, RayTCurrent());
        float alpha = g_Textures.SampleLevel(g_LinearSampler, float3(uv, mat.textureId), textureLod).a;
        if (alpha < mat.alphaCutoff) {
            IgnoreHit();
        }
    }
}

// -------------------------------------------------------------
// Closest Hit Shader (Full PBR + Texture2DArray + RT Shadows + Multi-Directional GI)
// -------------------------------------------------------------
[shader("closesthit")]
void ClosestHitShader(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    uint instanceIndex = InstanceID();
    uint primIndex = PrimitiveIndex();

    InstanceData inst = g_Instances[instanceIndex];
    MaterialData mat = g_Materials[inst.materialId];

    // Fetch triangle vertices and indices
    uint i0 = g_Indices[inst.indexOffset + primIndex * 3 + 0] + inst.vertexOffset;
    uint i1 = g_Indices[inst.indexOffset + primIndex * 3 + 1] + inst.vertexOffset;
    uint i2 = g_Indices[inst.indexOffset + primIndex * 3 + 2] + inst.vertexOffset;

    Vertex v0 = g_Vertices[i0];
    Vertex v1 = g_Vertices[i1];
    Vertex v2 = g_Vertices[i2];

    // Interpolate vertex attributes
    float3 barycentrics = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    float3 localNormal = safeNormalize(v0.normal * barycentrics.x + v1.normal * barycentrics.y + v2.normal * barycentrics.z,
                                       float3(0.0f, 1.0f, 0.0f));
    float2 uv = v0.uv * barycentrics.x + v1.uv * barycentrics.y + v2.uv * barycentrics.z;

    // Sample from Texture2DArray
    float4 texColor = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (mat.textureId >= 0) {
        const float textureLod = estimateTextureLod(v0, v1, v2, RayTCurrent());
        texColor = g_Textures.SampleLevel(g_LinearSampler, float3(uv, mat.textureId), textureLod);
    }

    // Transform normal to world space
    float3 worldNormal = safeNormalize(mul((float3x3)ObjectToWorld4x3(), localNormal),
                                       float3(0.0f, 1.0f, 0.0f));
    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 V = -WorldRayDirection();

    if (dot(worldNormal, V) < 0.0f) {
        worldNormal = -worldNormal;
    }

    // Material parameters
    float3 albedo = srgbToLinear(texColor.rgb) * mat.baseColor.rgb;

    // Reflected hits use a lightweight environment-lit material response. This
    // keeps all specular reflections to two DXR levels and avoids recursively
    // spawning reflection or shadow rays from the reflected hit.
    if ((payload.sampleStream & 0x80000000u) != 0u) {
        const float reflectedNdotL = saturate(dot(worldNormal, safeNormalize(-g_SceneCB.sunDirection, float3(0.0f, 1.0f, 0.0f))));
        const float reflectedAmbient = g_SceneCB.padPrev3 > 0.0f ? g_SceneCB.padPrev3 : 1.0f;
        const float skyHemi = saturate(0.5f + 0.5f * worldNormal.y);
        const float3 reflectedSky = lerp(g_SceneCB.skyHorizonColor.rgb * 0.55f,
            g_SceneCB.skyZenithColor.rgb, skyHemi) * (0.18f * skyHemi + 0.025f) * reflectedAmbient;
        const float3 reflectedSun = g_SceneCB.sunColor.rgb * g_SceneCB.sunIntensity
            * (reflectedNdotL / PI);
        payload.color = albedo * (reflectedSky + reflectedSun) + albedo * mat.emissive;
        payload.albedo = albedo;
        payload.normal = worldNormal;
        payload.hitPos = hitPos;
        payload.nonDiffuseRadiance = 0.0f;
        payload.hitT = RayTCurrent();
        payload.roughness = mat.roughness;
        return;
    }

    // The water branch is isolated by a material flag. Existing Sponza assets
    // keep the flag at zero and continue through the original PBR/GI path.
    if (mat.pad.x > 0.5f) {
        const float t = g_SceneCB.padPrev1;
        float waterRayT = RayTCurrent();
        float x = hitPos.x;
        float z = hitPos.z;
        float waveHeight;
        float2 waveGradient;
        float impactFoam = 0.0f;
        const float pixelFootprint = max(RayTCurrent(), 0.0f) * 2.0f * tan(g_SceneCB.fovY * 0.5f)
                                   / max(float(g_SceneCB.renderHeight), 1.0f);
        const float3 waterRayOrigin = WorldRayOrigin();
        const float3 waterRayDirection = WorldRayDirection();

        // The triangle quad is a conservative broad-phase surface. Re-solve
        // the ray against the analytic base + body + impact height field so
        // the visible hit and its depth move with the waves and floating props.
        [unroll]
        for (uint iteration = 0; iteration < 3; ++iteration) {
            float2 intersectionGradient;
            float intersectionHeight;
            float intersectionFoam;
            evaluateOceanInteractionSurface(x, z, t, pixelFootprint,
                intersectionHeight, intersectionGradient, intersectionFoam);
            const float residual = hitPos.y - intersectionHeight;
            const float rayDerivative = waterRayDirection.y
                - dot(intersectionGradient, float2(waterRayDirection.x, waterRayDirection.z));
            if (!isfinite(residual) || !isfinite(rayDerivative) || abs(residual) < 0.001f ||
                abs(rayDerivative) < 0.12f) break;

            const float maxRayStep = min(2.0f, 0.75f / max(abs(waterRayDirection.y), 0.25f));
            const float correction = clamp(residual / rayDerivative, -maxRayStep, maxRayStep);
            const float nextRayT = max(0.001f, waterRayT - correction);
            if (!isfinite(nextRayT) || abs(nextRayT - waterRayT) < 1.0e-5f) break;
            waterRayT = nextRayT;
            hitPos = waterRayOrigin + waterRayDirection * waterRayT;
            x = hitPos.x;
            z = hitPos.z;
        }

        // Re-evaluate shading at the corrected point. Height and waveGradient
        // below remain the exact analytic pair used by the Newton iterations.
        evaluateOceanSurface(x, z, t, pixelFootprint, waveHeight, waveGradient);
        const uint bodyCount = min(g_SceneCB.oceanBodyCount, 8u);
        [loop]
        for (uint bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
            const float4 body = g_SceneCB.oceanBodyPositionRadius[bodyIndex];
            const float4 motion = g_SceneCB.oceanBodyVelocityImpact[bodyIndex];
            const float4 impactOrigin = g_SceneCB.oceanBodyImpactPositionRadius[bodyIndex];
            const float verticalVelocity = g_SceneCB.oceanBodyVerticalState[bodyIndex].x;
            const float2 delta = float2(x - body.x, z - body.z);
            const float distanceToBody = length(delta);
            const float radius = max(body.w, 0.25f);

            // The CPU computes wetness from the same base wave and body half-height
            // used by Jolt. A negative core and positive annulus approximate volume
            // displacement around a floating body, even when it is nearly still.
            const float submersion = saturate(g_SceneCB.oceanBodyVerticalState[bodyIndex].z);
            const float radiusSquared = radius * radius;
            const float radialSquared = dot(delta, delta) / radiusSquared;
            const float normalizedRadius = sqrt(radialSquared);
            const float innerDisplacement = exp(-radialSquared);
            const float ridgeCoordinate = (radialSquared - 2.25f) / 2.0f;
            const float ridgeDisplacement = exp(-ridgeCoordinate * ridgeCoordinate);
            const float displacementProfile = -innerDisplacement + 0.2987677926f * ridgeDisplacement;
            const float displacementAmplitude = min(0.45f * radius, 0.45f) * submersion;
            waveHeight += displacementAmplitude * displacementProfile;
            const float ridgeSlopeFactor = 2.0f * 0.2987677926f * ridgeDisplacement * ridgeCoordinate;
            waveGradient += displacementAmplitude * (delta / radiusSquared)
                * (2.0f * innerDisplacement - ridgeSlopeFactor);
            if (submersion > 0.01f) {
                const float bodyRim = abs(distanceToBody - 1.36f * radius);
                impactFoam = max(impactFoam, 0.22f * submersion
                    * (1.0f - smoothstep(0.08f, 0.34f, bodyRim)));
            }

            const float impactAge = t - motion.z;
            const float impactStrength = saturate(motion.w);
            if (impactStrength > 0.001f && impactAge >= 0.0f && impactAge < 4.5f) {
                // Ripple rings stay at the point where the body entered the water.
                // Using the body's live position here made old rings follow its bounce.
                const float2 impactDelta = float2(x - impactOrigin.x, z - impactOrigin.z);
                const float impactDistanceRoot = sqrt(max(dot(impactDelta, impactDelta), 0.0f) + 0.0004f);
                const float impactDistance = impactDistanceRoot - 0.02f;
                const float phase = 4.6f * impactDistance - 7.2f * impactAge;
                const float decay = exp(-0.27f * impactDistance - 0.92f * impactAge);
                const float amplitude = 0.23f * impactStrength * decay;
                waveHeight += amplitude * sin(phase);
                const float radialDerivative = amplitude
                    * (4.6f * cos(phase) - 0.27f * sin(phase));
                waveGradient += radialDerivative * (impactDelta / impactDistanceRoot);
                const float ringDistance = abs(impactDistance - radius * 0.8f - 1.8f * impactAge);
                impactFoam = max(impactFoam, impactStrength * exp(-2.0f * impactAge)
                    * (1.0f - smoothstep(0.04f, 0.22f, ringDistance)));
            }

            // Vertical bobbing drives smaller continuous waves between discrete entries.
            // The derivative is analytic so normals remain consistent with the height.
            const float bodyDistanceRoot = sqrt(max(dot(delta, delta), 0.0f) + 0.0004f);
            const float smoothBodyDistance = bodyDistanceRoot - 0.02f;
            const float bobEnergy = submersion * saturate(abs(verticalVelocity) * 0.25f);
            if (bobEnergy > 0.01f) {
                const float bobPhase = 4.0f * smoothBodyDistance - 2.8f * t
                    + (verticalVelocity < 0.0f ? 0.0f : 3.14159265f);
                const float bobEnvelope = exp(-0.5f * smoothBodyDistance);
                const float bobAmplitude = 0.10f * bobEnergy * bobEnvelope;
                waveHeight += bobAmplitude * sin(bobPhase);
                const float radialBobDerivative = bobAmplitude
                    * (4.0f * cos(bobPhase) - 0.5f * sin(bobPhase));
                waveGradient += radialBobDerivative * (delta / bodyDistanceRoot);
            }

            const float2 horizontalVelocity = motion.xy;
            const float horizontalSpeed = length(horizontalVelocity);
            if (horizontalSpeed > 0.08f) {
                const float2 forward = horizontalVelocity / horizontalSpeed;
                const float2 side = float2(-forward.y, forward.x);
                const float along = dot(delta, forward);
                const float across = dot(delta, side);
                const float smoothAbsAcross = sqrt(across * across + 0.0004f) - 0.02f;
                const float wakeEnvelope = exp(-max(-along, 0.0f) * 0.22f - smoothAbsAcross * 0.48f);
                const float wakePhase = 1.45f * along + 2.8f * across - 1.3f * t;
                const float wakeAmplitude = 0.10f * saturate(horizontalSpeed * 0.16f);
                waveHeight += wakeAmplitude * wakeEnvelope * sin(wakePhase);
                const float envelopeAlongDerivative = along < 0.0f ? 0.22f : 0.0f;
                const float envelopeAcrossDerivative = -0.48f * across / sqrt(across * across + 0.0004f);
                const float derivativeAlong = wakeAmplitude * wakeEnvelope
                    * (envelopeAlongDerivative * sin(wakePhase) + 1.45f * cos(wakePhase));
                const float derivativeAcross = wakeAmplitude * wakeEnvelope
                    * (envelopeAcrossDerivative * sin(wakePhase) + 2.8f * cos(wakePhase));
                waveGradient += derivativeAlong * forward + derivativeAcross * side;
            }
        }
        // Short shading-only components fill the gaps between the buoyancy
        // waves. Keep the coherent components subtle and add filtered noise
        // derivatives so the surface does not resolve into parallel stripes.
        const float detailPhase0 = 2.8f * x + 3.6f * z + 2.2f * t;
        const float detailPhase1 = -5.5f * x + 3.0f * z - 4.0f * t;
        const float detailPhase2 = 9.0f * x - 6.0f * z + 7.0f * t;
        if (all(isfinite(float3(detailPhase0, detailPhase1, detailPhase2)))) {
            const float detailFade0 = 1.0f - smoothstep(0.012f, 0.085f, pixelFootprint);
            const float detailFade1 = 1.0f - smoothstep(0.006f, 0.045f, pixelFootprint);
            const float detailFade2 = 1.0f - smoothstep(0.003f, 0.022f, pixelFootprint);
            const float detailAmp0 = 0.016f * detailFade0;
            const float detailAmp1 = 0.008f * detailFade1;
            const float detailAmp2 = 0.003f * detailFade2;
            waveGradient.x += detailAmp0 * 2.8f * cos(detailPhase0)
                            - detailAmp1 * 5.5f * cos(detailPhase1)
                            + detailAmp2 * 9.0f * cos(detailPhase2);
            waveGradient.y += detailAmp0 * 3.6f * cos(detailPhase0)
                            + detailAmp1 * 3.0f * cos(detailPhase1)
                            - detailAmp2 * 6.0f * cos(detailPhase2);
        }
        // Multi-scale, analytic noise gradients give the water irregular
        // wind ripples without adding geometry or changing Jolt's wave field.
        const float2 rippleP0 = float2(1.35f * (x + 0.31f * z) + 0.12f * t,
                                       1.35f * (z - 0.17f * x) - 0.08f * t);
        const float2 rippleP1 = float2(4.2f * (x - 0.23f * z) - 0.31f * t,
                                       4.2f * (z + 0.19f * x) + 0.22f * t);
        const float2 rippleP2 = float2(10.0f * (x + 0.13f * z) + 0.72f * t,
                                       10.0f * (z - 0.11f * x) - 0.51f * t);
        if (all(isfinite(float4(rippleP0, rippleP1))) && all(isfinite(rippleP2))) {
            const float rippleFade0 = 1.0f - smoothstep(0.30f, 1.15f, 1.35f * pixelFootprint);
            const float rippleFade1 = 1.0f - smoothstep(0.30f, 1.15f, 4.2f * pixelFootprint);
            const float rippleFade2 = 1.0f - smoothstep(0.30f, 1.15f, 10.0f * pixelFootprint);
            const float2 ripple0 = oceanValueNoiseGradient(rippleP0);
            const float2 ripple1 = oceanValueNoiseGradient(rippleP1);
            const float2 ripple2 = oceanValueNoiseGradient(rippleP2);
            waveGradient.x += 0.080f * rippleFade0 * 1.35f * (ripple0.x - 0.17f * ripple0.y)
                            + 0.024f * rippleFade1 * 4.2f * (ripple1.x + 0.19f * ripple1.y)
                            + 0.0045f * rippleFade2 * 10.0f * (ripple2.x - 0.11f * ripple2.y);
            waveGradient.y += 0.080f * rippleFade0 * 1.35f * (0.31f * ripple0.x + ripple0.y)
                            + 0.024f * rippleFade1 * 4.2f * (-0.23f * ripple1.x + ripple1.y)
                            + 0.0045f * rippleFade2 * 10.0f * (0.13f * ripple2.x + ripple2.y);
        }
        const float slopeLength = length(waveGradient);
        worldNormal = safeNormalize(float3(-waveGradient.x, 1.0f, -waveGradient.y), float3(0.0f, 1.0f, 0.0f));
        // The DXR water primitive is a flat proxy while the shading normal
        // describes an undisplaced height field. Clamp only invalid backfacing
        // shading normals toward the geometric up hemisphere; flipping the
        // normal would create an underwater Fresnel/reflection discontinuity.
        const float minimumWaterNdotV = max(0.08f, 0.70f * saturate(V.y));
        const float waterNdotV = dot(worldNormal, V);
        if (waterNdotV < minimumWaterNdotV) {
            const float correction = saturate((minimumWaterNdotV - waterNdotV) / max(minimumWaterNdotV, 1.0e-4f));
            worldNormal = safeNormalize(lerp(worldNormal, float3(0.0f, 1.0f, 0.0f), correction),
                                        float3(0.0f, 1.0f, 0.0f));
        }
        if (g_SceneCB.renderMode == 2) {
            payload.color = worldNormal * 0.5f + 0.5f;
        } else if (g_SceneCB.renderMode == 3) {
            payload.color = albedo;
        } else {
            const float3 reflectionDirection = safeNormalize(reflect(-V, worldNormal), worldNormal);
            float3 reflectionColor = evaluateSky(reflectionDirection, false);
            if (g_SceneCB.enableRayTracedReflections > 0.5f && reflectionDirection.y > 0.015f) {
                RayDesc reflectionRay;
                reflectionRay.Origin = hitPos + worldNormal * 0.025f;
                // The BLAS broad phase remains at mean Y=0 even though this hit
                // is corrected onto the displaced surface. Start above both
                // representations so the reflection ray cannot self-hit the
                // flat proxy when a trough lies below its mean plane.
                reflectionRay.Origin.y = max(hitPos.y, 0.0f) + 0.03f;
                reflectionRay.Direction = reflectionDirection;
                reflectionRay.TMin = 0.01f;
                reflectionRay.TMax = 1000.0f;

                RayPayload reflectionPayload;
                reflectionPayload.color = 0.0f;
                reflectionPayload.albedo = 0.0f;
                reflectionPayload.normal = 0.0f;
                reflectionPayload.hitPos = 0.0f;
                reflectionPayload.nonDiffuseRadiance = 0.0f;
                reflectionPayload.hitT = -1.0f;
                reflectionPayload.sampleSeed = payload.sampleSeed;
                reflectionPayload.sampleStream = payload.sampleStream | 0x80000000u;
                TraceRay(g_Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, reflectionRay, reflectionPayload);
                reflectionColor = reflectionPayload.color;
            }
            const float nDotV = saturate(dot(worldNormal, V));
            const float fresnel = 0.02f + 0.98f * pow(1.0f - nDotV, 5.0f);
            const float3 sunDirection = safeNormalize(-g_SceneCB.sunDirection, float3(0.0f, 1.0f, 0.0f));
            const float sunNdotL = saturate(dot(worldNormal, sunDirection));
            const float3 deepWater = albedo * (0.32f + 0.48f * sunNdotL);
            const float3 waterHalfVector = safeNormalize(V + sunDirection, worldNormal);
            // Unresolved wind ripples broaden the sun lobe. The normal field
            // supplies detail; this roughness prevents a hard solar streak.
            const float waterRoughness = 0.34f;
            const float waterNdf = distributionGGX(worldNormal, waterHalfVector, waterRoughness);
            const float waterGeometry = geometrySmith(worldNormal, V, sunDirection, waterRoughness);
            const float3 waterFresnel = fresnelSchlick(saturate(dot(waterHalfVector, V)), float3(0.02f, 0.02f, 0.02f));
            const float3 waterSpecular = waterNdf * waterGeometry * waterFresnel
                / max(4.0f * nDotV * sunNdotL, 1.0e-4f)
                // Ocean demo uses a calibrated scale for this simplified
                // directional-light glint; it is not an area-light integral.
                * g_SceneCB.sunColor.rgb * (g_SceneCB.sunIntensity * 0.22f) * sunNdotL;
            const float foamNoise = oceanValueNoise(float2(x, z) * 1.7f + float2(t * 0.18f, -t * 0.11f));
            const float foamCrest = saturate((waveHeight - 0.35f) * 1.5f + (slopeLength - 0.80f) * 0.28f);
            const float foam = saturate((foamCrest - 0.82f) * 6.0f) * smoothstep(0.58f, 0.86f, foamNoise);
            const float3 foamColor = float3(0.56f, 0.70f, 0.74f);
            payload.color = lerp(deepWater, max(reflectionColor, 0.0f), fresnel)
                + waterSpecular
                + foamColor * max(foam * 0.22f, impactFoam * 0.62f);
        }
        payload.albedo = albedo;
        payload.normal = worldNormal;
        payload.hitPos = hitPos;
        payload.hitT = waterRayT;
        payload.roughness = mat.roughness;
        return;
    }

    float metallic = (g_SceneCB.metallicOverride >= 0.0f) ? g_SceneCB.metallicOverride : mat.metallic;
    float roughness = (g_SceneCB.roughnessOverride >= 0.0f) ? g_SceneCB.roughnessOverride : mat.roughness;
    roughness = clamp(roughness, 0.08f, 0.98f);
    metallic = clamp(metallic, 0.0f, 1.0f);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    if (mat.metallicRoughnessTextureId >= 0) {
        const float materialLod = estimateTextureLod(v0, v1, v2, RayTCurrent());
        const float2 metallicRoughness = g_Textures.SampleLevel(
            g_LinearSampler, float3(uv, mat.metallicRoughnessTextureId), materialLod).gb;
        roughness = clamp(roughness * metallicRoughness.x, 0.04f, 0.98f);
        metallic = saturate(metallic * metallicRoughness.y);
        F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    }

    const float3 viewFresnel = fresnelSchlick(saturate(dot(worldNormal, V)), F0);
    const float maxViewFresnel = max(viewFresnel.r, max(viewFresnel.g, viewFresnel.b));
    const uint reflectionSampleIndex = payload.sampleStream & 0x00ffffffu;
    // Use randomized systematic half-rate sampling: the per-pixel hash chooses
    // which sample-index parity is traced, so each consecutive pair contributes
    // one reflection ray. Weighting by 1/p (p=0.5) preserves the estimator mean
    // across the complementary Owen-scrambled Sobol subsequences.
    const bool traceReflectionThisSample = ((payload.sampleSeed ^ reflectionSampleIndex) & 1u) == 0u;
    const bool reflectionIsSignificant =
        (metallic > 0.15f && roughness < 0.70f) ||
        // Polished dielectrics have only ~4% normal-incidence F0. Do not
        // suppress their physically valid reflection ray at normal view angles.
        (roughness < 0.42f && maxViewFresnel > 0.0f);
    float3 tracedReflection = 0.0f;
    if (g_SceneCB.renderMode <= 1u && (payload.sampleStream & 0x40000000u) != 0u &&
        g_SceneCB.enableRayTracedReflections > 0.5f &&
        traceReflectionThisSample &&
        reflectionIsSignificant) {
        // Sample the isotropic GGX visible-normal distribution (VNDF). Its
        // reflected-direction PDF is D(H) G1(V) / (4 N.V), so the Cook-Torrance
        // throughput simplifies exactly to F * G1(L). This avoids wasting
        // samples below the macro-surface and reduces rough-stone reflection noise.
        // Keep reflection randomness in its own Sobol dimensions so enabling
        // reflections does not perturb the primary ray's shadow/GI samples.
        uint reflectionSampleStream = (payload.sampleStream & 0xc0ffffffu) | (16u << 24u);
        const float u1 = sampleSobol2D(reflectionSampleStream, payload.sampleSeed);
        const float u2 = sampleSobol2D(reflectionSampleStream, payload.sampleSeed);
        const float alpha = max(roughness * roughness, 0.0025f);
        const float3 microNormal = sampleGGXVisibleNormal(worldNormal, V, alpha, float2(u1, u2));
        const float nDotVReflect = saturate(dot(worldNormal, V));
        const float nDotHReflect = saturate(dot(worldNormal, microNormal));
        const float vDotHReflect = saturate(dot(V, microNormal));
        const float3 reflectionDirection = safeNormalize(reflect(-V, microNormal), worldNormal);
        const float nDotLReflect = saturate(dot(worldNormal, reflectionDirection));

        if (nDotVReflect > 1.0e-4f && nDotHReflect > 1.0e-4f &&
            vDotHReflect > 1.0e-4f && nDotLReflect > 1.0e-4f) {
            RayDesc reflectionRay;
            reflectionRay.Origin = hitPos + worldNormal * 0.005f;
            reflectionRay.Direction = reflectionDirection;
            reflectionRay.TMin = 0.001f;
            reflectionRay.TMax = 1000.0f;

            RayPayload reflectionPayload;
            reflectionPayload.color = 0.0f;
            reflectionPayload.albedo = 0.0f;
            reflectionPayload.normal = 0.0f;
            reflectionPayload.hitPos = 0.0f;
            reflectionPayload.nonDiffuseRadiance = 0.0f;
            reflectionPayload.hitT = -1.0f;
            reflectionPayload.roughness = 1.0f;
            reflectionPayload.sampleSeed = payload.sampleSeed;
            reflectionPayload.sampleStream = reflectionSampleStream | 0x80000000u;
            TraceRay(g_Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, reflectionRay, reflectionPayload);

            const float3 fresnel = fresnelSchlick(vDotHReflect, F0);
            const float brdfOverPdf = smithGGXG1(nDotLReflect, alpha);
            tracedReflection = max(reflectionPayload.color, 0.0f) * fresnel * brdfOverPdf * 2.0f;
        }
    }

    // Direct Sun Lighting with Soft Ray-Traced Shadow
    float3 L = safeNormalize(-g_SceneCB.sunDirection, float3(0.0f, 1.0f, 0.0f));
    float3 H = safeNormalize(V + L, worldNormal);
    float NdotL = saturate(dot(worldNormal, L));
    float NdotV = saturate(dot(worldNormal, V));

    float shadowVisibility = 1.0f;
    if (g_SceneCB.enableShadows != 0 && NdotL > 0.0f) {
        shadowVisibility = 0.0f;
        float3 jitteredL = L;
        if (g_SceneCB.shadowSoftness > 0.0f) {
            float3 tangent = abs(L.y) < 0.999f ? safeNormalize(cross(float3(0, 1, 0), L), float3(1, 0, 0))
                                                : safeNormalize(cross(float3(1, 0, 0), L), float3(0, 0, 1));
            float3 bitangent = cross(L, tangent);
            float diskR = 0.007f * sqrt(sampleSobol2D(payload.sampleStream, payload.sampleSeed)) * g_SceneCB.shadowSoftness;
            float diskTheta = 2.0f * PI * sampleSobol2D(payload.sampleStream, payload.sampleSeed);
            jitteredL = safeNormalize(L + (tangent * cos(diskTheta) + bitangent * sin(diskTheta)) * diskR, L);
        }

        RayDesc shadowRay;
        shadowRay.Origin = hitPos + worldNormal * 0.005f;
        shadowRay.Direction = jitteredL;
        shadowRay.TMin = 0.001f;
        shadowRay.TMax = 1000.0f;

        ShadowPayload shadowPayload;
        shadowPayload.visibility = 0.0f;

        TraceRay(
            g_Scene,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
            0xFF,
            0, 0, 1, // Shadow Miss shader index = 1
            shadowRay,
            shadowPayload
        );
        shadowVisibility = shadowPayload.visibility;
    }

    // Direct PBR BRDF
    float NDF = distributionGGX(worldNormal, H, roughness);
    float G = geometrySmith(worldNormal, V, L, roughness);
    float3 F = fresnelSchlick(saturate(dot(H, V)), F0);

    float3 numerator = NDF * G * F;
    float denominator = 4.0f * NdotV * NdotL + 0.0001f;
    float3 specular = numerator / denominator;

    float3 kS = F;
    float3 kD = (float3(1.0f, 1.0f, 1.0f) - kS) * (1.0f - metallic);
    float3 diffuse = kD * (albedo / PI);

    float3 directRadiance = (diffuse + specular) * g_SceneCB.sunColor.rgb * g_SceneCB.sunIntensity * NdotL * shadowVisibility;

    // Emissive
    float3 emissive = albedo * mat.emissive;
    payload.nonDiffuseRadiance = specular * g_SceneCB.sunColor.rgb * g_SceneCB.sunIntensity * NdotL * shadowVisibility
                               + emissive + tracedReflection;

    // Physical surface radiance: Direct sun light + emissive
    // Physical Sky Irradiance Base: In open-roof architectural models like Sponza,
    // diffuse hemispherical skylight illuminates all surfaces visible to the open sky.
    float skyHemi = saturate(0.5f + 0.5f * worldNormal.y);
    float3 skyLight = lerp(g_SceneCB.skyHorizonColor.rgb * 0.55f, g_SceneCB.skyZenithColor.rgb, skyHemi) * skyHemi * 0.32f;
    float3 ambientRecess = g_SceneCB.skyZenithColor.rgb * 0.04f;
    float ambientMultiplier = g_SceneCB.padPrev3 > 0.0f ? g_SceneCB.padPrev3 : 1.0f;
    float3 ambientBase = (skyLight + ambientRecess) * albedo * ambientMultiplier;

    if (g_SceneCB.renderMode == 1) {
        // Direct + RT Shadows only (single bounce mode with sky ambient fill)
        payload.color = directRadiance + emissive + ambientBase + tracedReflection;
    } else if (g_SceneCB.renderMode == 2) {
        // Normals debug mode
        payload.color = worldNormal * 0.5f + 0.5f;
    } else if (g_SceneCB.renderMode == 3) {
        // Albedo mode
        payload.color = albedo;
    } else {
        // Full Path Traced GI mode: Direct sun + Emissive + Physical Sky Ambient
        // Multi-bounce diffuse inter-reflections and banner color bleeding are gathered
        // cleanly by secondary rays in RayGenShader
        payload.color = directRadiance + emissive + ambientBase + tracedReflection;
    }

    payload.albedo = albedo;
    payload.normal = worldNormal;
    payload.hitPos = hitPos;
    payload.hitT = RayTCurrent();
    payload.roughness = roughness;
}

// -------------------------------------------------------------
// Primary Miss Shader (Atmosphere & Sun)
// -------------------------------------------------------------
[shader("miss")]
void MissShader(inout RayPayload payload) {
    // Reflected rays already evaluate direct sun at a material hit. Excluding
    // the analytic solar disk on reflected misses avoids counting that source
    // twice; primary camera misses still show the disk and its soft glow.
    const bool reflectedRay = (payload.sampleStream & 0x80000000u) != 0u;
    payload.color = evaluateSky(WorldRayDirection(), !reflectedRay);
    payload.hitT = -1.0f;
}

// -------------------------------------------------------------
// Shadow Miss Shader (Unoccluded = 1.0)
// -------------------------------------------------------------
[shader("miss")]
void ShadowMissShader(inout ShadowPayload payload) {
    payload.visibility = 1.0f;
}
