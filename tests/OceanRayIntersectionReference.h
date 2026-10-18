#pragma once

#include "../src/ecs/OceanWave.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Engine {

struct OceanRayIntersection {
    float rayT = 0.0f;
    float x = 0.0f;
    float z = 0.0f;
    float residual = 0.0f;
    uint32_t iterations = 0;
    bool converged = false;
};

// CPU reference for the three bounded Newton steps in the water closest-hit
// shader. SurfaceSample must return height and its X/Z derivatives, matching
// F(t)=origin.y+direction.y*t-height(x(t),z(t)) and
// F'(t)=direction.y-grad(height)·direction.xz.
template<class SurfaceSample>
inline bool solveOceanHeightFieldRay(const float origin[3], const float direction[3],
                                     float initialRayT, SurfaceSample&& sampleSurface,
                                     OceanRayIntersection& result) {
    if (!origin || !direction || !std::isfinite(initialRayT) || initialRayT < 0.0f) return false;
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(origin[axis]) || !std::isfinite(direction[axis])) return false;
    }
    const float directionLengthSquared = direction[0] * direction[0] +
                                         direction[1] * direction[1] +
                                         direction[2] * direction[2];
    if (!std::isfinite(directionLengthSquared) || directionLengthSquared <= 1.0e-12f) return false;

    float rayT = initialRayT;
    result = {};
    for (uint32_t iteration = 0; iteration < 3; ++iteration) {
        const float x = origin[0] + direction[0] * rayT;
        const float y = origin[1] + direction[1] * rayT;
        const float z = origin[2] + direction[2] * rayT;
        const OceanWaveSample surface = sampleSurface(x, z);
        const float residual = y - surface.height;
        const float derivative = direction[1] - surface.slopeX * direction[0]
                                             - surface.slopeZ * direction[2];
        result = {rayT, x, z, residual, iteration, false};
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            !std::isfinite(residual) || !std::isfinite(derivative) ||
            !std::isfinite(surface.height) || !std::isfinite(surface.slopeX) ||
            !std::isfinite(surface.slopeZ)) return false;
        if (std::abs(residual) < 0.001f) {
            result.converged = true;
            return true;
        }
        if (std::abs(derivative) < 0.12f) return true;

        const float maxRayStep = std::min(2.0f, 0.75f / std::max(std::abs(direction[1]), 0.25f));
        const float correction = std::clamp(residual / derivative, -maxRayStep, maxRayStep);
        const float nextRayT = std::max(0.001f, rayT - correction);
        if (!std::isfinite(nextRayT)) return false;
        result.iterations = iteration + 1;
        if (std::abs(nextRayT - rayT) < 1.0e-5f) return true;
        rayT = nextRayT;
    }

    const float x = origin[0] + direction[0] * rayT;
    const float y = origin[1] + direction[1] * rayT;
    const float z = origin[2] + direction[2] * rayT;
    const OceanWaveSample surface = sampleSurface(x, z);
    result.rayT = rayT;
    result.x = x;
    result.z = z;
    result.residual = y - surface.height;
    result.converged = std::isfinite(result.residual) && std::abs(result.residual) < 0.001f;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
           std::isfinite(result.residual) && std::isfinite(surface.slopeX) &&
           std::isfinite(surface.slopeZ);
}

} // namespace Engine
