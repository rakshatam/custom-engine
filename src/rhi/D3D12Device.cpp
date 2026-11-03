#include "D3D12Device.h"
#include <stdexcept>
#include <iostream>
#include <limits>

namespace Engine {

D3D12Device::~D3D12Device() {
    shutdown();
}

void D3D12Device::shutdown() {
    if (m_shutdown) return;
    if (!waitForGpu()) {
        std::cerr << "[D3D12 Error] GPU drain failed during shutdown; releasing device resources anyway.\n";
    }
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_shutdown = true;
}

bool D3D12Device::init(HWND hwnd, uint32_t width, uint32_t height) {
    m_shutdown = false;
    m_frameRecording = false;
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
    for (UINT i = 0;; ++i) {
        selectedAdapter.Reset();
        const HRESULT enumerateResult = m_dxgiFactory->EnumAdapters1(i, &selectedAdapter);
        if (enumerateResult == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enumerateResult)) {
            std::cerr << "[DXGI Error] Adapter enumeration failed: HRESULT 0x"
                      << std::hex << enumerateResult << std::dec << ".\n";
            return false;
        }
        DXGI_ADAPTER_DESC1 desc = {};
        const HRESULT descriptionResult = selectedAdapter->GetDesc1(&desc);
        if (FAILED(descriptionResult)) {
            std::cerr << "[DXGI Warning] Could not read adapter " << i << " description: HRESULT 0x"
                      << std::hex << descriptionResult << std::dec << ".\n";
            continue;
        }
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        // Try creating device
        ComPtr<ID3D12Device5> testDevice;
        if (SUCCEEDED(D3D12CreateDevice(selectedAdapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&testDevice)))) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
            if (SUCCEEDED(testDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
                if (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0) {
                    m_device = testDevice;
                    m_raytracingTier = options5.RaytracingTier;
                    const int utf8Bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                        desc.Description, -1, nullptr, 0, nullptr, nullptr);
                    if (utf8Bytes > 1) {
                        std::string gpuName(static_cast<size_t>(utf8Bytes), '\0');
                        const int convertedBytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                            desc.Description, -1, gpuName.data(), utf8Bytes, nullptr, nullptr);
                        if (convertedBytes > 1) {
                            gpuName.resize(static_cast<size_t>(convertedBytes - 1));
                            m_gpuName = std::move(gpuName);
                        }
                    }
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
    const HRESULT swapChainConvertResult = swapChain1.As(&m_swapChain);
    if (FAILED(swapChainConvertResult) || !m_swapChain) {
        std::cerr << "[DXGI Error] Could not obtain IDXGISwapChain3: HRESULT 0x"
                  << std::hex << swapChainConvertResult << std::dec << ".\n";
        return false;
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (m_frameIndex >= FRAME_COUNT) {
        std::cerr << "[DXGI Error] Initial back-buffer index is out of range: " << m_frameIndex << ".\n";
        return false;
    }

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
    const HRESULT initialCloseResult = m_commandList->Close();
    if (FAILED(initialCloseResult)) {
        std::cerr << "[D3D12 Error] Could not close the newly created command list: HRESULT 0x"
                  << std::hex << initialCloseResult << std::dec << ".\n";
        return false;
    }

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

bool D3D12Device::waitForGpu() {
    // An uninitialized or already-shutdown object has nothing to drain.
    if (!m_commandQueue && !m_fence) return true;
    if (!m_commandQueue || !m_fence || !m_fenceEvent) {
        std::cerr << "[D3D12 Error] Cannot wait for GPU: queue, fence, or fence event is unavailable.\n";
        return false;
    }
    if (m_currentFence == std::numeric_limits<uint64_t>::max()) {
        std::cerr << "[D3D12 Error] Fence value exhausted; refusing to wrap the GPU synchronization counter.\n";
        return false;
    }
    m_currentFence++;
    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), m_currentFence);
    if (FAILED(hr)) {
        std::cerr << "[D3D12 Error] Queue Signal failed while draining GPU work: HRESULT 0x"
                  << std::hex << hr << std::dec << ".\n";
        return false;
    }
    hr = m_fence->SetEventOnCompletion(m_currentFence, m_fenceEvent);
    if (FAILED(hr)) {
        std::cerr << "[D3D12 Error] SetEventOnCompletion failed while draining GPU work: HRESULT 0x"
                  << std::hex << hr << std::dec << ".\n";
        return false;
    }
    const DWORD waitResult = WaitForSingleObject(m_fenceEvent, INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        std::cerr << "[D3D12 Error] GPU fence wait failed; Win32 result=" << waitResult
                  << ", GetLastError=" << GetLastError() << ".\n";
        return false;
    }
    return true;
}

bool D3D12Device::moveToNextFrame() {
    if (!m_commandQueue || !m_fence || !m_swapChain || !m_fenceEvent || m_frameIndex >= FRAME_COUNT ||
        m_currentFence == std::numeric_limits<uint64_t>::max()) {
        std::cerr << "[D3D12 Error] Cannot advance frame: synchronization or swap-chain state is invalid.\n";
        return false;
    }
    m_currentFence++;
    const HRESULT signalResult = m_commandQueue->Signal(m_fence.Get(), m_currentFence);
    if (FAILED(signalResult)) {
        std::cerr << "[D3D12 Error] Queue Signal failed while advancing frame: HRESULT 0x"
                  << std::hex << signalResult << std::dec << ".\n";
        return false;
    }
    m_fenceValues[m_frameIndex] = m_currentFence;

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (m_frameIndex >= FRAME_COUNT) {
        std::cerr << "[D3D12 Error] Swap chain returned an invalid back-buffer index " << m_frameIndex << ".\n";
        return false;
    }

    const uint64_t completedValue = m_fence->GetCompletedValue();
    if (completedValue == std::numeric_limits<uint64_t>::max()) {
        std::cerr << "[D3D12 Error] Fence reports device removal while advancing frame.\n";
        return false;
    }
    if (completedValue < m_fenceValues[m_frameIndex]) {
        const HRESULT eventResult = m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        if (FAILED(eventResult)) {
            std::cerr << "[D3D12 Error] SetEventOnCompletion failed while advancing frame: HRESULT 0x"
                      << std::hex << eventResult << std::dec << ".\n";
            return false;
        }
        const DWORD waitResult = WaitForSingleObject(m_fenceEvent, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            std::cerr << "[D3D12 Error] Frame fence wait failed; Win32 result=" << waitResult
                      << ", GetLastError=" << GetLastError() << ".\n";
            return false;
        }
    }
    return true;
}

bool D3D12Device::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return true;
    if (width == m_width && height == m_height) return true;
    if (!m_swapChain || !m_device || !m_rtvHeap) return false;

