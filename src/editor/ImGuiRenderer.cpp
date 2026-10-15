#include "ImGuiRenderer.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "../../shaders/ImGuiVS_Bytecode.h"
#include "../../shaders/ImGuiPS_Bytecode.h"
#include <cstring>
#include <vector>
#include <iostream>
#include "../core/ImageCapture.h"

namespace Engine {

struct ImGuiCB {
    float mvp[4][4];
};

ImGuiRenderer::~ImGuiRenderer() {
    shutdown();
}

void ImGuiRenderer::shutdown() {
    if (!m_initialized) return;
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

bool ImGuiRenderer::init(D3D12Device& device, HWND hwnd) {
    m_hwnd = hwnd;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Apply dark theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]           = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_Header]             = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_HeaderHovered]      = ImVec4(0.28f, 0.32f, 0.38f, 1.00f);
    colors[ImGuiCol_HeaderActive]       = ImVec4(0.35f, 0.40f, 0.48f, 1.00f);
    colors[ImGuiCol_Button]             = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonHovered]      = ImVec4(0.30f, 0.34f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonActive]       = ImVec4(0.38f, 0.44f, 0.52f, 1.00f);
    colors[ImGuiCol_FrameBg]            = ImVec4(0.18f, 0.19f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.25f, 0.28f, 0.32f, 1.00f);
    colors[ImGuiCol_FrameBgActive]      = ImVec4(0.32f, 0.36f, 0.42f, 1.00f);
    colors[ImGuiCol_TitleBg]            = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgActive]      = ImVec4(0.15f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_CheckMark]          = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]         = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.45f, 0.75f, 1.00f, 1.00f);

    if (!ImGui_ImplWin32_Init(m_hwnd)) {
        return false;
    }

    if (!createDeviceObjects(device)) {
        return false;
    }

    uint32_t maxW = 1920, maxH = 1080;
    m_uiReadbackPitch = (maxW * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    m_uiReadbackBuffer = device.createBuffer(
        static_cast<uint64_t>(m_uiReadbackPitch) * maxH,
        D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST
    );

    m_initialized = true;
    return true;
}

