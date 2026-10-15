#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <memory>
#include "D3D12Device.h"
#include "ConstantBuffers.h"

namespace Engine {

class NRDDenoiser {
public:
    NRDDenoiser();
    ~NRDDenoiser();
    NRDDenoiser(const NRDDenoiser&) = delete;
    NRDDenoiser& operator=(const NRDDenoiser&) = delete;

    bool initialize(D3D12Device& device, uint32_t width, uint32_t height);
    bool isReady() const;

    // All input guides are left in UAV state by the DXR dispatch. The denoised
    // output's starting state is tracked separately because composition reads it.
    bool denoise(D3D12Device& device, const SceneCB& scene,
                 DirectX::FXMMATRIX viewToClip,
                 DirectX::FXMMATRIX worldToView,
                 ID3D12Resource* signal,
                 ID3D12Resource* normalRoughness,
                 ID3D12Resource* viewZ,
                 ID3D12Resource* motion,
                 ID3D12Resource* denoisedOutput,
                 bool outputIsShaderResource);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace Engine
