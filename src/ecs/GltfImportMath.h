#pragma once

#include <cmath>

namespace Engine {

inline bool transformGltfPosition(const float matrix[16], const float input[3], float output[3]) {
    if (!matrix || !input || !output) return false;
    const float x = input[0], y = input[1], z = input[2];
    const float w = matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
    if (!std::isfinite(w) || std::abs(w) <= 1.0e-8f) return false;
    const float inverseW = 1.0f / w;
    output[0] = (matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12]) * inverseW;
    output[1] = (matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13]) * inverseW;
    output[2] = (matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14]) * inverseW;
    return std::isfinite(output[0]) && std::isfinite(output[1]) && std::isfinite(output[2]);
}

inline bool transformGltfNormal(const float matrix[16], const float input[3], float output[3]) {
    if (!matrix || !input || !output || !std::isfinite(input[0]) ||
        !std::isfinite(input[1]) || !std::isfinite(input[2])) return false;
    const float a = matrix[0], b = matrix[4], c = matrix[8];
    const float d = matrix[1], e = matrix[5], f = matrix[9];
    const float g = matrix[2], h = matrix[6], i = matrix[10];
    const float c00 = e * i - f * h, c01 = f * g - d * i, c02 = d * h - e * g;
    const float c10 = c * h - b * i, c11 = a * i - c * g, c12 = b * g - a * h;
    const float c20 = b * f - c * e, c21 = c * d - a * f, c22 = a * e - b * d;
    const float determinant = a * c00 + b * c01 + c * c02;
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12f) return false;

    // Cofactors equal det(A) * inverse-transpose(A). The determinant sign is
    // restored before normalization so mirrored node transforms stay correct.
    const float nx = c00 * input[0] + c01 * input[1] + c02 * input[2];
    const float ny = c10 * input[0] + c11 * input[1] + c12 * input[2];
    const float nz = c20 * input[0] + c21 * input[1] + c22 * input[2];
    const float lengthSquared = nx * nx + ny * ny + nz * nz;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20f) return false;
    const float inverseLength = (determinant < 0.0f ? -1.0f : 1.0f) / std::sqrt(lengthSquared);
    output[0] = nx * inverseLength;
    output[1] = ny * inverseLength;
    output[2] = nz * inverseLength;
    return std::isfinite(output[0]) && std::isfinite(output[1]) && std::isfinite(output[2]);
}

} // namespace Engine
