#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>
#include "Scene.h"
#include "OceanWave.h"
#include "../core/Window.h"
#include <iostream>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <utility>
#include <array>
#include <limits>
#include <filesystem>

namespace Engine {

static inline uint64_t alignTo(uint64_t size, uint64_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

SceneEntity* Scene::getEntity(EntityID id) {
    for (auto& e : m_entities) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

bool Scene::updateEntityMaterial(EntityID id) {
    const auto entityIt = std::find_if(m_entities.begin(), m_entities.end(), [id](const SceneEntity& entity) {
        return entity.id == id;
    });
    if (entityIt == m_entities.end()) return false;

    const size_t entityIndex = static_cast<size_t>(std::distance(m_entities.begin(), entityIt));
    if (entityIndex >= m_allInstances.size()) return false;

    InstanceData& instance = m_allInstances[entityIndex];
    if (instance.materialId >= m_allMaterials.size()) return false;

    const uint32_t originalMaterialId = instance.materialId;
    const size_t materialUsers = static_cast<size_t>(std::count_if(m_allInstances.begin(), m_allInstances.end(),
        [originalMaterialId](const InstanceData& other) { return other.materialId == originalMaterialId; }));
    if (materialUsers > 1) {
        if (m_allMaterials.size() >= m_materialBufferCapacity) return false;
        m_allMaterials.push_back(m_allMaterials[originalMaterialId]);
        instance.materialId = static_cast<uint32_t>(m_allMaterials.size() - 1);
        m_instancesDirty = true;
    }

    MaterialData& material = m_allMaterials[instance.materialId];
    material.baseColor = XMFLOAT4(entityIt->material.baseColor.x, entityIt->material.baseColor.y,
        entityIt->material.baseColor.z, entityIt->material.baseColor.w);
    material.metallic = std::clamp(entityIt->material.metallic, 0.0f, 1.0f);
    material.roughness = std::clamp(entityIt->material.roughness, 0.04f, 1.0f);
    material.emissive = std::max(entityIt->material.emissive, 0.0f);
    // Keep imported texture IDs, alpha cutoff, and flags on the selected override.
    m_materialsDirty = true;
    ++m_queuedMaterialEdits;
    return true;
}

bool Scene::init(D3D12Device& device, const std::string& gltfPath) {
    m_camera.position = Vec3(5.3f, 2.4f, -0.4f);
    m_camera.yaw = 180.0f;
    m_camera.pitch = -1.0f;
    m_camera.fov = 65.0f;
    m_camera.moveSpeed = 6.0f;
    m_camera.lookSpeed = 0.15f;

    m_sun.direction = Vec3(-0.35f, -0.92f, 0.18f).normalized();
    m_sun.color = Vec3(1.0f, 0.96f, 0.88f);
    m_sun.intensity = 8.5f;

    m_sceneCbData.maxBounces = 1; // One indirect bounce keeps the default interactive; raise in settings for more GI.
    m_sceneCbData.frameCount = 0;
    m_sceneCbData.accumFrameCount = 1;
    m_sceneCbData.renderMode = 1; // Direct PBR + DXR shadows is the interactive default; path tracing is opt-in.
    m_sceneCbData.exposure = 1.25f;
    m_sceneCbData.roughnessOverride = -1.0f;
    m_sceneCbData.metallicOverride = -1.0f;
    m_sceneCbData.toneMappingMode = 0; // ACES Filmic
    m_sceneCbData.shadowSoftness = 0.35f;
    m_sceneCbData.enableShadows = 1;
    m_sceneCbData.enableRayTracedReflections = 1.0f;

    m_sceneCbData.enableTAA = 1; // Depth-aware temporal antialiasing enabled.
    m_sceneCbData.taaBlendWeight = 0.10f;
    m_sceneCbData.enableNRD = 1;
    m_sceneCbData.padPrev3 = 0.0f; // Ocean-only sky and ambient mode stays off for Sponza.

    m_sceneCbData.sunDirection = XMFLOAT3(m_sun.direction.x, m_sun.direction.y, m_sun.direction.z);
    m_sceneCbData.sunIntensity = m_sun.intensity;
    m_sceneCbData.sunColor = XMFLOAT4(m_sun.color.x, m_sun.color.y, m_sun.color.z, 1.0f);
    m_sceneCbData.skyZenithColor = XMFLOAT4(0.22f, 0.52f, 0.95f, 1.0f);
    m_sceneCbData.skyHorizonColor = XMFLOAT4(0.85f, 0.90f, 0.98f, 1.0f);

    bool loaded = loadAssimp(device, gltfPath);
    if (!loaded) {
        loaded = loadGltf(device, gltfPath);
    }
    m_loadedSourceAsset = loaded;
    if (!loaded || m_entities.empty()) {
        std::cout << "[Scene] Fallback: Building architectural procedural Sponza Atrium..." << std::endl;
        buildProceduralSponza(m_allVertices, m_allIndices);
    }

    return uploadGpuScene(device);
}

bool Scene::uploadGpuScene(D3D12Device& device) {
    if (m_allVertices.empty() || m_allIndices.empty() || m_allMaterials.empty() || m_allInstances.empty()) return false;
    m_totalTriangles = static_cast<uint32_t>(m_allIndices.size() / 3);
    const uint64_t vbSize = sizeof(Vertex) * m_allVertices.size();
    m_vertexBuffer = device.createBuffer(vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_vertexBuffer) return false;
    void* mapped = nullptr;
    if (FAILED(m_vertexBuffer->Map(0, nullptr, &mapped))) return false;
    std::memcpy(mapped, m_allVertices.data(), vbSize);
    m_vertexBuffer->Unmap(0, nullptr);

    const uint64_t ibSize = sizeof(uint32_t) * m_allIndices.size();
    m_indexBuffer = device.createBuffer(ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_indexBuffer || FAILED(m_indexBuffer->Map(0, nullptr, &mapped))) return false;
    std::memcpy(mapped, m_allIndices.data(), ibSize);
    m_indexBuffer->Unmap(0, nullptr);

    m_materialBufferCapacity = m_allMaterials.size() + m_entities.size();
    const uint64_t matSize = sizeof(MaterialData) * m_materialBufferCapacity;
    m_materialBuffer = device.createBuffer(matSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_materialBuffer || FAILED(m_materialBuffer->Map(0, nullptr, &mapped))) return false;
    std::memcpy(mapped, m_allMaterials.data(), sizeof(MaterialData) * m_allMaterials.size());
    m_materialBuffer->Unmap(0, nullptr);

    const uint64_t instSize = sizeof(InstanceData) * m_allInstances.size();
    m_instanceBuffer = device.createBuffer(instSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_instanceBuffer || FAILED(m_instanceBuffer->Map(0, nullptr, &mapped))) return false;
    std::memcpy(mapped, m_allInstances.data(), instSize);
    m_instanceBuffer->Unmap(0, nullptr);

    const uint64_t cbSize = alignTo(sizeof(SceneCB), 256);
    m_sceneCb = device.createBuffer(cbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_sceneCb) return false;

    device.beginFrame();
    m_blases.resize(m_entities.size());
    for (size_t i = 0; i < m_entities.size(); ++i) {
        auto& ent = m_entities[i];
        const bool isOpaque = i >= m_allInstances.size() || m_allInstances[i].materialId >= m_allMaterials.size()
            || m_allMaterials[m_allInstances[i].materialId].alphaCutoff <= 0.0f;
        m_blases[i].build(device, m_vertexBuffer.Get(), static_cast<uint64_t>(ent.mesh.vertexOffset) * sizeof(Vertex),
            ent.mesh.vertexCount, sizeof(Vertex), m_indexBuffer.Get(),
            static_cast<uint64_t>(ent.mesh.indexOffset) * sizeof(uint32_t), ent.mesh.indexCount, isOpaque);
        ent.mesh.blasIndex = static_cast<uint32_t>(i);
    }

    std::vector<Tlas::Instance> tlasInstances;
    tlasInstances.reserve(m_entities.size());
    for (size_t i = 0; i < m_entities.size(); ++i) {
        Tlas::Instance inst = {};
        inst.blasGpuAddress = m_blases[i].getGpuAddress();
        m_entities[i].transform.get3x4RowMajor(inst.transform);
        inst.instanceId = static_cast<uint32_t>(i);
        inst.hitGroupIndex = 0;
        tlasInstances.push_back(inst);
    }
    m_tlas.build(device, tlasInstances);
    device.endFrame();
    device.waitForGpu();
    return true;
}

bool Scene::initOcean(D3D12Device& device) {
    m_isOceanScene = true;
    m_camera.position = Vec3(0.0f, 4.8f, -12.5f);
    m_camera.yaw = 90.0f;
    m_camera.pitch = -4.0f;
    m_camera.fov = 65.0f;
    m_camera.moveSpeed = 10.0f;
    m_camera.lookSpeed = 0.15f;

    // Put the low sun off the camera centerline so the sea gets a broken,
    // offset highlight path instead of a narrow flare straight at the lens.
    m_sun.direction = Vec3(0.36f, -0.38f, -0.85f).normalized();
    m_sun.color = Vec3(0.94f, 0.97f, 1.0f);
    m_sun.intensity = 3.5f;
    m_sceneCbData.maxBounces = 1;
    m_sceneCbData.frameCount = 0;
    m_sceneCbData.accumFrameCount = 1;
    m_sceneCbData.renderMode = 1;
    m_sceneCbData.exposure = 1.05f;
    m_sceneCbData.roughnessOverride = -1.0f;
    m_sceneCbData.metallicOverride = -1.0f;
    m_sceneCbData.toneMappingMode = 0;
    m_sceneCbData.shadowSoftness = 0.2f;
    m_sceneCbData.enableShadows = 1;
    m_sceneCbData.enableTAA = 0; // Moving water and bodies should not leave temporal trails.
    m_sceneCbData.taaBlendWeight = 0.22f;
    m_sceneCbData.enableNRD = 0; // The moving ocean keeps its dedicated non-temporal renderer path.
    m_sceneCbData.enableRayTracedReflections = 1.0f;
    m_sceneCbData.sunDirection = XMFLOAT3(m_sun.direction.x, m_sun.direction.y, m_sun.direction.z);
    m_sceneCbData.sunIntensity = m_sun.intensity;
    m_sceneCbData.sunColor = XMFLOAT4(m_sun.color.x, m_sun.color.y, m_sun.color.z, 1.0f);
    m_sceneCbData.skyZenithColor = XMFLOAT4(0.19f, 0.31f, 0.40f, 1.0f);
    m_sceneCbData.skyHorizonColor = XMFLOAT4(0.48f, 0.58f, 0.63f, 1.0f);
    m_sceneCbData.padPrev3 = 5.0f; // Brighter open-water fill keeps unlit prop sides readable.

    MaterialData water = {};
    water.baseColor = XMFLOAT4(0.008f, 0.045f, 0.072f, 1.0f);
    water.metallic = 0.0f;
    water.roughness = 0.035f;
    water.textureId = -1;
    water.metallicRoughnessTextureId = -1;
    water.alphaCutoff = 0.0f;
    water.pad[0] = 1.0f; // Water shader branch; unused for every existing Sponza material.
    m_allMaterials.push_back(water);

    auto addEntity = [this](const std::string& name, uint32_t materialId, const std::vector<Vertex>& vertices,
                            const std::vector<uint32_t>& localIndices, const Vec3& position, const Vec3& halfExtent) {
        const uint32_t vertexOffset = static_cast<uint32_t>(m_allVertices.size());
        const uint32_t indexOffset = static_cast<uint32_t>(m_allIndices.size());
        m_allVertices.insert(m_allVertices.end(), vertices.begin(), vertices.end());
        for (uint32_t index : localIndices) m_allIndices.push_back(index);
        AABB bounds;
        bounds.expand(Vec3(-halfExtent.x, -halfExtent.y, -halfExtent.z));
        bounds.expand(Vec3( halfExtent.x,  halfExtent.y,  halfExtent.z));

        SceneEntity ent;
        ent.id = m_nextEntityId++;
        ent.name.name = name;
        ent.transform.position = position;
        ent.material.baseColor = Vec4(m_allMaterials[materialId].baseColor.x, m_allMaterials[materialId].baseColor.y,
            m_allMaterials[materialId].baseColor.z, 1.0f);
        ent.material.metallic = m_allMaterials[materialId].metallic;
        ent.material.roughness = m_allMaterials[materialId].roughness;
        ent.mesh.vertexOffset = vertexOffset;
        ent.mesh.vertexCount = static_cast<uint32_t>(vertices.size());
        ent.mesh.indexOffset = indexOffset;
        ent.mesh.indexCount = static_cast<uint32_t>(localIndices.size());
        ent.mesh.localAABB = bounds;
        m_allInstances.push_back({materialId, vertexOffset, indexOffset, 0});
        m_entities.push_back(std::move(ent));
        return m_entities.size() - 1;
    };

    // A distant ocean boundary should sit beyond the visible horizon; keep the
    // water as one large, inexpensive DXR quad instead of tessellating it.
    const float extent = 2000.0f;
    std::vector<Vertex> waterVertices(4);
    waterVertices[0] = {{-extent, 0.0f, -extent}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}};
    waterVertices[1] = {{-extent, 0.0f,  extent}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}};
    waterVertices[2] = {{ extent, 0.0f,  extent}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}};
    waterVertices[3] = {{ extent, 0.0f, -extent}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}};
    // The slab is a stable DXR intersection surface. Its shading normal and
    // Jolt buoyancy use the same multi-scale analytic wave field.
    addEntity("Ocean Water Surface", 0, waterVertices, {0, 1, 2, 0, 2, 3}, Vec3(0, 0.0f, 0), Vec3(extent, 0.01f, extent));

    const auto makeRoundedBox = [](const Vec3& half) {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        const Vec3 normals[6] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        constexpr uint32_t subdivisions = 20;
        const float radius = std::min(half.x, std::min(half.y, half.z)) * 0.30f;
        const Vec3 inner(half.x - radius, half.y - radius, half.z - radius);
        for (int face = 0; face < 6; ++face) {
            const Vec3 n = normals[face];
            Vec3 u = std::abs(n.y) > 0.5f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
            Vec3 v = Vec3::cross(n, u);
            const float normalExtent = std::abs(n.x) * half.x + std::abs(n.y) * half.y + std::abs(n.z) * half.z;
            const float uExtent = std::abs(u.x) * half.x + std::abs(u.y) * half.y + std::abs(u.z) * half.z;
            const float vExtent = std::abs(v.x) * half.x + std::abs(v.y) * half.y + std::abs(v.z) * half.z;
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            const uint32_t stride = subdivisions + 1;
            for (uint32_t y = 0; y <= subdivisions; ++y) {
                for (uint32_t x = 0; x <= subdivisions; ++x) {
                    const float uCoord = -uExtent + (2.0f * uExtent * x) / subdivisions;
                    const float vCoord = -vExtent + (2.0f * vExtent * y) / subdivisions;
                    const Vec3 point = n * normalExtent + u * uCoord + v * vCoord;
                    const Vec3 clamped(std::clamp(point.x, -inner.x, inner.x),
                                       std::clamp(point.y, -inner.y, inner.y),
                                       std::clamp(point.z, -inner.z, inner.z));
                    const Vec3 normalDelta = point - clamped;
                    const Vec3 smoothNormal = normalDelta.lengthSq() > 1.0e-12f
                        ? normalDelta.normalized() : n;
                    const Vec3 roundedPosition = clamped + smoothNormal * radius;
                    vertices.push_back({XMFLOAT3(roundedPosition.x, roundedPosition.y, roundedPosition.z),
                        XMFLOAT3(smoothNormal.x, smoothNormal.y, smoothNormal.z),
                        XMFLOAT2(static_cast<float>(x) / subdivisions, static_cast<float>(y) / subdivisions)});
                }
            }
            for (uint32_t y = 0; y < subdivisions; ++y) {
                for (uint32_t x = 0; x < subdivisions; ++x) {
                    const uint32_t a = base + y * stride + x;
                    const uint32_t b = a + stride;
                    indices.insert(indices.end(), {a, a + 1, b + 1, a, b + 1, b});
                }
            }
        }
        return std::pair{std::move(vertices), std::move(indices)};
    };

    const auto makeEllipsoid = [](const Vec3& half) {
        constexpr uint32_t longitudeSegments = 48;
        constexpr uint32_t latitudeSegments = 24;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve((longitudeSegments + 1) * (latitudeSegments + 1));
        indices.reserve(longitudeSegments * latitudeSegments * 6);
        for (uint32_t y = 0; y <= latitudeSegments; ++y) {
            const float theta = 3.14159265f * static_cast<float>(y) / latitudeSegments;
            const float sinTheta = std::sin(theta);
            const float cosTheta = std::cos(theta);
            for (uint32_t x = 0; x <= longitudeSegments; ++x) {
                const float phi = 2.0f * 3.14159265f * static_cast<float>(x) / longitudeSegments;
                const Vec3 unit(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
                const Vec3 position(unit.x * half.x, unit.y * half.y, unit.z * half.z);
                const Vec3 normal = Vec3(unit.x / half.x, unit.y / half.y, unit.z / half.z).normalized();
                vertices.push_back({XMFLOAT3(position.x, position.y, position.z),
                    XMFLOAT3(normal.x, normal.y, normal.z),
                    XMFLOAT2(static_cast<float>(x) / longitudeSegments, static_cast<float>(y) / latitudeSegments)});
            }
        }
        const uint32_t stride = longitudeSegments + 1;
        for (uint32_t y = 0; y < latitudeSegments; ++y) {
            for (uint32_t x = 0; x < longitudeSegments; ++x) {
                const uint32_t a = y * stride + x;
                const uint32_t b = a + stride;
                indices.insert(indices.end(), {a, a + 1, b + 1, a, b + 1, b});
            }
        }
        return std::pair{std::move(vertices), std::move(indices)};
    };

    const struct { const char* name; Vec3 position; Vec3 half; XMFLOAT4 color; bool buoy; } floats[] = {
        {"Floating Cedar Crate", {-3.6f, 2.7f, -2.0f}, {0.85f, 0.65f, 0.70f}, {0.55f, 0.27f, 0.10f, 1}, false},
        {"Floating Coral Buoy", {-1.3f, 3.4f,  0.8f}, {0.55f, 0.85f, 0.55f}, {0.95f, 0.24f, 0.09f, 1}, true},
        {"Floating Teal Cargo", { 1.3f, 2.9f,  3.0f}, {0.95f, 0.60f, 0.75f}, {0.05f, 0.65f, 0.60f, 1}, false},
        {"Floating Golden Crate", { 3.8f, 3.8f,  0.0f}, {0.75f, 0.72f, 0.75f}, {0.95f, 0.65f, 0.12f, 1}, false},
        {"Floating Ivory Buoy", { 0.1f, 2.5f, -4.1f}, {0.52f, 0.52f, 0.52f}, {0.85f, 0.90f, 0.82f, 1}, true},
        {"Floating Blue Crate", {-4.2f, 3.2f,  4.0f}, {0.72f, 0.72f, 0.72f}, {0.12f, 0.35f, 0.88f, 1}, false},
    };
    std::vector<size_t> floatEntityIndices;
    for (const auto& item : floats) {
        MaterialData mat = {};
        mat.baseColor = item.color;
        mat.roughness = item.buoy ? 0.28f : 0.38f;
        mat.metallic = item.buoy ? 0.05f : 0.0f;
        mat.emissive = 0.015f;
        mat.textureId = -1;
        mat.metallicRoughnessTextureId = -1;
        const uint32_t materialId = static_cast<uint32_t>(m_allMaterials.size());
        m_allMaterials.push_back(mat);
        auto [vertices, indices] = item.buoy ? makeEllipsoid(item.half) : makeRoundedBox(item.half);
        floatEntityIndices.push_back(addEntity(item.name, materialId, vertices, indices, item.position, item.half));
    }

    if (!createTextureArray(device, {}, {})) return false;
    if (!uploadGpuScene(device)) return false;
    m_joltWorld = std::make_unique<JoltPhysicsWorld>();
    if (!m_joltWorld->initialize()) return false;
    for (size_t i = 0; i < floatEntityIndices.size(); ++i) {
        const auto& ent = m_entities[floatEntityIndices[i]];
        const Vec3 h = floats[i].half;
        const int32_t body = m_joltWorld->addBox(ent.transform.position.x, ent.transform.position.y, ent.transform.position.z,
            h.x, h.y, h.z, 1.9f);
        if (body < 0) return false;
        m_floatBodyIndices.push_back(body);
        m_initialFloatHeights.push_back(ent.transform.position.y);
        m_oceanBodyHalfHeights.push_back(h.y);
        m_oceanBodyRadii.push_back(std::max(0.35f, std::sqrt(h.x * h.x + h.z * h.z) * 0.70f));
        const OceanWaveSample initialWave = evaluateOceanWave(ent.transform.position.x, ent.transform.position.z, m_oceanTime);
        m_oceanPreviousBottomClearance.push_back(ent.transform.position.y - h.y - initialWave.height);
        m_oceanImpactTimes.push_back(-1000.0f);
        m_oceanImpactStrengths.push_back(0.0f);
        m_oceanImpactPositions.push_back(ent.transform.position);
    }
    m_sceneCbData.oceanBodyCount = static_cast<uint32_t>(std::min<size_t>(m_floatBodyIndices.size(), 8));
    std::cout << "[Scene] Ocean scene ready: Jolt bodies=" << m_joltWorld->bodyCount()
              << ", ray-traced water reflections, DXR shadows; Sponza scene remains separate." << std::endl;
    return true;
}

