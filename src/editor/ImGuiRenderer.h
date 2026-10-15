#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include "../rhi/D3D12Device.h"

namespace Engine {

using Microsoft::WRL::ComPtr;

class ImGuiRenderer {
public:
    ImGuiRenderer() = default;
    ~ImGuiRenderer();

    bool init(D3D12Device& device, HWND hwnd);
    void shutdown();

    void newFrame();
    void render(D3D12Device& device);

    void copyBackBufferToReadback(D3D12Device& device, uint32_t width, uint32_t height);
    bool saveBackBufferToBmp(const std::string& filename, uint32_t width, uint32_t height, std::string* failureReason = nullptr);

private:
    bool createDeviceObjects(D3D12Device& device);
    bool createFontTexture(D3D12Device& device);

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState>  m_pipelineState;
    ComPtr<ID3D12Resource>      m_fontTexture;
    uint32_t                    m_fontSrvIndex = 0;

    ComPtr<ID3D12Resource>      m_vertexBuffer[D3D12Device::FRAME_COUNT];
    ComPtr<ID3D12Resource>      m_indexBuffer[D3D12Device::FRAME_COUNT];
    uint32_t                    m_vertexBufferSize[D3D12Device::FRAME_COUNT] = {};
    uint32_t                    m_indexBufferSize[D3D12Device::FRAME_COUNT] = {};

    HWND                        m_hwnd = nullptr;
    bool                        m_initialized = false;

    ComPtr<ID3D12Resource>      m_uiReadbackBuffer;
    uint32_t                    m_uiReadbackPitch = 0;
};

} // namespace Engine
