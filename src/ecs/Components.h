#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "../core/Math.h"

namespace Engine {

struct NameComponent {
    std::string name = "Entity";
};

struct TransformComponent {
    Vec3 position = Vec3(0.0f, 0.0f, 0.0f);
    Vec3 rotation = Vec3(0.0f, 0.0f, 0.0f); // Euler in degrees (pitch, yaw, roll)
    Vec3 scale    = Vec3(1.0f, 1.0f, 1.0f);

    Mat4 getTransformMatrix() const {
        float pRad = rotation.x * (3.14159265f / 180.0f);
        float yRad = rotation.y * (3.14159265f / 180.0f);
        float rRad = rotation.z * (3.14159265f / 180.0f);

        XMMATRIX S = XMMatrixScaling(scale.x, scale.y, scale.z);
        XMMATRIX R = XMMatrixRotationRollPitchYaw(pRad, yRad, rRad);
        XMMATRIX T = XMMatrixTranslation(position.x, position.y, position.z);
        XMMATRIX M = S * R * T;

        Mat4 res;
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(&res.m[0][0]), XMMatrixTranspose(M));
        return res;
    }

    void get3x4RowMajor(float out[3][4]) const {
        float pRad = rotation.x * (3.14159265f / 180.0f);
        float yRad = rotation.y * (3.14159265f / 180.0f);
        float rRad = rotation.z * (3.14159265f / 180.0f);

        XMMATRIX S = XMMatrixScaling(scale.x, scale.y, scale.z);
        XMMATRIX R = XMMatrixRotationRollPitchYaw(pRad, yRad, rRad);
        XMMATRIX T = XMMatrixTranslation(position.x, position.y, position.z);
        XMMATRIX M = S * R * T; // DirectX standard is row vectors v * M

        XMFLOAT4X4 mat;
        XMStoreFloat4x4(&mat, M);
        // D3D12_RAYTRACING_INSTANCE_DESC requires 3x4 row-major transform
        out[0][0] = mat._11; out[0][1] = mat._21; out[0][2] = mat._31; out[0][3] = mat._41;
        out[1][0] = mat._12; out[1][1] = mat._22; out[1][2] = mat._32; out[1][3] = mat._42;
        out[2][0] = mat._13; out[2][1] = mat._23; out[2][2] = mat._33; out[2][3] = mat._43;
    }
};

struct MaterialComponent {
    Vec4  baseColor = Vec4(0.85f, 0.85f, 0.85f, 1.0f);
    float metallic  = 0.0f;
    float roughness = 0.5f;
    float emissive  = 0.0f;
    int   textureId = -1;
};

struct MeshComponent {
    uint32_t vertexOffset = 0;
    uint32_t vertexCount  = 0;
    uint32_t indexOffset  = 0;
    uint32_t indexCount   = 0;
    uint32_t blasIndex    = 0;
    AABB     localAABB;
};

struct LightComponent {
    enum class Type { Directional, Point } type = Type::Directional;
    Vec3  direction = Vec3(-0.4f, -0.85f, 0.35f).normalized();
    Vec3  color     = Vec3(1.0f, 0.96f, 0.88f); // Warm sun
    float intensity = 5.0f;
};

struct CameraComponent {
    Vec3  position  = Vec3(0.0f, 2.0f, -5.0f);
    float yaw       = 90.0f;  // degrees
    float pitch     = 0.0f;   // degrees
    float fov       = 65.0f;  // degrees
    float nearPlane = 0.1f;
    float farPlane  = 1000.0f;
    float moveSpeed = 6.0f;
    float lookSpeed = 0.15f;

    Vec3 getForward() const {
        float pRad = pitch * (3.14159265f / 180.0f);
        float yRad = yaw * (3.14159265f / 180.0f);
        float x = std::cos(pRad) * std::cos(yRad);
        float y = std::sin(pRad);
        float z = std::cos(pRad) * std::sin(yRad);
        return Vec3(x, y, z).normalized();
    }

    Vec3 getRight() const {
        return Vec3::cross(getForward(), Vec3(0.0f, 1.0f, 0.0f)).normalized();
    }

    Vec3 getUp() const {
        return Vec3::cross(getRight(), getForward()).normalized();
    }

    XMMATRIX getViewMatrix() const {
        Vec3 f = getForward();
        XMVECTOR eye = XMVectorSet(position.x, position.y, position.z, 1.0f);
        XMVECTOR target = XMVectorSet(position.x + f.x, position.y + f.y, position.z + f.z, 1.0f);
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        return XMMatrixLookAtLH(eye, target, up);
    }

    XMMATRIX getProjMatrix(float aspect) const {
        const float safeFov = std::clamp(std::isfinite(fov) ? fov : 65.0f, 1.0f, 179.0f);
        const float safeAspect = std::clamp(std::isfinite(aspect) && aspect > 1.0e-4f ? aspect : 1.0f,
                                            1.0e-4f, 1.0e4f);
        const float safeNear = std::max(std::isfinite(nearPlane) ? nearPlane : 0.1f, 1.0e-4f);
        const float safeFar = std::max(std::isfinite(farPlane) ? farPlane : 1000.0f, safeNear + 1.0e-3f);
        return XMMatrixPerspectiveFovLH(safeFov * (3.14159265f / 180.0f), safeAspect, safeNear, safeFar);
    }
};

} // namespace Engine