bool Scene::initReflectionLab(D3D12Device& device, const std::string& gltfPath) {
    m_isOceanScene = false;
    m_isReflectionLabScene = false;
    m_sceneCbData = {};
    m_camera.moveSpeed = 6.0f;
    m_camera.lookSpeed = 0.15f;
    m_sun.direction = Vec3(-0.35f, -0.92f, 0.18f).normalized();
    m_sun.color = Vec3(1.0f, 0.96f, 0.88f);
    m_sun.intensity = 4.5f;
    m_sceneCbData.maxBounces = 0;
    m_sceneCbData.accumFrameCount = 1;
    m_sceneCbData.renderMode = 1;
    m_sceneCbData.exposure = 1.1f;
    m_sceneCbData.roughnessOverride = -1.0f;
    m_sceneCbData.metallicOverride = -1.0f;
    m_sceneCbData.enableShadows = 1;
    m_sceneCbData.enableRayTracedReflections = 1.0f;
    m_sceneCbData.enableTAA = 1;
    m_sceneCbData.taaBlendWeight = 0.10f;
    if (!loadGltf(device, gltfPath) || m_entities.empty()) return false;
    m_loadedSourceAsset = true;

    Vec3 boundsMin(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    Vec3 boundsMax(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
    for (const SceneEntity& entity : m_entities) {
        const AABB& local = entity.mesh.localAABB;
        const Vec3 scale = entity.transform.scale;
        const Vec3 corners[8] = {
            Vec3(local.min.x, local.min.y, local.min.z), Vec3(local.max.x, local.min.y, local.min.z),
            Vec3(local.min.x, local.max.y, local.min.z), Vec3(local.min.x, local.min.y, local.max.z),
            Vec3(local.max.x, local.max.y, local.min.z), Vec3(local.max.x, local.min.y, local.max.z),
            Vec3(local.min.x, local.max.y, local.max.z), Vec3(local.max.x, local.max.y, local.max.z)
        };
        for (const Vec3& corner : corners) {
            const Vec3 world = entity.transform.position + Vec3(corner.x * scale.x, corner.y * scale.y, corner.z * scale.z);
            boundsMin.x = std::min(boundsMin.x, world.x); boundsMin.y = std::min(boundsMin.y, world.y); boundsMin.z = std::min(boundsMin.z, world.z);
            boundsMax.x = std::max(boundsMax.x, world.x); boundsMax.y = std::max(boundsMax.y, world.y); boundsMax.z = std::max(boundsMax.z, world.z);
        }
    }
    if (!std::isfinite(boundsMin.x) || !std::isfinite(boundsMax.x)) return false;

    const Vec3 sourceCenter = (boundsMin + boundsMax) * 0.5f;
    const Vec3 sourceDimensions = boundsMax - boundsMin;
    const float sourceDiameter = sourceDimensions.length();
    if (!std::isfinite(sourceDiameter) || sourceDiameter <= 1.0e-6f) return false;
    // Some Khronos test glTFs are authored in millimeters (this sample's
    // diameter is about 0.01 units). Normalize only this lab scene so imported
    // assets fill the viewport and have useful camera near/far precision.
    const float unitScale = std::clamp(8.0f / sourceDiameter, 1.0e-4f, 1.0e4f);
    for (SceneEntity& entity : m_entities) {
        entity.transform.position = sourceCenter + (entity.transform.position - sourceCenter) * unitScale;
        entity.transform.scale = entity.transform.scale * unitScale;
    }
    m_transformsDirty = true;

    const Vec3 center = sourceCenter;
    const float radius = std::max(sourceDiameter * unitScale * 0.5f, 0.5f);
    m_camera.fov = 50.0f;
    m_camera.nearPlane = std::max(radius * 0.005f, 0.01f);
    m_camera.farPlane = std::max(radius * 100.0f, 100.0f);
    const float distance = radius * 1.35f / std::tan(m_camera.fov * 0.5f * (3.14159265f / 180.0f));
    m_camera.position = center + Vec3(0.0f, radius * 0.16f, -distance);
    const Vec3 target = center - m_camera.position;
    const float targetLength = std::max(target.length(), 1.0e-4f);
    m_camera.yaw = std::atan2(target.z, target.x) * (180.0f / 3.14159265f);
    m_camera.pitch = std::asin(std::clamp(target.y / targetLength, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
    m_camera.moveSpeed = std::max(radius * 0.8f, 1.0f);

    m_sun.direction = Vec3(-0.42f, -0.64f, 0.64f).normalized();
    m_sun.color = Vec3(1.0f, 0.91f, 0.77f);
    m_sun.intensity = 4.5f;
    m_sceneCbData.renderMode = 1;
    m_sceneCbData.maxBounces = 0;
    m_sceneCbData.enableNRD = 0;
    m_sceneCbData.enableRayTracedReflections = 1.0f;
    m_sceneCbData.exposure = 1.1f;
    m_sceneCbData.skyZenithColor = XMFLOAT4(0.16f, 0.25f, 0.40f, 1.0f);
    m_sceneCbData.skyHorizonColor = XMFLOAT4(0.82f, 0.67f, 0.46f, 1.0f);
    m_sceneCbData.sunDirection = XMFLOAT3(m_sun.direction.x, m_sun.direction.y, m_sun.direction.z);
    m_sceneCbData.sunIntensity = m_sun.intensity;
    m_sceneCbData.sunColor = XMFLOAT4(m_sun.color.x, m_sun.color.y, m_sun.color.z, 1.0f);
    m_isReflectionLabScene = true;
    m_isOceanScene = false;
    if (!createTextureArray(device, {}, {}) || !uploadGpuScene(device)) return false;
    resetAccumulation();
    float minRoughness = 1.0f, maxRoughness = 0.0f, maxMetallic = 0.0f;
    for (const MaterialData& material : m_allMaterials) {
        minRoughness = std::min(minRoughness, material.roughness);
        maxRoughness = std::max(maxRoughness, material.roughness);
        maxMetallic = std::max(maxMetallic, material.metallic);
    }
    std::cout << "[Reflection Lab] Loaded Khronos glTF PBR sample: " << gltfPath
              << ", entities=" << m_entities.size() << ", materials=" << m_allMaterials.size()
              << ", roughness=" << minRoughness << ".." << maxRoughness << ", max metallic=" << maxMetallic
              << ", DXR reflections enabled.\n";
    return true;
}

bool Scene::createTextureArray(D3D12Device& device, const std::vector<std::string>& textureUris,
                               const std::vector<uint8_t>& srgbLayers) {
    if (!textureUris.empty() && srgbLayers.size() != textureUris.size()) {
        std::cerr << "[Scene Error] Texture color-space metadata does not match texture-array layer count.\n";
        return false;
    }
    const UINT texWidth = 1024;
    const UINT texHeight = 1024;
    const UINT arraySize = textureUris.empty() ? 1 : static_cast<UINT>(textureUris.size());
    UINT mipLevels = 1;
    for (UINT dimension = std::max(texWidth, texHeight); dimension > 1; dimension >>= 1) ++mipLevels;
    const UINT subresourceCount = arraySize * mipLevels;

    static const std::array<float, 256> srgbToLinear = [] {
        std::array<float, 256> lut{};
        for (size_t i = 0; i < lut.size(); ++i) {
            const float value = static_cast<float>(i) / 255.0f;
            lut[i] = value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        }
        return lut;
    }();
    static const std::array<uint8_t, 65536> linearToSrgb = [] {
        std::array<uint8_t, 65536> lut{};
        for (size_t i = 0; i < lut.size(); ++i) {
            const float value = static_cast<float>(i) / 65535.0f;
            const float encoded = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
            lut[i] = static_cast<uint8_t>(std::clamp(std::lround(encoded * 255.0f), 0l, 255l));
        }
        return lut;
    }();
    const auto encodeLinear = [](float value) -> uint8_t {
        const size_t index = static_cast<size_t>(std::clamp(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f), 0l, 65535l));
        return linearToSrgb[index];
    };

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
    std::vector<UINT> numRows(subresourceCount);
    std::vector<UINT64> rowSizes(subresourceCount);
    UINT64 totalUploadSize = 0;

    // Allocate default texture array resource
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.MipLevels = static_cast<UINT16>(mipLevels);
    // Store authored bytes in UNORM so color textures can be decoded explicitly
    // while packed metallic/roughness layers stay linear data.
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.Width = texWidth;
    texDesc.Height = texHeight;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    texDesc.DepthOrArraySize = static_cast<UINT16>(arraySize);
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    device.getDevice()->GetCopyableFootprints(&texDesc, 0, subresourceCount, 0,
                                               footprints.data(), numRows.data(), rowSizes.data(), &totalUploadSize);

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CreationNodeMask = 1;
    defaultHeap.VisibleNodeMask = 1;

    HRESULT hr = device.getDevice()->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_textureArray)
    );
    if (FAILED(hr)) {
        std::cerr << "[Scene] Failed to create Texture2DArray resource: " << std::hex << hr << std::endl;
        return false;
    }

    // Allocate upload buffer
    ComPtr<ID3D12Resource> uploadBuffer = device.createBuffer(totalUploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!uploadBuffer) return false;

    uint8_t* mappedUpload = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedUpload));

    for (UINT layer = 0; layer < arraySize; ++layer) {
        const bool srgbLayer = layer < srgbLayers.size() && srgbLayers[layer] != 0;
        std::vector<uint8_t> mipPixels(static_cast<size_t>(texWidth) * texHeight * 4, 255);
        if (!textureUris.empty()) {
            int w = 0, h = 0, comp = 0;
            unsigned char* pixels = stbi_load(textureUris[layer].c_str(), &w, &h, &comp, 4);
            if (pixels) {
                if (w == (int)texWidth && h == (int)texHeight) {
                    std::memcpy(mipPixels.data(), pixels, mipPixels.size());
                } else {
                    for (UINT y = 0; y < texHeight; ++y) {
                        const float sourceY = std::clamp((static_cast<float>(y) + 0.5f) * h / texHeight - 0.5f, 0.0f, static_cast<float>(h - 1));
                        const int y0 = static_cast<int>(sourceY);
                        const int y1 = std::min(y0 + 1, h - 1);
                        const float fy = sourceY - y0;
                        for (UINT x = 0; x < texWidth; ++x) {
                            const float sourceX = std::clamp((static_cast<float>(x) + 0.5f) * w / texWidth - 0.5f, 0.0f, static_cast<float>(w - 1));
                            const int x0 = static_cast<int>(sourceX);
                            const int x1 = std::min(x0 + 1, w - 1);
                            const float fx = sourceX - x0;
                            const int dst = (y * texWidth + x) * 4;
                            const int samples[4] = { (y0 * w + x0) * 4, (y0 * w + x1) * 4,
                                                     (y1 * w + x0) * 4, (y1 * w + x1) * 4 };
                            const float weights[4] = { (1.0f - fx) * (1.0f - fy), fx * (1.0f - fy),
                                                       (1.0f - fx) * fy, fx * fy };
                            for (int channel = 0; channel < 3; ++channel) {
                                float linear = 0.0f;
                                for (int sample = 0; sample < 4; ++sample) {
                                    const uint8_t value = pixels[samples[sample] + channel];
                                    linear += (srgbLayer ? srgbToLinear[value] : value / 255.0f) * weights[sample];
                                }
                                mipPixels[dst + channel] = srgbLayer ? encodeLinear(linear)
                                    : static_cast<uint8_t>(std::clamp(std::lround(linear * 255.0f), 0l, 255l));
                            }
                            float alpha = 0.0f;
                            for (int sample = 0; sample < 4; ++sample)
                                alpha += (pixels[samples[sample] + 3] / 255.0f) * weights[sample];
                            mipPixels[dst + 3] = static_cast<uint8_t>(std::clamp(std::lround(alpha * 255.0f), 0l, 255l));
                        }
                    }
                }
                stbi_image_free(pixels);
            }
        }

        UINT mipWidth = texWidth;
        UINT mipHeight = texHeight;
        for (UINT mip = 0; mip < mipLevels; ++mip) {
            const UINT subresource = layer * mipLevels + mip;
            const auto& footprint = footprints[subresource];
            const size_t sourcePitch = static_cast<size_t>(mipWidth) * 4;
            for (UINT row = 0; row < numRows[subresource]; ++row) {
                std::memcpy(mappedUpload + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                            mipPixels.data() + static_cast<size_t>(row) * sourcePitch,
                            static_cast<size_t>(rowSizes[subresource]));
            }

            if (mip + 1 >= mipLevels) break;
            const UINT nextWidth = std::max(1u, mipWidth / 2);
            const UINT nextHeight = std::max(1u, mipHeight / 2);
            std::vector<uint8_t> nextPixels(static_cast<size_t>(nextWidth) * nextHeight * 4);
            for (UINT y = 0; y < nextHeight; ++y) {
                for (UINT x = 0; x < nextWidth; ++x) {
                    const size_t dst = (static_cast<size_t>(y) * nextWidth + x) * 4;
                    const size_t taps[4] = {
                        (static_cast<size_t>(y * 2) * mipWidth + x * 2) * 4,
                        (static_cast<size_t>(y * 2) * mipWidth + std::min(x * 2 + 1, mipWidth - 1)) * 4,
                        (static_cast<size_t>(std::min(y * 2 + 1, mipHeight - 1)) * mipWidth + x * 2) * 4,
                        (static_cast<size_t>(std::min(y * 2 + 1, mipHeight - 1)) * mipWidth + std::min(x * 2 + 1, mipWidth - 1)) * 4
                    };
                    for (int channel = 0; channel < 3; ++channel) {
                        float avg = 0.0f;
                        for (size_t tap : taps) {
                            const uint8_t value = mipPixels[tap + channel];
                            avg += srgbLayer ? srgbToLinear[value] : value / 255.0f;
                        }
                        avg *= 0.25f;
                        nextPixels[dst + channel] = srgbLayer ? encodeLinear(avg)
                            : static_cast<uint8_t>(std::clamp(std::lround(avg * 255.0f), 0l, 255l));
                    }
                    const uint32_t alpha = mipPixels[taps[0] + 3] + mipPixels[taps[1] + 3] + mipPixels[taps[2] + 3] + mipPixels[taps[3] + 3];
                    nextPixels[dst + 3] = static_cast<uint8_t>((alpha + 2u) / 4u);
                }
            }
            mipPixels = std::move(nextPixels);
            mipWidth = nextWidth;
            mipHeight = nextHeight;
        }
    }
    uploadBuffer->Unmap(0, nullptr);

    // Record copy commands
    device.beginFrame();
    auto cmdList = device.getCommandList();
    for (UINT i = 0; i < subresourceCount; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = m_textureArray.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = i;

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = uploadBuffer.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprints[i];

        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }

    device.transitionBarrier(m_textureArray.Get(), D3D12_RESOURCE_STATE_COPY_DEST, 
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    device.endFrame();
    device.waitForGpu();

    // Create SRV in descriptor heap
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = mipLevels;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = arraySize;

    m_textureArraySrvIndex = device.allocateSrvUavDescriptor();
    device.getDevice()->CreateShaderResourceView(
        m_textureArray.Get(),
        &srvDesc,
        device.getSrvUavCpuHandle(m_textureArraySrvIndex)
    );

    std::cout << "[Scene] Created Texture2DArray with " << arraySize 
              << " layers (1024x1024 RGBA8, " << mipLevels << " color-correct mips), SRV index = " << m_textureArraySrvIndex << std::endl;
    return true;
}

