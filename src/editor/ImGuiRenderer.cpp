#include "ImGuiRenderer.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "../../shaders/ImGuiVS_Bytecode.h"
#include "../../shaders/ImGuiPS_Bytecode.h"
#include "ImGuiProjectionMath.h"
#include <cstring>
#include <vector>
#include <iostream>
#include <limits>
#include <cmath>
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

    if (!pixels || width <= 0 || height <= 0) {
        std::cerr << "[ImGui Error] Font atlas returned invalid RGBA pixels or dimensions.\n";
        return false;
    }
    const uint64_t sourceRowBytes = static_cast<uint64_t>(width) * 4u;
    constexpr uint64_t rowPitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    if (sourceRowBytes > std::numeric_limits<uint64_t>::max() - (rowPitchAlignment - 1u)) {
        std::cerr << "[ImGui Error] Font atlas row pitch overflows the upload-size type.\n";
        return false;
    }
    const uint64_t uploadRowPitch = (sourceRowBytes + rowPitchAlignment - 1u) & ~(rowPitchAlignment - 1u);
    if (uploadRowPitch > std::numeric_limits<uint32_t>::max() ||
        uploadRowPitch == 0 || static_cast<uint64_t>(height) > std::numeric_limits<uint64_t>::max() / uploadRowPitch) {
        std::cerr << "[ImGui Error] Font atlas padded upload size is not representable.\n";
        return false;
    }
    const uint64_t uploadSize = uploadRowPitch * static_cast<uint64_t>(height);

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
    if (!m_fontTexture || !uploadBuffer || FAILED(uploadBuffer->Map(0, nullptr, &mapped)) || !mapped) {
        std::cerr << "[ImGui Error] Could not allocate/map the font texture upload resources.\n";
        return false;
    }
    std::memset(mapped, 0, static_cast<size_t>(uploadSize));
    for (int row = 0; row < height; ++row) {
        std::memcpy(static_cast<uint8_t*>(mapped) + static_cast<size_t>(row) * static_cast<size_t>(uploadRowPitch),
                    pixels + static_cast<size_t>(row) * static_cast<size_t>(sourceRowBytes),
                    static_cast<size_t>(sourceRowBytes));
    }
    uploadBuffer->Unmap(0, nullptr);

    if (!device.beginFrame()) return false;
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
    footprint.RowPitch = static_cast<UINT>(uploadRowPitch);

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = footprint;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    device.transitionBarrier(m_fontTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (!device.endFrame() || !device.waitForGpu()) {
        std::cerr << "[ImGui Error] Font texture upload did not complete on the GPU.\n";
        return false;
    }

    m_fontSrvIndex = device.allocateSrvUavDescriptor();
    if (m_fontSrvIndex == 0xffffffffu) {
        std::cerr << "[ImGui Error] Could not allocate the font SRV descriptor.\n";
        return false;
    }
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

    if (!drawData || drawData->TotalVtxCount == 0 || drawData->TotalIdxCount == 0) return;
    if (!drawData->Valid || drawData->TotalVtxCount < 0 || drawData->TotalIdxCount < 0 ||
        drawData->CmdListsCount <= 0 || drawData->CmdLists.Size != drawData->CmdListsCount ||
        !device.isFrameRecording()) {
        std::cerr << "[ImGui Error] Refused invalid draw counts or command-list state.\n";
        return;
    }
    int64_t summedVertices = 0;
    int64_t summedIndices = 0;
    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
        const ImDrawList* list = drawData->CmdLists[listIndex];
        if (!list || list->VtxBuffer.Size < 0 || list->IdxBuffer.Size < 0 ||
            (list->VtxBuffer.Size > 0 && !list->VtxBuffer.Data) ||
            (list->IdxBuffer.Size > 0 && !list->IdxBuffer.Data)) {
            std::cerr << "[ImGui Error] Refused a null or malformed UI draw list.\n";
            return;
        }
        summedVertices += list->VtxBuffer.Size;
        summedIndices += list->IdxBuffer.Size;
    }
    if (summedVertices != drawData->TotalVtxCount || summedIndices != drawData->TotalIdxCount) {
        std::cerr << "[ImGui Error] UI draw-list totals do not match the declared geometry totals.\n";
        return;
    }

    const uint32_t frameIndex = device.getFrameIndex();
    if (frameIndex >= D3D12Device::FRAME_COUNT || !m_vertexBuffer[frameIndex] || !m_indexBuffer[frameIndex]) {
        std::cerr << "[ImGui Error] Current frame has no valid UI vertex/index upload buffers.\n";
        return;
    }

    const uint64_t vertexBytes64 = static_cast<uint64_t>(drawData->TotalVtxCount) * sizeof(ImDrawVert);
    const uint64_t indexBytes64 = static_cast<uint64_t>(drawData->TotalIdxCount) * sizeof(ImDrawIdx);

    if (vertexBytes64 > m_vertexBufferSize[frameIndex] || indexBytes64 > m_indexBufferSize[frameIndex] ||
        vertexBytes64 > std::numeric_limits<uint32_t>::max() || indexBytes64 > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "[ImGui Error] UI geometry exceeds the preallocated vertex/index buffer capacity.\n";
        return;
    }
    const uint32_t vertexBytes = static_cast<uint32_t>(vertexBytes64);
    const uint32_t indexBytes = static_cast<uint32_t>(indexBytes64);

    // Upload vertices and indices
    ImDrawVert* vtxDst = nullptr;
    ImDrawIdx*  idxDst = nullptr;
    const HRESULT vertexMapResult = m_vertexBuffer[frameIndex]->Map(0, nullptr, reinterpret_cast<void**>(&vtxDst));
    if (FAILED(vertexMapResult) || !vtxDst) {
        if (SUCCEEDED(vertexMapResult)) m_vertexBuffer[frameIndex]->Unmap(0, nullptr);
        std::cerr << "[ImGui Error] Could not map UI vertex buffer: HRESULT 0x"
                  << std::hex << vertexMapResult << std::dec << ".\n";
        return;
    }
    const HRESULT indexMapResult = m_indexBuffer[frameIndex]->Map(0, nullptr, reinterpret_cast<void**>(&idxDst));
    if (FAILED(indexMapResult) || !idxDst) {
        if (SUCCEEDED(indexMapResult)) m_indexBuffer[frameIndex]->Unmap(0, nullptr);
        m_vertexBuffer[frameIndex]->Unmap(0, nullptr);
        std::cerr << "[ImGui Error] Could not map UI index buffer: HRESULT 0x"
                  << std::hex << indexMapResult << std::dec << ".\n";
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

    // Setup projection matrix. Express the scale using DisplaySize directly so
    // a finite but very large DisplayPos cannot overflow the R-L / B-T sums.
    float mvp[4][4] = {};
    if (!makeUiProjection(drawData->DisplayPos.x, drawData->DisplayPos.y,
                          drawData->DisplaySize.x, drawData->DisplaySize.y, mvp)) {
        std::cerr << "[ImGui Error] Refused a non-finite, zero-sized, or unrepresentable UI projection.\n";
        return;
    }

    auto cmdList = device.getCommandList();
    auto backBuffer = device.getCurrentBackBuffer();
    if (!cmdList || !backBuffer || !m_rootSignature || !m_pipelineState) {
        std::cerr << "[ImGui Error] Cannot record the UI draw without a command list, back buffer, or pipeline state.\n";
        return;
    }

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
    constexpr uint32_t maximumCaptureWidth = 1920;
    constexpr uint32_t maximumCaptureHeight = 1080;
    m_uiReadbackValid = false;
    if (!m_uiReadbackBuffer || !device.isFrameRecording() || width == 0 || height == 0 ||
        width > maximumCaptureWidth || height > maximumCaptureHeight) {
        std::cerr << "[ImGui Error] Refused a back-buffer capture outside the allocated 1920x1080 readback extent.\n";
        return;
    }

    auto backBuffer = device.getCurrentBackBuffer();
    if (!backBuffer || !device.getCommandList()) {
        std::cerr << "[ImGui Error] Cannot capture a missing back buffer or command list.\n";
        return;
    }
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
    m_uiReadbackValid = true;
}

bool ImGuiRenderer::saveBackBufferToBmp(const std::string& filename, uint32_t width, uint32_t height, std::string* failureReason) {
    if (width == 0 || height == 0 || width > 1920 || height > 1080) {
        if (failureReason) *failureReason = "editor capture dimensions exceed the allocated 1920x1080 readback extent";
        return false;
    }
    if (!m_uiReadbackValid) {
        if (failureReason) *failureReason = "no valid back-buffer capture has been recorded for this frame";
        return false;
    }
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
    const D3D12_RANGE noCpuWrites = { 0, 0 };
    m_uiReadbackBuffer->Unmap(0, &noCpuWrites);
    if (!ok) {
        if (failureReason) *failureReason = "editor BMP output could not be opened or written";
        std::cout << "[saveBackBufferToBmp] saveBmp returned false for " << filename << std::endl;
    }
    return ok;
}

} // namespace Engine
