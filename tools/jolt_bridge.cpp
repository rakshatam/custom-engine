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
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
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
    float halfX = 0.5f;
    float halfY = 0.5f;
    float halfZ = 0.5f;
    float waterRadius = 0.5f;
    bool ellipsoid = false;
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
        floats.reserve(256);
        waterImpacts.reserve(256);
    }
};

bool g_joltInitialized = false;
uint32_t g_worldCount = 0;

Engine::OceanWaveSample sampleWaterSurface(const World& world, float x, float z, float t, int32_t excludedBody) {
    Engine::OceanWaveSample wave = Engine::evaluateOceanWave(x, z, t);
    for (const WaterImpact& impact : world.waterImpacts) {
        // Keep the source body's wake in the sampled field so its next buoyancy
        // step feeds back from the impact. Its body-displacement field is
        // excluded below to avoid a self-lifting equilibrium artifact.
        const Engine::OceanImpactWaveState impactState{impact.x, impact.z, impact.time, impact.strength};
        const Engine::OceanWaveSample impactWave = Engine::evaluateOceanImpactWave(x, z, t, impactState);
        wave.height += impactWave.height;
        wave.slopeX += impactWave.slopeX;
        wave.slopeZ += impactWave.slopeZ;
        wave.verticalVelocity += impactWave.verticalVelocity;
    }
    const JPH::BodyInterface& bodies = world.physics.GetBodyInterface();
    for (size_t index = 0; index < world.floats.size(); ++index) {
        if (static_cast<int32_t>(index) == excludedBody) continue;
        const FloatBody& item = world.floats[index];
        const JPH::RVec3 position = bodies.GetPosition(item.id);
        const JPH::Vec3 velocity = bodies.GetLinearVelocity(item.id);
        const JPH::Quat rotation = bodies.GetRotation(item.id);
        const float rotation4[4] = {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()};
        float verticalSupport = 0.0f;
        const bool supportValid = item.ellipsoid
            ? Engine::tryGetOrientedEllipsoidVerticalHalfExtent(rotation4, item.halfX, item.halfY,
                                                                item.halfZ, verticalSupport)
            : Engine::tryGetOrientedBoxVerticalHalfExtent(rotation4, item.halfX, item.halfY,
                                                           item.halfZ, verticalSupport);
        if (!supportValid) continue;
        const Engine::OceanBodyWaveState body{
            static_cast<float>(position.GetX()), static_cast<float>(position.GetY()),
            static_cast<float>(position.GetZ()), item.waterRadius, verticalSupport,
            velocity.GetX(), velocity.GetY(), velocity.GetZ()
        };
        const Engine::OceanWaveSample displacement =
            Engine::evaluateOceanBodyDisplacement(x, z, t, body);
        wave.height += displacement.height;
        wave.slopeX += displacement.slopeX;
        wave.slopeZ += displacement.slopeZ;
        wave.verticalVelocity += displacement.verticalVelocity;
    }
    if (!std::isfinite(wave.height) || !std::isfinite(wave.slopeX) ||
        !std::isfinite(wave.slopeZ) || !std::isfinite(wave.verticalVelocity)) return {};
    return wave;
}

void waveSurface(const World& world, float x, float z, float t, int32_t excludedBody,
                 JPH::RVec3& position, JPH::Vec3& normal, JPH::Vec3& velocity) {
    const Engine::OceanWaveSample wave = sampleWaterSurface(world, x, z, t, excludedBody);
    const float normalLength = std::hypot(1.0f, wave.slopeX, wave.slopeZ);
    const float inverseNormalLength = std::isfinite(normalLength) && normalLength > 1.0e-8f
        ? 1.0f / normalLength : 1.0f;
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
    try {
        JoltWorldHandle* handle = new JoltWorldHandle();
        ++g_worldCount;
        return handle;
    } catch (...) {
        trace("Could not allocate a Jolt world handle.");
        return nullptr;
    }
}

__declspec(dllexport) void JoltBridge_DestroyWorld(JoltWorldHandle* handle) {
    if (!handle) return;
    delete handle;
    if (g_worldCount > 0) --g_worldCount;
    if (g_joltInitialized && g_worldCount == 0) {
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
    if (!handle || shapeKind < 0 || shapeKind > 2 ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(halfX) || !std::isfinite(halfY) || !std::isfinite(halfZ) ||
        !std::isfinite(buoyancy) || halfX <= 1.0e-4f || halfY <= 1.0e-4f || halfZ <= 1.0e-4f ||
        handle->world.floats.size() >= 256) return -1;
    try {
        JPH::RefConst<JPH::Shape> shape;
        if (shapeKind == 0) {
            shape = new JPH::SphereShape(halfX);
        } else if (shapeKind == 1) {
            shape = new JPH::BoxShape(JPH::Vec3(halfX, halfY, halfZ), 0.04f);
        } else {
            JPH::RefConst<JPH::Shape> unitSphere = new JPH::SphereShape(1.0f);
            shape = new JPH::ScaledShape(unitSphere.GetPtr(), JPH::Vec3(halfX, halfY, halfZ));
        }
        JPH::BodyCreationSettings settings(shape, JPH::RVec3(x, y, z), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, kMovingLayer);
        settings.mFriction = 0.55f;
        settings.mRestitution = 0.12f;
        settings.mLinearDamping = 0.12f;
        settings.mAngularDamping = 0.35f;
        JPH::BodyInterface& bodies = handle->world.physics.GetBodyInterface();
        const JPH::BodyID id = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
        if (id.IsInvalid()) return -1;
        const float radius = std::sqrt(halfX * halfZ);
        if (!std::isfinite(radius) || radius <= 1.0e-4f) {
            bodies.RemoveBody(id);
            bodies.DestroyBody(id);
            return -1;
        }
        handle->world.floats.push_back({id, std::clamp(buoyancy, 0.0f, 2.0f),
                                        halfX, halfY, halfZ, radius, shapeKind != 1});
        return static_cast<int32_t>(handle->world.floats.size() - 1);
    } catch (...) {
        trace("Body creation failed while allocating a shape or body record.");
        return -1;
    }
}

__declspec(dllexport) int32_t JoltBridge_Step(JoltWorldHandle* handle, float dt, float timeSeconds) {
    if (!handle || !std::isfinite(dt) || !std::isfinite(timeSeconds) || dt <= 0.0f || dt > 0.1f) return 0;
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
                    static_cast<int32_t>(index), surfacePosition, surfaceNormal, fluidVelocity);
        bodies.ApplyBuoyancyImpulse(item.id, surfacePosition, surfaceNormal, item.buoyancy,
                                    0.35f, 0.9f, fluidVelocity, gravity, dt);
    }
    const JPH::EPhysicsUpdateError updateError =
        world.physics.Update(dt, 1, &world.tempAllocator, &world.jobs);
    if (updateError != JPH::EPhysicsUpdateError::None) {
        trace("PhysicsSystem::Update reported error flags 0x%x.", static_cast<unsigned>(updateError));
        return 0;
    }
    return 1;
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
    for (int i = 0; i < 3; ++i) if (!std::isfinite(position3[i])) return 0;
    for (int i = 0; i < 4; ++i) if (!std::isfinite(rotation4[i])) return 0;
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
    if (!std::isfinite(sample.height) || !std::isfinite(sample.slopeX) ||
        !std::isfinite(sample.slopeZ) || !std::isfinite(sample.verticalVelocity)) return 0;
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
