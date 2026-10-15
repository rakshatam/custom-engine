#include "../src/core/Math.h"
#include "../src/ecs/Components.h"
#include "../src/ecs/OceanWave.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int g_failures = 0;

void check(bool condition, const char* name) {
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
} // namespace

int main() {
    using Engine::AABB;
    using Engine::Vec3;

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

    if (g_failures != 0) {
        std::cerr << "[MathTest] " << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "[MathTest] All 12 checks passed.\n";
    return 0;
}
