#pragma once
#include <cmath>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <cstdint>
#include <DirectXMath.h>

namespace Engine {

using namespace DirectX;

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float _x, float _y) : x(_x), y(_y) {}
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float v) : x(v), y(v), z(v) {}
    Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    Vec3 operator*(const Vec3& o) const { return Vec3(x * o.x, y * o.y, z * o.z); }
    Vec3 operator/(float s) const {
        if (!std::isfinite(s) || std::abs(s) <= 1.0e-8f) {
            throw std::domain_error("Vec3 division requires a finite, non-zero divisor");
        }
        const float inv = 1.0f / s;
        return Vec3(x * inv, y * inv, z * inv);
    }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }

    float lengthSq() const { return x * x + y * y + z * z; }
    float length() const { return std::hypot(x, y, z); }
    Vec3 normalized() const {
        float l = length();
        if (!std::isfinite(l)) throw std::domain_error("Vec3 normalization received a non-finite vector");
        return l > 1e-6f ? (*this / l) : Vec3(0.0f);
    }

    static float dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static Vec3 cross(const Vec3& a, const Vec3& b) {
        return Vec3(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        );
    }
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    Vec4() = default;
    Vec4(float _x, float _y, float _z, float _w = 1.0f) : x(_x), y(_y), z(_z), w(_w) {}
    Vec4(const Vec3& v, float _w = 1.0f) : x(v.x), y(v.y), z(v.z), w(_w) {}
};

struct Mat4 {
    float m[4][4] = {
        {1,0,0,0},
        {0,1,0,0},
        {0,0,1,0},
        {0,0,0,1}
    };

    static Mat4 identity() {
        return Mat4();
    }

    static Mat4 translation(const Vec3& t) {
        Mat4 res;
        res.m[0][3] = t.x;
        res.m[1][3] = t.y;
        res.m[2][3] = t.z;
        return res;
    }

    static Mat4 scale(const Vec3& s) {
        Mat4 res;
        res.m[0][0] = s.x;
        res.m[1][1] = s.y;
        res.m[2][2] = s.z;
        return res;
    }

    static Mat4 rotationY(float rad) {
        Mat4 res;
        float c = std::cos(rad), s = std::sin(rad);
        res.m[0][0] = c;  res.m[0][2] = s;
        res.m[2][0] = -s; res.m[2][2] = c;
        return res;
    }

    static Mat4 rotationX(float rad) {
        Mat4 res;
        float c = std::cos(rad), s = std::sin(rad);
        res.m[1][1] = c;  res.m[1][2] = -s;
        res.m[2][1] = s;  res.m[2][2] = c;
        return res;
    }

    static Mat4 rotationZ(float rad) {
        Mat4 res;
        float c = std::cos(rad), s = std::sin(rad);
        res.m[0][0] = c;  res.m[0][1] = -s;
        res.m[1][0] = s;  res.m[1][1] = c;
        return res;
    }

    static Mat4 rotationYawPitchRoll(float yawRad, float pitchRad, float rollRad) {
        XMMATRIX rot = XMMatrixRotationRollPitchYaw(pitchRad, yawRad, rollRad);
        Mat4 res;
        // Mat4's helpers use column vectors with translation in column 4;
        // DirectXMath uses row vectors, so transpose at this boundary.
        XMFLOAT4X4 transposed;
        XMStoreFloat4x4(&transposed, XMMatrixTranspose(rot));
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                res.m[row][column] = transposed.m[row][column];
            }
        }
        return res;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 res;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                res.m[i][j] = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    res.m[i][j] += m[i][k] * o.m[k][j];
                }
            }
        }
        return res;
    }

    // Converts to 3x4 row-major array for DXR D3D12_RAYTRACING_INSTANCE_DESC
    void to3x4RowMajor(float out[3][4]) const {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                out[r][c] = m[r][c];
            }
        }
    }
};

struct AABB {
    Vec3 min = Vec3(std::numeric_limits<float>::max());
    Vec3 max = Vec3(-std::numeric_limits<float>::max());

