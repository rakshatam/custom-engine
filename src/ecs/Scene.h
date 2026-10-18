#pragma once
#include <vector>
#include <string>
#include <memory>
#include <d3d12.h>
#include <wrl/client.h>
#include "Entity.h"
#include "EntityIndex.h"
#include "Components.h"
#include "../rhi/D3D12Device.h"
#include "../rhi/AccelerationStructure.h"
#include "../rhi/ConstantBuffers.h"
#include "../physics/JoltPhysicsWorld.h"

namespace Engine {

class Window;

struct SceneEntity {
    EntityID           id = NULL_ENTITY;
    std::string        sourceNodePath;
    NameComponent      name;
    TransformComponent transform;
    MaterialComponent  material;
    MeshComponent      mesh;
};

class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    bool init(D3D12Device& device, const std::string& gltfPath);
    bool initOcean(D3D12Device& device);
    bool initReflectionLab(D3D12Device& device, const std::string& gltfPath);
    void update(float dt, const Window& window, float viewportX, float viewportY, float viewportW, float viewportH,
                float renderAspect, uint32_t renderHeight, uint32_t frameIndex);
    void updateGpuBuffers(D3D12Device& device);

    EntityID rayPick(float screenX, float screenY, float viewportW, float viewportH);

    CameraComponent& getCamera() { return m_camera; }
    const CameraComponent& getCamera() const { return m_camera; }

    LightComponent& getSun() { return m_sun; }
    const LightComponent& getSun() const { return m_sun; }

    SceneCB& getSceneCB() { return m_sceneCbData; }
    const SceneCB& getSceneCB() const { return m_sceneCbData; }

    std::vector<SceneEntity>& getEntities() { return m_entities; }
    const std::vector<SceneEntity>& getEntities() const { return m_entities; }

    SceneEntity* getEntity(EntityID id);
    EntityID getSelectedEntityId() const { return m_selectedEntityId; }
    void setSelectedEntityId(EntityID id);
    bool updateEntityMaterial(EntityID id);

    Tlas& getTlas() { return m_tlas; }

    D3D12_GPU_VIRTUAL_ADDRESS getVertexBufferGpuAddress() const { return m_vertexBuffer ? m_vertexBuffer->GetGPUVirtualAddress() : 0; }
    D3D12_GPU_VIRTUAL_ADDRESS getIndexBufferGpuAddress() const { return m_indexBuffer ? m_indexBuffer->GetGPUVirtualAddress() : 0; }
    D3D12_GPU_VIRTUAL_ADDRESS getMaterialBufferGpuAddress() const { return m_materialBuffer ? m_materialBuffer->GetGPUVirtualAddress() : 0; }
    D3D12_GPU_VIRTUAL_ADDRESS getInstanceBufferGpuAddress() const { return m_instanceBuffer ? m_instanceBuffer->GetGPUVirtualAddress() : 0; }
    D3D12_GPU_VIRTUAL_ADDRESS getSceneCbGpuAddress() const {
        const auto& frameSceneCb = m_sceneCb[m_sceneCbFrameIndex];
        return frameSceneCb ? frameSceneCb->GetGPUVirtualAddress() : 0;
    }

    uint32_t getTotalTriangles() const { return m_totalTriangles; }
    uint32_t getTotalInstances() const { return static_cast<uint32_t>(m_entities.size()); }
    void markTransformsDirty() { m_transformsDirty = true; }

    uint32_t getTextureArraySrvIndex() const { return m_textureArraySrvIndex; }
    uint32_t getAccumFrameCount() const { return m_accumFrameCount; }
    void resetAccumulation() {
        m_accumFrameCount = 1;
        m_accumResetPending = true;
        if (m_freezeSampleSeedForValidation) m_validationSampleIndex = 0;
    }
    void setDeterministicSamplingForValidation(bool enabled) {
        m_freezeSampleSeedForValidation = enabled;
        m_validationSampleIndex = 0;
        m_accumFrameCount = 1;
        m_accumResetPending = true;
    }
    bool isOceanScene() const { return m_isOceanScene; }
    bool isReflectionLabScene() const { return m_isReflectionLabScene; }
    float getOceanSimulationTime() const { return m_oceanTime; }
    uint32_t getPhysicsBodyCount() const { return m_joltWorld ? m_joltWorld->bodyCount() : 0; }
    uint32_t consumeUploadedMaterialEdits() { const uint32_t count = m_uploadedMaterialEdits; m_uploadedMaterialEdits = 0; return count; }
    float getOceanMaxVerticalDisplacement() const { return m_oceanMaxVerticalDisplacement; }
    float getOceanMinBodyY() const { return m_oceanMinBodyY; }
    float getOceanMaxBodyY() const { return m_oceanMaxBodyY; }
    uint32_t getOceanImpactCount() const { return m_oceanImpactCount; }
    uint32_t getOceanEntryCrossingCount() const { return m_oceanEntryCrossingCount; }
    uint32_t getOceanExitCrossingCount() const { return m_oceanExitCrossingCount; }
    float getOceanBodyY(uint32_t bodySlot) const {
        const size_t entityIndex = static_cast<size_t>(bodySlot) + 1;
        return bodySlot < m_floatBodyIndices.size() && entityIndex < m_entities.size()
            ? m_entities[entityIndex].transform.position.y : 0.0f;
    }
    float getOceanDroppedSimulationTime() const { return m_oceanDroppedSimulationTime; }

private:
    bool loadAssimp(D3D12Device& device, const std::string& path);
    bool loadGltf(D3D12Device& device, const std::string& path);
    void buildProceduralSponza(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices);
    bool createTextureArray(D3D12Device& device, const std::vector<std::string>& textureUris,
                            const std::vector<uint8_t>& srgbLayers);
    bool uploadGpuScene(D3D12Device& device);
    bool rebuildEntityIndex();
    void updateOceanPhysics(float dt);