bool Scene::loadAssimp(D3D12Device& device, const std::string& path) {
    const aiScene* scene = aiImportFile(path.c_str(), 
        aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace | 
        aiProcess_JoinIdenticalVertices | aiProcess_PreTransformVertices | aiProcess_FlipUVs);
    if (!scene || !scene->mRootNode || scene->mNumMeshes == 0) {
        std::cerr << "[Scene] Assimp failed to load " << path << ": " << aiGetErrorString() << std::endl;
        return false;
    }

    m_allVertices.clear();
    m_allIndices.clear();
    m_allMaterials.clear();
    m_allInstances.clear();
    m_entities.clear();

    // 1. Collect materials and texture paths
    std::vector<std::string> texturePaths;
    std::vector<uint8_t> srgbTextureLayers;
    uint32_t missingMetallicRoughnessMaps = 0;
    const bool isBundledSponza = std::filesystem::path(path).filename() == "Sponza.gltf";
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        const aiMaterial* mat = scene->mMaterials[i];
        MaterialData md = {};
        md.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        md.metallic = 1.0f;
        md.roughness = 1.0f;
        md.emissive = 0.0f;
        md.textureId = -1;
        md.metallicRoughnessTextureId = -1;
        md.alphaCutoff = 0.0f;

        // Assimp exposes these glTF PBR factors as linear material properties.
        // glTF defaults both factors to 1 when omitted.
        mat->Get(AI_MATKEY_METALLIC_FACTOR, md.metallic);
        mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, md.roughness);

        aiColor4D diffColor;
        if (aiGetMaterialColor(mat, AI_MATKEY_BASE_COLOR, &diffColor) == AI_SUCCESS ||
            aiGetMaterialColor(mat, AI_MATKEY_COLOR_DIFFUSE, &diffColor) == AI_SUCCESS) {
            md.baseColor = XMFLOAT4(diffColor.r, diffColor.g, diffColor.b, diffColor.a);
        }

        aiString texPath;
        if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
            std::string uri = texPath.C_Str();
            std::string fullPath = "assets/sponza/" + uri;
            md.textureId = static_cast<int>(texturePaths.size());
            texturePaths.push_back(fullPath);
            srgbTextureLayers.push_back(1);
            md.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

            // Foliage / Cutout detection
            if (uri.find(".png") != std::string::npos && uri != "white.png") {
                md.alphaCutoff = 0.5f;
            }
        }
        if (mat->GetTexture(aiTextureType_GLTF_METALLIC_ROUGHNESS, 0, &texPath) == AI_SUCCESS) {
            const std::string metallicRoughnessPath = "assets/sponza/" + std::string(texPath.C_Str());
            if (std::filesystem::exists(metallicRoughnessPath)) {
                md.metallicRoughnessTextureId = static_cast<int>(texturePaths.size());
                texturePaths.push_back(metallicRoughnessPath);
                srgbTextureLayers.push_back(0);
            } else {
                // A white placeholder for a missing glTF metallic/roughness map
                // multiplies default factor 1/1 into a fully metallic, fully
                // rough material. That suppresses diffuse color and incorrectly
                // removes most surfaces from the specular-reflection path. Use
                // a conservative dielectric fallback until the source map exists.
                md.metallic = 0.0f;
                // In this bundled Sponza glTF, material 9 is the large,
                // horizontal stone floor (the source primitive spans the
                // atrium at y=-2.506 with upward-facing normals). Give only
                // that missing-map surface a polished-stone fallback so the
                // existing single-ray reflection path can show floor returns
                // without tracing every rough banner and masonry pixel.
                md.roughness = isBundledSponza && i == 9 ? 0.30f : 0.65f;
                ++missingMetallicRoughnessMaps;
                std::cerr << "[Scene Warning] Missing metallic/roughness map '" << metallicRoughnessPath
                          << "'; using dielectric fallback (metallic=0, roughness=" << md.roughness << ").\n";
            }
        }
        m_allMaterials.push_back(md);
    }

    if (missingMetallicRoughnessMaps > 0) {
        std::cerr << "[Scene Warning] Applied dielectric fallback to " << missingMetallicRoughnessMaps
                  << " materials with unavailable metallic/roughness maps.\n";
    }

    // 2. Create GPU Texture2DArray for all textures
    if (!createTextureArray(device, texturePaths, srgbTextureLayers)) {
        aiReleaseImport(scene);
        return false;
    }

    // 3. Extract meshes into vertices, indices, instances, and entities
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        uint32_t vOffset = static_cast<uint32_t>(m_allVertices.size());
        uint32_t iOffset = static_cast<uint32_t>(m_allIndices.size());

        AABB localAABB;
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            Vertex vert = {};
            vert.position = XMFLOAT3(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z);
            if (mesh->HasNormals()) {
                vert.normal = XMFLOAT3(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z);
            } else {
                vert.normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
            }
            if (mesh->HasTextureCoords(0)) {
                vert.uv = XMFLOAT2(mesh->mTextureCoords[0][v].x, mesh->mTextureCoords[0][v].y);
            } else {
                vert.uv = XMFLOAT2(0.0f, 0.0f);
            }
            localAABB.expand(Vec3(vert.position.x, vert.position.y, vert.position.z));
            m_allVertices.push_back(vert);
        }

        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices == 3) {
                m_allIndices.push_back(face.mIndices[0]);
                m_allIndices.push_back(face.mIndices[1]);
                m_allIndices.push_back(face.mIndices[2]);
            }
        }

        uint32_t matId = mesh->mMaterialIndex < m_allMaterials.size() ? mesh->mMaterialIndex : 0;

        SceneEntity ent;
        ent.id = m_nextEntityId++;
        ent.name.name = mesh->mName.length > 0 ? mesh->mName.C_Str() : ("Sponza_Submesh_" + std::to_string(ent.id));
        ent.transform.position = Vec3(0.0f, 0.0f, 0.0f);
        ent.transform.scale = Vec3(1.0f, 1.0f, 1.0f);

        ent.mesh.vertexOffset = vOffset;
        ent.mesh.vertexCount = mesh->mNumVertices;
        ent.mesh.indexOffset = iOffset;
        ent.mesh.indexCount = mesh->mNumFaces * 3;
        ent.mesh.localAABB = localAABB;

        ent.material.baseColor = Vec4(m_allMaterials[matId].baseColor.x, m_allMaterials[matId].baseColor.y, m_allMaterials[matId].baseColor.z, 1.0f);
        ent.material.metallic = m_allMaterials[matId].metallic;
        ent.material.roughness = m_allMaterials[matId].roughness;

        InstanceData instData = {};
        instData.materialId = matId;
        instData.vertexOffset = vOffset;
        instData.indexOffset = iOffset;
        m_allInstances.push_back(instData);

        m_entities.push_back(ent);
    }

    aiReleaseImport(scene);

    // Compute scene bounds
    AABB sceneBounds;
    for (const auto& v : m_allVertices) {
        sceneBounds.expand(Vec3(v.position.x, v.position.y, v.position.z));
    }

    std::cout << "[Scene] Assimp loaded Sponza: " << m_entities.size() << " submeshes, " 
              << m_allVertices.size() << " vertices, " << m_allIndices.size() / 3 << " triangles."
              << " Bounds: X[" << sceneBounds.min.x << ", " << sceneBounds.max.x << "] "
              << "Y[" << sceneBounds.min.y << ", " << sceneBounds.max.y << "] "
              << "Z[" << sceneBounds.min.z << ", " << sceneBounds.max.z << "]" << std::endl;
    return !m_entities.empty();
}

