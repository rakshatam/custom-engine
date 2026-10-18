#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Engine {

struct OceanWaveSample {
    float height = 0.0f;
    float slopeX = 0.0f;
    float slopeZ = 0.0f;
    float verticalVelocity = 0.0f;
};

// CPU counterpart of evaluateOceanSurface() in RayTracingPBR.hlsl. Keep the
// coefficient arrays in sync with the shader's wave phases and amplitudes.
inline OceanWaveSample evaluateOceanWave(float x, float z, float timeSeconds) {
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(timeSeconds)) {
        return {};
    }

    constexpr std::array<float, 5> amplitude{0.320f, 0.190f, 0.105f, 0.045f, 0.014f};
    constexpr std::array<float, 5> waveX{0.28f, -0.74f, 1.03f, -1.65f, 1.25f};
    constexpr std::array<float, 5> waveZ{0.62f, 0.41f, -0.78f, -0.18f, 1.82f};
    constexpr std::array<float, 5> angularFrequency{0.92f, -1.17f, 1.75f, -2.35f, 3.20f};

    OceanWaveSample result;
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
    if (!std::isfinite(result.height) || !std::isfinite(result.slopeX) ||
        !std::isfinite(result.slopeZ) || !std::isfinite(result.verticalVelocity)) return {};
    return result;
}

struct OceanBodyWaveState {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float radius = 0.0f;
    float halfHeight = 0.0f;
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    float velocityZ = 0.0f;
};

struct OceanImpactWaveState {
    float x = 0.0f;
    float z = 0.0f;
    float time = 0.0f;
    float strength = 0.0f;
};

// Outward decaying radial wave from a discrete impact. A smooth radius keeps
// the center differentiable instead of giving sin(k*r) an undefined radial
// gradient at r=0. Keep coefficients and smoothing radius in sync with HLSL.
inline OceanWaveSample evaluateOceanImpactWave(float x, float z, float timeSeconds,
                                               const OceanImpactWaveState& impact) {
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(timeSeconds) ||
        !std::isfinite(impact.x) || !std::isfinite(impact.z) ||
        !std::isfinite(impact.time) || !std::isfinite(impact.strength)) return {};
    const float age = timeSeconds - impact.time;
    if (!std::isfinite(age) || age < 0.0f || age >= 4.5f || impact.strength <= 0.001f) return {};

    constexpr float smoothingRadius = 0.02f;
    constexpr float waveNumber = 4.6f;
    constexpr float angularFrequency = 7.2f;
    constexpr float radialDecay = 0.27f;
    constexpr float temporalDecay = 0.92f;
    constexpr float baseAmplitude = 0.23f;
    const float dx = x - impact.x;
    const float dz = z - impact.z;
    const float smoothRadius = std::sqrt(dx * dx + dz * dz + smoothingRadius * smoothingRadius);
    if (!std::isfinite(smoothRadius) || smoothRadius <= 0.0f) return {};
    const float radialDistance = smoothRadius - smoothingRadius;
    const float phase = waveNumber * radialDistance - angularFrequency * age;
    const float decay = std::exp(-radialDecay * radialDistance - temporalDecay * age);
    const float amplitude = baseAmplitude * std::clamp(impact.strength, 0.0f, 1.0f) * decay;
    const float phaseSin = std::sin(phase);
    const float phaseCos = std::cos(phase);
    const float radialDerivative = amplitude *
        (waveNumber * phaseCos - radialDecay * phaseSin);

    OceanWaveSample result;
    result.height = amplitude * phaseSin;
    result.slopeX = radialDerivative * dx / smoothRadius;
    result.slopeZ = radialDerivative * dz / smoothRadius;
    result.verticalVelocity = amplitude *
        (-temporalDecay * phaseSin - angularFrequency * phaseCos);
    if (!std::isfinite(result.height) || !std::isfinite(result.slopeX) ||
        !std::isfinite(result.slopeZ) || !std::isfinite(result.verticalVelocity)) return {};
    return result;
}

