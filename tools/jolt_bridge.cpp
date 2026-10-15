#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include "../src/ecs/OceanWave.h"
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <cstdint>
#include <cstdarg>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr JPH::ObjectLayer kMovingLayer = 0;
constexpr JPH::ObjectLayer kStaticLayer = 1;
constexpr JPH::uint kLayerCount = 2;
constexpr JPH::BroadPhaseLayer kMovingBroadPhase(0);
constexpr JPH::BroadPhaseLayer kStaticBroadPhase(1);

void trace(const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    std::fprintf(stderr, "[Jolt] %s\n", message);
}

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return kLayerCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == kMovingLayer ? kMovingBroadPhase : kStaticBroadPhase;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return static_cast<JPH::BroadPhaseLayer::Type>(layer) == 0 ? "Moving" : "Static";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer objectLayer, JPH::BroadPhaseLayer) const override {
        return objectLayer == kMovingLayer;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == kMovingLayer || b == kMovingLayer;
    }
};

struct FloatBody {
    JPH::BodyID id;
    float buoyancy = 1.90f;
};

struct WaterImpact {
    uint32_t sourceBody = 0;
    float x = 0.0f;
    float z = 0.0f;
    float time = 0.0f;
    float strength = 0.0f;
};

struct World {
    BroadPhaseLayers broadPhase;
    ObjectVsBroadPhase objectVsBroadPhase;
    ObjectPairs objectPairs;
    JPH::PhysicsSystem physics;
    JPH::TempAllocatorImpl tempAllocator{2 * 1024 * 1024};
    JPH::JobSystemThreadPool jobs{2048, 8, 2};
    std::vector<FloatBody> floats;
    std::vector<WaterImpact> waterImpacts;

    World() {
        physics.Init(256, 0, 2048, 1024, broadPhase, objectVsBroadPhase, objectPairs);
        physics.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    }
};

bool g_joltInitialized = false;

Engine::OceanWaveSample sampleWaterSurface(const World& world, float x, float z, float t, int32_t excludedBody) {
    Engine::OceanWaveSample wave = Engine::evaluateOceanWave(x, z, t);
    for (const WaterImpact& impact : world.waterImpacts) {
        const float age = t - impact.time;
        if (static_cast<int32_t>(impact.sourceBody) == excludedBody || age < 0.0f || age >= 4.5f) continue;
        const float dx = x - impact.x;
        const float dz = z - impact.z;
        const float distance = std::sqrt(dx * dx + dz * dz);
        if (!std::isfinite(distance)) continue;
        const float phase = 4.6f * distance - 7.2f * age;
        const float decay = std::exp(-0.27f * distance - 0.92f * age);
        const float amplitude = 0.23f * impact.strength * decay;
        wave.height += amplitude * std::sin(phase);
        wave.verticalVelocity += amplitude * (-0.92f * std::sin(phase) - 7.2f * std::cos(phase));
        if (distance > 1.0e-4f) {
            const float radialDerivative = amplitude * (4.6f * std::cos(phase) - 0.27f * std::sin(phase));
            wave.slopeX += radialDerivative * dx / distance;
            wave.slopeZ += radialDerivative * dz / distance;
        }
    }
    if (!std::isfinite(wave.height) || !std::isfinite(wave.slopeX) ||
        !std::isfinite(wave.slopeZ) || !std::isfinite(wave.verticalVelocity)) return {};
    return wave;
}

void waveSurface(const World& world, float x, float z, float t, int32_t excludedBody,
                 JPH::RVec3& position, JPH::Vec3& normal, JPH::Vec3& velocity) {
    const Engine::OceanWaveSample wave = sampleWaterSurface(world, x, z, t, excludedBody);
    const float inverseNormalLength = 1.0f / std::sqrt(1.0f + wave.slopeX * wave.slopeX + wave.slopeZ * wave.slopeZ);
    position = JPH::RVec3(x, wave.height, z);
    normal = JPH::Vec3(-wave.slopeX * inverseNormalLength, inverseNormalLength, -wave.slopeZ * inverseNormalLength);
    velocity = JPH::Vec3(0.0f, wave.verticalVelocity, 0.0f);
}
} // namespace

struct JoltWorldHandle { World world; };

extern "C" {
__declspec(dllexport) JoltWorldHandle* JoltBridge_CreateWorld() {
    if (!g_joltInitialized) {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = trace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        g_joltInitialized = true;
    }
    return new JoltWorldHandle();
}

__declspec(dllexport) void JoltBridge_DestroyWorld(JoltWorldHandle* handle) {
    delete handle;
    if (g_joltInitialized) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
        g_joltInitialized = false;
    }
}

__declspec(dllexport) int32_t JoltBridge_AddFloatBody(JoltWorldHandle* handle, int32_t shapeKind,
                                                       float x, float y, float z,
                                                       float halfX, float halfY, float halfZ,
                                                       float buoyancy) {
    if (!handle) return -1;
    JPH::RefConst<JPH::Shape> shape;
    if (shapeKind == 0) {
        shape = new JPH::SphereShape(halfX);
    } else {
        shape = new JPH::BoxShape(JPH::Vec3(halfX, halfY, halfZ), 0.04f);
    }
    JPH::BodyCreationSettings settings(shape, JPH::RVec3(x, y, z), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, kMovingLayer);
    settings.mFriction = 0.55f;
    settings.mRestitution = 0.12f;
    settings.mLinearDamping = 0.12f;
    settings.mAngularDamping = 0.35f;
    JPH::BodyInterface& bodies = handle->world.physics.GetBodyInterface();
    const JPH::BodyID id = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (id.IsInvalid()) return -1;
    handle->world.floats.push_back({id, buoyancy});
    return static_cast<int32_t>(handle->world.floats.size() - 1);
}