bool Scene::loadGltf(D3D12Device& device, const std::string& path) {
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, path.c_str(), &data);
    if (result != cgltf_result_success) return false;

    result = cgltf_load_buffers(&options, data, path.c_str());
    if (result != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    m_allVertices.clear();
    m_allIndices.clear();
    m_allMaterials.clear();
    m_allInstances.clear();
    m_entities.clear();

    // Map materials
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const auto& mat = data->materials[i];
        MaterialData md = {};
        md.textureId = -1;
        md.metallicRoughnessTextureId = -1;
        if (mat.has_pbr_metallic_roughness) {
            md.baseColor = XMFLOAT4(
                mat.pbr_metallic_roughness.base_color_factor[0],
                mat.pbr_metallic_roughness.base_color_factor[1],
                mat.pbr_metallic_roughness.base_color_factor[2],
                mat.pbr_metallic_roughness.base_color_factor[3]
            );
            md.metallic = mat.pbr_metallic_roughness.metallic_factor;
            md.roughness = mat.pbr_metallic_roughness.roughness_factor;

            // Sample texture colors if available
            if (mat.pbr_metallic_roughness.base_color_texture.texture &&
                mat.pbr_metallic_roughness.base_color_texture.texture->image &&
                mat.pbr_metallic_roughness.base_color_texture.texture->image->uri) {
                std::string uri = mat.pbr_metallic_roughness.base_color_texture.texture->image->uri;
                std::string fullPath = "assets/sponza/" + uri;
                int imgW = 0, imgH = 0, imgComp = 0;
                unsigned char* pixels = stbi_load(fullPath.c_str(), &imgW, &imgH, &imgComp, 3);
                if (pixels && imgW > 0 && imgH > 0) {
                    uint64_t sumR = 0, sumG = 0, sumB = 0;
                    int totalPixels = imgW * imgH;
                    int step = std::max(1, totalPixels / 4096);
                    int samples = 0;
                    for (int p = 0; p < totalPixels; p += step) {
                        sumR += pixels[p * 3 + 0];
                        sumG += pixels[p * 3 + 1];
                        sumB += pixels[p * 3 + 2];
                        samples++;
                    }
                    if (samples > 0) {
                        float r = (sumR / (float)samples) / 255.0f;
                        float g = (sumG / (float)samples) / 255.0f;
                        float b = (sumB / (float)samples) / 255.0f;
                        // Linearize sRGB to linear PBR space
                        r = std::pow(r, 2.2f);
                        g = std::pow(g, 2.2f);
                        b = std::pow(b, 2.2f);
                        md.baseColor = XMFLOAT4(r, g, b, 1.0f);
                    }
                    stbi_image_free(pixels);
                }
            }
        } else {
            md.baseColor = XMFLOAT4(0.85f, 0.85f, 0.85f, 1.0f);
            md.metallic = 0.0f;
            md.roughness = 0.5f;
        }
        md.emissive = mat.emissive_factor[0] > 0.0f ? 2.0f : 0.0f;
        m_allMaterials.push_back(md);
    }

    if (m_allMaterials.empty()) {
        MaterialData def = {};
        def.baseColor = XMFLOAT4(0.85f, 0.85f, 0.85f, 1.0f);
        def.roughness = 0.5f;
        def.textureId = -1;
        def.metallicRoughnessTextureId = -1;
        m_allMaterials.push_back(def);
    }

    // Traverse nodes / meshes
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const auto& node = data->nodes[i];
        if (!node.mesh) continue;

        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const auto& prim = node.mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            uint32_t vOffset = static_cast<uint32_t>(m_allVertices.size());
            uint32_t iOffset = static_cast<uint32_t>(m_allIndices.size());

            const float* posData = nullptr;
            const float* normData = nullptr;
            const float* uvData = nullptr;
            cgltf_size numVerts = 0;

            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const auto& attr = prim.attributes[a];
                const auto* acc = attr.data;
                const uint8_t* buf = (const uint8_t*)acc->buffer_view->buffer->data + acc->buffer_view->offset + acc->offset;
                if (attr.type == cgltf_attribute_type_position) {
                    posData = (const float*)buf;
                    numVerts = acc->count;
                } else if (attr.type == cgltf_attribute_type_normal) {
                    normData = (const float*)buf;
                } else if (attr.type == cgltf_attribute_type_texcoord) {
                    uvData = (const float*)buf;
                }
            }

            if (!posData || numVerts == 0) continue;

            float sX = node.has_scale ? node.scale[0] : 1.0f;
            float sY = node.has_scale ? node.scale[1] : 1.0f;
            float sZ = node.has_scale ? node.scale[2] : 1.0f;

            AABB localAABB;
            for (cgltf_size v = 0; v < numVerts; ++v) {
                Vertex vert = {};
                vert.position = XMFLOAT3(posData[v * 3 + 0] * sX, posData[v * 3 + 1] * sY, posData[v * 3 + 2] * sZ);
                if (normData) {
                    vert.normal = XMFLOAT3(normData[v * 3 + 0], normData[v * 3 + 1], normData[v * 3 + 2]);
                } else {
                    vert.normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
                }
                if (uvData) {
                    vert.uv = XMFLOAT2(uvData[v * 2 + 0], uvData[v * 2 + 1]);
                }
                localAABB.expand(Vec3(vert.position.x, vert.position.y, vert.position.z));
                m_allVertices.push_back(vert);
            }

            // Indices
            uint32_t primIndexCount = 0;
            if (prim.indices) {
                const auto* acc = prim.indices;
                primIndexCount = static_cast<uint32_t>(acc->count);
                const uint8_t* buf = (const uint8_t*)acc->buffer_view->buffer->data + acc->buffer_view->offset + acc->offset;
                for (cgltf_size idx = 0; idx < acc->count; ++idx) {
                    uint32_t indexVal = 0;
                    if (acc->component_type == cgltf_component_type_r_16u) {
                        indexVal = ((const uint16_t*)buf)[idx];
                    } else if (acc->component_type == cgltf_component_type_r_32u) {
                        indexVal = ((const uint32_t*)buf)[idx];
                    } else if (acc->component_type == cgltf_component_type_r_8u) {
                        indexVal = ((const uint8_t*)buf)[idx];
                    }
                    m_allIndices.push_back(indexVal);
                }
            }

            // Create Entity
            SceneEntity ent;
            ent.id = m_nextEntityId++;
            ent.name.name = node.name ? node.name : ("Sponza_Mesh_" + std::to_string(ent.id));

            if (node.has_translation) {
                ent.transform.position = Vec3(node.translation[0] * sX, node.translation[1] * sY, node.translation[2] * sZ);
            }
            ent.transform.scale = Vec3(1.0f, 1.0f, 1.0f);

            ent.mesh.vertexOffset = vOffset;
            ent.mesh.vertexCount = static_cast<uint32_t>(numVerts);
            ent.mesh.indexOffset = iOffset;
            ent.mesh.indexCount = primIndexCount;
            ent.mesh.localAABB = localAABB;

            uint32_t matId = 0;
            if (prim.material) {
                matId = static_cast<uint32_t>(prim.material - data->materials);
            }
            ent.material.baseColor = Vec4(m_allMaterials[matId].baseColor.x, m_allMaterials[matId].baseColor.y, m_allMaterials[matId].baseColor.z, 1.0f);
            ent.material.metallic = m_allMaterials[matId].metallic;
            ent.material.roughness = m_allMaterials[matId].roughness;

            InstanceData instData = {};
            instData.materialId = matId;
            instData.vertexOffset = vOffset;
            instData.indexOffset = iOffset;
            m_allInstances.push_back(instData);

            m_entities.push_back(ent);
        }
    }

    cgltf_free(data);

    // Compute scene bounds
    AABB sceneBounds;
    for (const auto& v : m_allVertices) {
        sceneBounds.expand(Vec3(v.position.x, v.position.y, v.position.z));
    }
    Vec3 sceneCenter = (sceneBounds.min + sceneBounds.max) * 0.5f;

    // Position camera inside the Sponza central atrium corridor looking down +X
    m_camera.position = Vec3(-6.0f, 2.0f, 0.0f);
    m_camera.yaw = 0.0f; // looking down +X towards the other end of the atrium
    m_camera.pitch = 2.0f; // looking slightly up at the grand two-story colonnade and atrium
    m_camera.fov = 65.0f;
    m_camera.moveSpeed = 6.0f;
    m_camera.lookSpeed = 0.15f;

    std::cout << "[Scene] Successfully loaded glTF Sponza: " << m_entities.size() << " submeshes, " 
              << m_allVertices.size() << " vertices, " << m_allIndices.size() / 3 << " triangles."
              << " Bounds: X[" << sceneBounds.min.x << ", " << sceneBounds.max.x << "] "
              << "Y[" << sceneBounds.min.y << ", " << sceneBounds.max.y << "] "
              << "Z[" << sceneBounds.min.z << ", " << sceneBounds.max.z << "]"
              << " Camera: (" << m_camera.position.x << ", " << m_camera.position.y << ", " << m_camera.position.z << ")" << std::endl;
    return !m_entities.empty();
}

