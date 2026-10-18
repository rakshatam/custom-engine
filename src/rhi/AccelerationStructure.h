#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <array>
#include "D3D12Device.h"

namespace Engine {

using Microsoft::WRL::ComPtr;

// The BLAS wrapper/resource lifecycle is locally implemented and informed by
// O3DE Atom DX12 RHI RayTracingBlas.cpp (Apache-2.0 OR MIT; see README credits).
class Blas {
public:
    Blas() = default;
    ~Blas() = default;

    bool build(D3D12Device& device, 
               ID3D12Resource* vertexBuffer, uint64_t vertexByteOffset, uint32_t vertexCount, uint32_t vertexStride,
               ID3D12Resource* indexBuffer, uint64_t indexByteOffset, uint32_t indexCount,
               bool isOpaque = true);

    D3D12_GPU_VIRTUAL_ADDRESS getGpuAddress() const {
        return m_resultBuffer ? m_resultBuffer->GetGPUVirtualAddress() : 0;
    }

private:
    ComPtr<ID3D12Resource> m_scratchBuffer;
    ComPtr<ID3D12Resource> m_resultBuffer;
};

// The TLAS instance/build lifecycle is locally implemented and informed by
// O3DE Atom DX12 RHI RayTracingTlas.cpp (Apache-2.0 OR MIT; see README credits).
class Tlas {
public:
    struct Instance {
        uint64_t blasGpuAddress = 0;
        float transform[3][4] = {};
        uint32_t instanceId = 0;
        uint32_t hitGroupIndex = 0;
    };

    Tlas() = default;
    ~Tlas() = default;

    bool build(D3D12Device& device, const std::vector<Instance>& instances);

    D3D12_GPU_VIRTUAL_ADDRESS getGpuAddress() const {
        return m_resultBuffer ? m_resultBuffer->GetGPUVirtualAddress() : 0;
    }

private:
    ComPtr<ID3D12Resource> m_scratchBuffer;
    ComPtr<ID3D12Resource> m_resultBuffer;
    std::array<ComPtr<ID3D12Resource>, D3D12Device::FRAME_COUNT> m_instanceDescBuffers;
    uint32_t m_maxInstances = 0;
    uint32_t m_builtInstanceCount = 0;
    bool m_built = false;
};

} // namespace Engine
