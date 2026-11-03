#pragma once

#include <cmath>
#include <limits>

namespace Engine {

// Builds the row-vector UI projection used by ImGuiRenderer. The input is the
// display rectangle's top-left coordinate and its positive width/height.
inline bool makeUiProjection(float displayX, float displayY, float displayWidth,
                             float displayHeight, float out[4][4]) {
    if (!out || !std::isfinite(displayX) || !std::isfinite(displayY) ||
        !std::isfinite(displayWidth) || !std::isfinite(displayHeight) ||
        displayWidth <= 1.0e-6f || displayHeight <= 1.0e-6f) {
        return false;
    }

    const double x = displayX;
    const double y = displayY;
    const double width = displayWidth;
    const double height = displayHeight;
    const double sx = 2.0 / width;
    const double sy = -2.0 / height;
    const double tx = -1.0 - 2.0 * x / width;
    const double ty = 1.0 + 2.0 * y / height;
    const double maxFloat = std::numeric_limits<float>::max();
    if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(tx) || !std::isfinite(ty) ||
        std::abs(sx) > maxFloat || std::abs(sy) > maxFloat ||
        std::abs(tx) > maxFloat || std::abs(ty) > maxFloat) {
        return false;
    }

    const float candidate[4][4] = {
        {static_cast<float>(sx), 0.0f, 0.0f, 0.0f},
        {0.0f, static_cast<float>(sy), 0.0f, 0.0f},
        {0.0f, 0.0f, 0.5f, 0.0f},
        {static_cast<float>(tx), static_cast<float>(ty), 0.5f, 1.0f}
    };
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            out[row][column] = candidate[row][column];
        }
    }
    return true;
}

} // namespace Engine
