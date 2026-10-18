#pragma once

#include <cstdint>
#include <windows.h>

namespace Engine {

class JoltPhysicsWorld {
public:
    JoltPhysicsWorld() = default;
    ~JoltPhysicsWorld();
    JoltPhysicsWorld(const JoltPhysicsWorld&) = delete;
    JoltPhysicsWorld& operator=(const JoltPhysicsWorld&) = delete;

    bool initialize();
    int32_t addSphere(float x, float y, float z, float radius, float buoyancy = 1.9f);
    int32_t addBox(float x, float y, float z, float halfX, float halfY, float halfZ, float buoyancy = 1.9f);
    int32_t addEllipsoid(float x, float y, float z, float halfX, float halfY, float halfZ, float buoyancy = 1.9f);
    bool step(float dt, float timeSeconds);
    bool getTransform(uint32_t index, float position[3], float rotation[4]) const;
    bool getLinearVelocity(uint32_t index, float velocity[3]) const;
    bool getWaterSurface(uint32_t bodyIndex, float x, float z, float timeSeconds, float sample[4]) const;
    bool registerWaterImpact(uint32_t bodyIndex, float x, float z, float timeSeconds, float strength);
    uint32_t bodyCount() const;
    bool isReady() const { return m_world != nullptr; }

private:
    using CreateWorldFn = void* (*)();
    using DestroyWorldFn = void (*)(void*);
    using AddFloatBodyFn = int32_t (*)(void*, int32_t, float, float, float, float, float, float, float);
    using StepFn = int32_t (*)(void*, float, float);
    using GetBodyCountFn = uint32_t (*)(const void*);
    using GetBodyTransformFn = int32_t (*)(const void*, uint32_t, float*, float*);
    using GetBodyLinearVelocityFn = int32_t (*)(const void*, uint32_t, float*);
    using GetWaterSurfaceFn = int32_t (*)(const void*, uint32_t, float, float, float, float*);
    using RegisterWaterImpactFn = int32_t (*)(void*, uint32_t, float, float, float, float);

    HMODULE m_module = nullptr;
    DLL_DIRECTORY_COOKIE m_dllDirectoryCookie = nullptr;
    void* m_world = nullptr;
    CreateWorldFn m_createWorld = nullptr;
    DestroyWorldFn m_destroyWorld = nullptr;
    AddFloatBodyFn m_addFloatBody = nullptr;
    StepFn m_step = nullptr;
    GetBodyCountFn m_getBodyCount = nullptr;
    GetBodyTransformFn m_getBodyTransform = nullptr;
    GetBodyLinearVelocityFn m_getBodyLinearVelocity = nullptr;
    GetWaterSurfaceFn m_getWaterSurface = nullptr;
    RegisterWaterImpactFn m_registerWaterImpact = nullptr;
};

} // namespace Engine
