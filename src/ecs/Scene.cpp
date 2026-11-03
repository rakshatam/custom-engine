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
#include "GltfImportMath.h"
#include "../core/Window.h"
#include <iostream>
#include <cmath>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <utility>
#include <array>
#include <limits>
#include <filesystem>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Engine {

static inline uint64_t alignTo(uint64_t size, uint64_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

static std::vector<uint8_t> findMissingSourceMetallicRoughnessMaps(
    const std::string& gltfPath, size_t materialCount) {
    std::vector<uint8_t> missing(materialCount, 0);
    cgltf_options options = {};
    cgltf_data* rawData = nullptr;
    if (cgltf_parse_file(&options, gltfPath.c_str(), &rawData) != cgltf_result_success || !rawData) {
        return missing;
    }
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(rawData, &cgltf_free);
    const std::filesystem::path assetDirectory = std::filesystem::path(gltfPath).parent_path();
    const size_t count = std::min(materialCount, static_cast<size_t>(data->materials_count));
    for (size_t i = 0; i < count; ++i) {
        const cgltf_material& material = data->materials[i];
        if (!material.has_pbr_metallic_roughness) continue;
        const cgltf_texture* texture = material.pbr_metallic_roughness.metallic_roughness_texture.texture;
        if (!texture) continue;
        const cgltf_image* image = texture->image;
        if (!image && texture->has_basisu) image = texture->basisu_image;
        if (!image && texture->has_webp) image = texture->webp_image;
        if (!image || !image->uri || image->uri[0] == '\0' || std::string_view(image->uri).rfind("data:", 0) == 0) {
            missing[i] = 1;
            continue;
        }
        std::string uri(image->uri);
        cgltf_decode_uri(uri.data());
        std::filesystem::path imagePath(uri);
        if (imagePath.is_relative()) imagePath = assetDirectory / imagePath;
        std::error_code fileError;
        missing[i] = !std::filesystem::is_regular_file(imagePath.lexically_normal(), fileError) || fileError;
    }
    return missing;
}

static bool getBundledSponzaReflectionFallback(uint32_t materialId, float& roughness, const char*& role) {
    // The Khronos glTF sample does not include the 24 referenced metallic/roughness
    // images. Preserve a dielectric stone response and make the floor and structural
    // column surfaces explicitly polished limestone so their DXR returns are visible.
    switch (materialId) {
    case 9:  roughness = 0.18f; role = "floor"; return true;
    case 1:  roughness = 0.18f; role = "pillar-base"; return true;
    case 8:  roughness = 0.18f; role = "pillar-shaft"; return true;
    default: return false;
    }
}

SceneEntity* Scene::getEntity(EntityID id) {
    const auto index = m_entityIndex.find(id);
    return index && *index < m_entities.size() && m_entities[*index].id == id
        ? &m_entities[*index] : nullptr;
}

const SceneEntity* Scene::getEntity(EntityID id) const {
    const auto index = m_entityIndex.find(id);
    return index && *index < m_entities.size() && m_entities[*index].id == id
        ? &m_entities[*index] : nullptr;
}

Vec3 Scene::getSelectedEntityLocalAnchor() const {
    const SceneEntity* selected = getEntity(m_selectedEntityId);
    if (!selected) return Vec3(0.0f);
    if (m_hasSelectedLocalAnchor) return m_selectedLocalAnchor;
    return selected->mesh.localAABB.min * 0.5f + selected->mesh.localAABB.max * 0.5f;
}

void Scene::setSelectedEntityId(EntityID id, const Vec3* localAnchor) {
    m_selectedEntityId = (id == NULL_ENTITY || getEntity(id)) ? id : NULL_ENTITY;
    m_hasSelectedLocalAnchor = false;
    if (m_selectedEntityId == NULL_ENTITY || !localAnchor ||
        !std::isfinite(localAnchor->x) || !std::isfinite(localAnchor->y) || !std::isfinite(localAnchor->z)) {
        return;
    }
    m_selectedLocalAnchor = *localAnchor;
    m_hasSelectedLocalAnchor = true;
}

bool Scene::rebuildEntityIndex() {
    std::vector<EntityID> ids;
    ids.reserve(m_entities.size());
    for (const SceneEntity& entity : m_entities) ids.push_back(entity.id);
    if (!m_entityIndex.rebuild(ids)) {
        std::cerr << "[ECS Error] Scene has a null or duplicate EntityID; indexed lookup was not updated.\n";
        return false;
    }
    return true;
}

bool Scene::updateEntityMaterial(EntityID id) {
    const auto entityIndexLookup = m_entityIndex.find(id);
    if (!entityIndexLookup || *entityIndexLookup >= m_entities.size()) return false;
    const size_t entityIndex = *entityIndexLookup;
    const SceneEntity& entity = m_entities[entityIndex];

    const MaterialComponent& edited = entity.material;
    if (!std::isfinite(edited.baseColor.x) || !std::isfinite(edited.baseColor.y) ||
        !std::isfinite(edited.baseColor.z) || !std::isfinite(edited.baseColor.w) ||
        !std::isfinite(edited.metallic) || !std::isfinite(edited.roughness) ||
        !std::isfinite(edited.emissive)) {
        std::cerr << "[Scene Error] Refused non-finite material edit for entity '"
                  << entity.name.name << "'.\n";
        return false;
    }
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
    material.baseColor = XMFLOAT4(edited.baseColor.x, edited.baseColor.y, edited.baseColor.z, edited.baseColor.w);
    material.metallic = std::clamp(edited.metallic, 0.0f, 1.0f);
    material.roughness = std::clamp(edited.roughness, 0.04f, 1.0f);
    material.emissive = std::max(edited.emissive, 0.0f);
    // Keep imported texture IDs, alpha cutoff, and flags on the selected override.
    m_materialsDirty = true;
    ++m_queuedMaterialEdits;
    return true;
}

bool Scene::init(D3D12Device& device, const std::string& gltfPath) {
    m_isOceanScene = false;
    m_isReflectionLabScene = false;
    m_textureArrayLayerCount = 0;
    m_textureArrayResolution = 1024;
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
    m_sceneCbData.waterReflectionStrength = 1.0f;

    m_sceneCbData.enableTAA = 1; // Depth-aware temporal antialiasing enabled.
    m_sceneCbData.taaBlendWeight = 0.10f;
    m_sceneCbData.enableNRD = 1;
    m_sceneCbData.ambientMultiplier = 0.0f; // Ocean-only sky and ambient mode stays off for Sponza.

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
        m_allVertices.clear();
        m_allIndices.clear();
        m_allMaterials.clear();
        m_allInstances.clear();
        m_entities.clear();
        m_entityIndex.clear();
        m_nextEntityId = 1;
        buildProceduralSponza(m_allVertices, m_allIndices);
        if (!createTextureArray(device, {}, {})) return false;
    }

    return uploadGpuScene(device);
}

