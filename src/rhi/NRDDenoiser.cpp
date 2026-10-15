#include "NRDDenoiser.h"

#if ENGINE_ENABLE_NRD
#include "NRI.h"
#include "Extensions/NRIHelper.h"
#include "Extensions/NRIWrapperD3D12.h"
#include "NRD.h"
#include "NRDIntegration.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace Engine {

struct NRDDenoiser::State {
    nrd::Integration integration;
    nrd::Identifier denoiserId = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    bool ready = false;
    bool hasPreviousCamera = false;
    float previousViewToClip[16] = {};
    float previousWorldToView[16] = {};
};

namespace {
void copyColumnMajor(float destination[16], DirectX::FXMMATRIX matrix) {
    DirectX::XMFLOAT4X4 transposed;
    DirectX::XMStoreFloat4x4(&transposed, DirectX::XMMatrixTranspose(matrix));
    std::memcpy(destination, &transposed, sizeof(transposed));
}

void setExternalTexture(nrd::ResourceSnapshot& snapshot, nrd::ResourceType slot,
                        ID3D12Resource* texture, DXGI_FORMAT format,
                        nri::AccessBits access, nri::Layout layout) {
    nrd::Resource resource = {};
    resource.d3d12.resource = texture;
    resource.d3d12.format = static_cast<DXGIFormat>(format);
    resource.state.access = access;
    resource.state.layout = layout;
    resource.state.stages = nri::StageBits::COMPUTE_SHADER;
    snapshot.SetResource(slot, resource);
}
} // namespace

NRDDenoiser::NRDDenoiser() : m_state(std::make_unique<State>()) {}
NRDDenoiser::~NRDDenoiser() = default;

bool NRDDenoiser::initialize(D3D12Device& device, uint32_t width, uint32_t height) {
    if (!m_state || !device.getDevice() || !device.getCommandQueue() || width == 0 || height == 0 ||
        width > UINT16_MAX || height > UINT16_MAX) {
        return false;
    }

    ID3D12CommandQueue* queues[] = { device.getCommandQueue() };
    nri::QueueFamilyD3D12Desc queueFamily = {};
    queueFamily.d3d12Queues = queues;
    queueFamily.queueNum = 1;
    queueFamily.queueType = nri::QueueType::GRAPHICS;

    nri::DeviceCreationD3D12Desc deviceDesc = {};
    deviceDesc.d3d12Device = device.getDevice();
    deviceDesc.queueFamilies = &queueFamily;
    deviceDesc.queueFamilyNum = 1;
    deviceDesc.disableD3D12EnhancedBarriers = true;
    deviceDesc.disableNVAPIInitialization = true;

    const nrd::DenoiserDesc denoiserDesc = { m_state->denoiserId, nrd::Denoiser::REBLUR_DIFFUSE };
    nrd::InstanceCreationDesc instanceDesc = {};
    instanceDesc.denoisers = &denoiserDesc;
    instanceDesc.denoisersNum = 1;

    nrd::IntegrationCreationDesc integrationDesc = {};
    std::strncpy(integrationDesc.name, "Engine NRD", sizeof(integrationDesc.name) - 1);
    integrationDesc.resourceWidth = static_cast<uint16_t>(width);
    integrationDesc.resourceHeight = static_cast<uint16_t>(height);
    integrationDesc.queuedFrameNum = D3D12Device::FRAME_COUNT;
    integrationDesc.enableWholeLifetimeDescriptorCaching = false;
    integrationDesc.autoWaitForIdle = false;

    const nrd::Result result = m_state->integration.RecreateD3D12(integrationDesc, instanceDesc, deviceDesc);
    m_state->ready = result == nrd::Result::SUCCESS;
    m_state->hasPreviousCamera = false;
    m_state->width = width;
    m_state->height = height;
    if (!m_state->ready) {
        std::cerr << "[NRD] Reblur initialization failed with result " << static_cast<uint32_t>(result) << '\n';
    } else {
        std::cout << "[NRD] NVIDIA NRD " << static_cast<uint32_t>(nrd::GetLibraryDesc()->versionMajor)
                  << '.' << static_cast<uint32_t>(nrd::GetLibraryDesc()->versionMinor)
                  << " REBLUR_DIFFUSE ready at " << width << 'x' << height << '\n';
    }
    return m_state->ready;
}

bool NRDDenoiser::isReady() const {
    return m_state && m_state->ready;
}

