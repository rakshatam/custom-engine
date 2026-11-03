#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <memory>
#include <string>
#include "D3D12Device.h"
#include "ConstantBuffers.h"

#ifndef ENGINE_ENABLE_NRD
#define ENGINE_ENABLE_NRD 0
#endif
#ifndef ENGINE_ENABLE_DLSS
#define ENGINE_ENABLE_DLSS 0
#endif

#if ENGINE_ENABLE_DLSS
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;
#endif

namespace Engine {

using Microsoft::WRL::ComPtr;
class NRDDenoiser;

class RayTracingPipeline {
public:
    RayTracingPipeline();
    ~RayTracingPipeline();

    bool init(D3D12Device& device, uint32_t renderWidth, uint32_t renderHeight,
              uint32_t outputWidth = 0, uint32_t outputHeight = 0);
    bool resizeOutput(D3D12Device& device, uint32_t renderWidth, uint32_t renderHeight,
                      uint32_t outputWidth = 0, uint32_t outputHeight = 0);

    bool dispatch(D3D12Device& device,
                  D3D12_GPU_VIRTUAL_ADDRESS tlasAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS sceneCbAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS verticesAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS indicesAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS materialsAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS instancesAddress,
                  D3D12_GPU_DESCRIPTOR_HANDLE textureArrayHandle,
                  uint32_t environmentSrvIndex,
                  uint32_t width, uint32_t height,
                  const SceneCB& sceneData,
                  DirectX::FXMMATRIX viewToClip,
                  DirectX::FXMMATRIX worldToView);

    ID3D12Resource* getOutputTexture() const {
#if ENGINE_ENABLE_DLSS
        return m_dlssOutputIsActive ? m_dlssDisplayTexture.Get() : m_outputTexture.Get();
#else
        return m_outputTexture.Get();
#endif
    }
    uint32_t getOutputSrvIndex() const {
#if ENGINE_ENABLE_DLSS
        return m_dlssOutputIsActive ? m_dlssDisplaySrvIndex : m_outputSrvIndex;
#else
        return m_outputSrvIndex;
#endif
    }
    uint32_t getRenderWidth() const { return m_renderWidth; }
    uint32_t getRenderHeight() const { return m_renderHeight; }
    uint32_t getOutputWidth() const { return m_outputWidth; }
    uint32_t getOutputHeight() const { return m_outputHeight; }
    bool isNrdAvailable() const;
    bool isDlssAvailable() const;
    bool isDlssOutputActive() const {
#if ENGINE_ENABLE_DLSS
        return m_dlssOutputIsActive;
#else
        return false;
#endif
    }
    bool isRayReconstructionActive() const {
#if ENGINE_ENABLE_DLSS
        return m_dlssOutputIsActive && m_dlssFeatureIsRayReconstruction;
#else
        return false;
#endif
    }
    const std::string& getDlssStatus() const {
#if ENGINE_ENABLE_DLSS
        return m_dlssStatus;
#else
        static const std::string disabledStatus = "DLSS is disabled at build time";
        return disabledStatus;
#endif
    }
    void setDlssSettings(bool srEnabled, bool rrEnabled, float renderScale, uint32_t rrPreset);

    void copyOutputToReadback(D3D12Device& device);
    bool saveReadbackToBmp(D3D12Device& device, const std::string& filename, std::string* failureReason = nullptr);

private:
    bool createRootSignature(D3D12Device& device);
    bool createPipelineState(D3D12Device& device);
    bool createShaderBindingTable(D3D12Device& device);
    bool createOutputResources(D3D12Device& device, uint32_t width, uint32_t height,
                               uint32_t outputWidth, uint32_t outputHeight);
    bool createNrdCompositePipeline(D3D12Device& device);
    bool createDlssCompositePipeline(D3D12Device& device);
    void compositeDlss(D3D12Device& device, const SceneCB& sceneData);
    bool initializeDlss(D3D12Device& device);
    void shutdownDlss();
    void releaseDlssFeature();
    bool ensureDlssFeature(D3D12Device& device, ID3D12GraphicsCommandList* commandList, bool rayReconstruction);
    bool evaluateDlss(D3D12Device& device, const SceneCB& sceneData,
                      DirectX::FXMMATRIX viewToClip, DirectX::FXMMATRIX worldToView);
    void compositeNrd(D3D12Device& device, D3D12_GPU_VIRTUAL_ADDRESS sceneCbAddress,
                      const SceneCB& sceneData,
                      DirectX::FXMMATRIX viewToClip,
                      DirectX::FXMMATRIX worldToView);

    ComPtr<ID3D12RootSignature>  m_globalRootSignature;
    uint32_t                     m_defaultEnvironmentSrvIndex = UINT32_MAX;
    ComPtr<ID3D12StateObject>    m_stateObject;
    ComPtr<ID3D12StateObjectProperties> m_stateObjectProperties;

    ComPtr<ID3D12Resource>       m_shaderBindingTable;
    uint32_t                     m_rayGenRecordSize = 0;
    uint32_t                     m_missRecordSize = 0;
    uint32_t                     m_hitGroupRecordSize = 0;