bool Scene::uploadGpuScene(D3D12Device& device) {
    if (!rebuildEntityIndex()) return false;
    if (m_allVertices.empty() || m_allIndices.empty() || m_allMaterials.empty() || m_allInstances.empty() ||
        m_entities.size() != m_allInstances.size() || m_entities.size() > std::numeric_limits<uint32_t>::max() ||
        m_allMaterials.size() > std::numeric_limits<uint32_t>::max() ||
        m_allIndices.size() % 3 != 0 || m_allVertices.size() > std::numeric_limits<uint32_t>::max() ||
        m_allIndices.size() > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "[Scene Error] Scene arrays violate entity/instance or 32-bit DXR layout invariants.\n";
        return false;
    }
    for (size_t entityIndex = 0; entityIndex < m_entities.size(); ++entityIndex) {
        const SceneEntity& entity = m_entities[entityIndex];
        const InstanceData& instance = m_allInstances[entityIndex];
        if (entity.mesh.vertexCount < 3 || entity.mesh.indexCount < 3 || entity.mesh.indexCount % 3 != 0 ||
            entity.mesh.vertexOffset > m_allVertices.size() ||
            entity.mesh.vertexCount > m_allVertices.size() - entity.mesh.vertexOffset ||
            entity.mesh.indexOffset > m_allIndices.size() ||
            entity.mesh.indexCount > m_allIndices.size() - entity.mesh.indexOffset ||
            instance.materialId >= m_allMaterials.size() ||
            instance.vertexOffset != entity.mesh.vertexOffset || instance.indexOffset != entity.mesh.indexOffset) {
            std::cerr << "[Scene Error] Invalid draw range or instance/material mapping for entity '"
                      << entity.name.name << "'.\n";
            return false;
        }
        const float transformValues[] = {
            entity.transform.position.x, entity.transform.position.y, entity.transform.position.z,
            entity.transform.rotation.x, entity.transform.rotation.y, entity.transform.rotation.z,
            entity.transform.scale.x, entity.transform.scale.y, entity.transform.scale.z
        };
        for (float value : transformValues) {
            if (!std::isfinite(value)) {
                std::cerr << "[Scene Error] Non-finite transform for entity '" << entity.name.name << "'.\n";
                return false;
            }
        }
        if (std::abs(entity.transform.scale.x) <= 1.0e-8f ||
            std::abs(entity.transform.scale.y) <= 1.0e-8f ||
            std::abs(entity.transform.scale.z) <= 1.0e-8f) {
            std::cerr << "[Scene Error] Singular transform scale for entity '" << entity.name.name << "'.\n";
            return false;
        }
        for (size_t i = entity.mesh.indexOffset; i < static_cast<size_t>(entity.mesh.indexOffset) + entity.mesh.indexCount; ++i) {
            if (m_allIndices[i] >= entity.mesh.vertexCount) {
                std::cerr << "[Scene Error] Index " << m_allIndices[i] << " exceeds entity-local vertex count for '"
                          << entity.name.name << "'.\n";
                return false;
            }
        }
    }
    for (size_t i = 0; i < m_allVertices.size(); ++i) {
        const Vertex& vertex = m_allVertices[i];
        const float values[] = { vertex.position.x, vertex.position.y, vertex.position.z,
            vertex.normal.x, vertex.normal.y, vertex.normal.z, vertex.uv.x, vertex.uv.y };
        for (float value : values) {
            if (!std::isfinite(value)) {
                std::cerr << "[Scene Error] Non-finite vertex attribute at global vertex " << i << ".\n";
                return false;
            }
        }
    }
    for (size_t i = 0; i < m_allMaterials.size(); ++i) {
        const MaterialData& material = m_allMaterials[i];
        const float values[] = { material.baseColor.x, material.baseColor.y, material.baseColor.z,
            material.baseColor.w, material.metallic, material.roughness, material.emissive,
            material.alphaCutoff, material.emissiveColor.x, material.emissiveColor.y,
            material.emissiveColor.z, material.emissiveColor.w, material.dielectric.x,
            material.dielectric.y, material.dielectric.z, material.dielectric.w,
            material.volumeAbsorption.x, material.volumeAbsorption.y,
            material.volumeAbsorption.z, material.volumeAbsorption.w };
        for (float value : values) {
            if (!std::isfinite(value)) {
                std::cerr << "[Scene Error] Non-finite GPU material value at material " << i << ".\n";
                return false;
            }
        }
        const auto validTextureId = [this](int textureId) {
            if (textureId == -1) return true;
            return textureId >= 0 && static_cast<uint32_t>(textureId) < m_textureArrayLayerCount;
        };
        if (!validTextureId(material.textureId) || !validTextureId(material.metallicRoughnessTextureId) ||
            !validTextureId(material.normalTextureId) || !validTextureId(material.emissiveTextureId)) {
            std::cerr << "[Scene Error] Invalid texture layer at material " << i << ".\n";
            return false;
        }
        if (material.dielectric.x < 0.0f || material.dielectric.x > 1.0f ||
            material.dielectric.y < 1.0f || material.dielectric.y > 2.5f ||
            material.dielectric.z < 0.0f ||
            material.volumeAbsorption.x < 0.0f || material.volumeAbsorption.y < 0.0f ||
            material.volumeAbsorption.z < 0.0f) {
            std::cerr << "[Scene Error] Invalid dielectric or absorption data at material " << i << ".\n";
            return false;
        }
    }
    m_totalTriangles = static_cast<uint32_t>(m_allIndices.size() / 3);
    const uint64_t vbSize = sizeof(Vertex) * m_allVertices.size();
    m_vertexBuffer = device.createBuffer(vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_vertexBuffer) return false;
    void* mapped = nullptr;
    if (FAILED(m_vertexBuffer->Map(0, nullptr, &mapped)) || !mapped) {
        std::cerr << "[Scene Error] Failed to map vertex upload storage.\n";
        return false;
    }
    std::memcpy(mapped, m_allVertices.data(), vbSize);
    m_vertexBuffer->Unmap(0, nullptr);

    const uint64_t ibSize = sizeof(uint32_t) * m_allIndices.size();
    m_indexBuffer = device.createBuffer(ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_indexBuffer || FAILED(m_indexBuffer->Map(0, nullptr, &mapped)) || !mapped) {
        std::cerr << "[Scene Error] Failed to allocate or map index upload storage.\n";
        return false;
    }
    std::memcpy(mapped, m_allIndices.data(), ibSize);
    m_indexBuffer->Unmap(0, nullptr);

    m_materialBufferCapacity = m_allMaterials.size() + m_entities.size();
    const uint64_t matSize = sizeof(MaterialData) * m_materialBufferCapacity;
    m_materialBuffer = device.createBuffer(matSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_materialBuffer || FAILED(m_materialBuffer->Map(0, nullptr, &mapped)) || !mapped) {
        std::cerr << "[Scene Error] Failed to allocate or map material upload storage.\n";
        return false;
    }
    std::memcpy(mapped, m_allMaterials.data(), sizeof(MaterialData) * m_allMaterials.size());
    m_materialBuffer->Unmap(0, nullptr);

    const uint64_t instSize = sizeof(InstanceData) * m_allInstances.size();
    m_instanceBuffer = device.createBuffer(instSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_instanceBuffer || FAILED(m_instanceBuffer->Map(0, nullptr, &mapped)) || !mapped) {
        std::cerr << "[Scene Error] Failed to allocate or map instance upload storage.\n";
        return false;
    }
    std::memcpy(mapped, m_allInstances.data(), instSize);
    m_instanceBuffer->Unmap(0, nullptr);

    const uint64_t cbSize = alignTo(sizeof(SceneCB), 256);
    for (uint32_t frameIndex = 0; frameIndex < D3D12Device::FRAME_COUNT; ++frameIndex) {
        m_sceneCb[frameIndex] = device.createBuffer(cbSize, D3D12_HEAP_TYPE_UPLOAD,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!m_sceneCb[frameIndex]) {
            std::cerr << "[Scene Error] Failed to allocate SceneCB upload buffer for frame "
                      << frameIndex << ".\n";
            return false;
        }
    }

    if (!device.beginFrame()) return false;
    const auto finishGpuUpload = [&device]() {
        return device.endFrame() && device.waitForGpu();
    };
    m_blases.resize(m_entities.size());
    for (size_t i = 0; i < m_entities.size(); ++i) {
        auto& ent = m_entities[i];
        if (i >= m_allInstances.size() || ent.mesh.indexCount == 0 || ent.mesh.indexCount % 3 != 0) {
            std::cerr << "[Scene Error] Entity '" << ent.name.name << "' has no valid triangle instance data.\n";
            finishGpuUpload();
            return false;
        }
        const bool isOpaque = i >= m_allInstances.size() || m_allInstances[i].materialId >= m_allMaterials.size()
            || (m_allMaterials[m_allInstances[i].materialId].alphaCutoff <= 0.0f &&
                m_allMaterials[m_allInstances[i].materialId].dielectric.x <= 0.0f);
        if (!m_blases[i].build(device, m_vertexBuffer.Get(), static_cast<uint64_t>(ent.mesh.vertexOffset) * sizeof(Vertex),
            ent.mesh.vertexCount, sizeof(Vertex), m_indexBuffer.Get(),
            static_cast<uint64_t>(ent.mesh.indexOffset) * sizeof(uint32_t), ent.mesh.indexCount, isOpaque)) {
            std::cerr << "[Scene Error] BLAS build validation/allocation failed for entity '" << ent.name.name << "'.\n";
            finishGpuUpload();
            return false;
        }
        ent.mesh.blasIndex = static_cast<uint32_t>(i);
    }

    std::vector<Tlas::Instance> tlasInstances;
    tlasInstances.reserve(m_entities.size());
    for (size_t i = 0; i < m_entities.size(); ++i) {
        Tlas::Instance inst = {};
        inst.blasGpuAddress = m_blases[i].getGpuAddress();
        if (inst.blasGpuAddress == 0) {
            std::cerr << "[Scene Error] BLAS GPU address is null for entity '" << m_entities[i].name.name << "'.\n";
            finishGpuUpload();
            return false;
        }
        m_entities[i].transform.get3x4RowMajor(inst.transform);
        inst.instanceId = static_cast<uint32_t>(i);
        inst.hitGroupIndex = 0;
        tlasInstances.push_back(inst);
    }
    if (!m_tlas.build(device, tlasInstances)) {
        std::cerr << "[Scene Error] TLAS build validation/allocation failed for " << tlasInstances.size() << " instances.\n";
        finishGpuUpload();
        return false;
    }
    if (!finishGpuUpload()) {
        std::cerr << "[Scene Error] GPU acceleration-structure upload did not complete successfully.\n";
        return false;
    }
    return true;
}

bool Scene::initOcean(D3D12Device& device) {
    m_isOceanScene = true;
    m_isReflectionLabScene = false;
    m_textureArrayLayerCount = 0;
    m_textureArrayResolution = 1024;
    m_oceanImpactCount = 0;
    m_oceanEntryCrossingCount = 0;
    m_oceanExitCrossingCount = 0;
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
    m_sceneCbData.waterReflectionStrength = 2.5f;
    m_sceneCbData.sunDirection = XMFLOAT3(m_sun.direction.x, m_sun.direction.y, m_sun.direction.z);
    m_sceneCbData.sunIntensity = m_sun.intensity;
    m_sceneCbData.sunColor = XMFLOAT4(m_sun.color.x, m_sun.color.y, m_sun.color.z, 1.0f);
    m_sceneCbData.skyZenithColor = XMFLOAT4(0.19f, 0.31f, 0.40f, 1.0f);
    m_sceneCbData.skyHorizonColor = XMFLOAT4(0.48f, 0.58f, 0.63f, 1.0f);
    m_sceneCbData.ambientMultiplier = 5.0f; // Brighter open-water fill keeps unlit prop sides readable.

    MaterialData water = {};
    water.baseColor = XMFLOAT4(0.008f, 0.045f, 0.072f, 1.0f);
    water.metallic = 0.0f;
    water.roughness = 0.035f;
    water.textureId = -1;
    water.metallicRoughnessTextureId = -1;
    water.alphaCutoff = 0.0f;
    water.emissiveColor = XMFLOAT4(water.baseColor.x, water.baseColor.y, water.baseColor.z, 1.0f);
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
        ent.sourceNodePath = "Ocean";
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
        m_allInstances.push_back({materialId, vertexOffset, indexOffset});
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
        mat.emissiveColor = XMFLOAT4(item.color.x, item.color.y, item.color.z, 0.0f);
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
        const int32_t body = floats[i].buoy
            ? m_joltWorld->addEllipsoid(ent.transform.position.x, ent.transform.position.y, ent.transform.position.z,
                                        h.x, h.y, h.z, 1.9f)
            : m_joltWorld->addBox(ent.transform.position.x, ent.transform.position.y, ent.transform.position.z,
                                  h.x, h.y, h.z, 1.9f);
        if (body < 0) return false;
        m_floatBodyIndices.push_back(body);
        m_initialFloatHeights.push_back(ent.transform.position.y);
        m_oceanBodyHalfExtents.push_back(h);
        m_oceanBodyIsEllipsoid.push_back(floats[i].buoy);
        m_oceanBodyRadii.push_back(std::sqrt(h.x * h.z));
        const OceanWaveSample initialWave = evaluateOceanWave(ent.transform.position.x, ent.transform.position.z, m_oceanTime);
        m_oceanPreviousBottomClearance.push_back(ent.transform.position.y - h.y - initialWave.height);
        m_oceanImpactTimes.push_back(-1000.0f);
        m_oceanImpactStrengths.push_back(0.0f);
        m_oceanImpactPositions.push_back(ent.transform.position);
        const size_t bodySlot = m_oceanBodyRadii.size() - 1;
        const OceanBodyWaveState bodyState{ent.transform.position.x, ent.transform.position.y,
            ent.transform.position.z, m_oceanBodyRadii[bodySlot], h.y, 0.0f, 0.0f, 0.0f};
        const float wetness = oceanBodySubmersion(initialWave, bodyState);
        m_sceneCbData.oceanBodyPositionRadius[bodySlot] = XMFLOAT4(
            ent.transform.position.x, ent.transform.position.y, ent.transform.position.z,
            m_oceanBodyRadii[bodySlot]);
        m_sceneCbData.oceanBodyVelocityImpact[bodySlot] = XMFLOAT4(0.0f, 0.0f, -1000.0f, 0.0f);
        m_sceneCbData.oceanBodyImpactPositionRadius[bodySlot] = XMFLOAT4(
            ent.transform.position.x, initialWave.height, ent.transform.position.z,
            m_oceanBodyRadii[bodySlot]);
        m_sceneCbData.oceanBodyVerticalState[bodySlot] = XMFLOAT4(0.0f, h.y, wetness, 0.0f);
    }
    m_sceneCbData.oceanBodyCount = static_cast<uint32_t>(std::min<size_t>(m_floatBodyIndices.size(), 8));
    std::cout << "[Scene] Ocean scene ready: Jolt bodies=" << m_joltWorld->bodyCount()
              << ", ray-traced water reflections, DXR shadows; Sponza scene remains separate." << std::endl;
    return true;
}

