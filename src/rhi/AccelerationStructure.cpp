#include "AccelerationStructure.h"
#include <cstring>
#include <algorithm>

namespace Engine {

static inline uint64_t alignTo(uint64_t size, uint64_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

bool Blas::build(D3D12Device& device,
                 ID3D12Resource* vertexBuffer, uint64_t vertexByteOffset, uint32_t vertexCount, uint32_t vertexStride,
                 ID3D12Resource* indexBuffer, uint64_t indexByteOffset, uint32_t indexCount,
                 bool isOpaque) {
    D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
    geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geomDesc.Flags = isOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geomDesc.Triangles.VertexBuffer.StartAddress = vertexBuffer->GetGPUVirtualAddress() + vertexByteOffset;
    geomDesc.Triangles.VertexBuffer.StrideInBytes = vertexStride;
    geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geomDesc.Triangles.VertexCount = vertexCount;
    geomDesc.Triangles.IndexBuffer = indexBuffer->GetGPUVirtualAddress() + indexByteOffset;
    geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geomDesc.Triangles.IndexCount = indexCount;
    geomDesc.Triangles.Transform3x4 = 0;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geomDesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    device.getDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);

    uint64_t scratchSize = alignTo(prebuildInfo.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    uint64_t resultSize = alignTo(prebuildInfo.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

    m_scratchBuffer = device.createBuffer(scratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_resultBuffer = device.createBuffer(resultSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.DestAccelerationStructureData = m_resultBuffer->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = m_scratchBuffer->GetGPUVirtualAddress();

    device.getCommandList()->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    device.uavBarrier(m_resultBuffer.Get());

    return true;
}

bool Tlas::build(D3D12Device& device, const std::vector<Instance>& instances) {
    if (instances.empty()) return false;

    uint32_t numInstances = static_cast<uint32_t>(instances.size());
    uint64_t instanceDescsSize = alignTo(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * numInstances, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

    const bool capacityChanged = numInstances > m_maxInstances || !m_resultBuffer;
    if (capacityChanged) {
        m_maxInstances = numInstances;
        m_built = false;
        for (auto& frameBuffer : m_instanceDescBuffers) {
            frameBuffer = device.createBuffer(instanceDescsSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        }
    }
    ComPtr<ID3D12Resource>& instanceDescBuffer = m_instanceDescBuffers[device.getFrameIndex() % D3D12Device::FRAME_COUNT];
    if (!instanceDescBuffer) instanceDescBuffer = device.createBuffer(instanceDescsSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

    // Each swap-chain frame owns its upload data so the CPU cannot overwrite
    // descriptors that the GPU is still consuming from the prior frame.
    D3D12_RAYTRACING_INSTANCE_DESC* mappedData = nullptr;
    if (FAILED(instanceDescBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedData)))) return false;
    for (uint32_t i = 0; i < numInstances; ++i) {
        const auto& inst = instances[i];
        std::memcpy(mappedData[i].Transform, inst.transform, sizeof(float) * 12);
        mappedData[i].InstanceID = inst.instanceId;
        mappedData[i].InstanceMask = 0xFF;
        mappedData[i].InstanceContributionToHitGroupIndex = inst.hitGroupIndex;
        mappedData[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        mappedData[i].AccelerationStructure = inst.blasGpuAddress;
    }
    instanceDescBuffer->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
        | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    inputs.NumDescs = numInstances;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.InstanceDescs = instanceDescBuffer->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    device.getDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);

    uint64_t scratchSize = alignTo(std::max(prebuildInfo.ScratchDataSizeInBytes, prebuildInfo.UpdateScratchDataSizeInBytes), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    uint64_t resultSize = alignTo(prebuildInfo.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

    if (capacityChanged) {
        m_scratchBuffer = device.createBuffer(scratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        m_resultBuffer = device.createBuffer(resultSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    if (m_built) {
        buildDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        buildDesc.SourceAccelerationStructureData = m_resultBuffer->GetGPUVirtualAddress();
    }
    buildDesc.DestAccelerationStructureData = m_resultBuffer->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = m_scratchBuffer->GetGPUVirtualAddress();

    device.getCommandList()->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    device.uavBarrier(m_resultBuffer.Get());
    m_built = true;

    return true;
}

} // namespace Engine
