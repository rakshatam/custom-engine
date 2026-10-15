#include "D3D12Device.h"
#include <stdexcept>
#include <iostream>

namespace Engine {

D3D12Device::~D3D12Device() {
    shutdown();
}

void D3D12Device::shutdown() {
    waitForGpu();
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

bool D3D12Device::init(HWND hwnd, uint32_t width, uint32_t height) {
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    if (FAILED(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_dxgiFactory)))) {
        return false;
    }

    ComPtr<IDXGIFactory5> factory5;
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(m_dxgiFactory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing)))) {
        m_tearingSupported = allowTearing == TRUE;
    }
    m_swapChainFlags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    // Find adapter supporting DXR
    ComPtr<IDXGIAdapter1> selectedAdapter;
    for (UINT i = 0; m_dxgiFactory->EnumAdapters1(i, &selectedAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        selectedAdapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        // Try creating device
        ComPtr<ID3D12Device5> testDevice;
        if (SUCCEEDED(D3D12CreateDevice(selectedAdapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&testDevice)))) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
            if (SUCCEEDED(testDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
                if (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0) {
                    m_device = testDevice;
                    m_raytracingTier = options5.RaytracingTier;
                    char buf[128];
                    wcstombs(buf, desc.Description, sizeof(buf));
                    m_gpuName = buf;
                    break;
                }
            }
        }
    }

    if (!m_device) {
        return false;
    }

    // Command Queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)))) {
        return false;
    }

    // SwapChain
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = FRAME_COUNT;
    scDesc.Width = m_width;
    scDesc.Height = m_height;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;
    scDesc.Flags = m_swapChainFlags;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(m_dxgiFactory->CreateSwapChainForHwnd(m_commandQueue.Get(), m_hwnd, &scDesc, nullptr, nullptr, &swapChain1))) {
        return false;
    }
    swapChain1.As(&m_swapChain);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // RTV Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)))) {
        return false;
    }
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create RTV for back buffers
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])))) {
            return false;
        }
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    // SRV / UAV Descriptor Heap (Shader Visible)
    D3D12_DESCRIPTOR_HEAP_DESC srvUavHeapDesc = {};
    srvUavHeapDesc.NumDescriptors = 1024;
    srvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&srvUavHeapDesc, IID_PPV_ARGS(&m_srvUavHeap)))) {
        return false;
    }
    m_srvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_allocatedSrvUavCount = 0;

    // Command Allocators & List
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])))) {
            return false;
        }
    }

    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[m_frameIndex].Get(), nullptr, IID_PPV_ARGS(&m_commandList)))) {
        return false;
    }
    m_commandList->Close();

    // Fence & Event
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
        return false;
    }
    m_currentFence = 0;
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_fenceValues[i] = 0;
    }

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) return false;

    return true;
}

void D3D12Device::waitForGpu() {
    if (!m_commandQueue || !m_fence) return;
    m_currentFence++;
    m_commandQueue->Signal(m_fence.Get(), m_currentFence);
    m_fence->SetEventOnCompletion(m_currentFence, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);
}

void D3D12Device::moveToNextFrame() {
    m_currentFence++;
    m_commandQueue->Signal(m_fence.Get(), m_currentFence);
    m_fenceValues[m_frameIndex] = m_currentFence;

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void D3D12Device::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    waitForGpu();

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_renderTargets[i].Reset();
    }

    m_width = width;
    m_height = height;

    m_swapChain->ResizeBuffers(FRAME_COUNT, m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM, m_swapChainFlags);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::getCurrentBackBufferRtv() const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_frameIndex * m_rtvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::getSrvUavCpuHandle(uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += index * m_srvUavDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::getSrvUavGpuHandle(uint32_t index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += index * m_srvUavDescriptorSize;
    return handle;
}

uint32_t D3D12Device::allocateSrvUavDescriptor() {
    return m_allocatedSrvUavCount++;
}

ComPtr<ID3D12Resource> D3D12Device::createBuffer(uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = heapType;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        HRESULT reason = m_device->GetDeviceRemovedReason();
        char buf[256];
        snprintf(buf, sizeof(buf), "[D3D12 Error] CreateCommittedResource failed: hr=0x%08X (DeviceRemovedReason=0x%08X) size=%llu heapType=%u state=%u\n",
                 static_cast<unsigned int>(hr), static_cast<unsigned int>(reason), size, static_cast<unsigned int>(heapType), static_cast<unsigned int>(initialState));
        OutputDebugStringA(buf);
        std::cout << buf;
    }
    return resource;
}

ComPtr<ID3D12Resource> D3D12Device::createTexture2D(uint32_t width, uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;

    ComPtr<ID3D12Resource> resource;
    m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource));
    return resource;
}

void D3D12Device::transitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);
}

void D3D12Device::uavBarrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    m_commandList->ResourceBarrier(1, &barrier);
}

void D3D12Device::beginFrame() {
    m_commandAllocators[m_frameIndex]->Reset();
    m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_srvUavHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);
}

void D3D12Device::endFrame(bool vsyncEnabled) {
    m_commandList->Close();
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    const UINT presentFlags = (!vsyncEnabled && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    m_swapChain->Present(vsyncEnabled ? 1 : 0, presentFlags);
    moveToNextFrame();
}

} // namespace Engine
