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

namespace Engine {

using Microsoft::WRL::ComPtr;
class NRDDenoiser;

class RayTracingPipeline {
public:
    RayTracingPipeline();
    ~RayTracingPipeline();

    bool init(D3D12Device& device, uint32_t renderWidth, uint32_t renderHeight);
    void resizeOutput(D3D12Device& device, uint32_t renderWidth, uint32_t renderHeight);

    void dispatch(D3D12Device& device, 
                  D3D12_GPU_VIRTUAL_ADDRESS tlasAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS sceneCbAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS verticesAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS indicesAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS materialsAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS instancesAddress,
                  D3D12_GPU_DESCRIPTOR_HANDLE textureArrayHandle,
                  uint32_t width, uint32_t height,
                  const SceneCB& sceneData,
                  DirectX::FXMMATRIX viewToClip,
                  DirectX::FXMMATRIX worldToView);

    ID3D12Resource* getOutputTexture() const { return m_outputTexture.Get(); }
    uint32_t getOutputSrvIndex() const { return m_outputSrvIndex; }
    uint32_t getRenderWidth() const { return m_renderWidth; }
    uint32_t getRenderHeight() const { return m_renderHeight; }
    bool isNrdAvailable() const;

    void copyOutputToReadback(D3D12Device& device);
    bool saveReadbackToBmp(D3D12Device& device, const std::string& filename, std::string* failureReason = nullptr);

private:
    bool createRootSignature(D3D12Device& device);
    bool createPipelineState(D3D12Device& device);
    bool createShaderBindingTable(D3D12Device& device);
    bool createOutputResources(D3D12Device& device, uint32_t width, uint32_t height);
    bool createNrdCompositePipeline(D3D12Device& device);
    void compositeNrd(D3D12Device& device, D3D12_GPU_VIRTUAL_ADDRESS sceneCbAddress,
                      const SceneCB& sceneData,
                      DirectX::FXMMATRIX viewToClip,
                      DirectX::FXMMATRIX worldToView);

    ComPtr<ID3D12RootSignature>  m_globalRootSignature;
    ComPtr<ID3D12StateObject>    m_stateObject;
    ComPtr<ID3D12StateObjectProperties> m_stateObjectProperties;

    ComPtr<ID3D12Resource>       m_shaderBindingTable;
    uint32_t                     m_rayGenRecordSize = 0;
    uint32_t                     m_missRecordSize = 0;
    uint32_t                     m_hitGroupRecordSize = 0;

    ComPtr<ID3D12Resource>       m_outputTexture;
    ComPtr<ID3D12Resource>       m_accumulationTexture;
    ComPtr<ID3D12Resource>       m_historyTexture;
#if ENGINE_ENABLE_NRD
    ComPtr<ID3D12Resource>       m_nrdSignalTexture;
    ComPtr<ID3D12Resource>       m_nrdNormalRoughnessTexture;
    ComPtr<ID3D12Resource>       m_nrdViewZTexture;
    ComPtr<ID3D12Resource>       m_nrdMotionTexture;
    ComPtr<ID3D12Resource>       m_nrdAlbedoTexture;
    ComPtr<ID3D12Resource>       m_nrdOutputTexture;
    ComPtr<ID3D12RootSignature>  m_nrdCompositeRootSignature;
    ComPtr<ID3D12PipelineState>  m_nrdCompositePipeline;
    std::unique_ptr<NRDDenoiser> m_nrd;
    uint32_t                     m_nrdSignalUavIndex = 0;
    uint32_t                     m_nrdNormalRoughnessUavIndex = 0;
    uint32_t                     m_nrdViewZUavIndex = 0;
    uint32_t                     m_nrdMotionUavIndex = 0;
    uint32_t                     m_nrdAlbedoUavIndex = 0;
    uint32_t                     m_accumulationSrvIndex = 0;
    uint32_t                     m_nrdOutputSrvIndex = 0;
    uint32_t                     m_nrdAlbedoSrvIndex = 0;
    uint32_t                     m_nrdViewZSrvIndex = 0;
    bool                         m_nrdOutputIsShaderResource = false;
#endif
    uint32_t                     m_outputUavIndex = 0;
    uint32_t                     m_accumulationUavIndex = 0;
    uint32_t                     m_historyUavIndex = 0;
    uint32_t                     m_outputSrvIndex = 0;
    bool                         m_descriptorsAllocated = false;
    uint32_t                     m_renderWidth = 0;
    uint32_t                     m_renderHeight = 0;

    ComPtr<ID3D12Resource>       m_readbackBuffer;
    uint32_t                     m_readbackRowPitch = 0;
};

} // namespace Engine