    void expand(const Vec3& p) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            throw std::domain_error("AABB cannot include a non-finite point");
        }
        min.x = std::min(min.x, p.x);
        min.y = std::min(min.y, p.y);
        min.z = std::min(min.z, p.z);
        max.x = std::max(max.x, p.x);
        max.y = std::max(max.y, p.y);
        max.z = std::max(max.z, p.z);
    }

    bool rayIntersect(const Vec3& rayOrigin, const Vec3& rayDir, float& outT) const {
        const double origins[3] = {rayOrigin.x, rayOrigin.y, rayOrigin.z};
        const double directions[3] = {rayDir.x, rayDir.y, rayDir.z};
        const double minima[3] = {min.x, min.y, min.z};
        const double maxima[3] = {max.x, max.y, max.z};
        double tEnter = -std::numeric_limits<double>::infinity();
        double tExit = std::numeric_limits<double>::infinity();

        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(origins[axis]) || !std::isfinite(directions[axis]) ||
                !std::isfinite(minima[axis]) || !std::isfinite(maxima[axis]) || minima[axis] > maxima[axis]) {
                return false;
            }
            if (directions[axis] == 0.0) {
                if (origins[axis] < minima[axis] || origins[axis] > maxima[axis]) return false;
                continue;
            }
            const double inverseDirection = 1.0 / directions[axis];
            double t0 = (minima[axis] - origins[axis]) * inverseDirection;
            double t1 = (maxima[axis] - origins[axis]) * inverseDirection;
            if (t0 > t1) std::swap(t0, t1);
            tEnter = std::max(tEnter, t0);
            tExit = std::min(tExit, t1);
            if (std::isnan(tEnter) || std::isnan(tExit) || tEnter > tExit) return false;
        }

        if (tExit < 0.0f || !std::isfinite(tExit)) return false;
        const double hitT = tEnter >= 0.0 ? tEnter : tExit;
        if (!std::isfinite(hitT) || hitT > std::numeric_limits<float>::max()) return false;
        outT = static_cast<float>(hitT);
        return std::isfinite(outT);
    }
};

// Two-sided Möller–Trumbore ray/triangle intersection. rayDirection need not
// be normalized; outT is the parameter in origin + direction * t and is only
// written when a finite, forward hit is found.
inline bool rayTriangleIntersect(const Vec3& rayOrigin, const Vec3& rayDirection,
                                 const Vec3& v0, const Vec3& v1, const Vec3& v2,
                                 float& outT) {
    const double e1x = static_cast<double>(v1.x) - v0.x;
    const double e1y = static_cast<double>(v1.y) - v0.y;
    const double e1z = static_cast<double>(v1.z) - v0.z;
    const double e2x = static_cast<double>(v2.x) - v0.x;
    const double e2y = static_cast<double>(v2.y) - v0.y;
    const double e2z = static_cast<double>(v2.z) - v0.z;
    const double dx = rayDirection.x, dy = rayDirection.y, dz = rayDirection.z;
    const double e1Sq = e1x * e1x + e1y * e1y + e1z * e1z;
    const double e2Sq = e2x * e2x + e2y * e2y + e2z * e2z;
    const double directionSq = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(e1Sq) || !std::isfinite(e2Sq) || !std::isfinite(directionSq) ||
        e1Sq <= 1.0e-30 || e2Sq <= 1.0e-30 || directionSq <= 1.0e-30) return false;

    const double px = dy * e2z - dz * e2y;
    const double py = dz * e2x - dx * e2z;
    const double pz = dx * e2y - dy * e2x;
    const double determinant = e1x * px + e1y * py + e1z * pz;
    const double determinantTolerance = std::max(1.0e-30, 1.0e-8 * std::sqrt(e1Sq * e2Sq * directionSq));
    if (!std::isfinite(determinant) || std::abs(determinant) <= determinantTolerance) return false;

    const double inverseDeterminant = 1.0 / determinant;
    const double tx = static_cast<double>(rayOrigin.x) - v0.x;
    const double ty = static_cast<double>(rayOrigin.y) - v0.y;
    const double tz = static_cast<double>(rayOrigin.z) - v0.z;
    const double u = (tx * px + ty * py + tz * pz) * inverseDeterminant;
    constexpr double barycentricTolerance = 1.0e-7;
    if (!std::isfinite(u) || u < -barycentricTolerance || u > 1.0 + barycentricTolerance) return false;

    const double qx = ty * e1z - tz * e1y;
    const double qy = tz * e1x - tx * e1z;
    const double qz = tx * e1y - ty * e1x;
    const double v = (dx * qx + dy * qy + dz * qz) * inverseDeterminant;
    if (!std::isfinite(v) || v < -barycentricTolerance || u + v > 1.0 + barycentricTolerance) return false;

    const double t = (e2x * qx + e2y * qy + e2z * qz) * inverseDeterminant;
    if (!std::isfinite(t) || t < 0.0 || t > std::numeric_limits<float>::max()) return false;
    outT = static_cast<float>(t);
    return true;
}

} // namespace Engine