    if (!waitForGpu()) return false;

    const uint32_t oldWidth = m_width;
    const uint32_t oldHeight = m_height;
    const auto refreshBackBuffers = [&]() {
        for (auto& target : m_renderTargets) target.Reset();
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
            const HRESULT getBufferResult = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
            if (FAILED(getBufferResult) || !m_renderTargets[i]) {
                std::cerr << "[DXGI Error] GetBuffer failed while rebuilding swap-chain target " << i
                          << ": HRESULT 0x" << std::hex << getBufferResult << std::dec << ".\n";
                for (auto& target : m_renderTargets) target.Reset();
                return false;
            }
            m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
            rtvHandle.ptr += m_rtvDescriptorSize;
        }
        return true;
    };

    for (auto& target : m_renderTargets) target.Reset();
    const HRESULT resizeResult = m_swapChain->ResizeBuffers(
        FRAME_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, m_swapChainFlags);
    if (FAILED(resizeResult)) {
        std::cerr << "[DXGI Warning] ResizeBuffers failed (HRESULT 0x" << std::hex << resizeResult << std::dec
                  << "); retaining " << oldWidth << "x" << oldHeight << " swap-chain buffers.\n";
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        return refreshBackBuffers();
    }

    m_width = width;
    m_height = height;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (refreshBackBuffers()) return true;

    std::cerr << "[DXGI Error] Could not rebuild resized back buffers; attempting to restore the previous extent.\n";
    for (auto& target : m_renderTargets) target.Reset();
    const HRESULT rollbackResult = m_swapChain->ResizeBuffers(
        FRAME_COUNT, oldWidth, oldHeight, DXGI_FORMAT_R8G8B8A8_UNORM, m_swapChainFlags);
    if (FAILED(rollbackResult)) {
        std::cerr << "[DXGI Error] Swap-chain resize rollback failed: HRESULT 0x"
                  << std::hex << rollbackResult << std::dec << ".\n";
        return false;
    }
    m_width = oldWidth;
    m_height = oldHeight;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    return refreshBackBuffers();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::getCurrentBackBufferRtv() const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_frameIndex * m_rtvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::getSrvUavCpuHandle(uint32_t index) const {
    if (!m_srvUavHeap || index >= m_allocatedSrvUavCount || index >= SRV_UAV_DESCRIPTOR_CAPACITY) return {};
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += index * m_srvUavDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::getSrvUavGpuHandle(uint32_t index) const {
    if (!m_srvUavHeap || index >= m_allocatedSrvUavCount || index >= SRV_UAV_DESCRIPTOR_CAPACITY) return {};
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += index * m_srvUavDescriptorSize;
    return handle;
}

uint32_t D3D12Device::allocateSrvUavDescriptor() {
    if (!m_srvUavHeap || m_allocatedSrvUavCount >= SRV_UAV_DESCRIPTOR_CAPACITY) {
        std::cerr << "[D3D12 Error] Shader-visible descriptor heap is unavailable or exhausted.\n";
        return std::numeric_limits<uint32_t>::max();
    }
    return m_allocatedSrvUavCount++;
}

ComPtr<ID3D12Resource> D3D12Device::createBuffer(uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState, D3D12_RESOURCE_FLAGS flags) {
    if (!m_device || size == 0) {
        std::cerr << "[D3D12 Error] Refused to create a buffer without a device or with zero bytes.\n";
        return {};
    }
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
    if (!m_device || width == 0 || height == 0) {
        std::cerr << "[D3D12 Error] Refused to create a texture without a device or with zero dimensions.\n";
        return {};
    }
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
    const HRESULT hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        std::cerr << "[D3D12 Error] Create texture failed: HRESULT 0x" << std::hex << hr << std::dec
                  << " dimensions=" << width << "x" << height << " format=" << format << ".\n";
    }
    return resource;
}