__declspec(dllexport) void JoltBridge_Step(JoltWorldHandle* handle, float dt, float timeSeconds) {
    if (!handle || dt <= 0.0f) return;
    World& world = handle->world;
    JPH::BodyInterface& bodies = world.physics.GetBodyInterface();
    const JPH::Vec3 gravity = world.physics.GetGravity();
    for (size_t index = 0; index < world.floats.size(); ++index) {
        const FloatBody& item = world.floats[index];
        const JPH::RVec3 position = bodies.GetPosition(item.id);
        JPH::RVec3 surfacePosition;
        JPH::Vec3 surfaceNormal, fluidVelocity;
        // The water-entry event feeds back into its source body as well as nearby
        // bodies, so the body's next buoyancy impulse responds to the wave it made.
        waveSurface(world, static_cast<float>(position.GetX()), static_cast<float>(position.GetZ()), timeSeconds,
                    -1, surfacePosition, surfaceNormal, fluidVelocity);
        bodies.ApplyBuoyancyImpulse(item.id, surfacePosition, surfaceNormal, item.buoyancy,
                                    0.35f, 0.9f, fluidVelocity, gravity, dt);
    }
    world.physics.Update(dt, 1, &world.tempAllocator, &world.jobs);
}

__declspec(dllexport) uint32_t JoltBridge_GetBodyCount(const JoltWorldHandle* handle) {
    return handle ? static_cast<uint32_t>(handle->world.floats.size()) : 0;
}

__declspec(dllexport) int32_t JoltBridge_GetBodyTransform(const JoltWorldHandle* handle, uint32_t index,
                                                           float* position3, float* rotation4) {
    if (!handle || index >= handle->world.floats.size() || !position3 || !rotation4) return 0;
    const JPH::BodyInterface& bodies = handle->world.physics.GetBodyInterface();
    const FloatBody& item = handle->world.floats[index];
    const JPH::RVec3 position = bodies.GetPosition(item.id);
    const JPH::Quat rotation = bodies.GetRotation(item.id);
    position3[0] = static_cast<float>(position.GetX());
    position3[1] = static_cast<float>(position.GetY());
    position3[2] = static_cast<float>(position.GetZ());
    rotation4[0] = rotation.GetX();
    rotation4[1] = rotation.GetY();
    rotation4[2] = rotation.GetZ();
    rotation4[3] = rotation.GetW();
    return 1;
}

__declspec(dllexport) int32_t JoltBridge_GetBodyLinearVelocity(const JoltWorldHandle* handle, uint32_t index,
                                                                float* velocity3) {
    if (!handle || index >= handle->world.floats.size() || !velocity3) return 0;
    const JPH::BodyInterface& bodies = handle->world.physics.GetBodyInterface();
    const JPH::Vec3 velocity = bodies.GetLinearVelocity(handle->world.floats[index].id);
    velocity3[0] = velocity.GetX();
    velocity3[1] = velocity.GetY();
    velocity3[2] = velocity.GetZ();
    if (!std::isfinite(velocity3[0]) || !std::isfinite(velocity3[1]) || !std::isfinite(velocity3[2])) return 0;
    return 1;
}

__declspec(dllexport) int32_t JoltBridge_GetWaterSurface(const JoltWorldHandle* handle, uint32_t bodyIndex,
                                                          float x, float z, float timeSeconds, float* sample4) {
    if (!handle || bodyIndex >= handle->world.floats.size() || !sample4 ||
        !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(timeSeconds)) return 0;
    const Engine::OceanWaveSample sample = sampleWaterSurface(handle->world, x, z, timeSeconds,
        static_cast<int32_t>(bodyIndex));
    sample4[0] = sample.height;
    sample4[1] = sample.slopeX;
    sample4[2] = sample.slopeZ;
    sample4[3] = sample.verticalVelocity;
    return 1;
}

__declspec(dllexport) int32_t JoltBridge_RegisterWaterImpact(JoltWorldHandle* handle, uint32_t bodyIndex,
                                                              float x, float z, float timeSeconds, float strength) {
    if (!handle || bodyIndex >= handle->world.floats.size() || !std::isfinite(x) || !std::isfinite(z) ||
        !std::isfinite(timeSeconds) || !std::isfinite(strength)) return 0;
    World& world = handle->world;
    const auto existing = std::find_if(world.waterImpacts.begin(), world.waterImpacts.end(),
        [bodyIndex](const WaterImpact& impact) { return impact.sourceBody == bodyIndex; });
    const WaterImpact event{bodyIndex, x, z, timeSeconds, std::clamp(strength, 0.0f, 1.0f)};
    if (existing != world.waterImpacts.end()) *existing = event;
    else world.waterImpacts.push_back(event);
    return 1;
}
}
