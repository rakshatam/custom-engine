#include "AccelerationStructure.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace Engine {

static bool tryAlignTo(uint64_t size, uint64_t alignment, uint64_t& result) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        size > std::numeric_limits<uint64_t>::max() - (alignment - 1)) {
        return false;
    }
    result = (size + alignment - 1) & ~(alignment - 1);
    return result != 0;
}

static bool bufferRangeFits(ID3D12Resource* resource, uint64_t offset, uint64_t size) {
    if (!resource || size == 0) return false;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        offset <= desc.Width && size <= desc.Width - offset;
}

bool Blas::build(D3D12Device& device,
                 ID3D12Resource* vertexBuffer, uint64_t vertexByteOffset, uint32_t vertexCount, uint32_t vertexStride,
                 ID3D12Resource* indexBuffer, uint64_t indexByteOffset, uint32_t indexCount,
                 bool isOpaque) {
    ID3D12Device5* d3dDevice = device.getDevice();
    ID3D12GraphicsCommandList4* commandList = device.getCommandList();
    if (!d3dDevice || !commandList || vertexCount < 3 || vertexStride < sizeof(float) * 3 ||
        indexCount < 3 || indexCount % 3 != 0 ||
        vertexCount > std::numeric_limits<uint64_t>::max() / vertexStride ||
        indexCount > std::numeric_limits<uint64_t>::max() / sizeof(uint32_t) ||
        !bufferRangeFits(vertexBuffer, vertexByteOffset, static_cast<uint64_t>(vertexCount) * vertexStride) ||
        !bufferRangeFits(indexBuffer, indexByteOffset, static_cast<uint64_t>(indexCount) * sizeof(uint32_t)) ||
        vertexBuffer->GetGPUVirtualAddress() == 0 || indexBuffer->GetGPUVirtualAddress() == 0) {
        return false;
    }

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
    d3dDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
    uint64_t scratchSize = 0;
    uint64_t resultSize = 0;
    if (prebuildInfo.ScratchDataSizeInBytes == 0 || prebuildInfo.ResultDataMaxSizeInBytes == 0 ||
        !tryAlignTo(prebuildInfo.ScratchDataSizeInBytes,
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, scratchSize) ||
        !tryAlignTo(prebuildInfo.ResultDataMaxSizeInBytes,
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, resultSize)) {
        return false;
    }

    ComPtr<ID3D12Resource> scratch = device.createBuffer(scratchSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> result = device.createBuffer(resultSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!scratch || !result || scratch->GetGPUVirtualAddress() == 0 || result->GetGPUVirtualAddress() == 0) {
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    device.uavBarrier(result.Get());
    m_scratchBuffer = std::move(scratch);
    m_resultBuffer = std::move(result);
    return true;
}

bool Tlas::build(D3D12Device& device, const std::vector<Instance>& instances) {
    ID3D12Device5* d3dDevice = device.getDevice();
    ID3D12GraphicsCommandList4* commandList = device.getCommandList();
    if (!d3dDevice || !commandList || instances.empty() ||
        instances.size() > D3D12_RAYTRACING_MAX_INSTANCES_PER_TOP_LEVEL_ACCELERATION_STRUCTURE) {
        return false;
    }
    for (const Instance& instance : instances) {
        if (instance.blasGpuAddress == 0 || instance.instanceId > 0x00ffffffu ||
            instance.hitGroupIndex > 0x00ffffffu) return false;
        for (const auto& row : instance.transform) {
            for (float value : row) if (!std::isfinite(value)) return false;
        }
        const float (&m)[3][4] = instance.transform;
        const float determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12f) return false;
    }

    const uint32_t numInstances = static_cast<uint32_t>(instances.size());
    if (numInstances > std::numeric_limits<uint64_t>::max() / sizeof(D3D12_RAYTRACING_INSTANCE_DESC)) return false;
    uint64_t instanceDescsSize = 0;
    if (!tryAlignTo(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * static_cast<uint64_t>(numInstances),
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, instanceDescsSize)) return false;

    const bool capacityChanged = numInstances > m_maxInstances || !m_resultBuffer;
    std::array<ComPtr<ID3D12Resource>, D3D12Device::FRAME_COUNT> replacementDescBuffers;
    if (capacityChanged) {
        for (auto& frameBuffer : replacementDescBuffers) {
            frameBuffer = device.createBuffer(instanceDescsSize, D3D12_HEAP_TYPE_UPLOAD,
                                               D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!frameBuffer) return false;
        }
    }

    ComPtr<ID3D12Resource>& instanceDescBuffer = capacityChanged
        ? replacementDescBuffers[device.getFrameIndex() % D3D12Device::FRAME_COUNT]
        : m_instanceDescBuffers[device.getFrameIndex() % D3D12Device::FRAME_COUNT];
    if (!instanceDescBuffer) {
        instanceDescBuffer = device.createBuffer(instanceDescsSize, D3D12_HEAP_TYPE_UPLOAD,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!instanceDescBuffer) return false;
    }

    D3D12_RAYTRACING_INSTANCE_DESC* mappedData = nullptr;
    if (FAILED(instanceDescBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedData))) || !mappedData) return false;
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
    d3dDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
    uint64_t scratchSize = 0;
    uint64_t resultSize = 0;
    if (prebuildInfo.ScratchDataSizeInBytes == 0 || prebuildInfo.ResultDataMaxSizeInBytes == 0 ||
        !tryAlignTo(std::max(prebuildInfo.ScratchDataSizeInBytes, prebuildInfo.UpdateScratchDataSizeInBytes),
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, scratchSize) ||
        !tryAlignTo(prebuildInfo.ResultDataMaxSizeInBytes,
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, resultSize)) return false;

    ComPtr<ID3D12Resource> newScratch;
    ComPtr<ID3D12Resource> newResult;
    if (capacityChanged) {
        newScratch = device.createBuffer(scratchSize, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        newResult = device.createBuffer(resultSize, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!newScratch || !newResult || newScratch->GetGPUVirtualAddress() == 0 ||
            newResult->GetGPUVirtualAddress() == 0) return false;
    } else if (!m_scratchBuffer || !m_resultBuffer || m_scratchBuffer->GetGPUVirtualAddress() == 0 ||
               m_resultBuffer->GetGPUVirtualAddress() == 0) {
        return false;
    }

    ID3D12Resource* scratchBuffer = capacityChanged ? newScratch.Get() : m_scratchBuffer.Get();
    ID3D12Resource* resultBuffer = capacityChanged ? newResult.Get() : m_resultBuffer.Get();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    const bool canUpdate = m_built && !capacityChanged && m_builtInstanceCount == numInstances;
    if (canUpdate) {
        buildDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        buildDesc.SourceAccelerationStructureData = resultBuffer->GetGPUVirtualAddress();
    }
    buildDesc.DestAccelerationStructureData = resultBuffer->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = scratchBuffer->GetGPUVirtualAddress();
    commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    device.uavBarrier(resultBuffer);

    if (capacityChanged) {
        m_instanceDescBuffers = std::move(replacementDescBuffers);
        m_scratchBuffer = std::move(newScratch);
        m_resultBuffer = std::move(newResult);
        m_maxInstances = numInstances;
        m_built = false;
    }
    m_builtInstanceCount = numInstances;
    m_built = true;
    return true;
}

} // namespace Engine