// Gaussian depression plus a smooth Gaussian in r^2. The coefficient is
// chosen so the radial integral is zero: 2∫r profile(r²)dr = 0 on the plane.
inline float oceanBodyDisplacementProfile(float radialSquared) {
    if (!std::isfinite(radialSquared) || radialSquared < 0.0f) return 0.0f;
    constexpr float ridgeWeight = 0.2987677926f;
    const float ridgeCoordinate = (radialSquared - 2.25f) / 2.0f;
    return -std::exp(-radialSquared) + ridgeWeight * std::exp(-ridgeCoordinate * ridgeCoordinate);
}

inline float oceanBodyDisplacementProfileSlopeFactor(float radialSquared) {
    if (!std::isfinite(radialSquared) || radialSquared < 0.0f) return 0.0f;
    constexpr float ridgeWeight = 0.2987677926f;
    const float ridgeCoordinate = (radialSquared - 2.25f) / 2.0f;
    const float ridge = std::exp(-ridgeCoordinate * ridgeCoordinate);
    return 2.0f * std::exp(-radialSquared)
        - 2.0f * ridgeWeight * ridge * ridgeCoordinate;
}

inline float oceanBodySubmersion(const OceanWaveSample& waterAtBody,
                                 const OceanBodyWaveState& body) {
    if (!std::isfinite(waterAtBody.height) || !std::isfinite(body.y) ||
        !std::isfinite(body.halfHeight) || body.halfHeight <= 1.0e-4f) {
        return 0.0f;
    }
    const float raw = (waterAtBody.height + body.halfHeight - body.y) /
                      (2.0f * body.halfHeight);
    return std::isfinite(raw) ? std::clamp(raw, 0.0f, 1.0f) : 0.0f;
}

// A smooth, persistent body-displacement field. The negative center Gaussian
// is balanced by a wider positive annulus: integral(F(r)) dA = 0 for an
// infinite plane, so the approximation does not create net water volume.
// This is a height-field input for buoyancy and shading. The DXR water quad is
// still flat broad-phase geometry, but the closest-hit shader corrects the ray
// hit against this analytic field before writing depth or shading the surface.
inline OceanWaveSample evaluateOceanBodyDisplacement(float x, float z, float timeSeconds,
                                                      const OceanBodyWaveState& body) {
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(timeSeconds) ||
        !std::isfinite(body.x) || !std::isfinite(body.y) || !std::isfinite(body.z) ||
        !std::isfinite(body.radius) || !std::isfinite(body.halfHeight) ||
        !std::isfinite(body.velocityX) || !std::isfinite(body.velocityY) ||
        !std::isfinite(body.velocityZ) || body.radius <= 1.0e-4f || body.halfHeight <= 1.0e-4f) {
        return {};
    }

    const OceanWaveSample waterAtBody = evaluateOceanWave(body.x, body.z, timeSeconds);
    const float rawSubmersion = (waterAtBody.height + body.halfHeight - body.y) /
                                (2.0f * body.halfHeight);
    if (!std::isfinite(rawSubmersion) || rawSubmersion <= 0.0f) return {};
    const float submersion = std::clamp(rawSubmersion, 0.0f, 1.0f);
    const float submersionRate = rawSubmersion < 1.0f
        ? (waterAtBody.verticalVelocity + waterAtBody.slopeX * body.velocityX +
           waterAtBody.slopeZ * body.velocityZ - body.velocityY) / (2.0f * body.halfHeight)
        : 0.0f;
    const float amplitudeScale = std::min(0.45f * body.radius, 0.45f);
    const float amplitude = amplitudeScale * submersion;
    const float amplitudeRate = amplitudeScale * submersionRate;

    const float dx = x - body.x;
    const float dz = z - body.z;
    const float radiusSquared = body.radius * body.radius;
    const float radialSquared = (dx * dx + dz * dz) / radiusSquared;
    if (!std::isfinite(radialSquared)) return {};

    const float profile = oceanBodyDisplacementProfile(radialSquared);
    const float profileSlopeFactor = oceanBodyDisplacementProfileSlopeFactor(radialSquared);
    const float profileSlopeX = dx / radiusSquared * profileSlopeFactor;
    const float profileSlopeZ = dz / radiusSquared * profileSlopeFactor;

    OceanWaveSample result;
    result.height = amplitude * profile;
    result.slopeX = amplitude * profileSlopeX;
    result.slopeZ = amplitude * profileSlopeZ;
    result.verticalVelocity = amplitudeRate * profile - amplitude *
        (profileSlopeX * body.velocityX + profileSlopeZ * body.velocityZ);
    if (!std::isfinite(result.height) || !std::isfinite(result.slopeX) ||
        !std::isfinite(result.slopeZ) || !std::isfinite(result.verticalVelocity)) {
        return {};
    }
    return result;
}