bool NRDDenoiser::denoise(D3D12Device& device, const SceneCB& scene,
                          DirectX::FXMMATRIX viewToClip,
                          DirectX::FXMMATRIX worldToView,
                          ID3D12Resource* signal,
                          ID3D12Resource* normalRoughness,
                          ID3D12Resource* viewZ,
                          ID3D12Resource* motion,
                          ID3D12Resource* denoisedOutput,
                          bool outputIsShaderResource) {
    if (!isReady() || !signal || !normalRoughness || !viewZ || !motion || !denoisedOutput) return false;

    nrd::CommonSettings common = {};
    float currentViewToClip[16] = {};
    float currentWorldToView[16] = {};
    copyColumnMajor(currentViewToClip, viewToClip);
    copyColumnMajor(currentWorldToView, worldToView);
    std::memcpy(common.viewToClipMatrix, currentViewToClip, sizeof(currentViewToClip));
    std::memcpy(common.worldToViewMatrix, currentWorldToView, sizeof(currentWorldToView));
    const bool restartHistory = !m_state->hasPreviousCamera || scene.accumFrameCount <= 1;
    std::memcpy(common.viewToClipMatrixPrev,
                restartHistory ? currentViewToClip : m_state->previousViewToClip,
                sizeof(currentViewToClip));
    std::memcpy(common.worldToViewMatrixPrev,
                restartHistory ? currentWorldToView : m_state->previousWorldToView,
                sizeof(currentWorldToView));
    common.motionVectorScale[0] = common.motionVectorScale[1] = common.motionVectorScale[2] = 1.0f;
    const bool jitterEnabled = scene.enableTAA != 0;
    common.cameraJitter[0] = jitterEnabled ? scene.jitterOffset.x : 0.0f;
    common.cameraJitter[1] = jitterEnabled ? scene.jitterOffset.y : 0.0f;
    static constexpr float halton2[16] = { 0.0f, 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f,
                                           0.0625f, 0.5625f, 0.3125f, 0.8125f, 0.1875f, 0.6875f, 0.4375f, 0.9375f };
    static constexpr float halton3[16] = { 0.0f, 1.0f/3.0f, 2.0f/3.0f, 1.0f/9.0f, 4.0f/9.0f, 7.0f/9.0f,
                                           2.0f/9.0f, 5.0f/9.0f, 8.0f/9.0f, 1.0f/27.0f, 4.0f/27.0f,
                                           7.0f/27.0f, 10.0f/27.0f, 13.0f/27.0f, 16.0f/27.0f, 19.0f/27.0f };
    const uint32_t prevPhase = (scene.frameCount + 15u) % 16u;
    common.cameraJitterPrev[0] = jitterEnabled ? halton2[prevPhase] - 0.5f : 0.0f;
    common.cameraJitterPrev[1] = jitterEnabled ? halton3[prevPhase] - 0.5f : 0.0f;
    // This engine's static Sponza geometry has no object-space motion; the
    // zero-valued guide is therefore a world-space motion field.
    common.isMotionVectorInWorldSpace = true;
    common.resourceSize[0] = common.resourceSizePrev[0] = static_cast<uint16_t>(m_state->width);
    common.resourceSize[1] = common.resourceSizePrev[1] = static_cast<uint16_t>(m_state->height);
    common.rectSize[0] = common.rectSizePrev[0] = common.resourceSize[0];
    common.rectSize[1] = common.rectSizePrev[1] = common.resourceSize[1];
    common.viewZScale = 1.0f;
    common.denoisingRange = 100000.0f;
    common.disocclusionThreshold = 0.01f;
    common.frameIndex = scene.frameCount;
    common.accumulationMode = scene.accumFrameCount <= 1
        ? nrd::AccumulationMode::RESTART
        : nrd::AccumulationMode::CONTINUE;

    m_state->integration.NewFrame();
    if (m_state->integration.SetCommonSettings(common) != nrd::Result::SUCCESS) return false;

    nrd::ReblurSettings settings = {};
    settings.hitDistanceParameters.A = 3.0f;
    settings.hitDistanceParameters.B = 0.1f;
    settings.hitDistanceParameters.C = 20.0f;
    // Static Sponza is accumulated for more than 30 frames in the editor.
    // Retain a longer stable history to reduce one-sample path variance while
    // keeping a short fast history for camera reprojection/disocclusions.
    settings.maxAccumulatedFrameNum = 48;
    settings.maxFastAccumulatedFrameNum = 8;
    settings.enableAntiFirefly = true;
    if (m_state->integration.SetDenoiserSettings(m_state->denoiserId, &settings) != nrd::Result::SUCCESS) return false;

    nrd::ResourceSnapshot resources;
    resources.restoreInitialState = true;
    const nri::AccessBits rw = nri::AccessBits::SHADER_RESOURCE_STORAGE;
    const nri::Layout rwLayout = nri::Layout::SHADER_RESOURCE_STORAGE;
    setExternalTexture(resources, nrd::ResourceType::IN_MV, motion, DXGI_FORMAT_R32G32B32A32_FLOAT, rw, rwLayout);
    setExternalTexture(resources, nrd::ResourceType::IN_NORMAL_ROUGHNESS, normalRoughness, DXGI_FORMAT_R10G10B10A2_UNORM, rw, rwLayout);
    setExternalTexture(resources, nrd::ResourceType::IN_VIEWZ, viewZ, DXGI_FORMAT_R32_FLOAT, rw, rwLayout);
    setExternalTexture(resources, nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, signal, DXGI_FORMAT_R32G32B32A32_FLOAT, rw, rwLayout);
    const nri::AccessBits outputAccess = outputIsShaderResource ? nri::AccessBits::SHADER_RESOURCE : rw;
    const nri::Layout outputLayout = outputIsShaderResource ? nri::Layout::SHADER_RESOURCE : rwLayout;
    setExternalTexture(resources, nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, denoisedOutput,
                       DXGI_FORMAT_R32G32B32A32_FLOAT, outputAccess, outputLayout);

    nri::CommandBufferD3D12Desc commandBuffer = {};
    commandBuffer.d3d12CommandList = device.getCommandList();
    m_state->integration.DenoiseD3D12(&m_state->denoiserId, 1, commandBuffer, resources);
    std::memcpy(m_state->previousViewToClip, currentViewToClip, sizeof(currentViewToClip));
    std::memcpy(m_state->previousWorldToView, currentWorldToView, sizeof(currentWorldToView));
    m_state->hasPreviousCamera = true;
    return true;
}

} // namespace Engine

#else

namespace Engine {
struct NRDDenoiser::State {};
NRDDenoiser::NRDDenoiser() : m_state(std::make_unique<State>()) {}
NRDDenoiser::~NRDDenoiser() = default;
bool NRDDenoiser::initialize(D3D12Device&, uint32_t, uint32_t) { return false; }
bool NRDDenoiser::isReady() const { return false; }
bool NRDDenoiser::denoise(D3D12Device&, const SceneCB&, DirectX::FXMMATRIX, DirectX::FXMMATRIX,
                          ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, bool) { return false; }
} // namespace Engine

#endif