void D3D12Device::transitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    if (!m_commandList || !resource) {
        std::cerr << "[D3D12 Error] Skipped resource transition with a null command list/resource.\n";
        return;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);
}

void D3D12Device::uavBarrier(ID3D12Resource* resource) {
    if (!m_commandList || !resource) {
        std::cerr << "[D3D12 Error] Skipped UAV barrier with a null command list/resource.\n";
        return;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    m_commandList->ResourceBarrier(1, &barrier);
}

bool D3D12Device::beginFrame() {
    if (m_frameRecording || m_frameIndex >= FRAME_COUNT || !m_commandAllocators[m_frameIndex] ||
        !m_commandList || !m_srvUavHeap) {
        std::cerr << "[D3D12 Error] Cannot begin frame: command state is invalid or a frame is already recording.\n";
        return false;
    }

    HRESULT hr = m_commandAllocators[m_frameIndex]->Reset();
    if (FAILED(hr)) {
        std::cerr << "[D3D12 Error] Command allocator Reset failed: HRESULT 0x"
                  << std::hex << hr << std::dec << ".\n";
        return false;
    }
    hr = m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);
    if (FAILED(hr)) {
        std::cerr << "[D3D12 Error] Command list Reset failed: HRESULT 0x"
                  << std::hex << hr << std::dec << ".\n";
        return false;
    }
    m_frameRecording = true;

    ID3D12DescriptorHeap* heaps[] = { m_srvUavHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);
    return true;
}

bool D3D12Device::endFrame(bool vsyncEnabled) {
    if (!m_frameRecording || !m_commandList || !m_commandQueue || !m_swapChain) {
        std::cerr << "[D3D12 Error] Cannot end frame: there is no active frame or a required object is unavailable.\n";
        return false;
    }
    const HRESULT closeResult = m_commandList->Close();
    m_frameRecording = false;
    if (FAILED(closeResult)) {
        std::cerr << "[D3D12 Error] Command list Close failed: HRESULT 0x"
                  << std::hex << closeResult << std::dec << ".\n";
        return false;
    }
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    const UINT presentFlags = (!vsyncEnabled && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT presentResult = m_swapChain->Present(vsyncEnabled ? 1 : 0, presentFlags);
    if (FAILED(presentResult)) {
        std::cerr << "[DXGI Error] Swap-chain Present failed: HRESULT 0x"
                  << std::hex << presentResult << std::dec << ".\n";
    }
    const bool frameAdvanced = moveToNextFrame();
    return SUCCEEDED(presentResult) && frameAdvanced;
}

} // namespace Engine
