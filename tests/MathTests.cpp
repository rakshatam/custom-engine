#include "../src/core/Math.h"
#include "../src/ecs/Components.h"
#include "../src/ecs/OceanWave.h"
#include "OceanRayIntersectionReference.h"
#include "../src/ecs/GltfImportMath.h"
#include "../src/ecs/EntityIndex.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* name) {
    ++g_checks;
    if (condition) {
        std::cout << "[MathTest] PASS: " << name << '\n';
    } else {
        std::cerr << "[MathTest] FAIL: " << name << '\n';
        ++g_failures;
    }
}

template<class Function>
bool throwsDomainError(Function&& function) {
    try {
        function();
    } catch (const std::domain_error&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

double ggxG1Reference(double nDotDirection, double alpha) {
    const double nDot = std::clamp(nDotDirection, 0.0, 1.0);
    const double alphaSquared = alpha * alpha;
    return (2.0 * nDot) /
        (nDot + std::sqrt(alphaSquared + (1.0 - alphaSquared) * nDot * nDot));
}

double ggxDReference(double nDotHalf, double alpha) {
    const double alphaSquared = alpha * alpha;
    const double nDot = std::clamp(nDotHalf, 0.0, 1.0);
    const double denominator = nDot * nDot * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / (3.14159265358979323846 * denominator * denominator);
}

bool ggxVndfEstimatorIdentityHolds() {
    constexpr double roughnessValues[] = { 0.08, 0.18, 0.42, 0.70, 0.98 };
    constexpr double cosines[] = { 0.05, 0.2, 0.55, 0.9, 1.0 };
    constexpr double f0Values[] = { 0.04, 0.8 };
    for (double roughness : roughnessValues) {
        const double alpha = roughness * roughness;
        for (double nDotV : cosines) {
            const double g1V = ggxG1Reference(nDotV, alpha);
            for (double nDotL : cosines) {
                const double g1L = ggxG1Reference(nDotL, alpha);
                for (double nDotH : cosines) {
                    const double distribution = ggxDReference(nDotH, alpha);
                    for (double vDotH : cosines) {
                        const double fresnelFactor = std::pow(1.0 - vDotH, 5.0);
                        for (double f0 : f0Values) {
                            const double fresnel = f0 + (1.0 - f0) * fresnelFactor;
                            const double pdf = distribution * g1V / (4.0 * nDotV);
                            const double brdf = distribution * g1V * g1L * fresnel /
                                (4.0 * nDotV * nDotL);
                            const double explicitWeight = brdf * nDotL / pdf;
                            const double simplifiedWeight = fresnel * g1L;
                            if (!std::isfinite(explicitWeight) ||
                                std::abs(explicitWeight - simplifiedWeight) > 1.0e-10) {
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool complementaryHalfRateSequencesPreserveMean() {
    constexpr unsigned int sampleCount = 64;
    double fullSum = 0.0;
    double phaseSums[2] = { 0.0, 0.0 };
    unsigned int phaseCounts[2] = { 0u, 0u };
    for (unsigned int sample = 0; sample < sampleCount; ++sample) {
        const double x = static_cast<double>(sample);
        const double radiance = 0.25 + std::pow(std::sin(x * 0.37), 2.0) + 0.01 * x;
        fullSum += radiance;
        for (unsigned int phase = 0; phase < 2; ++phase) {
            if (((sample ^ phase) & 1u) == 0u) {
                phaseSums[phase] += 2.0 * radiance;
                ++phaseCounts[phase];
            }
        }
    }
    const double fullMean = fullSum / sampleCount;
    const double phasedMean = (phaseSums[0] + phaseSums[1]) / (2.0 * sampleCount);
    return phaseCounts[0] == sampleCount / 2 && phaseCounts[1] == sampleCount / 2 &&
        std::abs(fullMean - phasedMean) < 1.0e-12;
}

bool waterDielectricReflectionIsPhysicallyBounded() {
    constexpr double waterF0 = 0.02;
    constexpr double waterIor = 1.333;
    const double iorRatio = (waterIor - 1.0) / (waterIor + 1.0);
    const double derivedF0 = iorRatio * iorRatio;
    if (std::abs(derivedF0 - waterF0) > 5.0e-4) return false;

    double previousFresnel = 1.0;
    for (unsigned int step = 0; step <= 100; ++step) {
        const double nDotV = static_cast<double>(step) / 100.0;
        const double fresnel = waterF0 + (1.0 - waterF0) * std::pow(1.0 - nDotV, 5.0);
        if (!std::isfinite(fresnel) || fresnel < waterF0 || fresnel > 1.0 ||
            fresnel > previousFresnel + 1.0e-12) {
            return false;
        }
        previousFresnel = fresnel;
    }

    constexpr double nx = 0.0, ny = 1.0, nz = 0.0;
    constexpr double vx = 0.6, vy = 0.8, vz = 0.0;
    // HLSL reflect(-V,N): I - 2 dot(N,I) N, where I=-V.
    const double ix = -vx, iy = -vy, iz = -vz;
    const double incidentDotNormal = ix * nx + iy * ny + iz * nz;
    const double lx = ix - 2.0 * incidentDotNormal * nx;
    const double ly = iy - 2.0 * incidentDotNormal * ny;
    const double lz = iz - 2.0 * incidentDotNormal * nz;
    const double reflectedLength = std::sqrt(lx * lx + ly * ly + lz * lz);
    const double reflectedHemisphere = lx * nx + ly * ny + lz * nz;
    return std::abs(reflectedLength - 1.0) < 1.0e-12 &&
        std::abs(reflectedHemisphere - 0.8) < 1.0e-12;
}
} // namespace

int main() {
    using Engine::AABB;
    using Engine::Vec3;

    check(ggxVndfEstimatorIdentityHolds(),
          "GGX VNDF reflected PDF and Cook-Torrance throughput reduce to F times G1(L)");
    check(complementaryHalfRateSequencesPreserveMean(),
          "complementary half-rate reflection sequences preserve the full-sample estimator mean");
    check(waterDielectricReflectionIsPhysicallyBounded(),
          "water Schlick Fresnel matches the dielectric IOR and reflection stays unit-length in the upper hemisphere");

    Engine::EntityIndex entityIndex;
    check(entityIndex.rebuild({17u, 42u, 96u}) && entityIndex.find(42u) == 1u && !entityIndex.find(99u),
          "ECS stable-ID index resolves dense render indices and misses unknown IDs");
    check(!entityIndex.rebuild({17u, 17u}) && entityIndex.find(42u) == 1u,
          "duplicate ECS IDs are rejected without corrupting the previous index");
    check(!entityIndex.rebuild({Engine::NULL_ENTITY}) && entityIndex.size() == 3u,
          "null ECS IDs are rejected transactionally");

    AABB box;
    box.expand(Vec3(-1.0f, -1.0f, -1.0f));
    box.expand(Vec3(1.0f, 1.0f, 1.0f));

    float hitDistance = 0.0f;
    check(box.rayIntersect(Vec3(-2.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), hitDistance) &&
          std::abs(hitDistance - 1.0f) < 1.0e-5f,
          "axis-aligned hit returns the nearest positive distance");
    check(box.rayIntersect(Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), hitDistance) &&
          std::abs(hitDistance - 1.0f) < 1.0e-5f,
          "ray starting inside returns the exit distance");
    check(!box.rayIntersect(Vec3(-2.0f, 0.0f, -2.0f), Vec3(-1.0e-10f, 0.0f, 1.0f), hitDistance),
          "near-parallel ray outside a slab does not produce a false hit");
    check(!box.rayIntersect(Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 0.0f), hitDistance),
          "zero-length ray is rejected");

    check(throwsDomainError([] { (void)(Vec3(1.0f, 2.0f, 3.0f) / 0.0f); }),
          "zero vector divisor fails explicitly");
    check(throwsDomainError([] {
        (void)Vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f).normalized();
    }), "non-finite vector normalization fails explicitly");

    Engine::CameraComponent camera;
    camera.fov = std::numeric_limits<float>::quiet_NaN();
    camera.nearPlane = 0.0f;
    camera.farPlane = 0.0f;
    const DirectX::XMMATRIX projection = camera.getProjMatrix(std::numeric_limits<float>::quiet_NaN());
    DirectX::XMFLOAT4 determinant;
    DirectX::XMStoreFloat4(&determinant, DirectX::XMMatrixDeterminant(projection));
    check(std::isfinite(determinant.x) && std::abs(determinant.x) > 1.0e-12f,
          "invalid camera projection inputs are clamped to a finite matrix");

    constexpr float waveX = 1.37f;
    constexpr float waveZ = -2.11f;
    constexpr float waveTime = 3.75f;
    constexpr float finiteDifferenceStep = 1.0e-3f;
    const Engine::OceanWaveSample wave = Engine::evaluateOceanWave(waveX, waveZ, waveTime);
    const float finiteSlopeX = (Engine::evaluateOceanWave(waveX + finiteDifferenceStep, waveZ, waveTime).height -
                                Engine::evaluateOceanWave(waveX - finiteDifferenceStep, waveZ, waveTime).height) /
                               (2.0f * finiteDifferenceStep);
    const float finiteSlopeZ = (Engine::evaluateOceanWave(waveX, waveZ + finiteDifferenceStep, waveTime).height -
                                Engine::evaluateOceanWave(waveX, waveZ - finiteDifferenceStep, waveTime).height) /
                               (2.0f * finiteDifferenceStep);
    const float finiteVelocity = (Engine::evaluateOceanWave(waveX, waveZ, waveTime + finiteDifferenceStep).height -
                                  Engine::evaluateOceanWave(waveX, waveZ, waveTime - finiteDifferenceStep).height) /
                                 (2.0f * finiteDifferenceStep);
    check(std::isfinite(wave.height) && std::abs(finiteSlopeX - wave.slopeX) < 2.0e-4f,
          "analytic ocean X slope matches finite-difference height derivative");
    check(std::isfinite(wave.height) && std::abs(finiteSlopeZ - wave.slopeZ) < 2.0e-4f,
          "analytic ocean Z slope matches finite-difference height derivative");
    check(std::isfinite(wave.height) && std::abs(finiteVelocity - wave.verticalVelocity) < 2.0e-4f,
          "analytic ocean vertical velocity matches finite-difference time derivative");
    const Engine::OceanWaveSample invalidWave = Engine::evaluateOceanWave(
        std::numeric_limits<float>::infinity(), 0.0f, 0.0f);
    const Engine::OceanWaveSample overflowWave = Engine::evaluateOceanWave(
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    check(invalidWave.height == 0.0f && invalidWave.slopeX == 0.0f && invalidWave.slopeZ == 0.0f &&
          invalidWave.verticalVelocity == 0.0f,
          "non-finite ocean input returns a finite rest surface");
    check(overflowWave.height == 0.0f && overflowWave.slopeX == 0.0f && overflowWave.slopeZ == 0.0f &&
          overflowWave.verticalVelocity == 0.0f,
          "overflowing ocean phase returns a finite rest surface");

    const Engine::OceanImpactWaveState impact{ -0.4f, 0.7f, 0.65f, 0.9f };
    const float impactSampleX = 0.52f, impactSampleZ = -0.22f, impactSampleTime = 1.37f;
    const Engine::OceanWaveSample impactSample = Engine::evaluateOceanImpactWave(
        impactSampleX, impactSampleZ, impactSampleTime, impact);
    const float impactSlopeX = (
        Engine::evaluateOceanImpactWave(impactSampleX + finiteDifferenceStep, impactSampleZ,
                                        impactSampleTime, impact).height -
        Engine::evaluateOceanImpactWave(impactSampleX - finiteDifferenceStep, impactSampleZ,
                                        impactSampleTime, impact).height) /
        (2.0f * finiteDifferenceStep);
    const float impactSlopeZ = (
        Engine::evaluateOceanImpactWave(impactSampleX, impactSampleZ + finiteDifferenceStep,
                                        impactSampleTime, impact).height -
        Engine::evaluateOceanImpactWave(impactSampleX, impactSampleZ - finiteDifferenceStep,
                                        impactSampleTime, impact).height) /
        (2.0f * finiteDifferenceStep);
    const float impactVelocity = (
        Engine::evaluateOceanImpactWave(impactSampleX, impactSampleZ,
                                        impactSampleTime + finiteDifferenceStep, impact).height -
        Engine::evaluateOceanImpactWave(impactSampleX, impactSampleZ,
                                        impactSampleTime - finiteDifferenceStep, impact).height) /
        (2.0f * finiteDifferenceStep);
    check(std::abs(impactSlopeX - impactSample.slopeX) < 5.0e-4f &&
          std::abs(impactSlopeZ - impactSample.slopeZ) < 5.0e-4f,
          "smoothed impact-wave spatial derivatives match finite differences");
    check(std::abs(impactVelocity - impactSample.verticalVelocity) < 5.0e-4f,
          "impact-wave time derivative matches finite differences");
    const Engine::OceanWaveSample impactCenter = Engine::evaluateOceanImpactWave(
        impact.x, impact.z, impact.time, impact);
    check(std::abs(impactCenter.slopeX) < 1.0e-7f && std::abs(impactCenter.slopeZ) < 1.0e-7f &&
          std::abs(impactCenter.height) < 1.0e-7f,
          "smoothed radial impact starts with finite zero center height and gradient");
    const Engine::OceanWaveSample expiredImpact = Engine::evaluateOceanImpactWave(
        impactSampleX, impactSampleZ, impact.time + 4.5f, impact);
    check(expiredImpact.height == 0.0f && expiredImpact.slopeX == 0.0f &&
          expiredImpact.slopeZ == 0.0f && expiredImpact.verticalVelocity == 0.0f,
          "expired radial impact contributes no height, slope, or velocity");

    Engine::OceanBodyWaveState body;
    body.x = -0.3f;
    body.y = 0.2f;
    body.z = 0.4f;
    body.radius = 1.1f;
    body.halfHeight = 1.4f;
    body.velocityX = 0.23f;
    body.velocityY = 0.08f;
    body.velocityZ = -0.17f;
    const float bodyTime = 1.2f;
    const auto centerWater = Engine::evaluateOceanWave(body.x, body.z, bodyTime);
    body.y = centerWater.height + body.halfHeight - 0.60f * (2.0f * body.halfHeight);
    const float sampleX = 1.9f;
    const float sampleZ = -0.8f;
    const auto displacement = Engine::evaluateOceanBodyDisplacement(sampleX, sampleZ, bodyTime, body);
    const auto centerDisplacement = Engine::evaluateOceanBodyDisplacement(body.x, body.z, bodyTime, body);
    const float bodySlopeX = (Engine::evaluateOceanBodyDisplacement(sampleX + finiteDifferenceStep, sampleZ, bodyTime, body).height -
                              Engine::evaluateOceanBodyDisplacement(sampleX - finiteDifferenceStep, sampleZ, bodyTime, body).height) /
                             (2.0f * finiteDifferenceStep);
    const float bodySlopeZ = (Engine::evaluateOceanBodyDisplacement(sampleX, sampleZ + finiteDifferenceStep, bodyTime, body).height -
                              Engine::evaluateOceanBodyDisplacement(sampleX, sampleZ - finiteDifferenceStep, bodyTime, body).height) /
                             (2.0f * finiteDifferenceStep);
    auto movingBodyAt = [&](float time) {
        Engine::OceanBodyWaveState state = body;
        const float offset = time - bodyTime;
        state.x += state.velocityX * offset;
        state.y += state.velocityY * offset;
        state.z += state.velocityZ * offset;
        return state;
    };
    const float bodyVelocity = (
        Engine::evaluateOceanBodyDisplacement(sampleX, sampleZ, bodyTime + finiteDifferenceStep,
                                               movingBodyAt(bodyTime + finiteDifferenceStep)).height -
        Engine::evaluateOceanBodyDisplacement(sampleX, sampleZ, bodyTime - finiteDifferenceStep,
                                               movingBodyAt(bodyTime - finiteDifferenceStep)).height) /
        (2.0f * finiteDifferenceStep);
    check(Engine::oceanBodySubmersion(centerWater, body) > 0.59f &&
          Engine::oceanBodySubmersion(centerWater, body) < 0.61f,
          "floating-body immersion fraction is bounded and uses vertical shape extent");
    check(centerDisplacement.height < 0.0f,
          "submerged body produces a persistent center depression");
    const float centerFiniteSlopeX = (
        Engine::evaluateOceanBodyDisplacement(body.x + finiteDifferenceStep, body.z, bodyTime, body).height -
        Engine::evaluateOceanBodyDisplacement(body.x - finiteDifferenceStep, body.z, bodyTime, body).height) /
        (2.0f * finiteDifferenceStep);
    check(std::abs(centerDisplacement.slopeX) < 1.0e-7f &&
          std::abs(centerDisplacement.slopeZ) < 1.0e-7f &&
          std::abs(centerFiniteSlopeX) < 1.0e-4f,
          "body-height profile has a finite zero gradient at its radial center");
    Engine::OceanBodyWaveState fullySubmergedBody = body;
    fullySubmergedBody.y = centerWater.height - fullySubmergedBody.halfHeight - 0.5f;
    const auto fullySubmergedDisplacement = Engine::evaluateOceanBodyDisplacement(
        fullySubmergedBody.x, fullySubmergedBody.z, bodyTime, fullySubmergedBody);
    check(Engine::oceanBodySubmersion(centerWater, fullySubmergedBody) == 1.0f &&
          fullySubmergedDisplacement.height < 0.0f,
          "fully submerged bodies retain saturated water displacement");
    check(std::abs(bodySlopeX - displacement.slopeX) < 5.0e-4f &&
          std::abs(bodySlopeZ - displacement.slopeZ) < 5.0e-4f,
          "body displacement gradients match finite-difference height derivatives");
    check(std::abs(bodyVelocity - displacement.verticalVelocity) < 6.0e-4f,
          "moving-body displacement velocity matches finite-difference time derivative");
    const auto ringDisplacement = Engine::evaluateOceanBodyDisplacement(
        body.x + 1.65f * body.radius, body.z, bodyTime, body);
    check(ringDisplacement.height > 0.10f * std::min(0.45f * body.radius, 0.45f) *
              Engine::oceanBodySubmersion(centerWater, body),
          "body displacement produces a visible raised annular ridge outside the body footprint");
    double radialIntegral = 0.0;
    constexpr int radialIntervals = 12000;
    constexpr double radialLimit = 12.0;
    constexpr double radialStep = radialLimit / radialIntervals;
    for (int i = 0; i < radialIntervals; ++i) {
        const double radius0 = i * radialStep;
        const double radius1 = (i + 1) * radialStep;
        const double value0 = 2.0 * radius0 * Engine::oceanBodyDisplacementProfile(
            static_cast<float>(radius0 * radius0));
        const double value1 = 2.0 * radius1 * Engine::oceanBodyDisplacementProfile(
            static_cast<float>(radius1 * radius1));
        radialIntegral += 0.5 * (value0 + value1) * radialStep;
    }
    check(std::abs(radialIntegral) < 2.0e-5,
          "body depression and visible annular ridge conserve the height-field integral numerically");

    Engine::OceanRayIntersection rayHit;
    const float verticalOrigin[3] = {0.0f, 3.0f, 0.0f};
    const float verticalDirection[3] = {0.0f, -1.0f, 0.0f};
    const bool verticalSolved = Engine::solveOceanHeightFieldRay(
        verticalOrigin, verticalDirection, 3.0f,
        [](float, float) { return Engine::OceanWaveSample{1.0f, 0.0f, 0.0f, 0.0f}; }, rayHit);
    check(verticalSolved && rayHit.converged && std::abs(rayHit.rayT - 2.0f) < 1.0e-5f &&
          std::abs(rayHit.residual) < 0.001f,
          "vertical ray converges to a constant displaced water height");
    const float obliqueOrigin[3] = {1.0f, 4.0f, 2.0f};
    const float obliqueDirection[3] = {0.2f, -0.9f, 0.38f};
    const float planeRoot = (4.0f - 0.4f * 1.0f - 0.2f * 2.0f - 0.25f) /
                            (0.9f + 0.4f * 0.2f + 0.2f * 0.38f);
    const bool planeSolved = Engine::solveOceanHeightFieldRay(
        obliqueOrigin, obliqueDirection, 4.0f / 0.9f,
        [](float x, float z) { return Engine::OceanWaveSample{0.4f * x + 0.2f * z + 0.25f,
                                                               0.4f, 0.2f, 0.0f}; }, rayHit);
    check(planeSolved && rayHit.converged && std::abs(rayHit.rayT - planeRoot) < 2.0e-4f,
          "oblique ray uses the analytic surface gradient for a sloped-plane intersection");
    const float grazingDirection[3] = {1.0f, 0.0f, 0.0f};
    const bool grazingFinite = Engine::solveOceanHeightFieldRay(
        verticalOrigin, grazingDirection, 3.0f,
        [](float, float) { return Engine::OceanWaveSample{0.5f, 0.0f, 0.0f, 0.0f}; }, rayHit);
    check(grazingFinite && !rayHit.converged && rayHit.iterations == 0 &&
          std::abs(rayHit.rayT - 3.0f) < 1.0e-6f,
          "grazing ray derivative guard preserves a finite broad-phase hit without a false correction");
    const float distantOrigin[3] = {0.0f, 100.0f, 0.0f};
    const bool boundedSteps = Engine::solveOceanHeightFieldRay(
        distantOrigin, verticalDirection, 1.0f,
        [](float, float) { return Engine::OceanWaveSample{}; }, rayHit);
    check(boundedSteps && !rayHit.converged && rayHit.iterations == 3 &&
          rayHit.rayT > 1.0f && rayHit.rayT <= 3.251f && std::isfinite(rayHit.residual),
          "large ray-height residual uses exactly three bounded positive Newton steps");
    const float invalidRayDirection[3] = {0.0f, 0.0f, 0.0f};
    check(!Engine::solveOceanHeightFieldRay(
              verticalOrigin, invalidRayDirection, 3.0f,
              [](float, float) { return Engine::OceanWaveSample{}; }, rayHit),
          "zero-direction water ray is rejected before Newton division");
    const bool nonFiniteSurface = Engine::solveOceanHeightFieldRay(
        verticalOrigin, verticalDirection, 3.0f,
        [](float, float) {
            return Engine::OceanWaveSample{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f};
        }, rayHit);
    check(!nonFiniteSurface,
          "non-finite water-height samples are rejected before ray shading");
    Engine::OceanBodyWaveState dryBody = body;
    dryBody.y = centerWater.height + dryBody.halfHeight + 0.1f;
    const auto dryDisplacement = Engine::evaluateOceanBodyDisplacement(sampleX, sampleZ, bodyTime, dryBody);
    check(dryDisplacement.height == 0.0f && dryDisplacement.slopeX == 0.0f &&
          dryDisplacement.slopeZ == 0.0f && dryDisplacement.verticalVelocity == 0.0f,
          "airborne bodies do not displace the water field");

    float verticalExtent = 0.0f;
    const float identityQuaternion[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    check(Engine::tryGetOrientedBoxVerticalHalfExtent(identityQuaternion, 0.6f, 1.2f, 0.8f, verticalExtent) &&
          std::abs(verticalExtent - 1.2f) < 1.0e-5f,
          "identity-rotated box support extent equals its local half-height");
    const float quarterTurnZ[4] = {0.0f, 0.0f, 0.70710678f, 0.70710678f};
    check(Engine::tryGetOrientedBoxVerticalHalfExtent(quarterTurnZ, 0.6f, 1.2f, 0.8f, verticalExtent) &&
          std::abs(verticalExtent - 0.6f) < 1.0e-5f,
          "rotated box water-entry support extent follows its quaternion");
    check(Engine::tryGetOrientedEllipsoidVerticalHalfExtent(quarterTurnZ, 0.6f, 1.2f, 0.8f, verticalExtent) &&
          std::abs(verticalExtent - 0.6f) < 1.0e-5f,
          "rotated ellipsoid support extent uses the scaled-sphere support equation");
    const float invalidQuaternion[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    check(!Engine::tryGetOrientedBoxVerticalHalfExtent(invalidQuaternion, 1.0f, 1.0f, 1.0f, verticalExtent),
          "invalid orientation is rejected before computing box support");

    const float translatedMatrix[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 4, -2, 3, 1};
    const float sourcePosition[3] = {1.0f, 0.0f, 2.0f};
    float transformedPosition[3] = {};
    check(Engine::transformGltfPosition(translatedMatrix, sourcePosition, transformedPosition) &&
          std::abs(transformedPosition[0] - 4.0f) < 1.0e-6f &&
          std::abs(transformedPosition[1] + 1.0f) < 1.0e-6f &&
          std::abs(transformedPosition[2] - 5.0f) < 1.0e-6f,
          "glTF column-major node matrix applies rotation and translation to vertices");
    const float nonUniformScale[16] = {2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1};
    const float sourceNormal[3] = {1.0f, 1.0f, 0.0f};
    float transformedNormal[3] = {};
    check(Engine::transformGltfNormal(nonUniformScale, sourceNormal, transformedNormal) &&
          std::abs(transformedNormal[0] - 0.4472136f) < 1.0e-5f &&
          std::abs(transformedNormal[1] - 0.8944272f) < 1.0e-5f,
          "glTF normals use normalized inverse-transpose under non-uniform scale");
    const float mirroredScale[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float forwardNormal[3] = {0.0f, 0.0f, 1.0f};
    check(Engine::transformGltfNormal(mirroredScale, forwardNormal, transformedNormal) &&
          std::abs(transformedNormal[2] - 1.0f) < 1.0e-6f,
          "mirrored glTF node transform preserves inverse-transpose normal orientation");
    const float singularMatrix[16] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    check(!Engine::transformGltfNormal(singularMatrix, forwardNormal, transformedNormal),
          "singular glTF node transform rejects undefined normal matrix");

    if (g_failures != 0) {
        std::cerr << "[MathTest] " << g_failures << " / " << g_checks << " check(s) failed.\n";
        return 1;
    }
    std::cout << "[MathTest] All " << g_checks << " checks passed.\n";
    return 0;
}