inline bool tryGetOrientedBoxVerticalHalfExtent(const float quaternion[4], float halfX,
                                                float halfY, float halfZ, float& outHalfExtent) {
    if (!quaternion || !std::isfinite(halfX) || !std::isfinite(halfY) || !std::isfinite(halfZ) ||
        halfX <= 0.0f || halfY <= 0.0f || halfZ <= 0.0f) {
        return false;
    }
    const float qx = quaternion[0], qy = quaternion[1], qz = quaternion[2], qw = quaternion[3];
    if (!std::isfinite(qx) || !std::isfinite(qy) || !std::isfinite(qz) || !std::isfinite(qw)) return false;
    const float normSquared = qx * qx + qy * qy + qz * qz + qw * qw;
    if (!std::isfinite(normSquared) || normSquared <= 1.0e-12f) return false;
    const float invNorm = 1.0f / std::sqrt(normSquared);
    const float x = qx * invNorm, y = qy * invNorm, z = qz * invNorm, w = qw * invNorm;
    const float worldYFromLocalX = 2.0f * (x * y + w * z);
    const float worldYFromLocalY = 1.0f - 2.0f * (x * x + z * z);
    const float worldYFromLocalZ = 2.0f * (y * z - w * x);
    outHalfExtent = std::abs(worldYFromLocalX) * halfX +
                    std::abs(worldYFromLocalY) * halfY +
                    std::abs(worldYFromLocalZ) * halfZ;
    return std::isfinite(outHalfExtent) && outHalfExtent > 0.0f;
}

inline bool tryGetOrientedEllipsoidVerticalHalfExtent(const float quaternion[4], float radiusX,
                                                       float radiusY, float radiusZ, float& outHalfExtent) {
    if (!quaternion || !std::isfinite(radiusX) || !std::isfinite(radiusY) || !std::isfinite(radiusZ) ||
        radiusX <= 0.0f || radiusY <= 0.0f || radiusZ <= 0.0f) {
        return false;
    }
    const float qx = quaternion[0], qy = quaternion[1], qz = quaternion[2], qw = quaternion[3];
    if (!std::isfinite(qx) || !std::isfinite(qy) || !std::isfinite(qz) || !std::isfinite(qw)) return false;
    const float normSquared = qx * qx + qy * qy + qz * qz + qw * qw;
    if (!std::isfinite(normSquared) || normSquared <= 1.0e-12f) return false;
    const float invNorm = 1.0f / std::sqrt(normSquared);
    const float x = qx * invNorm, y = qy * invNorm, z = qz * invNorm, w = qw * invNorm;
    const float worldYFromLocalX = 2.0f * (x * y + w * z);
    const float worldYFromLocalY = 1.0f - 2.0f * (x * x + z * z);
    const float worldYFromLocalZ = 2.0f * (y * z - w * x);
    const float supportX = radiusX * worldYFromLocalX;
    const float supportY = radiusY * worldYFromLocalY;
    const float supportZ = radiusZ * worldYFromLocalZ;
    outHalfExtent = std::sqrt(supportX * supportX + supportY * supportY + supportZ * supportZ);
    return std::isfinite(outHalfExtent) && outHalfExtent > 0.0f;
}

} // namespace Engine
