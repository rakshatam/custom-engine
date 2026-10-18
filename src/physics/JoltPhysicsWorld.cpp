#include "JoltPhysicsWorld.h"
#include <iostream>
#include <cwchar>
#include <string>
#include <cmath>

namespace Engine {

JoltPhysicsWorld::~JoltPhysicsWorld() {
    if (m_world && m_destroyWorld) m_destroyWorld(m_world);
    m_world = nullptr;
    if (m_module) FreeLibrary(m_module);
    if (m_dllDirectoryCookie) RemoveDllDirectory(m_dllDirectoryCookie);
    m_dllDirectoryCookie = nullptr;
}

bool JoltPhysicsWorld::initialize() {
    if (m_world) return true;
    wchar_t executablePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(executablePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    std::wstring runtimeDirectory = std::wstring(executablePath) + L"jolt_runtime";
    m_dllDirectoryCookie = AddDllDirectory(runtimeDirectory.c_str());
    std::wstring bridgePath = std::wstring(executablePath) + L"JoltBridge.dll";
    m_module = LoadLibraryExW(bridgePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!m_module) {
        std::cerr << "[Jolt] Could not load JoltBridge.dll (" << GetLastError() << ")" << std::endl;
        return false;
    }
    m_createWorld = reinterpret_cast<CreateWorldFn>(GetProcAddress(m_module, "JoltBridge_CreateWorld"));
    m_destroyWorld = reinterpret_cast<DestroyWorldFn>(GetProcAddress(m_module, "JoltBridge_DestroyWorld"));
    m_addFloatBody = reinterpret_cast<AddFloatBodyFn>(GetProcAddress(m_module, "JoltBridge_AddFloatBody"));
    m_step = reinterpret_cast<StepFn>(GetProcAddress(m_module, "JoltBridge_Step"));
    m_getBodyCount = reinterpret_cast<GetBodyCountFn>(GetProcAddress(m_module, "JoltBridge_GetBodyCount"));
    m_getBodyTransform = reinterpret_cast<GetBodyTransformFn>(GetProcAddress(m_module, "JoltBridge_GetBodyTransform"));
    m_getBodyLinearVelocity = reinterpret_cast<GetBodyLinearVelocityFn>(GetProcAddress(m_module, "JoltBridge_GetBodyLinearVelocity"));
    m_getWaterSurface = reinterpret_cast<GetWaterSurfaceFn>(GetProcAddress(m_module, "JoltBridge_GetWaterSurface"));
    m_registerWaterImpact = reinterpret_cast<RegisterWaterImpactFn>(GetProcAddress(m_module, "JoltBridge_RegisterWaterImpact"));
    if (!m_createWorld || !m_destroyWorld || !m_addFloatBody || !m_step || !m_getBodyCount ||
        !m_getBodyTransform || !m_getBodyLinearVelocity || !m_getWaterSurface || !m_registerWaterImpact) {
        std::cerr << "[Jolt] Bridge exports are incomplete." << std::endl;
        FreeLibrary(m_module);
        m_module = nullptr;
        if (m_dllDirectoryCookie) { RemoveDllDirectory(m_dllDirectoryCookie); m_dllDirectoryCookie = nullptr; }
        return false;
    }
    m_world = m_createWorld();
    if (!m_world) {
        std::cerr << "[Jolt] PhysicsSystem creation failed." << std::endl;
        FreeLibrary(m_module);
        m_module = nullptr;
        if (m_dllDirectoryCookie) { RemoveDllDirectory(m_dllDirectoryCookie); m_dllDirectoryCookie = nullptr; }
        return false;
    }
    return true;
}

int32_t JoltPhysicsWorld::addSphere(float x, float y, float z, float radius, float buoyancy) {
    return m_world && std::isfinite(radius) && radius > 1.0e-4f && std::isfinite(buoyancy)
        ? m_addFloatBody(m_world, 0, x, y, z, radius, radius, radius, buoyancy) : -1;
}

int32_t JoltPhysicsWorld::addBox(float x, float y, float z, float halfX, float halfY, float halfZ, float buoyancy) {
    return m_world && std::isfinite(buoyancy)
        ? m_addFloatBody(m_world, 1, x, y, z, halfX, halfY, halfZ, buoyancy) : -1;
}

int32_t JoltPhysicsWorld::addEllipsoid(float x, float y, float z, float halfX, float halfY, float halfZ, float buoyancy) {
    return m_world && std::isfinite(buoyancy)
        ? m_addFloatBody(m_world, 2, x, y, z, halfX, halfY, halfZ, buoyancy) : -1;
}

bool JoltPhysicsWorld::step(float dt, float timeSeconds) {
    return m_world && m_step && std::isfinite(dt) && std::isfinite(timeSeconds) &&
        m_step(m_world, dt, timeSeconds) != 0;
}

bool JoltPhysicsWorld::getTransform(uint32_t index, float position[3], float rotation[4]) const {
    if (!m_world || !m_getBodyTransform || !position || !rotation ||
        m_getBodyTransform(m_world, index, position, rotation) == 0) return false;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(position[i])) return false;
    for (int i = 0; i < 4; ++i) if (!std::isfinite(rotation[i])) return false;
    return true;
}

bool JoltPhysicsWorld::getLinearVelocity(uint32_t index, float velocity[3]) const {
    if (!m_world || !m_getBodyLinearVelocity || !velocity ||
        m_getBodyLinearVelocity(m_world, index, velocity) == 0) return false;
    return std::isfinite(velocity[0]) && std::isfinite(velocity[1]) && std::isfinite(velocity[2]);
}

bool JoltPhysicsWorld::getWaterSurface(uint32_t bodyIndex, float x, float z, float timeSeconds, float sample[4]) const {
    if (!m_world || !m_getWaterSurface || !sample || !std::isfinite(x) || !std::isfinite(z) ||
        !std::isfinite(timeSeconds) || m_getWaterSurface(m_world, bodyIndex, x, z, timeSeconds, sample) == 0) return false;
    for (int i = 0; i < 4; ++i) if (!std::isfinite(sample[i])) return false;
    return true;
}

bool JoltPhysicsWorld::registerWaterImpact(uint32_t bodyIndex, float x, float z, float timeSeconds, float strength) {
    return m_world && m_registerWaterImpact &&
        m_registerWaterImpact(m_world, bodyIndex, x, z, timeSeconds, strength) != 0;
}

uint32_t JoltPhysicsWorld::bodyCount() const {
    return m_world && m_getBodyCount ? m_getBodyCount(m_world) : 0;
}

} // namespace Engine