bool Scene::initReflectionLab(D3D12Device& device, const std::string& gltfPath) {
    m_isOceanScene = false;
    m_isReflectionLabScene = false;
    m_textureArrayLayerCount = 0;
    m_textureArrayResolution = 1024;
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
    for (Vertex& vertex : m_allVertices) {
        vertex.position.x = sourceCenter.x + (vertex.position.x - sourceCenter.x) * unitScale;
        vertex.position.y = sourceCenter.y + (vertex.position.y - sourceCenter.y) * unitScale;
        vertex.position.z = sourceCenter.z + (vertex.position.z - sourceCenter.z) * unitScale;
    }
    for (SceneEntity& entity : m_entities) {
        entity.transform.position = Vec3(0.0f);
        entity.transform.rotation = Vec3(0.0f);
        entity.transform.scale = Vec3(1.0f);
        AABB normalizedBounds;
        const size_t vertexBegin = entity.mesh.vertexOffset;
        const size_t vertexEnd = vertexBegin + entity.mesh.vertexCount;
        if (vertexEnd > m_allVertices.size()) return false;
        for (size_t i = vertexBegin; i < vertexEnd; ++i) {
            const XMFLOAT3& p = m_allVertices[i].position;
            normalizedBounds.expand(Vec3(p.x, p.y, p.z));
        }
        entity.mesh.localAABB = normalizedBounds;
    }

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
    if (!uploadGpuScene(device)) return false;
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
    ID3D12Device5* d3dDevice = device.getDevice();
    if (!d3dDevice) {
        std::cerr << "[Scene Error] Cannot create the texture array because D3D12 is not initialized.\n";
        return false;
    }
    if (textureUris.size() > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) {
        std::cerr << "[Scene Error] Texture array exceeds the D3D12 layer limit.\n";
        return false;
    }
    if (!textureUris.empty() && srgbLayers.size() != textureUris.size()) {
        std::cerr << "[Scene Error] Texture color-space metadata does not match texture-array layer count.\n";
        return false;
    }
    const UINT texWidth = m_textureArrayResolution;
    const UINT texHeight = m_textureArrayResolution;
    if (texWidth == 0 || texWidth > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        (texWidth & (texWidth - 1u)) != 0) {
        std::cerr << "[Scene Error] Texture-array resolution must be a supported power of two.\n";
        return false;
    }
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
    d3dDevice->GetCopyableFootprints(&texDesc, 0, subresourceCount, 0,
                                     footprints.data(), numRows.data(), rowSizes.data(), &totalUploadSize);
    if (totalUploadSize == 0) {
        std::cerr << "[Scene Error] D3D12 returned an empty texture upload layout.\n";
        return false;
    }

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
    const HRESULT mapResult = uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedUpload));
    if (FAILED(mapResult) || !mappedUpload) {
        std::cerr << "[Scene Error] Could not map the texture-array upload buffer (HRESULT 0x"
                  << std::hex << mapResult << std::dec << ").\n";
        return false;
    }

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
                            const size_t samples[4] = {
                                (static_cast<size_t>(y0) * static_cast<size_t>(w) + static_cast<size_t>(x0)) * 4,
                                (static_cast<size_t>(y0) * static_cast<size_t>(w) + static_cast<size_t>(x1)) * 4,
                                (static_cast<size_t>(y1) * static_cast<size_t>(w) + static_cast<size_t>(x0)) * 4,
                                (static_cast<size_t>(y1) * static_cast<size_t>(w) + static_cast<size_t>(x1)) * 4};
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
            } else {
                std::cerr << "[Scene Warning] Image decode failed for texture layer " << layer << " ('"
                          << textureUris[layer] << "'): " << stbi_failure_reason() << "; using white fallback.\n";
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
    if (!device.beginFrame()) return false;
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

    if (!device.endFrame() || !device.waitForGpu()) {
        std::cerr << "[Scene Error] Texture-array upload did not complete successfully.\n";
        return false;
    }


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
    if (m_textureArraySrvIndex == std::numeric_limits<uint32_t>::max()) return false;
    device.getDevice()->CreateShaderResourceView(
        m_textureArray.Get(),
        &srvDesc,
        device.getSrvUavCpuHandle(m_textureArraySrvIndex)
    );

    m_sceneCbData.textureArrayResolution = texWidth;
    m_textureArrayLayerCount = arraySize;
    std::cout << "[Scene] Created Texture2DArray with " << arraySize
              << " layers (" << texWidth << 'x' << texHeight << " RGBA8, " << mipLevels
              << " color-correct mips), SRV index = " << m_textureArraySrvIndex << std::endl;
    return true;
}