void Scene::buildProceduralSponza(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices) {
    auto addBox = [&](const Vec3& center, const Vec3& extents, const XMFLOAT4& color, float metallic, float roughness, const std::string& name) {
        uint32_t vStart = static_cast<uint32_t>(outVertices.size());
        uint32_t iStart = static_cast<uint32_t>(outIndices.size());

        Vec3 h = extents * 0.5f;
        Vec3 p[8] = {
            center + Vec3(-h.x, -h.y, -h.z),
            center + Vec3( h.x, -h.y, -h.z),
            center + Vec3( h.x,  h.y, -h.z),
            center + Vec3(-h.x,  h.y, -h.z),
            center + Vec3(-h.x, -h.y,  h.z),
            center + Vec3( h.x, -h.y,  h.z),
            center + Vec3( h.x,  h.y,  h.z),
            center + Vec3(-h.x,  h.y,  h.z),
        };

        struct Face { int i0, i1, i2, i3; Vec3 n; };
        Face faces[6] = {
            { 0, 1, 2, 3, Vec3(0, 0, -1) }, // Front
            { 5, 4, 7, 6, Vec3(0, 0,  1) }, // Back
            { 4, 0, 3, 7, Vec3(-1, 0, 0) }, // Left
            { 1, 5, 6, 2, Vec3( 1, 0, 0) }, // Right
            { 3, 2, 6, 7, Vec3(0,  1, 0) }, // Top
            { 4, 5, 1, 0, Vec3(0, -1, 0) }, // Bottom
        };

        AABB aabb;
        for (int f = 0; f < 6; ++f) {
            uint32_t base = static_cast<uint32_t>(outVertices.size()) - vStart;
            const auto& face = faces[f];
            Vertex v0 = { XMFLOAT3(p[face.i0].x, p[face.i0].y, p[face.i0].z), XMFLOAT3(face.n.x, face.n.y, face.n.z), XMFLOAT2(0, 1) };
            Vertex v1 = { XMFLOAT3(p[face.i1].x, p[face.i1].y, p[face.i1].z), XMFLOAT3(face.n.x, face.n.y, face.n.z), XMFLOAT2(1, 1) };
            Vertex v2 = { XMFLOAT3(p[face.i2].x, p[face.i2].y, p[face.i2].z), XMFLOAT3(face.n.x, face.n.y, face.n.z), XMFLOAT2(1, 0) };
            Vertex v3 = { XMFLOAT3(p[face.i3].x, p[face.i3].y, p[face.i3].z), XMFLOAT3(face.n.x, face.n.y, face.n.z), XMFLOAT2(0, 0) };
            aabb.expand(p[face.i0]); aabb.expand(p[face.i1]); aabb.expand(p[face.i2]); aabb.expand(p[face.i3]);

            outVertices.push_back(v0);
            outVertices.push_back(v1);
            outVertices.push_back(v2);
            outVertices.push_back(v3);

            outIndices.push_back(base + 0);
            outIndices.push_back(base + 1);
            outIndices.push_back(base + 2);
            outIndices.push_back(base + 0);
            outIndices.push_back(base + 2);
            outIndices.push_back(base + 3);
        }

        uint32_t matId = static_cast<uint32_t>(m_allMaterials.size());
        MaterialData mat = {};
        mat.baseColor = color;
        mat.metallic = metallic;
        mat.roughness = roughness;
        mat.textureId = -1;
        mat.metallicRoughnessTextureId = -1;
        m_allMaterials.push_back(mat);

        SceneEntity ent;
        ent.id = m_nextEntityId++;
        ent.name.name = name;
        ent.transform.position = center;
        ent.material.baseColor = Vec4(color.x, color.y, color.z, color.w);
        ent.material.metallic = metallic;
        ent.material.roughness = roughness;
        ent.mesh.vertexOffset = vStart;
        ent.mesh.vertexCount = 24;
        ent.mesh.indexOffset = iStart;
        ent.mesh.indexCount = 36;
        ent.mesh.localAABB = aabb;

        InstanceData inst = {};
        inst.materialId = matId;
        inst.vertexOffset = vStart;
        inst.indexOffset = iStart;
        m_allInstances.push_back(inst);

        m_entities.push_back(ent);
    };

    // 1. The Main Floor (stone marble)
    addBox(Vec3(0.0f, -0.2f, 0.0f), Vec3(28.0f, 0.4f, 40.0f), XMFLOAT4(0.82f, 0.80f, 0.78f, 1.0f), 0.0f, 0.35f, "Floor_Courtyard");

    // 2. Colonnades: 14 Grand Pillars on East and West sides
    for (int side = -1; side <= 1; side += 2) {
        float x = side * 8.0f;
        for (int zIdx = -5; zIdx <= 5; ++zIdx) {
            float z = zIdx * 3.5f;
            // Base
            addBox(Vec3(x, 0.3f, z), Vec3(1.2f, 0.6f, 1.2f), XMFLOAT4(0.75f, 0.72f, 0.68f, 1.0f), 0.0f, 0.6f, "Column_Base_" + std::to_string(zIdx));
            // Column shaft
            addBox(Vec3(x, 4.0f, z), Vec3(0.85f, 7.0f, 0.85f), XMFLOAT4(0.92f, 0.90f, 0.88f, 1.0f), 0.0f, 0.4f, "Column_Shaft_" + std::to_string(zIdx));
            // Capital
            addBox(Vec3(x, 7.7f, z), Vec3(1.3f, 0.4f, 1.3f), XMFLOAT4(0.88f, 0.84f, 0.75f, 1.0f), 0.1f, 0.5f, "Column_Capital_" + std::to_string(zIdx));
            // Vault Arch Beam connecting columns
            if (zIdx < 5) {
                addBox(Vec3(x, 8.2f, z + 1.75f), Vec3(0.9f, 0.6f, 3.5f), XMFLOAT4(0.85f, 0.82f, 0.78f, 1.0f), 0.0f, 0.5f, "Arch_Beam_" + std::to_string(zIdx));
            }
        }
    }

    // 3. Second Tier Balcony & Arches
    addBox(Vec3(-8.0f, 9.0f, 0.0f), Vec3(3.0f, 0.5f, 38.0f), XMFLOAT4(0.86f, 0.84f, 0.80f, 1.0f), 0.0f, 0.4f, "Balcony_East");
    addBox(Vec3( 8.0f, 9.0f, 0.0f), Vec3(3.0f, 0.5f, 38.0f), XMFLOAT4(0.86f, 0.84f, 0.80f, 1.0f), 0.0f, 0.4f, "Balcony_West");

    // 4. North and South Grand Portals
    addBox(Vec3(0.0f, 4.5f, -19.5f), Vec3(18.0f, 9.0f, 1.0f), XMFLOAT4(0.78f, 0.75f, 0.70f, 1.0f), 0.0f, 0.6f, "Portal_Wall_South");
    addBox(Vec3(0.0f, 4.5f,  19.5f), Vec3(18.0f, 9.0f, 1.0f), XMFLOAT4(0.78f, 0.75f, 0.70f, 1.0f), 0.0f, 0.6f, "Portal_Wall_North");

    // 5. Grand Vaulted Ceiling Beams
    for (int zIdx = -5; zIdx <= 5; zIdx += 2) {
        float z = zIdx * 3.5f;
        addBox(Vec3(0.0f, 12.0f, z), Vec3(17.0f, 0.8f, 0.8f), XMFLOAT4(0.45f, 0.32f, 0.22f, 1.0f), 0.0f, 0.7f, "Roof_Timber_" + std::to_string(zIdx));
    }

    // 6. Royal Crimson Velvet Banners
    addBox(Vec3(-7.5f, 4.5f, -6.0f), Vec3(0.1f, 6.0f, 1.8f), XMFLOAT4(0.75f, 0.08f, 0.12f, 1.0f), 0.0f, 0.8f, "Banner_Crimson_East");
    addBox(Vec3( 7.5f, 4.5f, -6.0f), Vec3(0.1f, 6.0f, 1.8f), XMFLOAT4(0.12f, 0.45f, 0.78f, 1.0f), 0.0f, 0.8f, "Banner_Azure_West");
    addBox(Vec3(-7.5f, 4.5f,  6.0f), Vec3(0.1f, 6.0f, 1.8f), XMFLOAT4(0.75f, 0.08f, 0.12f, 1.0f), 0.0f, 0.8f, "Banner_Crimson_East2");
    addBox(Vec3( 7.5f, 4.5f,  6.0f), Vec3(0.1f, 6.0f, 1.8f), XMFLOAT4(0.12f, 0.45f, 0.78f, 1.0f), 0.0f, 0.8f, "Banner_Azure_West2");

    // 7. Center Ornate Pedestal
    addBox(Vec3(0.0f, 0.5f, 0.0f), Vec3(2.5f, 1.0f, 2.5f), XMFLOAT4(0.95f, 0.95f, 0.95f, 1.0f), 0.2f, 0.25f, "Center_Pedestal_Base");
    addBox(Vec3(0.0f, 1.8f, 0.0f), Vec3(1.2f, 1.6f, 1.2f), XMFLOAT4(0.95f, 0.85f, 0.45f, 1.0f), 0.9f, 0.15f, "Center_Gold_Statue");
}

