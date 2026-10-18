#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

using Microsoft::WRL::ComPtr;

class D3D12Device {
public:
    static constexpr uint32_t FRAME_COUNT = 2;
    static constexpr uint32_t SRV_UAV_DESCRIPTOR_CAPACITY = 1024;

    D3D12Device() = default;
    ~D3D12Device();

    bool init(HWND hwnd, uint32_t width, uint32_t height);
    void shutdown();

    void waitForGpu();
    void moveToNextFrame();

    bool resize(uint32_t width, uint32_t height);

    ID3D12Device5* getDevice() const { return m_device.Get(); }
    ID3D12CommandQueue* getCommandQueue() const { return m_commandQueue.Get(); }
    ID3D12GraphicsCommandList4* getCommandList() const { return m_commandList.Get(); }
    ID3D12DescriptorHeap* getSrvUavHeap() const { return m_srvUavHeap.Get(); }
    ID3D12DescriptorHeap* getRtvHeap() const { return m_rtvHeap.Get(); }
    IDXGISwapChain3* getSwapChain() const { return m_swapChain.Get(); }
    uint32_t getFrameIndex() const { return m_frameIndex; }
    ID3D12Resource* getCurrentBackBuffer() const { return m_renderTargets[m_frameIndex].Get(); }

    D3D12_CPU_DESCRIPTOR_HANDLE getCurrentBackBufferRtv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE getSrvUavCpuHandle(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE getSrvUavGpuHandle(uint32_t index) const;

    uint32_t allocateSrvUavDescriptor();

    ComPtr<ID3D12Resource> createBuffer(uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> createTexture2D(uint32_t width, uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState);

    void transitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void uavBarrier(ID3D12Resource* resource);

    void beginFrame();
    void endFrame(bool vsyncEnabled = false);

    std::string getGpuName() const { return m_gpuName; }
    D3D12_RAYTRACING_TIER getRaytracingTier() const { return m_raytracingTier; }

private:
    ComPtr<IDXGIFactory4>               m_dxgiFactory;
    ComPtr<ID3D12Device5>               m_device;
    ComPtr<ID3D12CommandQueue>          m_commandQueue;
    ComPtr<IDXGISwapChain3>             m_swapChain;
    ComPtr<ID3D12DescriptorHeap>        m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap>        m_srvUavHeap;

    ComPtr<ID3D12CommandAllocator>      m_commandAllocators[FRAME_COUNT];
    ComPtr<ID3D12Resource>              m_renderTargets[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList4>  m_commandList;

    ComPtr<ID3D12Fence>                 m_fence;
    HANDLE                              m_fenceEvent = nullptr;
    uint64_t                            m_fenceValues[FRAME_COUNT] = {};
    uint64_t                            m_currentFence = 0;
    uint32_t                            m_frameIndex = 0;

    uint32_t                            m_rtvDescriptorSize = 0;
    uint32_t                            m_srvUavDescriptorSize = 0;
    uint32_t                            m_allocatedSrvUavCount = 0;

    uint32_t                            m_width = 0;
    uint32_t                            m_height = 0;
    uint32_t                            m_swapChainFlags = 0;
    bool                                m_tearingSupported = false;
    HWND                                m_hwnd = nullptr;
    std::string                         m_gpuName = "Unknown GPU";
    D3D12_RAYTRACING_TIER               m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
};

} // namespace Engine