bool ImGuiRenderer::createDeviceObjects(D3D12Device& device) {
    // Root Signature:
    // 0: 16 DWORD constants for 4x4 matrix
    // 1: Descriptor Table (1 SRV for texture)
    // Static sampler for bilinear texture filtering

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 16;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 2;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &sampler;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, errBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errBlob))) {
        return false;
    }
    if (FAILED(device.getDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)))) {
        return false;
    }

    // Input layout
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (UINT)IM_OFFSETOF(ImDrawVert, pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (UINT)IM_OFFSETOF(ImDrawVert, uv),  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)IM_OFFSETOF(ImDrawVert, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.InputLayout = { inputLayout, 3 };
    psoDesc.VS = { g_ImGuiVS_Bytecode, sizeof(g_ImGuiVS_Bytecode) };
    psoDesc.PS = { g_ImGuiPS_Bytecode, sizeof(g_ImGuiPS_Bytecode) };

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // Alpha blend
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(device.getDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)))) {
        return false;
    }

    // Preallocate vertex and index buffers for each frame in flight (up to 65536 vertices)
    for (uint32_t i = 0; i < D3D12Device::FRAME_COUNT; ++i) {
        m_vertexBufferSize[i] = 65536 * sizeof(ImDrawVert);
        m_vertexBuffer[i] = device.createBuffer(m_vertexBufferSize[i], D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        m_indexBufferSize[i] = 131072 * sizeof(ImDrawIdx);
        m_indexBuffer[i] = device.createBuffer(m_indexBufferSize[i], D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    }

    return createFontTexture(device);
}

bool ImGuiRenderer::createFontTexture(D3D12Device& device) {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    uint64_t uploadSize = width * height * 4;

    m_fontTexture = device.createTexture2D(
        width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST
    );

    auto uploadBuffer = device.createBuffer(
        uploadSize,
        D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ
    );

    void* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, &mapped);
    std::memcpy(mapped, pixels, uploadSize);
    uploadBuffer->Unmap(0, nullptr);

    device.beginFrame();
    auto cmdList = device.getCommandList();

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_fontTexture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
    footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    footprint.Width = width;
    footprint.Height = height;
    footprint.Depth = 1;
    footprint.RowPitch = width * 4;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = footprint;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    device.transitionBarrier(m_fontTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    device.endFrame();
    device.waitForGpu();

    m_fontSrvIndex = device.allocateSrvUavDescriptor();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device.getDevice()->CreateShaderResourceView(m_fontTexture.Get(), &srvDesc, device.getSrvUavCpuHandle(m_fontSrvIndex));

    io.Fonts->SetTexID(static_cast<ImTextureID>(device.getSrvUavGpuHandle(m_fontSrvIndex).ptr));
    return true;
}

void ImGuiRenderer::newFrame() {
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiRenderer::render(D3D12Device& device) {
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();

    if (!drawData || drawData->TotalVtxCount == 0) return;

    uint32_t frameIndex = device.getFrameIndex();

    uint32_t vertexBytes = drawData->TotalVtxCount * sizeof(ImDrawVert);
    uint32_t indexBytes  = drawData->TotalIdxCount * sizeof(ImDrawIdx);

    if (vertexBytes > m_vertexBufferSize[frameIndex] || indexBytes > m_indexBufferSize[frameIndex]) {
        return; // Exceeds preallocated buffer size
    }

    // Upload vertices and indices
    ImDrawVert* vtxDst = nullptr;
    ImDrawIdx*  idxDst = nullptr;
    if (FAILED(m_vertexBuffer[frameIndex]->Map(0, nullptr, reinterpret_cast<void**>(&vtxDst)))) {
        return;
    }
    if (FAILED(m_indexBuffer[frameIndex]->Map(0, nullptr, reinterpret_cast<void**>(&idxDst)))) {
        m_vertexBuffer[frameIndex]->Unmap(0, nullptr);
        return;
    }

    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* cmdList = drawData->CmdLists[n];
        std::memcpy(vtxDst, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
        std::memcpy(idxDst, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));
        vtxDst += cmdList->VtxBuffer.Size;
        idxDst += cmdList->IdxBuffer.Size;
    }

    m_vertexBuffer[frameIndex]->Unmap(0, nullptr);
    m_indexBuffer[frameIndex]->Unmap(0, nullptr);

    // Setup projection matrix
    float L = drawData->DisplayPos.x;
    float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
    float T = drawData->DisplayPos.y;
    float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
    float mvp[4][4] = {
        { 2.0f/(R-L),    0.0f,          0.0f, 0.0f },
        { 0.0f,          2.0f/(T-B),    0.0f, 0.0f },
        { 0.0f,          0.0f,          0.5f, 0.0f },
        { (R+L)/(L-R),  (T+B)/(B-T),    0.5f, 1.0f },
    };

    auto cmdList = device.getCommandList();
    auto backBuffer = device.getCurrentBackBuffer();

    device.transitionBarrier(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = device.getCurrentBackBufferRtv();
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList->SetPipelineState(m_pipelineState.Get());
    cmdList->SetGraphicsRoot32BitConstants(0, 16, mvp, 0);

    ID3D12DescriptorHeap* heaps[] = { device.getSrvUavHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    D3D12_VIEWPORT vp = { 0, 0, drawData->DisplaySize.x, drawData->DisplaySize.y, 0.0f, 1.0f };
    cmdList->RSSetViewports(1, &vp);

    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    vbView.BufferLocation = m_vertexBuffer[frameIndex]->GetGPUVirtualAddress();
    vbView.StrideInBytes = sizeof(ImDrawVert);
    vbView.SizeInBytes = vertexBytes;
    cmdList->IASetVertexBuffers(0, 1, &vbView);

    D3D12_INDEX_BUFFER_VIEW ibView = {};
    ibView.BufferLocation = m_indexBuffer[frameIndex]->GetGPUVirtualAddress();
    ibView.Format = sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    ibView.SizeInBytes = indexBytes;
    cmdList->IASetIndexBuffer(&ibView);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    int globalVtxOffset = 0;
    int globalIdxOffset = 0;
    ImVec2 clipOff = drawData->DisplayPos;

    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* list = drawData->CmdLists[n];
        for (int cmd_i = 0; cmd_i < list->CmdBuffer.Size; ++cmd_i) {
            const ImDrawCmd* pcmd = &list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback) {
                pcmd->UserCallback(list, pcmd);
            } else {
                D3D12_RECT r = {
                    (LONG)(pcmd->ClipRect.x - clipOff.x),
                    (LONG)(pcmd->ClipRect.y - clipOff.y),
                    (LONG)(pcmd->ClipRect.z - clipOff.x),
                    (LONG)(pcmd->ClipRect.w - clipOff.y)
                };
                if (r.right > r.left && r.bottom > r.top) {
                    cmdList->RSSetScissorRects(1, &r);
                    D3D12_GPU_DESCRIPTOR_HANDLE textureHandle;
                    textureHandle.ptr = pcmd->TextureId ? (UINT64)pcmd->TextureId : device.getSrvUavGpuHandle(m_fontSrvIndex).ptr;
                    cmdList->SetGraphicsRootDescriptorTable(1, textureHandle);
                    cmdList->DrawIndexedInstanced(pcmd->ElemCount, 1, pcmd->IdxOffset + globalIdxOffset, pcmd->VtxOffset + globalVtxOffset, 0);
                }
            }
        }
        globalIdxOffset += list->IdxBuffer.Size;
        globalVtxOffset += list->VtxBuffer.Size;
    }

    device.transitionBarrier(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
}

void ImGuiRenderer::copyBackBufferToReadback(D3D12Device& device, uint32_t width, uint32_t height) {
    if (!m_uiReadbackBuffer) return;

    auto backBuffer = device.getCurrentBackBuffer();
    device.transitionBarrier(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_uiReadbackBuffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = m_uiReadbackPitch;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = backBuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    device.getCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    device.transitionBarrier(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
}

bool ImGuiRenderer::saveBackBufferToBmp(const std::string& filename, uint32_t width, uint32_t height, std::string* failureReason) {
    if (!m_uiReadbackBuffer) {
        if (failureReason) *failureReason = "editor backbuffer readback buffer was not allocated";
        std::cout << "[saveBackBufferToBmp] m_uiReadbackBuffer is null!" << std::endl;
        return false;
    }
    uint8_t* mappedData = nullptr;
    HRESULT hr = m_uiReadbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedData));
    if (FAILED(hr)) {
        if (failureReason) {
            char detail[96];
            snprintf(detail, sizeof(detail), "editor readback Map failed (hr=0x%08X)", static_cast<unsigned int>(hr));
            *failureReason = detail;
        }
        std::cout << "[saveBackBufferToBmp] Map failed: hr=0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    bool ok = ImageCapture::saveBmp(filename, width, height, mappedData, m_uiReadbackPitch);
    m_uiReadbackBuffer->Unmap(0, nullptr);
    if (!ok) {
        if (failureReason) *failureReason = "editor BMP output could not be opened or written";
        std::cout << "[saveBackBufferToBmp] saveBmp returned false for " << filename << std::endl;
    }
    return ok;
}

} // namespace Engine