void Scene::update(float dt, const Window& window, float viewportX, float viewportY, float viewportW, float viewportH, float renderAspect, uint32_t renderHeight) {
    m_renderHeight = std::max(1u, renderHeight);
    m_frameCount++;
    if (renderAspect > 0.0f) {
        m_viewportAspect = renderAspect;
    }

    // Camera movement
    float speed = m_camera.moveSpeed;
    if (window.isKeyDown(VK_SHIFT)) speed *= 2.5f;

    Vec3 fwd = m_camera.getForward();
    Vec3 right = m_camera.getRight();
    Vec3 up = Vec3(0.0f, 1.0f, 0.0f);

    if (window.isKeyDown('W')) m_camera.position += fwd * speed * dt;
    if (window.isKeyDown('S')) m_camera.position -= fwd * speed * dt;
    if (window.isKeyDown('D')) m_camera.position += right * speed * dt;
    if (window.isKeyDown('A')) m_camera.position -= right * speed * dt;
    if (window.isKeyDown(VK_SPACE) || window.isKeyDown('E')) m_camera.position += up * speed * dt;
    if (window.isKeyDown(VK_CONTROL) || window.isKeyDown('Q')) m_camera.position -= up * speed * dt;

    // Mouse look with Right Mouse Button
    if (window.isMouseButtonDown(1)) {
        Vec2 delta = window.getMouseDelta();
        m_camera.yaw += delta.x * m_camera.lookSpeed;
        m_camera.pitch -= delta.y * m_camera.lookSpeed;
        m_camera.pitch = std::clamp(m_camera.pitch, -88.0f, 88.0f);
        m_isLooking = true;
    } else {
        m_isLooking = false;
    }

    // Accumulation logic: check if camera moved relative to previous rendered frame
    m_cameraMoved = !m_firstFrame && (std::abs(m_camera.position.x - m_lastCameraPos.x) > 0.0005f ||
                     std::abs(m_camera.position.y - m_lastCameraPos.y) > 0.0005f ||
                     std::abs(m_camera.position.z - m_lastCameraPos.z) > 0.0005f ||
                     std::abs(m_camera.yaw - m_lastCameraYaw) > 0.005f ||
                     std::abs(m_camera.pitch - m_lastCameraPitch) > 0.005f);
    // A stationary Sponza scene can retain NRD's world-space history while the
    // camera moves; NRD reprojects it from the previous camera matrices. Reset
    // only on an explicit lighting/material/scene change. The ocean has NRD
    // disabled and keeps the conventional camera-motion reset behavior.
    const bool useNrdCameraReprojection = !m_isOceanScene &&
        m_sceneCbData.renderMode == 0 && m_sceneCbData.enableNRD != 0;
    if (m_firstFrame || (m_cameraMoved && !useNrdCameraReprojection) || m_accumResetPending) {
        m_accumFrameCount = 1;
        m_accumResetPending = false;
    } else {
        m_accumFrameCount++;
    }

    if (m_isOceanScene) updateOceanPhysics(dt);

    // Ray pick on Left Click
    if (window.isMouseButtonDown(0) && !m_isLooking) {
        Vec2 mPos = window.getMousePos();
        float relX = mPos.x - viewportX;
        float relY = mPos.y - viewportY;
        if (relX >= 0.0f && relX <= viewportW && relY >= 0.0f && relY <= viewportH) {
            EntityID hit = rayPick(relX, relY, viewportW, viewportH);
            if (hit != NULL_ENTITY) {
                m_selectedEntityId = hit;
            }
        }
    }
}