bool Scene::createEnvironmentMaps(D3D12Device& device, const std::string& hdrPath, float intensity) {
    constexpr uint32_t radianceWidth = 1024;
    constexpr uint32_t radianceHeight = 512;
    constexpr uint32_t irradianceWidth = 128;
    constexpr uint32_t irradianceHeight = 64;
    constexpr uint32_t irradianceSamples = 128;
    constexpr float pi = 3.14159265358979323846f;

    int sourceWidth = 0;
    int sourceHeight = 0;
    int sourceComponents = 0;
    std::unique_ptr<float, decltype(&stbi_image_free)> sourcePixels(
        stbi_loadf(hdrPath.c_str(), &sourceWidth, &sourceHeight, &sourceComponents, 3), &stbi_image_free);
    if (!sourcePixels || sourceWidth < 2 || sourceHeight < 1 ||
        std::abs(static_cast<double>(sourceWidth) / sourceHeight - 2.0) > 0.01 ||
        !std::isfinite(intensity) || intensity < 0.0f) {
        std::cerr << "[Environment Warning] Could not decode a valid 2:1 HDR environment map '" << hdrPath
                  << "': " << (sourcePixels ? "invalid dimensions or intensity" : stbi_failure_reason()) << ".\n";
        return false;
    }
    (void)sourceComponents;

    const auto sampleSourceUv = [&](float u, float v) {
        u -= std::floor(u); // Equirectangular longitude wraps across the seam.
        v = std::clamp(v, 0.0f, 1.0f);
        const float x = u * sourceWidth - 0.5f;
        const float y = v * sourceHeight - 0.5f;
        const int x0Unwrapped = static_cast<int>(std::floor(x));
        const int x1Unwrapped = x0Unwrapped + 1;
        const int x0 = (x0Unwrapped % sourceWidth + sourceWidth) % sourceWidth;
        const int x1 = (x1Unwrapped % sourceWidth + sourceWidth) % sourceWidth;
        const int y0Unclamped = static_cast<int>(std::floor(y));
        const int y0 = std::clamp(y0Unclamped, 0, sourceHeight - 1);
        const int y1 = std::clamp(y0Unclamped + 1, 0, sourceHeight - 1);
        const float fx = x - std::floor(x);
        const float fy = y - std::floor(y);
        const auto fetch = [&](int px, int py, int channel) {
            const size_t index = (static_cast<size_t>(py) * sourceWidth + px) * 3 + channel;
            const float value = sourcePixels.get()[index];
            return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0e6f) : 0.0f;
        };
        float sampledChannels[3] = {};
        for (int channel = 0; channel < 3; ++channel) {
            const float top = fetch(x0, y0, channel) * (1.0f - fx) + fetch(x1, y0, channel) * fx;
            const float bottom = fetch(x0, y1, channel) * (1.0f - fx) + fetch(x1, y1, channel) * fx;
            sampledChannels[channel] = top * (1.0f - fy) + bottom * fy;
        }
        return XMFLOAT3(sampledChannels[0], sampledChannels[1], sampledChannels[2]);
    };
    const auto directionToUv = [pi](const Vec3& direction) {
        const Vec3 d = direction.normalized();
        return XMFLOAT2(std::atan2(d.z, d.x) / (2.0f * pi) + 0.5f,
                        std::acos(std::clamp(d.y, -1.0f, 1.0f)) / pi);
    };

    std::vector<XMFLOAT4> radianceBase(static_cast<size_t>(radianceWidth) * radianceHeight);
    for (uint32_t y = 0; y < radianceHeight; ++y) {
        for (uint32_t x = 0; x < radianceWidth; ++x) {
            XMFLOAT3 sum{};
            constexpr uint32_t downsampleGrid = 4;
            for (uint32_t sy = 0; sy < downsampleGrid; ++sy) {
                for (uint32_t sx = 0; sx < downsampleGrid; ++sx) {
                    const float u = (static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / downsampleGrid) / radianceWidth;
                    const float v = (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / downsampleGrid) / radianceHeight;
                    const XMFLOAT3 sample = sampleSourceUv(u, v);
                    sum.x += sample.x; sum.y += sample.y; sum.z += sample.z;
                }
            }
            const float scale = 1.0f / static_cast<float>(downsampleGrid * downsampleGrid);
            radianceBase[static_cast<size_t>(y) * radianceWidth + x] =
                XMFLOAT4(sum.x * scale, sum.y * scale, sum.z * scale, 1.0f);
        }
    }

    std::vector<std::vector<XMFLOAT4>> radianceMips;
    radianceMips.push_back(std::move(radianceBase));
    uint32_t mipWidth = radianceWidth;
    uint32_t mipHeight = radianceHeight;
    while (mipWidth > 1 || mipHeight > 1) {
        const uint32_t nextWidth = std::max(mipWidth / 2, 1u);
        const uint32_t nextHeight = std::max(mipHeight / 2, 1u);
        std::vector<XMFLOAT4> next(static_cast<size_t>(nextWidth) * nextHeight);
        const std::vector<XMFLOAT4>& previous = radianceMips.back();
        for (uint32_t y = 0; y < nextHeight; ++y) {
            for (uint32_t x = 0; x < nextWidth; ++x) {
                XMFLOAT4 sum{};
                for (uint32_t oy = 0; oy < 2; ++oy) {
                    for (uint32_t ox = 0; ox < 2; ++ox) {
                        const XMFLOAT4& value = previous[static_cast<size_t>(std::min(y * 2 + oy, mipHeight - 1)) * mipWidth +
                                                          std::min(x * 2 + ox, mipWidth - 1)];
                        sum.x += value.x; sum.y += value.y; sum.z += value.z;
                    }
                }
                next[static_cast<size_t>(y) * nextWidth + x] = XMFLOAT4(sum.x * 0.25f, sum.y * 0.25f,
                                                                         sum.z * 0.25f, 1.0f);
            }
        }
        radianceMips.push_back(std::move(next));
        mipWidth = nextWidth;
        mipHeight = nextHeight;
    }

    const auto radicalInverseVdc = [](uint32_t bits) {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    };
    std::vector<XMFLOAT4> irradiancePixels(static_cast<size_t>(irradianceWidth) * irradianceHeight);
    for (uint32_t y = 0; y < irradianceHeight; ++y) {
        for (uint32_t x = 0; x < irradianceWidth; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / irradianceWidth;
            const float v = (static_cast<float>(y) + 0.5f) / irradianceHeight;
            const float longitude = (u - 0.5f) * (2.0f * pi);
            const float latitude = v * pi;
            const Vec3 normal(std::sin(latitude) * std::cos(longitude), std::cos(latitude),
                              std::sin(latitude) * std::sin(longitude));
            const Vec3 helper = std::abs(normal.y) < 0.999f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
            const Vec3 tangent = Vec3::cross(helper, normal).normalized();
            const Vec3 bitangent = Vec3::cross(normal, tangent).normalized();
            XMFLOAT3 sum{};
            for (uint32_t i = 0; i < irradianceSamples; ++i) {
                const float sampleU = (static_cast<float>(i) + 0.5f) / irradianceSamples;
                const float sampleV = radicalInverseVdc(i);
                const float radius = std::sqrt(sampleU);
                const float phi = 2.0f * pi * sampleV;
                const Vec3 direction = tangent * (radius * std::cos(phi)) +
                    bitangent * (radius * std::sin(phi)) + normal * std::sqrt(std::max(1.0f - sampleU, 0.0f));
                const XMFLOAT2 uv = directionToUv(direction);
                const XMFLOAT3 sample = sampleSourceUv(uv.x, uv.y);
                sum.x += sample.x; sum.y += sample.y; sum.z += sample.z;
            }
            const float inverseSamples = 1.0f / irradianceSamples;
            irradiancePixels[static_cast<size_t>(y) * irradianceWidth + x] =
                XMFLOAT4(sum.x * inverseSamples, sum.y * inverseSamples, sum.z * inverseSamples, 1.0f);
        }
    }
    sourcePixels.reset();

    const auto uploadFloatTexture = [&](const std::vector<std::vector<XMFLOAT4>>& mips,
                                        uint32_t width, uint32_t height,
                                        ComPtr<ID3D12Resource>& resource) {
        if (mips.empty() || mips.size() > std::numeric_limits<UINT16>::max()) return false;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = static_cast<UINT16>(mips.size());
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;
        if (FAILED(device.getDevice()->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)))) return false;

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips.size());
        std::vector<UINT> rowCounts(mips.size());
        std::vector<UINT64> rowSizes(mips.size());
        UINT64 uploadSize = 0;
        device.getDevice()->GetCopyableFootprints(&desc, 0, static_cast<UINT>(mips.size()), 0,
            footprints.data(), rowCounts.data(), rowSizes.data(), &uploadSize);
        ComPtr<ID3D12Resource> upload = device.createBuffer(uploadSize, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!upload || uploadSize == 0) return false;
        uint8_t* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        if (FAILED(upload->Map(0, &readRange, reinterpret_cast<void**>(&mapped))) || !mapped) return false;
        uint32_t mipW = width;
        uint32_t mipH = height;
        for (UINT mip = 0; mip < mips.size(); ++mip) {
            if (mips[mip].size() != static_cast<size_t>(mipW) * mipH ||
                rowSizes[mip] != static_cast<UINT64>(mipW) * sizeof(XMFLOAT4)) {
                upload->Unmap(0, nullptr);
                return false;
            }
            for (UINT row = 0; row < rowCounts[mip]; ++row) {
                std::memcpy(mapped + footprints[mip].Offset + static_cast<size_t>(row) * footprints[mip].Footprint.RowPitch,
                    mips[mip].data() + static_cast<size_t>(row) * mipW,
                    static_cast<size_t>(rowSizes[mip]));
            }
            mipW = std::max(mipW / 2, 1u);
            mipH = std::max(mipH / 2, 1u);
        }
        upload->Unmap(0, nullptr);
        if (!device.beginFrame()) return false;
        ID3D12GraphicsCommandList* commands = device.getCommandList();
        if (!commands) {
            device.endFrame();
            return false;
        }
        for (UINT mip = 0; mip < mips.size(); ++mip) {
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = mip;
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[mip];
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        device.transitionBarrier(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return device.endFrame() && device.waitForGpu();
    };

    std::vector<std::vector<XMFLOAT4>> irradianceMips;
    irradianceMips.push_back(std::move(irradiancePixels));
    if (!uploadFloatTexture(radianceMips, radianceWidth, radianceHeight, m_environmentMap) ||
        !uploadFloatTexture(irradianceMips, irradianceWidth, irradianceHeight, m_environmentIrradiance)) {
        std::cerr << "[Environment Warning] HDR environment texture upload failed for '" << hdrPath << "'.\n";
        m_environmentMap.Reset();
        m_environmentIrradiance.Reset();
        return false;
    }

    m_environmentSrvIndex = device.allocateSrvUavDescriptor();
    const uint32_t irradianceSrvIndex = device.allocateSrvUavDescriptor();
    if (m_environmentSrvIndex == std::numeric_limits<uint32_t>::max() ||
        irradianceSrvIndex != m_environmentSrvIndex + 1u) {
        std::cerr << "[Environment Warning] Could not reserve contiguous HDR environment SRVs.\n";
        m_environmentSrvIndex = std::numeric_limits<uint32_t>::max();
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MostDetailedMip = 0;
    srv.Texture2D.MipLevels = static_cast<UINT>(radianceMips.size());
    device.getDevice()->CreateShaderResourceView(m_environmentMap.Get(), &srv,
        device.getSrvUavCpuHandle(m_environmentSrvIndex));
    srv.Texture2D.MipLevels = 1;
    device.getDevice()->CreateShaderResourceView(m_environmentIrradiance.Get(), &srv,
        device.getSrvUavCpuHandle(irradianceSrvIndex));
    m_sceneCbData.environmentMapEnabled = 1;
    m_sceneCbData.environmentIntensity = intensity;
    m_sceneCbData.environmentMapMaxMip = static_cast<float>(radianceMips.size() - 1u);
    std::cout << "[Environment] Loaded HDR '" << hdrPath << "' at intensity " << intensity
              << ", radiance=" << radianceWidth << 'x' << radianceHeight << " with " << radianceMips.size()
              << " mips, diffuse convolution=" << irradianceWidth << 'x' << irradianceHeight
              << " (" << irradianceSamples << " cosine-weighted samples/texel).\n";
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

    m_hasImportedCamera = false;
    if (scene->mNumCameras > 0 && scene->mCameras && scene->mCameras[0]) {
        const aiCamera* importedCamera = scene->mCameras[0];
        const Vec3 position(importedCamera->mPosition.x, importedCamera->mPosition.y, importedCamera->mPosition.z);
        const Vec3 forward(importedCamera->mLookAt.x, importedCamera->mLookAt.y, importedCamera->mLookAt.z);
        const float forwardLength = forward.length();
        if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
            std::isfinite(forwardLength) && forwardLength > 1.0e-6f) {
            m_importedCameraPosition = position;
            m_importedCameraForward = forward / forwardLength;
            m_hasImportedCamera = true;
            std::cout << "[Scene] Preserved embedded camera '" << importedCamera->mName.C_Str()
                      << "' from " << std::filesystem::path(path).filename().string() << ".\n";
        } else {
            std::cerr << "[Scene Warning] Ignored an invalid embedded camera in '" << path << "'.\n";
        }
    }

    m_allVertices.clear();
    m_allIndices.clear();
    m_allMaterials.clear();
    m_allInstances.clear();
    m_entities.clear();
    m_entityIndex.clear();

    // 1. Collect materials and texture paths
    std::vector<std::string> texturePaths;
    std::vector<uint8_t> srgbTextureLayers;
    uint32_t missingMetallicRoughnessMaps = 0;
    const bool isBundledSponza = std::filesystem::path(path).filename() == "Sponza.gltf";
    const std::vector<uint8_t> missingSourceMetallicRoughnessMaps = isBundledSponza
        ? findMissingSourceMetallicRoughnessMaps(path, scene->mNumMaterials)
        : std::vector<uint8_t>{};
    const std::filesystem::path assetDirectory = std::filesystem::path(path).parent_path();
    const auto resolveAssimpTexture = [&assetDirectory](const std::string& uri) {
        const std::filesystem::path source(uri);
        std::filesystem::path resolved = (source.is_absolute() ? source : assetDirectory / source).lexically_normal();
        if (!std::filesystem::exists(resolved)) {
            std::string extension = resolved.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".dds") {
                std::filesystem::path pngFallback = resolved;
                pngFallback.replace_extension(".png");
                if (std::filesystem::exists(pngFallback)) resolved = std::move(pngFallback);
            }
        }
        return resolved.string();
    };
    std::unordered_map<std::string, int> assimpTextureLayerByPath;
    const auto addAssimpTexture = [&](const std::string& uri, bool srgb, const char* role) {
        const std::string resolved = resolveAssimpTexture(uri);
        if (!std::filesystem::is_regular_file(resolved)) {
            std::cerr << "[Scene Warning] Assimp " << role << " texture not found: " << resolved << ".\n";
            return -1;
        }
        std::error_code canonicalError;
        std::filesystem::path canonical = std::filesystem::weakly_canonical(resolved, canonicalError);
        std::string key = (canonicalError ? std::filesystem::path(resolved).lexically_normal() : canonical).string();
        std::transform(key.begin(), key.end(), key.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        key += srgb ? "|srgb" : "|linear";
        auto& layerMap = assimpTextureLayerByPath;
        auto& texturePathsForLayer = texturePaths;
        auto& srgbLayersForLayer = srgbTextureLayers;
        if (const auto existing = layerMap.find(key); existing != layerMap.end())
            return existing->second;
        if (texturePathsForLayer.size() >= D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
            texturePathsForLayer.size() >= static_cast<size_t>(std::numeric_limits<int>::max() - 2)) {
            std::cerr << "[Scene Error] Assimp texture array exceeds the D3D12 or material-ID limit.\n";
            return -1;
        }
        const int layer = static_cast<int>(texturePathsForLayer.size());
        texturePathsForLayer.push_back(resolved);
        srgbLayersForLayer.push_back(srgb ? 1u : 0u);
        const int materialTextureId = layer;
        layerMap.emplace(std::move(key), materialTextureId);
        return materialTextureId;
    };
    uint32_t loadedNormalMaps = 0;
    uint32_t loadedEmissiveMaps = 0;
    if (scene->mNumMaterials == 0) {
        MaterialData defaultMaterial = {};
        defaultMaterial.baseColor = XMFLOAT4(0.85f, 0.85f, 0.85f, 1.0f);
        defaultMaterial.metallic = 0.0f;
        defaultMaterial.roughness = 0.5f;
        defaultMaterial.textureId = -1;
        defaultMaterial.metallicRoughnessTextureId = -1;
        m_allMaterials.push_back(defaultMaterial);
    }
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        const aiMaterial* mat = scene->mMaterials[i];
        MaterialData md = {};
        md.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        // Assimp FBX materials commonly contain legacy diffuse/specular data,
        // not glTF-style PBR factors. Treat an unspecified FBX surface as a
        // dielectric with moderate roughness; defaulting it to metal destroys
        // diffuse color and makes ordinary plaster/wood look wrong.
        md.metallic = 0.0f;
        md.roughness = 0.5f;
        md.emissive = 0.0f;
        md.alphaCutoff = 0.0f;

        // Read authored PBR factors where available. For legacy FBX materials,
        // approximate roughness from the Phong exponent when no PBR factor is
        // present: roughness ~= sqrt(2 / (shininess + 2)).
        float authoredMetallic = 0.0f;
        float authoredRoughness = 0.5f;
        const bool hasMetallicFactor = mat->Get(AI_MATKEY_METALLIC_FACTOR, authoredMetallic) == AI_SUCCESS &&
            std::isfinite(authoredMetallic);
        const bool hasRoughnessFactor = mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, authoredRoughness) == AI_SUCCESS &&
            std::isfinite(authoredRoughness);
        if (hasMetallicFactor) md.metallic = std::clamp(authoredMetallic, 0.0f, 1.0f);
        if (hasRoughnessFactor) {
            md.roughness = std::clamp(authoredRoughness, 0.04f, 1.0f);
        } else {
            float shininess = 0.0f;
            if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && std::isfinite(shininess)) {
                md.roughness = std::clamp(std::sqrt(2.0f / (std::max(shininess, 0.0f) + 2.0f)), 0.04f, 1.0f);
            }
        }

        aiColor4D diffColor;
        if (aiGetMaterialColor(mat, AI_MATKEY_BASE_COLOR, &diffColor) == AI_SUCCESS ||
            aiGetMaterialColor(mat, AI_MATKEY_COLOR_DIFFUSE, &diffColor) == AI_SUCCESS) {
            md.baseColor = XMFLOAT4(diffColor.r, diffColor.g, diffColor.b, diffColor.a);
        }

        aiString texPath;
        if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
            std::string uri = texPath.C_Str();
            md.textureId = addAssimpTexture(uri, true, "base-color");
            // Preserve the authored material factor. PBR base color is
            // the sampled sRGB texture multiplied by this linear factor.

            // Foliage / Cutout detection
            if (md.textureId != -1 && (uri.find(".png") != std::string::npos ||
                uri.find(".dds") != std::string::npos) && uri != "white.png") {
                md.alphaCutoff = 0.5f;
            }
        }
        const aiReturn hasGlTfMetallicRoughness = mat->GetTexture(
            aiTextureType_GLTF_METALLIC_ROUGHNESS, 0, &texPath);
        if (hasGlTfMetallicRoughness == AI_SUCCESS) {
            md.metallicRoughnessTextureId = addAssimpTexture(texPath.C_Str(), false, "metallic-roughness");
            // A loaded glTF metallic-roughness map stores its roughness in G
            // and metalness in B, so absent scalar factors are neutral multipliers.
            if (md.metallicRoughnessTextureId != -1) {
                if (!hasMetallicFactor) md.metallic = 1.0f;
                if (!hasRoughnessFactor) md.roughness = 1.0f;
            }
        }

        if (mat->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS) {
            md.normalTextureId = addAssimpTexture(texPath.C_Str(), false, "normal");
            if (md.normalTextureId != -1) ++loadedNormalMaps;
        }

        aiColor3D emissiveColor(0.0f, 0.0f, 0.0f);
        const bool hasEmissiveFactor = mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveColor) == AI_SUCCESS;
        float emissiveIntensity = 1.0f;
        if (mat->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveIntensity) != AI_SUCCESS ||
            !std::isfinite(emissiveIntensity) || emissiveIntensity < 0.0f) {
            emissiveIntensity = 1.0f;
        }
        if (hasEmissiveFactor) {
            md.emissiveColor = XMFLOAT4(emissiveColor.r, emissiveColor.g, emissiveColor.b, 0.0f);
            md.emissive = emissiveIntensity;
        }
        if (mat->GetTexture(aiTextureType_EMISSIVE, 0, &texPath) == AI_SUCCESS) {
            md.emissiveTextureId = addAssimpTexture(texPath.C_Str(), true, "emissive");
            if (md.emissiveTextureId != -1) {
                ++loadedEmissiveMaps;
                // Some FBX files provide only an emissive map and no factor;
                // in that case treat the map as authored radiance.
                if (!hasEmissiveFactor) {
                    md.emissiveColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 0.0f);
                    md.emissive = emissiveIntensity;
                }
            }
        }
        // Assimp omits glTF texture slots when the referenced image is absent, so
        // GetTexture() can fail before the old missing-file branch is reached.
        // Read the source glTF material references and apply the fallback in either
        // case; do not silently leave the glTF default (metallic=1, roughness=1).
        if (isBundledSponza && i < missingSourceMetallicRoughnessMaps.size() &&
            missingSourceMetallicRoughnessMaps[i] && md.metallicRoughnessTextureId == -1) {
            md.metallic = 0.0f;
            md.roughness = 0.65f;
            const char* role = "dielectric-stone";
            float reflectionRoughness = md.roughness;
            if (getBundledSponzaReflectionFallback(i, reflectionRoughness, role)) {
                md.roughness = reflectionRoughness;
                std::cout << "[DXR Sponza Material] id=" << i << " surface=" << role
                          << " metallic=0 roughness=" << md.roughness << ".\n";
            }
            ++missingMetallicRoughnessMaps;
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
    std::cout << "[Scene] Assimp material maps: " << texturePaths.size() << " unique color/data layers, "
              << loadedNormalMaps << " normal bindings, " << loadedEmissiveMaps << " emissive bindings.\n";

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
        ent.sourceNodePath = std::filesystem::path(path).stem().string() + " meshes";
        ent.name.name = mesh->mName.length > 0 ? mesh->mName.C_Str()
            : (std::filesystem::path(path).stem().string() + "_Submesh_" + std::to_string(ent.id));
        ent.transform.position = Vec3(0.0f, 0.0f, 0.0f);
        ent.transform.scale = Vec3(1.0f, 1.0f, 1.0f);

        ent.mesh.vertexOffset = vOffset;
        ent.mesh.vertexCount = mesh->mNumVertices;
        ent.mesh.indexOffset = iOffset;
        ent.mesh.indexCount = static_cast<uint32_t>(m_allIndices.size() - iOffset);
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

    const std::string assetName = std::filesystem::path(path).stem().string();
    std::cout << "[Scene] Assimp loaded '" << assetName << "': " << m_entities.size() << " submeshes, "
              << m_allVertices.size() << " vertices, " << m_allIndices.size() / 3 << " triangles."
              << " Bounds: X[" << sceneBounds.min.x << ", " << sceneBounds.max.x << "] "
              << "Y[" << sceneBounds.min.y << ", " << sceneBounds.max.y << "] "
              << "Z[" << sceneBounds.min.z << ", " << sceneBounds.max.z << "]" << std::endl;
    return !m_entities.empty();
}