    ComPtr<ID3D12Resource>       m_outputTexture;
    ComPtr<ID3D12Resource>       m_accumulationTexture;
    ComPtr<ID3D12Resource>       m_historyTexture;
#if ENGINE_ENABLE_DLSS
    ComPtr<ID3D12Resource>       m_dlssInputColorTexture;
    ComPtr<ID3D12Resource>       m_dlssDepthTexture;
    ComPtr<ID3D12Resource>       m_dlssMotionTexture;
    ComPtr<ID3D12Resource>       m_dlssNormalRoughnessTexture;
    ComPtr<ID3D12Resource>       m_dlssDiffuseAlbedoTexture;
    ComPtr<ID3D12Resource>       m_dlssSpecularAlbedoTexture;
    ComPtr<ID3D12Resource>       m_dlssSpecularHitDistanceTexture;
    ComPtr<ID3D12Resource>       m_dlssOutputHdrTexture;
    ComPtr<ID3D12Resource>       m_dlssDisplayTexture;
    ComPtr<ID3D12RootSignature>  m_dlssCompositeRootSignature;
    ComPtr<ID3D12PipelineState>  m_dlssCompositePipeline;
    NVSDK_NGX_Parameter*         m_ngxParameters = nullptr;
    NVSDK_NGX_Handle*            m_ngxFeature = nullptr;
    ID3D12Device*                m_ngxDevice = nullptr;
    uint32_t                     m_dlssInputColorUavIndex = 0;
    uint32_t                     m_dlssDepthUavIndex = 0;
    uint32_t                     m_dlssMotionUavIndex = 0;
    uint32_t                     m_dlssNormalRoughnessUavIndex = 0;
    uint32_t                     m_dlssDiffuseAlbedoUavIndex = 0;
    uint32_t                     m_dlssSpecularAlbedoUavIndex = 0;
    uint32_t                     m_dlssSpecularHitDistanceUavIndex = 0;
    uint32_t                     m_dlssOutputHdrSrvIndex = 0;
    uint32_t                     m_dlssDisplayUavIndex = 0;
    uint32_t                     m_dlssDisplaySrvIndex = 0;
    bool                         m_ngxInitialized = false;
    bool                         m_dlssApiAvailable = false;
    bool                         m_dlssOutputIsActive = false;
    bool                         m_dlssDisplayIsShaderResource = false;
    bool                         m_dlssFeatureIsRayReconstruction = false;
    bool                         m_dlssFeatureNeedsCreation = true;
    bool                         m_dlssFeatureFailed = false;
    bool                         m_dlssResetHistory = true;
    bool                         m_dlssSrRequested = false;
    bool                         m_dlssRrRequested = false;
    float                        m_dlssRenderScale = 0.667f;
    uint32_t                     m_dlssRrPreset = 4;
    std::string                  m_dlssStatus = "Not initialized";
#endif
#if ENGINE_ENABLE_NRD
    ComPtr<ID3D12Resource>       m_nrdSignalTexture;
    ComPtr<ID3D12Resource>       m_nrdSpecularSignalTexture;
    ComPtr<ID3D12Resource>       m_nrdNormalRoughnessTexture;
    ComPtr<ID3D12Resource>       m_nrdViewZTexture;
    ComPtr<ID3D12Resource>       m_nrdMotionTexture;
    ComPtr<ID3D12Resource>       m_nrdDiffuseFactorTexture;
    ComPtr<ID3D12Resource>       m_nrdSpecularFactorTexture;
    ComPtr<ID3D12Resource>       m_nrdDiffuseOutputTexture;
    ComPtr<ID3D12Resource>       m_nrdSpecularOutputTexture;
    ComPtr<ID3D12RootSignature>  m_nrdCompositeRootSignature;
    ComPtr<ID3D12PipelineState>  m_nrdCompositePipeline;
    std::unique_ptr<NRDDenoiser> m_nrd;
    uint32_t                     m_nrdSignalUavIndex = 0;
    uint32_t                     m_nrdSpecularSignalUavIndex = 0;
    uint32_t                     m_nrdNormalRoughnessUavIndex = 0;
    uint32_t                     m_nrdViewZUavIndex = 0;
    uint32_t                     m_nrdMotionUavIndex = 0;
    uint32_t                     m_nrdDiffuseFactorUavIndex = 0;
    uint32_t                     m_nrdSpecularFactorUavIndex = 0;
    uint32_t                     m_accumulationSrvIndex = 0;
    uint32_t                     m_nrdDiffuseOutputSrvIndex = 0;
    uint32_t                     m_nrdSpecularOutputSrvIndex = 0;
    uint32_t                     m_nrdDiffuseFactorSrvIndex = 0;
    uint32_t                     m_nrdSpecularFactorSrvIndex = 0;
    uint32_t                     m_nrdViewZSrvIndex = 0;
    bool                         m_nrdOutputsAreShaderResources = false;
#endif
    uint32_t                     m_outputUavIndex = 0;
    uint32_t                     m_accumulationUavIndex = 0;
    uint32_t                     m_historyUavIndex = 0;
    uint32_t                     m_outputSrvIndex = 0;
    bool                         m_descriptorsAllocated = false;
    bool                         m_outputIsShaderResource = false;
    uint32_t                     m_renderWidth = 0;
    uint32_t                     m_renderHeight = 0;
    uint32_t                     m_outputWidth = 0;
    uint32_t                     m_outputHeight = 0;

    ComPtr<ID3D12Resource>       m_readbackBuffer;
    uint32_t                     m_readbackRowPitch = 0;
    uint32_t                     m_readbackWidth = 0;
    uint32_t                     m_readbackHeight = 0;
};

} // namespace Engine