void Scene::updateOceanPhysics(float dt) {
    if (!m_joltWorld || !m_joltWorld->isReady()) return;
    const float fixedStep = 1.0f / 60.0f;
    m_oceanStepAccumulator += std::clamp(dt, 0.0f, 0.1f);
    uint32_t steps = 0;
    while (m_oceanStepAccumulator >= fixedStep && steps < 4) {
        m_oceanTime += fixedStep;
        m_joltWorld->step(fixedStep, m_oceanTime);

        // Detect entries after every physics substep. Checking only once per rendered
        // frame can miss a fast re-entry when the body rebounds between frames.
        for (size_t i = 0; i < m_floatBodyIndices.size(); ++i) {
            const uint32_t bodyIndex = static_cast<uint32_t>(m_floatBodyIndices[i]);
            float position[3] = {}, rotation[4] = {}, velocity[3] = {};
            float surface[4] = {};
            if (!m_joltWorld->getTransform(bodyIndex, position, rotation) ||
                !m_joltWorld->getLinearVelocity(bodyIndex, velocity) ||
                !m_joltWorld->getWaterSurface(bodyIndex, position[0], position[2], m_oceanTime, surface)) {
                continue;
            }
            if (!std::isfinite(position[1]) || !std::isfinite(velocity[1]) || !std::isfinite(surface[0])) continue;

            const float halfHeight = i < m_oceanBodyHalfHeights.size() ? m_oceanBodyHalfHeights[i] : 0.0f;
            const float bottomClearance = position[1] - halfHeight - surface[0];
            if (i >= m_oceanPreviousBottomClearance.size() || i >= m_oceanImpactTimes.size() ||
                i >= m_oceanImpactStrengths.size() || i >= m_oceanImpactPositions.size()) continue;
            const float previousClearance = m_oceanPreviousBottomClearance[i];
            if (previousClearance > 0.04f && bottomClearance <= 0.04f && velocity[1] < -0.45f) {
                const float strength = std::clamp(-velocity[1] / 5.0f, 0.25f, 1.0f);
                m_oceanImpactTimes[i] = m_oceanTime;
                m_oceanImpactStrengths[i] = strength;
                m_oceanImpactPositions[i] = Vec3(position[0], surface[0], position[2]);
                if (!m_joltWorld->registerWaterImpact(bodyIndex, position[0], position[2], m_oceanTime, strength)) {
                    std::cerr << "[Ocean] Failed to register water impact for body " << bodyIndex << std::endl;
                }
                ++m_oceanImpactCount;
            }
            m_oceanPreviousBottomClearance[i] = bottomClearance;
        }

        m_oceanStepAccumulator -= fixedStep;
        ++steps;
    }
    if (steps == 0) return;

    m_oceanMinBodyY = 1e30f;
    m_oceanMaxBodyY = -1e30f;
    for (size_t i = 0; i < m_floatBodyIndices.size(); ++i) {
        float position[3] = {}, rotation[4] = {};
        if (!m_joltWorld->getTransform(static_cast<uint32_t>(m_floatBodyIndices[i]), position, rotation)) continue;
        float velocity[3] = {};
        if (!m_joltWorld->getLinearVelocity(static_cast<uint32_t>(m_floatBodyIndices[i]), velocity)) continue;
        SceneEntity& ent = m_entities[i + 1]; // Entity zero is the ocean plane.
        ent.transform.position = Vec3(position[0], position[1], position[2]);
        m_oceanMinBodyY = std::min(m_oceanMinBodyY, position[1]);
        m_oceanMaxBodyY = std::max(m_oceanMaxBodyY, position[1]);
        if (i < m_initialFloatHeights.size()) {
            m_oceanMaxVerticalDisplacement = std::max(m_oceanMaxVerticalDisplacement,
                std::abs(position[1] - m_initialFloatHeights[i]));
        }

        const size_t bodySlot = i;
        if (bodySlot < 8) {
            m_sceneCbData.oceanBodyPositionRadius[bodySlot] = XMFLOAT4(
                position[0], position[1], position[2],
                bodySlot < m_oceanBodyRadii.size() ? m_oceanBodyRadii[bodySlot] : 0.5f);
            m_sceneCbData.oceanBodyVelocityImpact[bodySlot] = XMFLOAT4(
                velocity[0], velocity[2],
                bodySlot < m_oceanImpactTimes.size() ? m_oceanImpactTimes[bodySlot] : -1000.0f,
                bodySlot < m_oceanImpactStrengths.size() ? m_oceanImpactStrengths[bodySlot] : 0.0f);
            const Vec3 impactPosition = bodySlot < m_oceanImpactPositions.size()
                ? m_oceanImpactPositions[bodySlot] : Vec3(position[0], position[1], position[2]);
            m_sceneCbData.oceanBodyImpactPositionRadius[bodySlot] = XMFLOAT4(
                impactPosition.x, impactPosition.y, impactPosition.z,
                bodySlot < m_oceanBodyRadii.size() ? m_oceanBodyRadii[bodySlot] : 0.5f);
            m_sceneCbData.oceanBodyVerticalState[bodySlot] = XMFLOAT4(velocity[1], 0.0f, 0.0f, 0.0f);
        }

        XMFLOAT4 q(rotation[0], rotation[1], rotation[2], rotation[3]);
        XMFLOAT4X4 matrix;
        XMStoreFloat4x4(&matrix, XMMatrixRotationQuaternion(XMLoadFloat4(&q)));
        const float pitch = std::asin(std::clamp(-matrix._32, -1.0f, 1.0f));
        const float yaw = std::atan2(matrix._31, matrix._33);
        const float roll = std::atan2(matrix._12, matrix._22);
        constexpr float toDegrees = 57.2957795131f;
        ent.transform.rotation = Vec3(pitch * toDegrees, yaw * toDegrees, roll * toDegrees);
    }
    m_sceneCbData.oceanBodyCount = static_cast<uint32_t>(std::min<size_t>(m_floatBodyIndices.size(), 8));
    m_transformsDirty = true;
    m_accumFrameCount = 1;
    m_accumResetPending = false;
}

