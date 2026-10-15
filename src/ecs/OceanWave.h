#pragma once

#include <array>
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

} // namespace Engine
