#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine {

inline bool getGltfLinearDeterminant(const float matrix[16], double& determinant) {
    if (!matrix) return false;
    const double a = matrix[0], b = matrix[4], c = matrix[8];
    const double d = matrix[1], e = matrix[5], f = matrix[9];
    const double g = matrix[2], h = matrix[6], i = matrix[10];
    const double scale = std::max({std::abs(a), std::abs(b), std::abs(c), std::abs(d),
                                   std::abs(e), std::abs(f), std::abs(g), std::abs(h), std::abs(i)});
    determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const double singularTolerance = 64.0 * std::numeric_limits<double>::epsilon() * scale * scale * scale;
    return std::isfinite(determinant) && std::isfinite(singularTolerance) && scale > 0.0 &&
        std::abs(determinant) > singularTolerance;
}

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
    const double a = matrix[0], b = matrix[4], c = matrix[8];
    const double d = matrix[1], e = matrix[5], f = matrix[9];
    const double g = matrix[2], h = matrix[6], i = matrix[10];
    double determinant = 0.0;
    if (!getGltfLinearDeterminant(matrix, determinant)) return false;
    const double c00 = e * i - f * h, c01 = f * g - d * i, c02 = d * h - e * g;
    const double c10 = c * h - b * i, c11 = a * i - c * g, c12 = b * g - a * h;
    const double c20 = b * f - c * e, c21 = c * d - a * f, c22 = a * e - b * d;

    // Cofactors equal det(A) * inverse-transpose(A). The determinant sign is
    // restored before normalization so mirrored node transforms stay correct.
    const double orientation = determinant < 0.0 ? -1.0 : 1.0;
    const double nx = orientation * (c00 * input[0] + c01 * input[1] + c02 * input[2]);
    const double ny = orientation * (c10 * input[0] + c11 * input[1] + c12 * input[2]);
    const double nz = orientation * (c20 * input[0] + c21 * input[1] + c22 * input[2]);
    const double length = std::hypot(nx, ny, nz);
    if (!std::isfinite(length) || length <= 1.0e-30) return false;
    output[0] = static_cast<float>(nx / length);
    output[1] = static_cast<float>(ny / length);
    output[2] = static_cast<float>(nz / length);
    return std::isfinite(output[0]) && std::isfinite(output[1]) && std::isfinite(output[2]);
}

} // namespace Engine