    std::vector<SceneEntity> m_entities;
    EntityIndex m_entityIndex;
    EntityID m_selectedEntityId = NULL_ENTITY;
    EntityID m_nextEntityId = 1;

    CameraComponent m_camera;
    LightComponent  m_sun;
    SceneCB         m_sceneCbData = {};

    std::vector<Vertex>       m_allVertices;
    std::vector<uint32_t>     m_allIndices;
    std::vector<MaterialData> m_allMaterials;
    std::vector<InstanceData> m_allInstances;

    std::vector<Blas>         m_blases;
    Tlas                      m_tlas;

    ComPtr<ID3D12Resource>    m_vertexBuffer;
    ComPtr<ID3D12Resource>    m_indexBuffer;
    ComPtr<ID3D12Resource>    m_materialBuffer;
    ComPtr<ID3D12Resource>    m_instanceBuffer;
    ComPtr<ID3D12Resource>    m_sceneCb[D3D12Device::FRAME_COUNT];
    uint32_t                  m_sceneCbFrameIndex = 0;
    ComPtr<ID3D12Resource>    m_textureArray;
    uint32_t                  m_textureArraySrvIndex = 0;

    uint32_t m_totalTriangles = 0;
    size_t   m_materialBufferCapacity = 0;
    uint32_t m_queuedMaterialEdits = 0;
    uint32_t m_uploadedMaterialEdits = 0;
    bool     m_materialsDirty = false;
    bool     m_instancesDirty = false;
    uint32_t m_frameCount = 0;
    uint32_t m_accumFrameCount = 1;
    uint32_t m_validationSampleIndex = 0;
    uint32_t m_renderHeight = 1080;
    float    m_viewportAspect = 16.0f / 9.0f;
    bool     m_accumResetPending = false;
    Vec3     m_lastCameraPos = Vec3(0, 0, 0);
    float    m_lastCameraYaw = 0.0f;
    float    m_lastCameraPitch = 0.0f;
    bool     m_isLooking = false;
    bool     m_transformsDirty = false;
    Vec3     m_lastCamForward = Vec3(0, 0, 0);
    Vec3     m_lastCamRight   = Vec3(0, 0, 0);
    Vec3     m_lastCamUp      = Vec3(0, 0, 0);
    bool     m_firstFrame = true;
    bool     m_cameraMoved = false;
    bool     m_isOceanScene = false;
    bool     m_isReflectionLabScene = false;
    bool     m_loadedSourceAsset = false;
    bool     m_freezeSampleSeedForValidation = false;
    float    m_oceanTime = 0.0f;
    float    m_oceanStepAccumulator = 0.0f;
    float    m_oceanDroppedSimulationTime = 0.0f;
    std::unique_ptr<JoltPhysicsWorld> m_joltWorld;
    std::vector<int32_t> m_floatBodyIndices;
    std::vector<float> m_initialFloatHeights;
    std::vector<Vec3> m_oceanBodyHalfExtents;
    std::vector<bool> m_oceanBodyIsEllipsoid;
    std::vector<float> m_oceanBodyRadii;
    std::vector<float> m_oceanPreviousBottomClearance;
    std::vector<float> m_oceanImpactTimes;
    std::vector<float> m_oceanImpactStrengths;
    std::vector<Vec3> m_oceanImpactPositions;
    uint32_t m_oceanImpactCount = 0;
    uint32_t m_oceanEntryCrossingCount = 0;
    uint32_t m_oceanExitCrossingCount = 0;
    float m_oceanMaxVerticalDisplacement = 0.0f;
    float m_oceanMinBodyY = 1e30f;
    float m_oceanMaxBodyY = -1e30f;
};

} // namespace Engine