EntityID Scene::rayPick(float screenX, float screenY, float viewportW, float viewportH) {
    if (!std::isfinite(screenX) || !std::isfinite(screenY) || !std::isfinite(viewportW) ||
        !std::isfinite(viewportH) || viewportW <= 0.0f || viewportH <= 0.0f) return NULL_ENTITY;

    float ndcX = (screenX / viewportW) * 2.0f - 1.0f;
    float ndcY = 1.0f - (screenY / viewportH) * 2.0f;

    float aspect = viewportW / viewportH;
    XMMATRIX V = m_camera.getViewMatrix();
    XMMATRIX P = m_camera.getProjMatrix(aspect);
    XMMATRIX VP = XMMatrixMultiply(V, P);
    XMVECTOR determinantVector;
    XMMATRIX invVP = XMMatrixInverse(&determinantVector, VP);
    float determinant = 0.0f;
    XMStoreFloat(&determinant, determinantVector);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12f) return NULL_ENTITY;

    XMVECTOR nearP = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invVP);
    XMVECTOR farP  = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invVP);
    XMFLOAT3 origF, farF;
    XMStoreFloat3(&origF, nearP);
    XMStoreFloat3(&farF, farP);
    Vec3 rayOrig(origF.x, origF.y, origF.z);
    Vec3 rayDelta(farF.x - origF.x, farF.y - origF.y, farF.z - origF.z);
    const float rayLength = rayDelta.length();
    if (!std::isfinite(rayLength) || rayLength <= 1.0e-6f) return NULL_ENTITY;
    const Vec3 rayDir = rayDelta / rayLength;

    if (!std::isfinite(rayOrig.x) || !std::isfinite(rayOrig.y) || !std::isfinite(rayOrig.z)) return NULL_ENTITY;

    float closestT = 1e30f;
    EntityID bestId = NULL_ENTITY;

    for (const auto& ent : m_entities) {
        const float pitch = ent.transform.rotation.x * (3.14159265f / 180.0f);
        const float yaw = ent.transform.rotation.y * (3.14159265f / 180.0f);
        const float roll = ent.transform.rotation.z * (3.14159265f / 180.0f);
        const XMMATRIX world = XMMatrixScaling(ent.transform.scale.x, ent.transform.scale.y, ent.transform.scale.z)
            * XMMatrixRotationRollPitchYaw(pitch, yaw, roll)
            * XMMatrixTranslation(ent.transform.position.x, ent.transform.position.y, ent.transform.position.z);
        AABB worldBounds;
        for (uint32_t corner = 0; corner < 8; ++corner) {
            const XMVECTOR local = XMVectorSet(
                (corner & 1u) ? ent.mesh.localAABB.max.x : ent.mesh.localAABB.min.x,
                (corner & 2u) ? ent.mesh.localAABB.max.y : ent.mesh.localAABB.min.y,
                (corner & 4u) ? ent.mesh.localAABB.max.z : ent.mesh.localAABB.min.z, 1.0f);
            XMFLOAT3 transformed;
            XMStoreFloat3(&transformed, XMVector3TransformCoord(local, world));
            worldBounds.expand(Vec3(transformed.x, transformed.y, transformed.z));
        }
        float t = 0.0f;
        if (worldBounds.rayIntersect(rayOrig, rayDir, t)) {
            if (t < closestT) {
                closestT = t;
                bestId = ent.id;
            }
        }
    }

    return bestId;
}

void Scene::updateGpuBuffers(D3D12Device& device) {
    if (m_materialsDirty) {
        if (m_allMaterials.size() > m_materialBufferCapacity || !m_materialBuffer) {
            std::cerr << "[Scene Error] Material edit exceeded the allocated GPU material capacity." << std::endl;
            m_materialsDirty = false;
            m_instancesDirty = false;
            m_queuedMaterialEdits = 0;
        } else {
            // This is an infrequent editor operation. Drain prior frames before modifying
            // upload memory that an earlier DXR dispatch may still be reading.
            device.waitForGpu();
            void* materialMapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            if (SUCCEEDED(m_materialBuffer->Map(0, &readRange, &materialMapped))) {
                std::memcpy(materialMapped, m_allMaterials.data(), sizeof(MaterialData) * m_allMaterials.size());
                m_materialBuffer->Unmap(0, nullptr);

                bool instancesUploaded = !m_instancesDirty;
                if (m_instancesDirty && m_instanceBuffer) {
                    void* instanceMapped = nullptr;
                    if (SUCCEEDED(m_instanceBuffer->Map(0, &readRange, &instanceMapped))) {
                        std::memcpy(instanceMapped, m_allInstances.data(), sizeof(InstanceData) * m_allInstances.size());
                        m_instanceBuffer->Unmap(0, nullptr);
                        instancesUploaded = true;
                    }
                }
                if (instancesUploaded) {
                    std::cout << "[Scene] Uploaded material edits: " << m_queuedMaterialEdits
                        << " entity override(s); GPU materials=" << m_allMaterials.size() << std::endl;
                    m_uploadedMaterialEdits = m_queuedMaterialEdits;
                    m_materialsDirty = false;
                    m_instancesDirty = false;
                    m_queuedMaterialEdits = 0;
                } else {
                    std::cerr << "[Scene Error] Could not map the GPU instance buffer for a material override." << std::endl;
                }
            } else {
                std::cerr << "[Scene Error] Could not map the GPU material buffer for an Inspector edit." << std::endl;
            }
        }
    }

    // 1. Update TLAS instances only if transforms changed
    if (m_transformsDirty) {
        std::cout << "[Scene] Rebuilding TLAS..." << std::endl;
        std::vector<Tlas::Instance> tlasInstances;
        tlasInstances.reserve(m_entities.size());
        for (size_t i = 0; i < m_entities.size(); ++i) {
            const auto& ent = m_entities[i];
            Tlas::Instance inst = {};
            inst.blasGpuAddress = m_blases[i].getGpuAddress();
            ent.transform.get3x4RowMajor(inst.transform);
            inst.instanceId = static_cast<uint32_t>(i);
            inst.hitGroupIndex = 0;
            tlasInstances.push_back(inst);
        }
        m_tlas.build(device, tlasInstances);
        m_transformsDirty = false;
    }

    // 2. Update SceneCB
    const float aspect = std::clamp(std::isfinite(m_viewportAspect) && m_viewportAspect > 1.0e-4f
        ? m_viewportAspect : 1.0f, 1.0e-4f, 1.0e4f);
    XMMATRIX V = m_camera.getViewMatrix();
    XMMATRIX P = m_camera.getProjMatrix(aspect);
    XMMATRIX VP = XMMatrixMultiply(V, P);
    XMVECTOR determinantVector;
    XMMATRIX invVP = XMMatrixInverse(&determinantVector, VP);
    float determinant = 0.0f;
    XMStoreFloat(&determinant, determinantVector);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12f) {
        std::cerr << "[Scene Error] Skipped GPU camera update because view-projection matrix is singular.\n";
        return;
    }

    XMStoreFloat4x4(&m_sceneCbData.viewProjInv, XMMatrixTranspose(invVP));
    m_sceneCbData.cameraPos = XMFLOAT3(m_camera.position.x, m_camera.position.y, m_camera.position.z);
    m_sceneCbData.sunDirection = XMFLOAT3(m_sun.direction.x, m_sun.direction.y, m_sun.direction.z);
    m_sceneCbData.sunIntensity = m_sun.intensity;
    m_sceneCbData.sunColor = XMFLOAT4(m_sun.color.x, m_sun.color.y, m_sun.color.z, 1.0f);
    m_sceneCbData.frameCount = m_freezeSampleSeedForValidation ? 0u : m_frameCount;
    m_sceneCbData.accumFrameCount = m_accumFrameCount;
    m_sceneCbData.renderHeight = m_renderHeight;
    m_sceneCbData.padPrev1 = m_oceanTime;

    Vec3 fwd = m_camera.getForward();
    Vec3 rgt = m_camera.getRight();
    Vec3 up  = m_camera.getUp();
    m_sceneCbData.camForward = XMFLOAT3(fwd.x, fwd.y, fwd.z);
    m_sceneCbData.fovY = m_camera.fov * (3.14159265f / 180.0f);
    m_sceneCbData.camRight = XMFLOAT3(rgt.x, rgt.y, rgt.z);
    m_sceneCbData.aspectRatio = aspect;
    m_sceneCbData.camUp = XMFLOAT3(up.x, up.y, up.z);
    m_sceneCbData.cameraMoved = m_cameraMoved ? 1.0f : 0.0f;

    // Previous-frame camera parameters for analytic temporal reprojection.
    if (m_firstFrame) {
        m_lastCameraPos  = m_camera.position;
        m_lastCamForward = fwd;
        m_lastCamRight   = rgt;
        m_lastCamUp      = up;
        m_firstFrame = false;
    }
    m_sceneCbData.prevCameraPos  = XMFLOAT3(m_lastCameraPos.x, m_lastCameraPos.y, m_lastCameraPos.z);
    m_sceneCbData.prevCamForward = XMFLOAT3(m_lastCamForward.x, m_lastCamForward.y, m_lastCamForward.z);
    m_sceneCbData.prevCamRight   = XMFLOAT3(m_lastCamRight.x, m_lastCamRight.y, m_lastCamRight.z);
    m_sceneCbData.prevCamUp      = XMFLOAT3(m_lastCamUp.x, m_lastCamUp.y, m_lastCamUp.z);

    m_lastCameraPos   = m_camera.position;
    m_lastCamForward  = fwd;
    m_lastCamRight    = rgt;
    m_lastCamUp       = up;
    m_lastCameraYaw   = m_camera.yaw;
    m_lastCameraPitch = m_camera.pitch;

    // Halton (2, 3) low-discrepancy sub-pixel jitter sequence (16 phases)
    static const float halton2[16] = {
        0.0f/1.0f, 1.0f/2.0f, 1.0f/4.0f, 3.0f/4.0f,
        1.0f/8.0f, 5.0f/8.0f, 3.0f/8.0f, 7.0f/8.0f,
        1.0f/16.0f, 9.0f/16.0f, 5.0f/16.0f, 13.0f/16.0f,
        3.0f/16.0f, 11.0f/16.0f, 7.0f/16.0f, 15.0f/16.0f
    };
    static const float halton3[16] = {
        0.0f/1.0f, 1.0f/3.0f, 2.0f/3.0f, 1.0f/9.0f,
        4.0f/9.0f, 7.0f/9.0f, 2.0f/9.0f, 5.0f/9.0f,
        8.0f/9.0f, 1.0f/27.0f, 4.0f/27.0f, 7.0f/27.0f,
        10.0f/27.0f, 13.0f/27.0f, 16.0f/27.0f, 19.0f/27.0f
    };
    uint32_t phase = m_frameCount % 16;
    m_sceneCbData.jitterOffset.x = (halton2[phase] - 0.5f);
    m_sceneCbData.jitterOffset.y = (halton3[phase] - 0.5f);

    // Map and update SceneCB
    void* cbMapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 }; // We do not intend to read from this resource on the CPU
    if (SUCCEEDED(m_sceneCb->Map(0, &readRange, &cbMapped))) {
        std::memcpy(cbMapped, &m_sceneCbData, sizeof(SceneCB));
        m_sceneCb->Unmap(0, nullptr);
    }
}

} // namespace Engine