bool Scene::loadGltf(D3D12Device& device, const std::string& path) {
    cgltf_options options = {};
    cgltf_data* rawData = nullptr;
    const cgltf_result parseResult = cgltf_parse_file(&options, path.c_str(), &rawData);
    if (parseResult != cgltf_result_success || !rawData) {
        std::cerr << "[glTF Error] Parse failed for '" << path << "' (result " << parseResult << ").\n";
        return false;
    }
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(rawData, &cgltf_free);
    const cgltf_result bufferResult = cgltf_load_buffers(&options, data.get(), path.c_str());
    if (bufferResult != cgltf_result_success) {
        std::cerr << "[glTF Error] Buffer load failed for '" << path << "' (result " << bufferResult << ").\n";
        return false;
    }
    const cgltf_result validationResult = cgltf_validate(data.get());
    if (validationResult != cgltf_result_success) {
        std::cerr << "[glTF Error] Validation failed for '" << path << "' (result " << validationResult << ").\n";
        return false;
    }

    m_allVertices.clear();
    m_allIndices.clear();
    m_allMaterials.clear();
    m_allInstances.clear();
    m_entities.clear();
    m_entityIndex.clear();
    m_nextEntityId = 1;

    std::vector<std::string> texturePaths;
    std::vector<uint8_t> srgbTextureLayers;
    std::unordered_map<std::string, int> textureLayerByKey;
    const std::filesystem::path assetDirectory = std::filesystem::path(path).parent_path();
    auto addExternalTexture = [&](const cgltf_texture_view& view, bool srgb) -> int {
        const cgltf_texture* texture = view.texture;
        const cgltf_image* image = texture ? texture->image : nullptr;
        if (!image && texture) image = texture->basisu_image ? texture->basisu_image : texture->webp_image;
        if (!image) return -1;
        if (!image->uri || image->uri[0] == '\0') {
            if (image->buffer_view) {
                std::cerr << "[glTF Warning] Embedded buffer-view texture is not supported yet; material factor fallback used.\n";
            }
            return -1;
        }
        std::string uri = image->uri;
        if (uri.rfind("data:", 0) == 0) {
            std::cerr << "[glTF Warning] Embedded data-URI texture is not supported yet; material factor fallback used.\n";
            return -1;
        }
        cgltf_decode_uri(uri.data());
        std::filesystem::path imagePath(uri);
        if (imagePath.is_relative()) imagePath = assetDirectory / imagePath;
        imagePath = imagePath.lexically_normal();
        std::error_code fileError;
        if (!std::filesystem::is_regular_file(imagePath, fileError) || fileError) {
            std::cerr << "[glTF Warning] Texture image not found: " << imagePath.string() << ".\n";
            return -1;
        }
        const std::string key = imagePath.string() + (srgb ? "|srgb" : "|linear");
        const auto existing = textureLayerByKey.find(key);
        if (existing != textureLayerByKey.end()) return existing->second;
        if (texturePaths.size() >= std::numeric_limits<uint16_t>::max()) {
            std::cerr << "[glTF Error] Texture array exceeds the D3D12 array-layer limit.\n";
            return -1;
        }
        const int layer = static_cast<int>(texturePaths.size());
        texturePaths.push_back(imagePath.string());
        srgbTextureLayers.push_back(srgb ? 1u : 0u);
        textureLayerByKey.emplace(key, layer);
        return layer;
    };

    MaterialData defaultMaterial = {};
    defaultMaterial.baseColor = XMFLOAT4(0.85f, 0.85f, 0.85f, 1.0f);
    defaultMaterial.metallic = 0.0f;
    defaultMaterial.roughness = 0.5f;
    m_allMaterials.push_back(defaultMaterial);

    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& material = data->materials[i];
        MaterialData imported = {};
        imported.baseColor = XMFLOAT4(0.85f, 0.85f, 0.85f, 1.0f);
        imported.metallic = 1.0f;
        imported.roughness = 1.0f;
        if (material.has_pbr_metallic_roughness) {
            const auto& pbr = material.pbr_metallic_roughness;
            imported.baseColor = XMFLOAT4(pbr.base_color_factor[0], pbr.base_color_factor[1],
                                           pbr.base_color_factor[2], pbr.base_color_factor[3]);
            imported.metallic = pbr.metallic_factor;
            imported.roughness = pbr.roughness_factor;
            imported.textureId = addExternalTexture(pbr.base_color_texture, true);
            imported.metallicRoughnessTextureId = addExternalTexture(pbr.metallic_roughness_texture, false);
        } else {
            imported.metallic = 0.0f;
            imported.roughness = 0.5f;
        }
        imported.baseColor.x = std::isfinite(imported.baseColor.x) ? std::clamp(imported.baseColor.x, 0.0f, 1.0f) : 0.85f;
        imported.baseColor.y = std::isfinite(imported.baseColor.y) ? std::clamp(imported.baseColor.y, 0.0f, 1.0f) : 0.85f;
        imported.baseColor.z = std::isfinite(imported.baseColor.z) ? std::clamp(imported.baseColor.z, 0.0f, 1.0f) : 0.85f;
        imported.baseColor.w = std::isfinite(imported.baseColor.w) ? std::clamp(imported.baseColor.w, 0.0f, 1.0f) : 1.0f;
        imported.metallic = std::isfinite(imported.metallic) ? std::clamp(imported.metallic, 0.0f, 1.0f) : 0.0f;
        imported.roughness = std::isfinite(imported.roughness) ? std::clamp(imported.roughness, 0.04f, 1.0f) : 0.5f;
        const float emissive = std::max({material.emissive_factor[0], material.emissive_factor[1],
                                         material.emissive_factor[2]});
        imported.emissiveColor = XMFLOAT4(
            std::isfinite(material.emissive_factor[0]) ? std::max(material.emissive_factor[0], 0.0f) : 0.0f,
            std::isfinite(material.emissive_factor[1]) ? std::max(material.emissive_factor[1], 0.0f) : 0.0f,
            std::isfinite(material.emissive_factor[2]) ? std::max(material.emissive_factor[2], 0.0f) : 0.0f,
            0.0f);
        imported.emissive = std::isfinite(emissive) && emissive > 0.0f ? 1.0f : 0.0f;
        imported.normalTextureId = addExternalTexture(material.normal_texture, false);
        imported.emissiveTextureId = addExternalTexture(material.emissive_texture, true);
        if (material.alpha_mode == cgltf_alpha_mode_mask) {
            imported.alphaCutoff = std::isfinite(material.alpha_cutoff)
                ? std::clamp(material.alpha_cutoff, 0.0f, 1.0f) : 0.5f;
        } else if (material.alpha_mode == cgltf_alpha_mode_blend) {
            std::cerr << "[glTF Warning] BLEND transparency is not implemented; material '" << (material.name ? material.name : "(unnamed)")
                      << "' is rendered opaque.\n";
        }
        m_allMaterials.push_back(imported);
    }

    std::unordered_set<const cgltf_node*> visitedNodes;
    auto importNode = [&](const cgltf_node* root, const auto& visitSelf, const std::string& parentPath) -> void {
        if (!root) return;
        if (!visitedNodes.insert(root).second) {
            std::cerr << "[glTF Warning] Repeated/cyclic scene node skipped: "
                      << (root->name ? root->name : "(unnamed)") << ".\n";
            return;
        }
        const std::string nodeName = root->name && root->name[0] != '\0'
            ? root->name : ("node_" + std::to_string(static_cast<size_t>(root - data->nodes)));
        const std::string nodePath = parentPath.empty() ? nodeName : parentPath + "/" + nodeName;
        float worldMatrix[16] = {};
        cgltf_node_transform_world(root, worldMatrix);
        bool matrixFinite = true;
        for (float value : worldMatrix) matrixFinite = matrixFinite && std::isfinite(value);
        double worldDeterminant = 0.0;
        const bool matrixInvertible = matrixFinite && getGltfLinearDeterminant(worldMatrix, worldDeterminant);
        if (!matrixInvertible) {
            std::cerr << "[glTF Error] Non-finite or singular world transform on node '"
                      << (root->name ? root->name : "(unnamed)") << "'.\n";
        } else if (root->mesh) {
            for (cgltf_size primitiveIndex = 0; primitiveIndex < root->mesh->primitives_count; ++primitiveIndex) {
                const cgltf_primitive& primitive = root->mesh->primitives[primitiveIndex];
                if (primitive.type != cgltf_primitive_type_triangles) {
                    std::cerr << "[glTF Warning] Skipped non-triangle primitive on node '"
                              << (root->name ? root->name : "(unnamed)") << "'.\n";
                    continue;
                }
                const cgltf_accessor* positions = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
                const cgltf_accessor* normals = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
                const cgltf_accessor* texcoords = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0);
                if (!positions || positions->type != cgltf_type_vec3 || positions->count < 3 ||
                    positions->count > std::numeric_limits<uint32_t>::max() ||
                    (normals && (normals->type != cgltf_type_vec3 || normals->count != positions->count)) ||
                    (texcoords && (texcoords->type != cgltf_type_vec2 || texcoords->count != positions->count))) {
                    std::cerr << "[glTF Error] Invalid vertex accessor layout on node '"
                              << (root->name ? root->name : "(unnamed)") << "'.\n";
                    continue;
                }

                std::vector<Vertex> localVertices(static_cast<size_t>(positions->count));
                AABB localBounds;
                bool primitiveValid = true;
                for (cgltf_size vertexIndex = 0; vertexIndex < positions->count; ++vertexIndex) {
                    cgltf_float sourcePosition[3] = {};
                    if (!cgltf_accessor_read_float(positions, vertexIndex, sourcePosition, 3)) {
                        primitiveValid = false;
                        break;
                    }
                    Vertex& vertex = localVertices[vertexIndex];
                    float transformedPosition[3] = {};
                    if (!transformGltfPosition(worldMatrix, sourcePosition, transformedPosition)) {
                        primitiveValid = false;
                        break;
                    }
                    vertex.position = XMFLOAT3(transformedPosition[0], transformedPosition[1], transformedPosition[2]);
                    if (normals) {
                        cgltf_float sourceNormal[3] = {};
                        float transformedNormal[3] = {};
                        if (!cgltf_accessor_read_float(normals, vertexIndex, sourceNormal, 3) ||
                            !transformGltfNormal(worldMatrix, sourceNormal, transformedNormal)) {
                            primitiveValid = false;
                            break;
                        }
                        vertex.normal = XMFLOAT3(transformedNormal[0], transformedNormal[1], transformedNormal[2]);
                    } else {
                        vertex.normal = XMFLOAT3(0.0f, 0.0f, 0.0f);
                    }
                    if (texcoords) {
                        cgltf_float uv[2] = {};
                        if (!cgltf_accessor_read_float(texcoords, vertexIndex, uv, 2) ||
                            !std::isfinite(uv[0]) || !std::isfinite(uv[1])) {
                            primitiveValid = false;
                            break;
                        }
                        vertex.uv = XMFLOAT2(uv[0], uv[1]);
                    }
                    localBounds.expand(Vec3(vertex.position.x, vertex.position.y, vertex.position.z));
                }
                if (!primitiveValid) {
                    std::cerr << "[glTF Error] Could not decode/transform vertex data on node '"
                              << (root->name ? root->name : "(unnamed)") << "'.\n";
                    continue;
                }

                std::vector<uint32_t> localIndices;
                if (primitive.indices) {
                    const cgltf_accessor* indexAccessor = primitive.indices;
                    if (indexAccessor->type != cgltf_type_scalar || indexAccessor->count < 3 ||
                        indexAccessor->count % 3 != 0 ||
                        indexAccessor->count > std::numeric_limits<uint32_t>::max()) {
                        std::cerr << "[glTF Error] Invalid triangle index accessor on node '"
                                  << (root->name ? root->name : "(unnamed)") << "'.\n";
                        continue;
                    }
                    localIndices.reserve(static_cast<size_t>(indexAccessor->count));
                    for (cgltf_size index = 0; index < indexAccessor->count; ++index) {
                        const cgltf_size value = cgltf_accessor_read_index(indexAccessor, index);
                        if (value >= positions->count) {
                            primitiveValid = false;
                            break;
                        }
                        localIndices.push_back(static_cast<uint32_t>(value));
                    }
                } else {
                    if (positions->count % 3 != 0) {
                        std::cerr << "[glTF Error] Non-indexed primitive vertex count is not divisible by three.\n";
                        continue;
                    }
                    localIndices.reserve(static_cast<size_t>(positions->count));
                    for (cgltf_size index = 0; index < positions->count; ++index)
                        localIndices.push_back(static_cast<uint32_t>(index));
                }
                if (!primitiveValid || localIndices.size() < 3) {
                    std::cerr << "[glTF Error] Invalid triangle index data on node '"
                              << (root->name ? root->name : "(unnamed)") << "'.\n";
                    continue;
                }

                if (!normals) {
                    std::vector<Vec3> normalSums(localVertices.size(), Vec3(0.0f));
                    const float orientationSign = worldDeterminant < 0.0 ? -1.0f : 1.0f;
                    for (size_t index = 0; index < localIndices.size(); index += 3) {
                        const Vertex& a = localVertices[localIndices[index]];
                        const Vertex& b = localVertices[localIndices[index + 1]];
                        const Vertex& c = localVertices[localIndices[index + 2]];
                        const Vec3 edge1 = Vec3(b.position.x - a.position.x, b.position.y - a.position.y, b.position.z - a.position.z);
                        const Vec3 edge2 = Vec3(c.position.x - a.position.x, c.position.y - a.position.y, c.position.z - a.position.z);
                        const Vec3 faceNormal = Vec3::cross(edge1, edge2) * orientationSign;
                        if (!std::isfinite(faceNormal.x) || !std::isfinite(faceNormal.y) || !std::isfinite(faceNormal.z)) {
                            primitiveValid = false;
                            break;
                        }
                        normalSums[localIndices[index]] += faceNormal;
                        normalSums[localIndices[index + 1]] += faceNormal;
                        normalSums[localIndices[index + 2]] += faceNormal;
                    }
                    if (!primitiveValid) continue;
                    for (size_t i = 0; i < localVertices.size(); ++i) {
                        const float lengthSquared = normalSums[i].lengthSq();
                        if (std::isfinite(lengthSquared) && lengthSquared > 1.0e-20f) {
                            const float invLength = 1.0f / std::sqrt(lengthSquared);
                            localVertices[i].normal = XMFLOAT3(normalSums[i].x * invLength,
                                                               normalSums[i].y * invLength,
                                                               normalSums[i].z * invLength);
                        } else {
                            localVertices[i].normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
                        }
                    }
                }
                if (m_allVertices.size() > std::numeric_limits<uint32_t>::max() - localVertices.size() ||
                    m_allIndices.size() > std::numeric_limits<uint32_t>::max() - localIndices.size()) {
                    std::cerr << "[glTF Error] Mesh data exceeds the engine's 32-bit draw/index offsets.\n";
                    return;
                }
                const uint32_t vertexOffset = static_cast<uint32_t>(m_allVertices.size());
                const uint32_t indexOffset = static_cast<uint32_t>(m_allIndices.size());
                m_allVertices.insert(m_allVertices.end(), localVertices.begin(), localVertices.end());
                m_allIndices.insert(m_allIndices.end(), localIndices.begin(), localIndices.end());

                uint32_t materialIndex = 0;
                if (primitive.material) {
                    const cgltf_size resolvedIndex = cgltf_material_index(data.get(), primitive.material);
                    if (resolvedIndex >= data->materials_count || resolvedIndex + 1 >= m_allMaterials.size()) {
                        std::cerr << "[glTF Error] Primitive references a material outside the parsed material table.\n";
                        m_allVertices.resize(vertexOffset);
                        m_allIndices.resize(indexOffset);
                        continue;
                    }
                    materialIndex = static_cast<uint32_t>(resolvedIndex + 1);
                }
                const MaterialData& material = m_allMaterials[materialIndex];
                SceneEntity entity;
                entity.id = m_nextEntityId++;
                entity.sourceNodePath = nodePath;
                entity.name.name = std::string(root->name ? root->name : "gltf_node") +
                    "_primitive_" + std::to_string(primitiveIndex);
                entity.transform.position = Vec3(0.0f);
                entity.transform.rotation = Vec3(0.0f);
                entity.transform.scale = Vec3(1.0f);
                entity.material.baseColor = Vec4(material.baseColor.x, material.baseColor.y, material.baseColor.z, material.baseColor.w);
                entity.material.metallic = material.metallic;
                entity.material.roughness = material.roughness;
                entity.mesh.vertexOffset = vertexOffset;
                entity.mesh.vertexCount = static_cast<uint32_t>(localVertices.size());
                entity.mesh.indexOffset = indexOffset;
                entity.mesh.indexCount = static_cast<uint32_t>(localIndices.size());
                entity.mesh.localAABB = localBounds;

                InstanceData instance = {};
                instance.materialId = materialIndex;
                instance.vertexOffset = vertexOffset;
                instance.indexOffset = indexOffset;
                m_allInstances.push_back(instance);
                m_entities.push_back(std::move(entity));
            }
        }
        for (cgltf_size childIndex = 0; childIndex < root->children_count; ++childIndex)
            visitSelf(root->children[childIndex], visitSelf, nodePath);
    };

    if (data->scene) {
        for (cgltf_size rootIndex = 0; rootIndex < data->scene->nodes_count; ++rootIndex)
            importNode(data->scene->nodes[rootIndex], importNode, {});
    } else {
        for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex) {
            const cgltf_node* node = &data->nodes[nodeIndex];
            if (!node->parent) importNode(node, importNode, {});
        }
    }
    if (m_entities.empty() || m_allVertices.empty() || m_allIndices.empty()) {
        std::cerr << "[glTF Error] No valid triangle meshes were imported from '" << path << "'.\n";
        return false;
    }
    if (!createTextureArray(device, texturePaths, srgbTextureLayers)) {
        std::cerr << "[glTF Error] Could not create the imported texture array for '" << path << "'.\n";
        return false;
    }

    AABB sceneBounds;
    for (const Vertex& vertex : m_allVertices)
        sceneBounds.expand(Vec3(vertex.position.x, vertex.position.y, vertex.position.z));
    if (!std::isfinite(sceneBounds.min.x) || !std::isfinite(sceneBounds.max.x)) {
        std::cerr << "[glTF Error] Imported scene bounds are invalid for '" << path << "'.\n";
        return false;
    }
    const Vec3 sceneCenter = (sceneBounds.min + sceneBounds.max) * 0.5f;
    m_camera.position = sceneCenter + Vec3(-6.0f, 2.0f, 0.0f);
    m_camera.yaw = 0.0f;
    m_camera.pitch = 2.0f;
    m_camera.fov = 65.0f;
    m_camera.moveSpeed = 6.0f;
    m_camera.lookSpeed = 0.15f;

    std::cout << "[Scene] Successfully imported glTF scene '" << path << "': "
              << m_entities.size() << " mesh primitives, " << m_allVertices.size() << " vertices, "
              << m_allIndices.size() / 3 << " triangles, " << m_allMaterials.size() << " materials.\n";
    return true;
}
void Scene::buildProceduralSponza(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices) {
    auto addBox = [&](const Vec3& center, const Vec3& extents, const XMFLOAT4& color, float metallic, float roughness, const std::string& name) {
        uint32_t vStart = static_cast<uint32_t>(outVertices.size());
        uint32_t iStart = static_cast<uint32_t>(outIndices.size());

        Vec3 h = extents * 0.5f;
        Vec3 p[8] = {
            Vec3(-h.x, -h.y, -h.z),
            Vec3( h.x, -h.y, -h.z),
            Vec3( h.x,  h.y, -h.z),
            Vec3(-h.x,  h.y, -h.z),
            Vec3(-h.x, -h.y,  h.z),
            Vec3( h.x, -h.y,  h.z),
            Vec3( h.x,  h.y,  h.z),
            Vec3(-h.x,  h.y,  h.z),
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
        mat.emissiveColor = XMFLOAT4(color.x, color.y, color.z, 0.0f);
        mat.textureId = -1;
        mat.metallicRoughnessTextureId = -1;
        m_allMaterials.push_back(mat);

        SceneEntity ent;
        ent.id = m_nextEntityId++;
        ent.sourceNodePath = "Procedural Sponza";
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

void Scene::update(float dt, const Window& window, float viewportX, float viewportY, float viewportW,
                   float viewportH, float renderAspect, uint32_t renderHeight, uint32_t frameIndex) {
    const float movementDt = std::isfinite(dt) && dt > 0.0f ? std::min(dt, 0.1f) : 0.0f;
    m_sceneCbFrameIndex = frameIndex % D3D12Device::FRAME_COUNT;
    m_renderHeight = std::max(1u, renderHeight);
    m_frameCount++;
    if (renderAspect > 0.0f) {
        m_viewportAspect = renderAspect;
    }

    // Keep A/B capture camera matrices identical even if the user moves the
    // mouse or presses movement keys while the automated window is active.
    if (!m_freezeSampleSeedForValidation) {
        float speed = m_camera.moveSpeed;
        if (window.isKeyDown(VK_SHIFT)) speed *= 2.5f;

        const Vec3 fwd = m_camera.getForward();
        const Vec3 right = m_camera.getRight();
        const Vec3 up = Vec3(0.0f, 1.0f, 0.0f);

        if (window.isKeyDown('W')) m_camera.position += fwd * speed * movementDt;
        if (window.isKeyDown('S')) m_camera.position -= fwd * speed * movementDt;
        if (window.isKeyDown('D')) m_camera.position += right * speed * movementDt;
        if (window.isKeyDown('A')) m_camera.position -= right * speed * movementDt;
        if (window.isKeyDown(VK_SPACE) || window.isKeyDown('E')) m_camera.position += up * speed * movementDt;
        if (window.isKeyDown(VK_CONTROL) || window.isKeyDown('Q')) m_camera.position -= up * speed * movementDt;

        // Mouse look with Right Mouse Button
        if (window.isMouseButtonDown(1)) {
            const Vec2 delta = window.getMouseDelta();
            m_camera.yaw += delta.x * m_camera.lookSpeed;
            m_camera.pitch -= delta.y * m_camera.lookSpeed;
            m_camera.pitch = std::clamp(m_camera.pitch, -88.0f, 88.0f);
            m_isLooking = true;
        } else {
            m_isLooking = false;
        }
    } else {
        m_isLooking = false;
    }

    // Accumulation logic: check if camera moved relative to previous rendered frame
    m_cameraMoved = !m_firstFrame && (std::abs(m_camera.position.x - m_lastCameraPos.x) > 0.0005f ||
                     std::abs(m_camera.position.y - m_lastCameraPos.y) > 0.0005f ||
                     std::abs(m_camera.position.z - m_lastCameraPos.z) > 0.0005f ||
                     std::abs(m_camera.yaw - m_lastCameraYaw) > 0.005f ||
                     std::abs(m_camera.pitch - m_lastCameraPitch) > 0.005f);
    // A stationary architectural scene can retain NRD's world-space history
    // while the camera moves; NRD reprojects it from the previous camera
    // matrices. Reset only on an explicit lighting/material/scene change. The
    // ocean has NRD disabled and keeps conventional camera-motion resets.
    const bool useNrdCameraReprojection = !m_isOceanScene &&
        m_sceneCbData.renderMode <= 1u && m_sceneCbData.enableNRD != 0;
    if (m_freezeSampleSeedForValidation) {
        // Each A/B state replays the same low-discrepancy sample sequence and
        // accumulates it from a clean history. This reduces reflection variance
        // without letting the on/off captures use different random samples.
        if (m_firstFrame || m_accumResetPending) {
            m_accumFrameCount = 1;
            m_accumResetPending = false;
        } else {
            m_accumFrameCount = std::min(m_accumFrameCount + 1u, 256u);
        }
    } else if (m_firstFrame || (m_cameraMoved && !useNrdCameraReprojection) || m_accumResetPending) {
        m_accumFrameCount = 1;
        m_accumResetPending = false;
    } else {
        // Shader accumulation is clamped to 256 samples; saturating here keeps
        // the temporal denominator stable and prevents uint wraparound.
        if (m_accumFrameCount < 256u) ++m_accumFrameCount;
    }

    if (m_isOceanScene) updateOceanPhysics(dt);

    // Ray pick on Left Click
    if (window.wasMouseButtonPressed(0) && !m_isLooking) {
        Vec2 mPos = window.getMousePos();
        float relX = mPos.x - viewportX;
        float relY = mPos.y - viewportY;
        if (relX >= 0.0f && relX <= viewportW && relY >= 0.0f && relY <= viewportH) {
            Vec3 localHit(0.0f);
            EntityID hit = rayPick(relX, relY, viewportW, viewportH, &localHit);
            // A click on empty viewport space clears selection. This also makes
            // the visual selection overlay follow the editor's current state.
            setSelectedEntityId(hit, hit != NULL_ENTITY ? &localHit : nullptr);
        }
    }
}

void Scene::updateOceanPhysics(float dt) {
    if (!m_joltWorld || !m_joltWorld->isReady()) return;
    const float fixedStep = 1.0f / 60.0f;
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    const float maximumFrameInput = fixedStep * 4.0f;
    const float admittedFrameTime = std::min(dt, maximumFrameInput);
    m_oceanDroppedSimulationTime = std::min(std::numeric_limits<float>::max(),
        m_oceanDroppedSimulationTime + (dt - admittedFrameTime));
    m_oceanStepAccumulator = std::min(m_oceanStepAccumulator + admittedFrameTime, fixedStep * 5.0f);
    uint32_t steps = 0;
    while (m_oceanStepAccumulator >= fixedStep && steps < 4) {
        const float nextSimulationTime = m_oceanTime + fixedStep;
        if (!m_joltWorld->step(fixedStep, nextSimulationTime)) {
            std::cerr << "[Ocean Error] Jolt fixed-step update failed; this frame's remaining physics time was discarded.\n";
            m_oceanDroppedSimulationTime = std::min(std::numeric_limits<float>::max(),
                m_oceanDroppedSimulationTime + m_oceanStepAccumulator);
            m_oceanStepAccumulator = 0.0f;
            break;
        }
        m_oceanTime = nextSimulationTime;

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

            if (i >= m_oceanBodyHalfExtents.size()) {
                std::cerr << "[Ocean Error] Missing shape extents for Jolt body " << bodyIndex << ".\n";
                continue;
            }
            const Vec3 halfExtents = m_oceanBodyHalfExtents[i];
            float verticalSupport = 0.0f;
            const bool isEllipsoid = i < m_oceanBodyIsEllipsoid.size() && m_oceanBodyIsEllipsoid[i];
            const bool supportValid = isEllipsoid
                ? tryGetOrientedEllipsoidVerticalHalfExtent(rotation, halfExtents.x, halfExtents.y,
                                                             halfExtents.z, verticalSupport)
                : tryGetOrientedBoxVerticalHalfExtent(rotation, halfExtents.x, halfExtents.y,
                                                       halfExtents.z, verticalSupport);
            if (!supportValid) {
                std::cerr << "[Ocean Error] Invalid Jolt orientation/extents for body " << bodyIndex
                          << "; skipping water-entry test for this substep.\n";
                continue;
            }
            const float bottomClearance = position[1] - verticalSupport - surface[0];
            if (i >= m_oceanPreviousBottomClearance.size() || i >= m_oceanImpactTimes.size() ||
                i >= m_oceanImpactStrengths.size() || i >= m_oceanImpactPositions.size()) continue;
            const float previousClearance = m_oceanPreviousBottomClearance[i];
            const bool crossedIntoWater = previousClearance > 0.04f && bottomClearance <= 0.04f;
            const bool crossedOutOfWater = previousClearance <= 0.04f && bottomClearance > 0.04f;
            if (crossedIntoWater) ++m_oceanEntryCrossingCount;
            if (crossedOutOfWater) ++m_oceanExitCrossingCount;
            if (crossedIntoWater && velocity[1] < -0.45f) {
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
        if (i + 1 >= m_entities.size()) {
            std::cerr << "[Ocean Error] Jolt body has no corresponding ECS entity (slot " << i << ").\n";
            continue;
        }
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
            if (bodySlot >= m_oceanBodyHalfExtents.size()) {
                std::cerr << "[Ocean Error] Missing shape extent for GPU body slot " << bodySlot << ".\n";
                continue;
            }
            const Vec3 halfExtents = m_oceanBodyHalfExtents[bodySlot];
            float verticalSupport = 0.0f;
            const bool isEllipsoid = bodySlot < m_oceanBodyIsEllipsoid.size() && m_oceanBodyIsEllipsoid[bodySlot];
            const bool supportValid = isEllipsoid
                ? tryGetOrientedEllipsoidVerticalHalfExtent(rotation, halfExtents.x, halfExtents.y,
                                                            halfExtents.z, verticalSupport)
                : tryGetOrientedBoxVerticalHalfExtent(rotation, halfExtents.x, halfExtents.y,
                                                       halfExtents.z, verticalSupport);
            if (!supportValid) {
                std::cerr << "[Ocean Error] Could not derive GPU body support for slot " << bodySlot << ".\n";
                continue;
            }
            const OceanBodyWaveState bodyState{position[0], position[1], position[2],
                bodySlot < m_oceanBodyRadii.size() ? m_oceanBodyRadii[bodySlot] : 0.5f,
                verticalSupport, velocity[0], velocity[1], velocity[2]};
            // Match the body displacement field's CPU/GPU amplitude: immersion
            // is defined against the shared base wave. Jolt buoyancy itself
            // samples impacts and neighboring body displacement as well.
            const OceanWaveSample baseWater = evaluateOceanWave(position[0], position[2], m_oceanTime);
            m_sceneCbData.oceanBodyVerticalState[bodySlot] = XMFLOAT4(
                velocity[1], verticalSupport, oceanBodySubmersion(baseWater, bodyState), 0.0f);
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

EntityID Scene::rayPick(float screenX, float screenY, float viewportW, float viewportH, Vec3* outLocalHit) {
    if (outLocalHit) *outLocalHit = Vec3(0.0f);
    if (!std::isfinite(screenX) || !std::isfinite(screenY) || !std::isfinite(viewportW) ||
        !std::isfinite(viewportH) || viewportW <= 0.0f || viewportH <= 0.0f) return NULL_ENTITY;

    float ndcX = (screenX / viewportW) * 2.0f - 1.0f;
    float ndcY = 1.0f - (screenY / viewportH) * 2.0f;

    const float aspect = std::clamp(std::isfinite(m_viewportAspect) && m_viewportAspect > 1.0e-4f
        ? m_viewportAspect : viewportW / viewportH, 1.0e-4f, 1.0e4f);
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
    XMMATRIX bestWorld = XMMatrixIdentity();

    for (const auto& ent : m_entities) {
        const Vec3 localMin = ent.mesh.localAABB.min;
        const Vec3 localMax = ent.mesh.localAABB.max;
        const auto finiteVec3 = [](const Vec3& value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };
        if (!finiteVec3(ent.transform.position) || !finiteVec3(ent.transform.rotation) ||
            !finiteVec3(ent.transform.scale) || !finiteVec3(localMin) || !finiteVec3(localMax) ||
            std::abs(ent.transform.scale.x) <= 1.0e-8f ||
            std::abs(ent.transform.scale.y) <= 1.0e-8f ||
            std::abs(ent.transform.scale.z) <= 1.0e-8f ||
            localMin.x > localMax.x || localMin.y > localMax.y || localMin.z > localMax.z ||
            ent.mesh.vertexOffset > m_allVertices.size() ||
            ent.mesh.vertexCount > m_allVertices.size() - ent.mesh.vertexOffset ||
            ent.mesh.indexOffset > m_allIndices.size() ||
            ent.mesh.indexCount > m_allIndices.size() - ent.mesh.indexOffset || ent.mesh.indexCount < 3) {
            continue;
        }
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
            if (!std::isfinite(transformed.x) || !std::isfinite(transformed.y) || !std::isfinite(transformed.z)) {
                worldBounds = AABB{};
                break;
            }
            worldBounds.expand(Vec3(transformed.x, transformed.y, transformed.z));
        }
        float boundsT = 0.0f;
        if (!worldBounds.rayIntersect(rayOrig, rayDir, boundsT)) continue;

        const size_t meshIndexBegin = ent.mesh.indexOffset;
        const size_t meshIndexEnd = meshIndexBegin + ent.mesh.indexCount;
        for (size_t index = meshIndexBegin; index + 2 < meshIndexEnd; index += 3) {
            const uint32_t localIndex0 = m_allIndices[index + 0];
            const uint32_t localIndex1 = m_allIndices[index + 1];
            const uint32_t localIndex2 = m_allIndices[index + 2];
            if (localIndex0 >= ent.mesh.vertexCount || localIndex1 >= ent.mesh.vertexCount ||
                localIndex2 >= ent.mesh.vertexCount) continue;

            const auto worldVertex = [&ent, &world, this](uint32_t localIndex, Vec3& result) {
                const XMFLOAT3& position = m_allVertices[ent.mesh.vertexOffset + localIndex].position;
                if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return false;
                XMFLOAT3 transformed;
                XMStoreFloat3(&transformed, XMVector3TransformCoord(
                    XMVectorSet(position.x, position.y, position.z, 1.0f), world));
                if (!std::isfinite(transformed.x) || !std::isfinite(transformed.y) || !std::isfinite(transformed.z)) return false;
                result = Vec3(transformed.x, transformed.y, transformed.z);
                return true;
            };
            Vec3 v0, v1, v2;
            if (!worldVertex(localIndex0, v0) || !worldVertex(localIndex1, v1) || !worldVertex(localIndex2, v2)) continue;
            float t = 0.0f;
            if (rayTriangleIntersect(rayOrig, rayDir, v0, v1, v2, t) && t < closestT) {
                closestT = t;
                bestId = ent.id;
                bestWorld = world;
            }
        }
    }

    if (bestId != NULL_ENTITY && outLocalHit) {
        XMVECTOR worldDeterminant;
        const XMMATRIX inverseWorld = XMMatrixInverse(&worldDeterminant, bestWorld);
        float determinant = 0.0f;
        XMStoreFloat(&determinant, worldDeterminant);
        if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12f) return NULL_ENTITY;
        const Vec3 worldHit = rayOrig + rayDir * closestT;
        XMFLOAT3 localHit;
        XMStoreFloat3(&localHit, XMVector3TransformCoord(
            XMVectorSet(worldHit.x, worldHit.y, worldHit.z, 1.0f), inverseWorld));
        if (!std::isfinite(localHit.x) || !std::isfinite(localHit.y) || !std::isfinite(localHit.z)) return NULL_ENTITY;
        *outLocalHit = Vec3(localHit.x, localHit.y, localHit.z);
    }

    return bestId;
}

bool Scene::updateGpuBuffers(D3D12Device& device, std::string* failureReason) {
    if (failureReason) failureReason->clear();
    const auto fail = [failureReason](const char* reason) {
        if (failureReason) *failureReason = reason;
        return false;
    };
    if (m_materialsDirty) {
        if (m_allMaterials.size() > m_materialBufferCapacity || !m_materialBuffer) {
            std::cerr << "[Scene Error] Material edit exceeded the allocated GPU material capacity." << std::endl;
            m_materialsDirty = false;
            m_instancesDirty = false;
            m_queuedMaterialEdits = 0;
        } else {
            // This is an infrequent editor operation. Drain prior frames before modifying
            // upload memory that an earlier DXR dispatch may still be reading.
            if (!device.waitForGpu()) return fail("GPU drain failed before writing edited materials.");
            void* materialMapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            const HRESULT materialMapResult = m_materialBuffer->Map(0, &readRange, &materialMapped);
            if (FAILED(materialMapResult) || !materialMapped) {
                std::cerr << "[Scene Error] Could not map the GPU material buffer for an Inspector edit: HRESULT 0x"
                          << std::hex << materialMapResult << std::dec << ".\n";
                return fail("Could not map the material upload buffer.");
            }
            if (!m_allMaterials.empty()) {
                std::memcpy(materialMapped, m_allMaterials.data(), sizeof(MaterialData) * m_allMaterials.size());
            }
            m_materialBuffer->Unmap(0, nullptr);

            if (m_instancesDirty) {
                if (!m_instanceBuffer) {
                    std::cerr << "[Scene Error] GPU instance buffer is missing for a material override.\n";
                    return fail("Instance buffer is missing during a material edit.");
                }
                void* instanceMapped = nullptr;
                const HRESULT instanceMapResult = m_instanceBuffer->Map(0, &readRange, &instanceMapped);
                if (FAILED(instanceMapResult) || !instanceMapped) {
                    std::cerr << "[Scene Error] Could not map the GPU instance buffer for a material override: HRESULT 0x"
                              << std::hex << instanceMapResult << std::dec << ".\n";
                    return fail("Could not map the instance upload buffer.");
                }
                if (!m_allInstances.empty()) {
                    std::memcpy(instanceMapped, m_allInstances.data(), sizeof(InstanceData) * m_allInstances.size());
                }
                m_instanceBuffer->Unmap(0, nullptr);
            }

            std::cout << "[Scene] Uploaded material edits: " << m_queuedMaterialEdits
                << " entity override(s); GPU materials=" << m_allMaterials.size() << std::endl;
            m_uploadedMaterialEdits = m_queuedMaterialEdits;
            m_materialsDirty = false;
            m_instancesDirty = false;
            m_queuedMaterialEdits = 0;
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
        if (m_tlas.build(device, tlasInstances)) {
            m_transformsDirty = false;
        } else {
            std::cerr << "[Scene Error] TLAS update failed; transform changes remain pending for retry.\n";
            return fail("TLAS update failed while applying scene transforms.");
        }
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
        return fail("Camera view-projection matrix is singular or non-finite.");
    }

    XMStoreFloat4x4(&m_sceneCbData.viewProjInv, XMMatrixTranspose(invVP));
    m_sceneCbData.cameraPos = XMFLOAT3(m_camera.position.x, m_camera.position.y, m_camera.position.z);
    m_sceneCbData.sunDirection = XMFLOAT3(m_sun.direction.x, m_sun.direction.y, m_sun.direction.z);
    m_sceneCbData.sunIntensity = m_sun.intensity;
    m_sceneCbData.sunColor = XMFLOAT4(m_sun.color.x, m_sun.color.y, m_sun.color.z, 1.0f);
    if (m_freezeSampleSeedForValidation) {
        m_sceneCbData.frameCount = m_validationSampleIndex;
        m_validationSampleIndex = std::min(m_validationSampleIndex + 1u, 0x00ffffffu);
    } else {
        m_sceneCbData.frameCount = m_frameCount;
    }
    m_sceneCbData.accumFrameCount = m_accumFrameCount;
    m_sceneCbData.dlssReset = m_accumFrameCount <= 1u ? 1u : 0u;
    m_sceneCbData.renderHeight = m_renderHeight;
    m_sceneCbData.oceanTime = m_oceanTime;

    Vec3 fwd = m_camera.getForward();
    Vec3 rgt = m_camera.getRight();
    Vec3 up  = m_camera.getUp();
    m_sceneCbData.camForward = XMFLOAT3(fwd.x, fwd.y, fwd.z);
    m_sceneCbData.fovY = m_camera.fov * (3.14159265f / 180.0f);
    m_sceneCbData.cameraClipPlanes = XMFLOAT2(m_camera.nearPlane, m_camera.farPlane);
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

    // Halton (2, 3) low-discrepancy sub-pixel jitter sequence (16 phases).
    // Freeze every sampling input during A/B capture, including this field,
    // so tests stay stable if antialiasing is enabled in a future capture path.
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
    if (m_freezeSampleSeedForValidation) {
        m_sceneCbData.jitterOffset = XMFLOAT2(0.0f, 0.0f);
        m_sceneCbData.prevJitterX = 0.0f;
        m_sceneCbData.prevJitterY = 0.0f;
    } else {
        const uint32_t phase = m_frameCount % 16;
        const uint32_t previousPhase = (phase + 15u) % 16u;
        m_sceneCbData.jitterOffset.x = halton2[phase] - 0.5f;
        m_sceneCbData.jitterOffset.y = halton3[phase] - 0.5f;
        m_sceneCbData.prevJitterX = halton2[previousPhase] - 0.5f;
        m_sceneCbData.prevJitterY = halton3[previousPhase] - 0.5f;
    }

    // Map and update SceneCB
    void* cbMapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 }; // We do not intend to read from this resource on the CPU
    ComPtr<ID3D12Resource>& frameSceneCb = m_sceneCb[m_sceneCbFrameIndex];
    const HRESULT sceneCbMapResult = frameSceneCb
        ? frameSceneCb->Map(0, &readRange, &cbMapped) : E_POINTER;
    if (SUCCEEDED(sceneCbMapResult) && cbMapped) {
        std::memcpy(cbMapped, &m_sceneCbData, sizeof(SceneCB));
        frameSceneCb->Unmap(0, nullptr);
    } else {
        const HRESULT deviceRemovedReason = device.getDevice() ? device.getDevice()->GetDeviceRemovedReason() : E_POINTER;
        std::cerr << "[Scene Error] Could not map SceneCB for frame " << m_sceneCbFrameIndex
                  << "; map HRESULT=0x" << std::hex << static_cast<uint32_t>(sceneCbMapResult)
                  << ", device-removed HRESULT=0x" << static_cast<uint32_t>(deviceRemovedReason)
                  << std::dec << ".\n";
        if (failureReason) {
            *failureReason = "SceneCB map HRESULT " + std::to_string(static_cast<uint32_t>(sceneCbMapResult)) +
                ", device-removed HRESULT " + std::to_string(static_cast<uint32_t>(deviceRemovedReason));
        }
        return false;
    }
    return true;
}

} // namespace Engine
