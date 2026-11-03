#include "RayTracingPipeline.h"
#include "RayTracingPBR_Bytecode.h"
#if ENGINE_ENABLE_DLSS
#include "DlssComposite_Bytecode.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_d3d.h>
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#include <nvsdk_ngx_params_dlssd.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <windows.h>
#include <filesystem>
#endif
#include "NRDDenoiser.h"
#if ENGINE_ENABLE_NRD
#include "NrdComposite_Bytecode.h"
#endif
#include "../core/ImageCapture.h"
#include <bit>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace Engine {

RayTracingPipeline::RayTracingPipeline() = default;
RayTracingPipeline::~RayTracingPipeline() {
    shutdownDlss();
}

static inline uint64_t alignTo(uint64_t size, uint64_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

bool RayTracingPipeline::init(D3D12Device& device, uint32_t renderWidth, uint32_t renderHeight,
                              uint32_t outputWidth, uint32_t outputHeight) {
    if (outputWidth == 0) outputWidth = renderWidth;
    if (outputHeight == 0) outputHeight = renderHeight;
    if (!createRootSignature(device)) {
        std::cerr << "[DXR Error] Global root-signature creation failed.\n";
        return false;
    }
    if (!createPipelineState(device)) {
        std::cerr << "[DXR Error] Ray-tracing state-object creation failed.\n";
        return false;
    }
    if (!createShaderBindingTable(device)) {
        std::cerr << "[DXR Error] Shader-binding-table creation failed.\n";
        return false;
    }
    if (!createOutputResources(device, renderWidth, renderHeight, outputWidth, outputHeight)) {
        std::cerr << "[DXR Error] Ray-tracing output-resource creation failed.\n";
        return false;
    }
#if ENGINE_ENABLE_DLSS
    if (!createDlssCompositePipeline(device)) {
        m_dlssStatus = "DLSS composite pipeline unavailable";
        std::cerr << "[DLSS] Output composition pipeline failed; DLSS remains disabled.\n";
    }
    initializeDlss(device); // Optional: keep the renderer usable when NGX or the driver is unavailable.
#endif
#if ENGINE_ENABLE_NRD
    m_nrd = std::make_unique<NRDDenoiser>();
    if (createNrdCompositePipeline(device)) {
        if (!m_nrd->initialize(device, renderWidth, renderHeight)) {
            std::cerr << "[NRD Warning] Denoiser initialization failed; direct ray-traced output remains available.\n";
        }
    } else {
        std::cerr << "[NRD Warning] Composite pipeline creation failed; direct ray-traced output remains available.\n";
    }
#endif
    return true;
}

bool RayTracingPipeline::isNrdAvailable() const {
#if ENGINE_ENABLE_NRD
    return m_nrd && m_nrd->isReady() && m_nrdCompositePipeline;
#else
    return false;
#endif
}

bool RayTracingPipeline::isDlssAvailable() const {
#if ENGINE_ENABLE_DLSS
    return m_dlssApiAvailable && m_dlssCompositePipeline;
#else
    return false;
#endif
}

void RayTracingPipeline::setDlssSettings(bool srEnabled, bool rrEnabled, float renderScale,
                                         uint32_t rrPreset) {
#if ENGINE_ENABLE_DLSS
    const bool nextSr = srEnabled && !rrEnabled;
    const float nextScale = std::isfinite(renderScale) ? std::clamp(renderScale, 0.5f, 1.0f) : 0.667f;
    const uint32_t nextPreset = std::clamp(rrPreset, 4u, 6u);
    const bool recreateFeature = m_dlssSrRequested != nextSr || m_dlssRrRequested != rrEnabled ||
        std::abs(m_dlssRenderScale - nextScale) > 1.0e-4f || m_dlssRrPreset != nextPreset;
    if (recreateFeature) {
        m_dlssSrRequested = nextSr;
        m_dlssRrRequested = rrEnabled;
        m_dlssRenderScale = nextScale;
        m_dlssRrPreset = nextPreset;
        m_dlssFeatureNeedsCreation = true;
        m_dlssFeatureFailed = false;
        m_dlssResetHistory = true;
        m_dlssOutputIsActive = false;
    }
    if (!nextSr && !rrEnabled) {
        m_dlssOutputIsActive = false;
        m_dlssStatus = "Off; native renderer output active";
    } else if (recreateFeature) {
        m_dlssStatus = rrEnabled ? "Ray Reconstruction requested; activation pending"
                                 : "Super Resolution requested; activation pending";
    }
#else
    (void)srEnabled; (void)rrEnabled; (void)renderScale; (void)rrPreset;
#endif
}

bool RayTracingPipeline::createRootSignature(D3D12Device& device) {
    // Root Parameters:
    // 0: Descriptor Table (2 UAVs: u0 = g_Output, u1 = g_Accumulation)
    // 1: SRV t0 (RaytracingAccelerationStructure g_Scene)
    // 2: CBV b0 (SceneCB g_SceneCB)
    // 3: SRV t1 (StructuredBuffer<Vertex> g_Vertices)
    // 4: SRV t2 (StructuredBuffer<uint> g_Indices)
    // 5: SRV t3 (StructuredBuffer<MaterialData> g_Materials)
    // 6: SRV t4 (StructuredBuffer<InstanceData> g_Instances)
    // 7: Descriptor Table (1 SRV: t5 = Texture2DArray g_Textures)
    // 8: Descriptor Table (2 SRVs: t6/t7 = environment radiance/irradiance)

    D3D12_DESCRIPTOR_RANGE uavRanges[3] = {};
    UINT uavRangeCount = 0;
    auto addUavRange = [&](UINT baseRegister, UINT count) {
        auto& range = uavRanges[uavRangeCount++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = count;
        range.BaseShaderRegister = baseRegister;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    };
    addUavRange(0, 3); // output, accumulation and reprojection history
#if ENGINE_ENABLE_NRD
    addUavRange(3, 7); // NRD diffuse/specular signals and guides
#endif
#if ENGINE_ENABLE_DLSS
    addUavRange(10, 7); // HDR color, depth, motion, material and hit-distance guides
#endif

    D3D12_DESCRIPTOR_RANGE srvTexRange = {};
    srvTexRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvTexRange.NumDescriptors = 1; // t5: g_Textures (Texture2DArray)
    srvTexRange.BaseShaderRegister = 5;
    srvTexRange.RegisterSpace = 0;
    srvTexRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE srvEnvironmentRange = {};
    srvEnvironmentRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvEnvironmentRange.NumDescriptors = 2;
    srvEnvironmentRange.BaseShaderRegister = 6;
    srvEnvironmentRange.RegisterSpace = 0;
    srvEnvironmentRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[9] = {};

    // 0: UAV Table
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = uavRangeCount;
    rootParams[0].DescriptorTable.pDescriptorRanges = uavRanges;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 1: Scene TLAS SRV
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[1].Descriptor.ShaderRegister = 0; // t0
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 2: Scene CBV
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 0; // b0
    rootParams[2].Descriptor.RegisterSpace = 0;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 3: Vertices SRV
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[3].Descriptor.ShaderRegister = 1; // t1
    rootParams[3].Descriptor.RegisterSpace = 0;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 4: Indices SRV
    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[4].Descriptor.ShaderRegister = 2; // t2
    rootParams[4].Descriptor.RegisterSpace = 0;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 5: Materials SRV
    rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[5].Descriptor.ShaderRegister = 3; // t3
    rootParams[5].Descriptor.RegisterSpace = 0;
    rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 6: Instances SRV
    rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[6].Descriptor.ShaderRegister = 4; // t4
    rootParams[6].Descriptor.RegisterSpace = 0;
    rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 7: Textures Descriptor Table
    rootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[7].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[7].DescriptorTable.pDescriptorRanges = &srvTexRange;
    rootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 8: Environment maps. A typed-null pair is bound for scenes without an HDRI.
    rootParams[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[8].DescriptorTable.pDescriptorRanges = &srvEnvironmentRange;
    rootParams[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplerDescs[2] = {};
    samplerDescs[0].Filter = D3D12_FILTER_ANISOTROPIC;
    samplerDescs[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDescs[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDescs[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDescs[0].MaxAnisotropy = 16;
    samplerDescs[0].ShaderRegister = 0; // s0: material textures
    samplerDescs[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescs[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDescs[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDescs[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDescs[1].MaxAnisotropy = 1;
    samplerDescs[1].ShaderRegister = 1; // s1: equirectangular HDR map
    for (D3D12_STATIC_SAMPLER_DESC& sampler : samplerDescs) {
        sampler.MipLODBias = 0.0f;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = static_cast<UINT>(std::size(rootParams));
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = static_cast<UINT>(std::size(samplerDescs));
    rootSigDesc.pStaticSamplers = samplerDescs;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, errorBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errorBlob))) {
        return false;
    }

    if (FAILED(device.getDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_globalRootSignature)))) {
        return false;
    }

    m_defaultEnvironmentSrvIndex = device.allocateSrvUavDescriptor();
    const uint32_t fallbackIrradianceIndex = device.allocateSrvUavDescriptor();
    if (m_defaultEnvironmentSrvIndex == std::numeric_limits<uint32_t>::max() ||
        fallbackIrradianceIndex != m_defaultEnvironmentSrvIndex + 1u) {
        std::cerr << "[DXR Error] Could not reserve a contiguous typed-null environment SRV pair.\n";
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC fallbackEnvironmentSrv = {};
    fallbackEnvironmentSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    fallbackEnvironmentSrv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    fallbackEnvironmentSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    fallbackEnvironmentSrv.Texture2D.MostDetailedMip = 0;
    fallbackEnvironmentSrv.Texture2D.MipLevels = 1;
    device.getDevice()->CreateShaderResourceView(nullptr, &fallbackEnvironmentSrv,
        device.getSrvUavCpuHandle(m_defaultEnvironmentSrvIndex));
    device.getDevice()->CreateShaderResourceView(nullptr, &fallbackEnvironmentSrv,
        device.getSrvUavCpuHandle(fallbackIrradianceIndex));

    return true;
}

bool RayTracingPipeline::createNrdCompositePipeline(D3D12Device& device) {
#if ENGINE_ENABLE_NRD
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
#if ENGINE_ENABLE_DLSS
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 6;
    ranges[2].BaseShaderRegister = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
#else
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 6;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
#endif

    D3D12_ROOT_PARAMETER parameters[4] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    UINT parameterCount = 3;
#if ENGINE_ENABLE_DLSS
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &ranges[2];
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants.ShaderRegister = 0;
    parameters[3].Constants.RegisterSpace = 0;
    parameters[3].Constants.Num32BitValues = 2;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameterCount = 4;
#else
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.ShaderRegister = 0;
    parameters[2].Constants.RegisterSpace = 0;
    parameters[2].Constants.Num32BitValues = 2;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
#endif

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = parameterCount;
    rootDesc.pParameters = parameters;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device.getDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                        IID_PPV_ARGS(&m_nrdCompositeRootSignature)))) {
        std::cerr << "[NRD] Failed to create composition root signature.\n";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_nrdCompositeRootSignature.Get();
    psoDesc.CS.pShaderBytecode = g_NrdComposite_Bytecode;
    psoDesc.CS.BytecodeLength = sizeof(g_NrdComposite_Bytecode);
    if (FAILED(device.getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_nrdCompositePipeline)))) {
        std::cerr << "[NRD] Failed to create composition compute pipeline.\n";
        return false;
    }
    return true;
#else
    (void)device;
    return false;
#endif
}

bool RayTracingPipeline::createDlssCompositePipeline(D3D12Device& device) {
#if ENGINE_ENABLE_DLSS
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[3] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.ShaderRegister = 0;
    parameters[2].Constants.RegisterSpace = 0;
    parameters[2].Constants.Num32BitValues = 2;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = static_cast<UINT>(std::size(parameters));
    rootDesc.pParameters = parameters;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device.getDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                        IID_PPV_ARGS(&m_dlssCompositeRootSignature)))) {
        std::cerr << "[DLSS] Failed to create output composition root signature.\n";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_dlssCompositeRootSignature.Get();
    psoDesc.CS.pShaderBytecode = g_DlssComposite_Bytecode;
    psoDesc.CS.BytecodeLength = sizeof(g_DlssComposite_Bytecode);
    if (FAILED(device.getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_dlssCompositePipeline)))) {
        std::cerr << "[DLSS] Failed to create output composition compute pipeline.\n";
        return false;
    }
    return true;
#else
    (void)device;
    return false;
#endif
}

bool RayTracingPipeline::initializeDlss(D3D12Device& device) {
#if ENGINE_ENABLE_DLSS
    const char* appIdText = std::getenv("ENGINE_NGX_APPLICATION_ID");
    unsigned long long appId = 0;
    const bool hasAppId = appIdText && *appIdText;
    if (hasAppId) {
        char* parsedEnd = nullptr;
        errno = 0;
        appId = std::strtoull(appIdText, &parsedEnd, 0);
        if (errno == ERANGE || parsedEnd == appIdText || *parsedEnd != '\0' || appId == 0) {
            m_dlssStatus = "Invalid ENGINE_NGX_APPLICATION_ID";
            std::cerr << "[DLSS] " << m_dlssStatus << ".\n";
            return false;
        }
    }

    // NVIDIA's NGX SDK supports a GUID-like Project ID for custom engines that
    // do not have an NVIDIA-issued Application ID. This UUIDv5 is derived from
    // the engine's canonical repository URL; forks should set
    // ENGINE_NGX_PROJECT_ID to their own stable GUID-like identifier.
    constexpr char defaultProjectId[] = "58e5af7d-f814-526c-98d7-8f87fa351cb9";
    const char* projectId = std::getenv("ENGINE_NGX_PROJECT_ID");
    if (!projectId || !*projectId) projectId = defaultProjectId;
    const auto isGuidLike = [](const char* value) {
        if (!value || std::strlen(value) != 36) return false;
        for (size_t i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (value[i] != '-') return false;
            } else {
                const char c = value[i];
                const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (!hex) return false;
            }
        }
        return true;
    };
    if (!hasAppId && !isGuidLike(projectId)) {
        m_dlssStatus = "Invalid ENGINE_NGX_PROJECT_ID (expected GUID format)";
        std::cerr << "[DLSS] " << m_dlssStatus << ".\n";
        return false;
    }

    wchar_t localAppData[MAX_PATH] = {};
    const DWORD pathLength = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH) {
        m_dlssStatus = "LOCALAPPDATA is unavailable for NGX cache";
        std::cerr << "[DLSS] " << m_dlssStatus << ".\n";
        return false;
    }
    std::filesystem::path ngxDataPath = std::filesystem::path(localAppData) / L"CustomEngine" / L"NGX";
    std::error_code fileError;
    std::filesystem::create_directories(ngxDataPath, fileError);
    if (fileError) {
        m_dlssStatus = "Cannot create NGX data directory";
        std::cerr << "[DLSS] " << m_dlssStatus << ": " << fileError.message() << ".\n";
        return false;
    }

    const NVSDK_NGX_Result initResult = hasAppId
        ? NVSDK_NGX_D3D12_Init(appId, ngxDataPath.c_str(), device.getDevice())
        : NVSDK_NGX_D3D12_Init_with_ProjectID(projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
              ENGINE_VERSION_STRING, ngxDataPath.c_str(), device.getDevice());
    if (!NVSDK_NGX_SUCCEED(initResult)) {
        m_dlssStatus = std::string("NGX ") + (hasAppId ? "Application ID" : "Project ID") +
            " initialization failed (" + std::to_string(static_cast<unsigned int>(initResult)) + ")";
        std::cerr << "[DLSS] " << m_dlssStatus << ".\n";
        return false;
    }
    m_ngxInitialized = true;
    m_ngxDevice = device.getDevice();
    const NVSDK_NGX_Result paramsResult = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_ngxParameters);
    if (!NVSDK_NGX_SUCCEED(paramsResult) || !m_ngxParameters) {
        m_dlssStatus = "NGX capability query failed (" + std::to_string(static_cast<unsigned int>(paramsResult)) + ")";
        shutdownDlss();
        std::cerr << "[DLSS] " << m_dlssStatus << ".\n";
        return false;
    }
    m_dlssApiAvailable = true;
    m_dlssStatus = hasAppId ? "NGX initialized with NVIDIA Application ID; SR/RR feature creation pending"
                            : "NGX initialized with custom-engine Project ID; SR/RR feature creation pending";
    std::cout << "[DLSS] " << m_dlssStatus << " on GPU: " << device.getGpuName() << ".\n";
    return true;
#else
    (void)device;
    return false;
#endif
}

void RayTracingPipeline::releaseDlssFeature() {
#if ENGINE_ENABLE_DLSS
    if (m_ngxFeature) {
        const NVSDK_NGX_Result releaseResult = NVSDK_NGX_D3D12_ReleaseFeature(m_ngxFeature);
        if (!NVSDK_NGX_SUCCEED(releaseResult)) {
            std::cerr << "[DLSS] Feature release failed (" << static_cast<unsigned int>(releaseResult) << ").\n";
        }
        m_ngxFeature = nullptr;
    }
    m_dlssOutputIsActive = false;
#endif
}

void RayTracingPipeline::shutdownDlss() {
#if ENGINE_ENABLE_DLSS
    releaseDlssFeature();
    if (m_ngxParameters && m_ngxInitialized) {
        NVSDK_NGX_D3D12_DestroyParameters(m_ngxParameters);
        m_ngxParameters = nullptr;
    }
    if (m_ngxInitialized) {
        const NVSDK_NGX_Result shutdownResult = NVSDK_NGX_D3D12_Shutdown1(m_ngxDevice);
        if (!NVSDK_NGX_SUCCEED(shutdownResult)) {
            std::cerr << "[DLSS] NGX shutdown reported " << static_cast<unsigned int>(shutdownResult) << ".\n";
        }
    }
    m_ngxInitialized = false;
    m_ngxDevice = nullptr;
    m_dlssApiAvailable = false;
#endif
}

bool RayTracingPipeline::ensureDlssFeature(D3D12Device& device, ID3D12GraphicsCommandList* commandList,
                                           bool rayReconstruction) {
#if ENGINE_ENABLE_DLSS
    if (!m_dlssApiAvailable || !m_ngxParameters || !commandList || !m_dlssInputColorTexture ||
        !m_dlssOutputHdrTexture || m_renderWidth == 0 || m_renderHeight == 0 ||
        m_outputWidth == 0 || m_outputHeight == 0) return false;
    if (m_ngxFeature && !m_dlssFeatureNeedsCreation && m_dlssFeatureIsRayReconstruction == rayReconstruction) return true;
    if (m_ngxFeature) {
        if (!device.waitForGpu()) {
            m_dlssStatus = "Cannot recreate NGX feature while GPU work is in flight";
            return false;
        }
        releaseDlssFeature();
    }

    // Match the SDK quality hint to the actual input/output ratio. The slider
    // bottoms out at 0.5, so Ultra Performance (roughly one-third resolution)
    // is not a reachable mode. Use midpoint boundaries between the standard
    // 0.50 Performance, 0.58 Balanced, and 0.667 Quality ratios.
    NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (m_renderWidth == m_outputWidth && m_renderHeight == m_outputHeight)
        quality = NVSDK_NGX_PerfQuality_Value_DLAA;
    else if (m_dlssRenderScale < 0.54f) quality = NVSDK_NGX_PerfQuality_Value_MaxPerf;
    else if (m_dlssRenderScale < 0.624f) quality = NVSDK_NGX_PerfQuality_Value_Balanced;

    constexpr int hdrAndLowResolutionMotion = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    NVSDK_NGX_Result createResult = NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if (rayReconstruction) {
        NVSDK_NGX_DLSSD_Create_Params createParams = {};
        createParams.InWidth = m_renderWidth;
        createParams.InHeight = m_renderHeight;
        createParams.InTargetWidth = m_outputWidth;
        createParams.InTargetHeight = m_outputHeight;
        createParams.InPerfQualityValue = quality;
        createParams.InFeatureCreateFlags = hdrAndLowResolutionMotion;
        createParams.InEnableOutputSubrects = false;
        createParams.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        createParams.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        createParams.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;

        const char* hint = NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality;
        if (quality == NVSDK_NGX_PerfQuality_Value_Balanced) hint = NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced;
        else if (quality == NVSDK_NGX_PerfQuality_Value_MaxPerf || quality == NVSDK_NGX_PerfQuality_Value_UltraPerformance)
            hint = NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance;
        NVSDK_NGX_Parameter_SetUI(m_ngxParameters, hint, m_dlssRrPreset);
        createResult = NGX_D3D12_CREATE_DLSSD_EXT(commandList, 1, 1, &m_ngxFeature, m_ngxParameters, &createParams);
    } else {
        NVSDK_NGX_DLSS_Create_Params createParams = {};
        createParams.Feature.InWidth = m_renderWidth;
        createParams.Feature.InHeight = m_renderHeight;
        createParams.Feature.InTargetWidth = m_outputWidth;
        createParams.Feature.InTargetHeight = m_outputHeight;
        createParams.Feature.InPerfQualityValue = quality;
        createParams.InFeatureCreateFlags = hdrAndLowResolutionMotion;
        createParams.InEnableOutputSubrects = false;
        createResult = NGX_D3D12_CREATE_DLSS_EXT(commandList, 1, 1, &m_ngxFeature, m_ngxParameters, &createParams);
    }
    if (!NVSDK_NGX_SUCCEED(createResult) || !m_ngxFeature) {
        m_ngxFeature = nullptr;
        m_dlssStatus = std::string(rayReconstruction ? "Ray Reconstruction" : "Super Resolution") +
            " feature creation failed (" + std::to_string(static_cast<unsigned int>(createResult)) + ")";
        std::cerr << "[DLSS] " << m_dlssStatus << "; using native renderer output.\n";
        m_dlssFeatureNeedsCreation = true;
        m_dlssFeatureFailed = true;
        return false;
    }
    m_dlssFeatureIsRayReconstruction = rayReconstruction;
    m_dlssFeatureNeedsCreation = false;
    m_dlssStatus = rayReconstruction ? "Ray Reconstruction active" : "Super Resolution active";
    std::cout << "[DLSS] " << m_dlssStatus << ": " << m_renderWidth << 'x' << m_renderHeight << " -> "
              << m_outputWidth << 'x' << m_outputHeight << ", RR preset " << m_dlssRrPreset << ".\n";
    return true;
#else
    (void)commandList;
    (void)device;
    (void)rayReconstruction;
    return false;
#endif
}

bool RayTracingPipeline::evaluateDlss(D3D12Device& device, const SceneCB& sceneData,
                                      DirectX::FXMMATRIX viewToClip, DirectX::FXMMATRIX worldToView) {
#if ENGINE_ENABLE_DLSS
    m_dlssOutputIsActive = false;
    const bool rayReconstruction = sceneData.enableDLSSRR != 0;
    if (!m_dlssSrRequested && !m_dlssRrRequested) {
        m_dlssStatus = "Off; native renderer output active";
        return false;
    }
    if (sceneData.enableDLSS == 0 || sceneData.renderMode > 1u) {
        m_dlssStatus = "Requested but unavailable for this scene or render mode; native output active";
        return false;
    }
    if (!isDlssAvailable()) {
        m_dlssStatus = "NGX unavailable; native output active";
        return false;
    }
    if (m_dlssFeatureFailed) return false;
    auto* commandList = device.getCommandList();
    if (!ensureDlssFeature(device, commandList, rayReconstruction)) return false;

    ID3D12Resource* inputs[] = {
        m_dlssInputColorTexture.Get(), m_dlssDepthTexture.Get(), m_dlssMotionTexture.Get(),
        m_dlssNormalRoughnessTexture.Get(), m_dlssDiffuseAlbedoTexture.Get(),
        m_dlssSpecularAlbedoTexture.Get(), m_dlssSpecularHitDistanceTexture.Get()
    };
    for (ID3D12Resource* resource : inputs) {
        if (!resource) return false;
        device.uavBarrier(resource);
        device.transitionBarrier(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    NVSDK_NGX_Result evalResult = NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if (rayReconstruction) {
        NVSDK_NGX_D3D12_DLSSD_Eval_Params evalParams = {};
        evalParams.pInDiffuseAlbedo = m_dlssDiffuseAlbedoTexture.Get();
        evalParams.pInSpecularAlbedo = m_dlssSpecularAlbedoTexture.Get();
        evalParams.pInNormals = m_dlssNormalRoughnessTexture.Get();
        evalParams.pInColor = m_dlssInputColorTexture.Get();
        evalParams.pInOutput = m_dlssOutputHdrTexture.Get();
        evalParams.pInDepth = m_dlssDepthTexture.Get();
        evalParams.pInMotionVectors = m_dlssMotionTexture.Get();
        evalParams.pInSpecularHitDistance = m_dlssSpecularHitDistanceTexture.Get();
        evalParams.InJitterOffsetX = sceneData.jitterOffset.x;
        evalParams.InJitterOffsetY = sceneData.jitterOffset.y;
        evalParams.InRenderSubrectDimensions.Width = m_renderWidth;
        evalParams.InRenderSubrectDimensions.Height = m_renderHeight;
        evalParams.InReset = (sceneData.dlssReset != 0 || m_dlssResetHistory) ? 1 : 0;
        evalParams.InMVScaleX = 1.0f;
        evalParams.InMVScaleY = 1.0f;
        evalParams.InPreExposure = 1.0f;
        evalParams.InExposureScale = 1.0f;
        evalParams.InToneMapperType = sceneData.toneMappingMode == 0 ? NVSDK_NGX_TONEMAPPER_ACES :
            (sceneData.toneMappingMode == 1 ? NVSDK_NGX_TONEMAPPER_REINHARD : NVSDK_NGX_TONEMAPPER_STRING);
        DirectX::XMFLOAT4X4 worldToViewMatrix;
        DirectX::XMFLOAT4X4 viewToClipMatrix;
        DirectX::XMStoreFloat4x4(&worldToViewMatrix, worldToView);
        DirectX::XMStoreFloat4x4(&viewToClipMatrix, viewToClip);
        evalParams.pInWorldToViewMatrix = reinterpret_cast<float*>(&worldToViewMatrix);
        evalParams.pInViewToClipMatrix = reinterpret_cast<float*>(&viewToClipMatrix);
        evalResult = NGX_D3D12_EVALUATE_DLSSD_EXT(commandList, m_ngxFeature, m_ngxParameters, &evalParams);
    } else {
        NVSDK_NGX_D3D12_DLSS_Eval_Params evalParams = {};
        evalParams.Feature.pInColor = m_dlssInputColorTexture.Get();
        evalParams.Feature.pInOutput = m_dlssOutputHdrTexture.Get();
        evalParams.Feature.InSharpness = 0.0f; // The DLSS 3.10 SDK marks sharpening unsupported.
        evalParams.pInDepth = m_dlssDepthTexture.Get();
        evalParams.pInMotionVectors = m_dlssMotionTexture.Get();
        evalParams.InJitterOffsetX = sceneData.jitterOffset.x;
        evalParams.InJitterOffsetY = sceneData.jitterOffset.y;
        evalParams.InRenderSubrectDimensions.Width = m_renderWidth;
        evalParams.InRenderSubrectDimensions.Height = m_renderHeight;
        evalParams.InReset = (sceneData.dlssReset != 0 || m_dlssResetHistory) ? 1 : 0;
        evalParams.InMVScaleX = 1.0f;
        evalParams.InMVScaleY = 1.0f;
        evalParams.InPreExposure = 1.0f;
        evalParams.InExposureScale = 1.0f;
        evalResult = NGX_D3D12_EVALUATE_DLSS_EXT(commandList, m_ngxFeature, m_ngxParameters, &evalParams);
    }

    for (ID3D12Resource* resource : inputs) {
        device.transitionBarrier(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (!NVSDK_NGX_SUCCEED(evalResult)) {
        m_dlssStatus = std::string(rayReconstruction ? "Ray Reconstruction" : "Super Resolution") +
            " evaluation failed (" + std::to_string(static_cast<unsigned int>(evalResult)) + ")";
        std::cerr << "[DLSS] " << m_dlssStatus << "; native output is shown.\n";
        m_dlssFeatureFailed = true;
        return false;
    }
    compositeDlss(device, sceneData);
    m_dlssResetHistory = false;
    return m_dlssOutputIsActive;
#else
    (void)device; (void)sceneData; (void)viewToClip; (void)worldToView;
    return false;
#endif
}

void RayTracingPipeline::compositeDlss(D3D12Device& device, const SceneCB& sceneData) {
#if ENGINE_ENABLE_DLSS
    if (!device.isFrameRecording() || !m_dlssCompositeRootSignature || !m_dlssCompositePipeline ||
        !m_dlssOutputHdrTexture || !m_dlssDisplayTexture) {
        m_dlssStatus = "DLSS output composition unavailable";
        return;
    }
    auto* commandList = device.getCommandList();
    if (m_dlssDisplayIsShaderResource) {
        device.transitionBarrier(m_dlssDisplayTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_dlssDisplayIsShaderResource = false;
    }
    device.transitionBarrier(m_dlssOutputHdrTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = { device.getSrvUavHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(m_dlssCompositeRootSignature.Get());
    commandList->SetPipelineState(m_dlssCompositePipeline.Get());
    commandList->SetComputeRootDescriptorTable(0, device.getSrvUavGpuHandle(m_dlssOutputHdrSrvIndex));
    commandList->SetComputeRootDescriptorTable(1, device.getSrvUavGpuHandle(m_dlssDisplayUavIndex));
    const uint32_t settings[] = { std::bit_cast<uint32_t>(sceneData.exposure), sceneData.toneMappingMode };
    commandList->SetComputeRoot32BitConstants(2, 2, settings, 0);
    commandList->Dispatch((m_outputWidth + 7u) / 8u, (m_outputHeight + 7u) / 8u, 1);
    device.uavBarrier(m_dlssDisplayTexture.Get());
    device.transitionBarrier(m_dlssDisplayTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    device.transitionBarrier(m_dlssOutputHdrTexture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_dlssDisplayIsShaderResource = true;
    m_dlssOutputIsActive = true;
#else
    (void)device; (void)sceneData;
#endif
}

bool RayTracingPipeline::createPipelineState(D3D12Device& device) {
    // 5 Subobjects:
    // 0: DXIL Library
    // 1: HitGroup
    // 2: Shader Config
    // 3: Global Root Signature
    // 4: Pipeline Config

    D3D12_STATE_SUBOBJECT subobjects[5] = {};

    // 0: DXIL Library
    D3D12_EXPORT_DESC exports[] = {
        { L"RayGenShader", nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"ClosestHitShader", nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"AnyHitShader", nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"MissShader", nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"ShadowMissShader", nullptr, D3D12_EXPORT_FLAG_NONE }
    };

    D3D12_DXIL_LIBRARY_DESC dxilDesc = {};
    dxilDesc.DXILLibrary.pShaderBytecode = g_RayTracingPBR_Bytecode;
    dxilDesc.DXILLibrary.BytecodeLength = sizeof(g_RayTracingPBR_Bytecode);
    dxilDesc.NumExports = 5;
    dxilDesc.pExports = exports;

    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[0].pDesc = &dxilDesc;

    // 1: HitGroup
    D3D12_HIT_GROUP_DESC hitGroupDesc = {};
    hitGroupDesc.HitGroupExport = L"HitGroup_PBR";
    hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroupDesc.ClosestHitShaderImport = L"ClosestHitShader";
    hitGroupDesc.AnyHitShaderImport = L"AnyHitShader";

    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[1].pDesc = &hitGroupDesc;

    // 2: Shader Config
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    // RayPayload carries primary material factors, denoiser signals, and
    // bounded dielectric traversal state. Keep explicit headroom for DXIL.
    shaderConfig.MaxPayloadSizeInBytes = 128;
    shaderConfig.MaxAttributeSizeInBytes = 8; // float2 barycentrics

    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[2].pDesc = &shaderConfig;

    // 3: Global Root Signature
    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSig = {};
    globalRootSig.pGlobalRootSignature = m_globalRootSignature.Get();

    subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[3].pDesc = &globalRootSig;

    // 4: Pipeline Config
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 2; // Primary ray plus one bounded reflection/transmission ray.

    subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[4].pDesc = &pipelineConfig;

    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = 5;
    stateObjectDesc.pSubobjects = subobjects;

    const HRESULT stateObjectResult = device.getDevice()->CreateStateObject(
        &stateObjectDesc, IID_PPV_ARGS(&m_stateObject));
    if (FAILED(stateObjectResult)) {
        std::cerr << "[DXR Error] CreateStateObject failed with HRESULT 0x" << std::hex
                  << static_cast<uint32_t>(stateObjectResult) << std::dec << ".\n";
        return false;
    }

    return SUCCEEDED(m_stateObject.As(&m_stateObjectProperties)) && m_stateObjectProperties;
}

bool RayTracingPipeline::createShaderBindingTable(D3D12Device& device) {
    uint32_t shaderIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32 bytes

    m_rayGenRecordSize = static_cast<uint32_t>(alignTo(shaderIdSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT));
    m_missRecordSize = static_cast<uint32_t>(alignTo(shaderIdSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT));
    m_hitGroupRecordSize = static_cast<uint32_t>(alignTo(shaderIdSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT));

    uint64_t rayGenSectionSize = alignTo(m_rayGenRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    uint64_t missSectionSize   = alignTo(m_missRecordSize * 2, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    uint64_t hitGroupSectionSize = alignTo(m_hitGroupRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    uint64_t sbtSize = rayGenSectionSize + missSectionSize + hitGroupSectionSize;

    ComPtr<ID3D12Resource> shaderBindingTable = device.createBuffer(
        sbtSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!shaderBindingTable) return false;

    uint8_t* mappedData = nullptr;
    if (FAILED(shaderBindingTable->Map(0, nullptr, reinterpret_cast<void**>(&mappedData))) || !mappedData) return false;

    // 1. RayGen Record
    void* rayGenId = m_stateObjectProperties->GetShaderIdentifier(L"RayGenShader");
    void* missId = m_stateObjectProperties->GetShaderIdentifier(L"MissShader");
    void* shadowMissId = m_stateObjectProperties->GetShaderIdentifier(L"ShadowMissShader");
    void* hitGroupId = m_stateObjectProperties->GetShaderIdentifier(L"HitGroup_PBR");
    if (!rayGenId || !missId || !shadowMissId || !hitGroupId) {
        shaderBindingTable->Unmap(0, nullptr);
        std::cerr << "[DXR Error] Shader identifier missing while building the shader binding table.\n";
        return false;
    }
    std::memcpy(mappedData, rayGenId, shaderIdSize);

    // 2. Miss Records (Primary Miss + Shadow Miss)
    uint8_t* missPtr = mappedData + rayGenSectionSize;
    std::memcpy(missPtr, missId, shaderIdSize);
    std::memcpy(missPtr + m_missRecordSize, shadowMissId, shaderIdSize);

    // 3. HitGroup Record
    uint8_t* hitGroupPtr = mappedData + rayGenSectionSize + missSectionSize;
    std::memcpy(hitGroupPtr, hitGroupId, shaderIdSize);

    shaderBindingTable->Unmap(0, nullptr);
    m_shaderBindingTable = std::move(shaderBindingTable);
    return true;
}

bool RayTracingPipeline::createOutputResources(D3D12Device& device, uint32_t width, uint32_t height,
                                               uint32_t outputWidth, uint32_t outputHeight) {
    if (!device.getDevice() || width == 0 || height == 0 || outputWidth == 0 || outputHeight == 0) return false;

    ComPtr<ID3D12Resource> outputTexture = device.createTexture2D(
        width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );

    ComPtr<ID3D12Resource> accumulationTexture = device.createTexture2D(
        width, height,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );

    ComPtr<ID3D12Resource> historyTexture = device.createTexture2D(
        width, height,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
#if ENGINE_ENABLE_DLSS
    ComPtr<ID3D12Resource> dlssInputColorTexture = device.createTexture2D(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> dlssDepthTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> dlssMotionTexture = device.createTexture2D(width, height, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> dlssNormalRoughnessTexture = device.createTexture2D(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> dlssDiffuseAlbedoTexture = device.createTexture2D(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> dlssSpecularAlbedoTexture = device.createTexture2D(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> dlssSpecularHitDistanceTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> dlssOutputHdrTexture = device.createTexture2D(outputWidth, outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> dlssDisplayTexture = device.createTexture2D(outputWidth, outputHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
#endif
    if (!outputTexture || !accumulationTexture || !historyTexture) {
        std::cerr << "[DXR Error] Primary render texture allocation failed at " << width << "x" << height << ".\n";
        return false;
    }
#if ENGINE_ENABLE_DLSS
    if (!dlssInputColorTexture || !dlssDepthTexture || !dlssMotionTexture || !dlssNormalRoughnessTexture ||
        !dlssDiffuseAlbedoTexture || !dlssSpecularAlbedoTexture || !dlssSpecularHitDistanceTexture ||
        !dlssOutputHdrTexture || !dlssDisplayTexture) {
        std::cerr << "[DLSS Error] Guide/output allocation failed at input " << width << 'x' << height
                  << " and output " << outputWidth << 'x' << outputHeight << ".\n";
        return false;
    }
#endif

#if ENGINE_ENABLE_NRD
    ComPtr<ID3D12Resource> nrdSignalTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> nrdSpecularSignalTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> nrdNormalRoughnessTexture = device.createTexture2D(width, height, DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> nrdViewZTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> nrdMotionTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> nrdDiffuseFactorTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> nrdSpecularFactorTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> nrdDiffuseOutputTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> nrdSpecularOutputTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!nrdSignalTexture || !nrdSpecularSignalTexture || !nrdNormalRoughnessTexture || !nrdViewZTexture ||
        !nrdMotionTexture || !nrdDiffuseFactorTexture || !nrdSpecularFactorTexture ||
        !nrdDiffuseOutputTexture || !nrdSpecularOutputTexture) {
        std::cerr << "[NRD Error] Denoiser texture allocation failed at " << width << "x" << height << ".\n";
        return false;
    }
#endif

    const uint64_t rowPitch64 = (static_cast<uint64_t>(outputWidth) * 4u +
        D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
        ~static_cast<uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    if (rowPitch64 > std::numeric_limits<uint32_t>::max() ||
        rowPitch64 == 0 || outputHeight > std::numeric_limits<uint64_t>::max() / rowPitch64) return false;
    const uint32_t readbackRowPitch = static_cast<uint32_t>(rowPitch64);
    ComPtr<ID3D12Resource> readbackBuffer = device.createBuffer(
        rowPitch64 * outputHeight, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readbackBuffer) {
        std::cerr << "[DXR Error] Readback allocation failed at " << outputWidth << "x" << outputHeight << ".\n";
        return false;
    }

    if (!m_descriptorsAllocated) {
        m_outputUavIndex = device.allocateSrvUavDescriptor();
        m_accumulationUavIndex = device.allocateSrvUavDescriptor();
        m_historyUavIndex = device.allocateSrvUavDescriptor();
#if ENGINE_ENABLE_NRD
        m_nrdSignalUavIndex = device.allocateSrvUavDescriptor();
        m_nrdSpecularSignalUavIndex = device.allocateSrvUavDescriptor();
        m_nrdNormalRoughnessUavIndex = device.allocateSrvUavDescriptor();
        m_nrdViewZUavIndex = device.allocateSrvUavDescriptor();
        m_nrdMotionUavIndex = device.allocateSrvUavDescriptor();
        m_nrdDiffuseFactorUavIndex = device.allocateSrvUavDescriptor();
        m_nrdSpecularFactorUavIndex = device.allocateSrvUavDescriptor();
#endif
#if ENGINE_ENABLE_DLSS
        m_dlssInputColorUavIndex = device.allocateSrvUavDescriptor();
        m_dlssDepthUavIndex = device.allocateSrvUavDescriptor();
        m_dlssMotionUavIndex = device.allocateSrvUavDescriptor();
        m_dlssNormalRoughnessUavIndex = device.allocateSrvUavDescriptor();
        m_dlssDiffuseAlbedoUavIndex = device.allocateSrvUavDescriptor();
        m_dlssSpecularAlbedoUavIndex = device.allocateSrvUavDescriptor();
        m_dlssSpecularHitDistanceUavIndex = device.allocateSrvUavDescriptor();
#endif
        m_outputSrvIndex = device.allocateSrvUavDescriptor();
#if ENGINE_ENABLE_NRD
        m_accumulationSrvIndex = device.allocateSrvUavDescriptor();
        m_nrdDiffuseOutputSrvIndex = device.allocateSrvUavDescriptor();
        m_nrdDiffuseFactorSrvIndex = device.allocateSrvUavDescriptor();
        m_nrdViewZSrvIndex = device.allocateSrvUavDescriptor();
        m_nrdSpecularOutputSrvIndex = device.allocateSrvUavDescriptor();
        m_nrdSpecularFactorSrvIndex = device.allocateSrvUavDescriptor();
#endif
#if ENGINE_ENABLE_DLSS
        m_dlssOutputHdrSrvIndex = device.allocateSrvUavDescriptor();
        m_dlssDisplayUavIndex = device.allocateSrvUavDescriptor();
        m_dlssDisplaySrvIndex = device.allocateSrvUavDescriptor();
#endif
        const uint32_t descriptorIndices[] = {
            m_outputUavIndex, m_accumulationUavIndex, m_historyUavIndex, m_outputSrvIndex
#if ENGINE_ENABLE_NRD
            , m_nrdSignalUavIndex, m_nrdSpecularSignalUavIndex, m_nrdNormalRoughnessUavIndex, m_nrdViewZUavIndex,
            m_nrdMotionUavIndex, m_nrdDiffuseFactorUavIndex, m_nrdSpecularFactorUavIndex, m_accumulationSrvIndex,
            m_nrdDiffuseOutputSrvIndex, m_nrdDiffuseFactorSrvIndex, m_nrdViewZSrvIndex,
            m_nrdSpecularOutputSrvIndex, m_nrdSpecularFactorSrvIndex
#endif
#if ENGINE_ENABLE_DLSS
            , m_dlssInputColorUavIndex, m_dlssDepthUavIndex, m_dlssMotionUavIndex,
            m_dlssNormalRoughnessUavIndex, m_dlssDiffuseAlbedoUavIndex, m_dlssSpecularAlbedoUavIndex,
            m_dlssSpecularHitDistanceUavIndex, m_dlssOutputHdrSrvIndex, m_dlssDisplayUavIndex, m_dlssDisplaySrvIndex
#endif
        };
        for (uint32_t index : descriptorIndices) {
            if (index == 0xffffffffu) {
                std::cerr << "[DXR Error] Descriptor allocation exhausted while creating render-target views.\n";
                return false;
            }
        }
        m_descriptorsAllocated = true;
    }

    // UAV u0 for final output
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc0 = {};
    uavDesc0.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc0.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device.getDevice()->CreateUnorderedAccessView(outputTexture.Get(), nullptr, &uavDesc0, device.getSrvUavCpuHandle(m_outputUavIndex));

    // UAV u1 for accumulation buffer
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc1 = {};
    uavDesc1.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uavDesc1.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device.getDevice()->CreateUnorderedAccessView(accumulationTexture.Get(), nullptr, &uavDesc1, device.getSrvUavCpuHandle(m_accumulationUavIndex));

    // UAV u2 for the temporal history and progressive accumulation.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc2 = {};
    uavDesc2.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uavDesc2.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device.getDevice()->CreateUnorderedAccessView(historyTexture.Get(), nullptr, &uavDesc2, device.getSrvUavCpuHandle(m_historyUavIndex));

#if ENGINE_ENABLE_NRD
    D3D12_UNORDERED_ACCESS_VIEW_DESC nrdUavDesc = {};
    nrdUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    nrdUavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device.getDevice()->CreateUnorderedAccessView(nrdSignalTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdSignalUavIndex));
    device.getDevice()->CreateUnorderedAccessView(nrdSpecularSignalTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdSpecularSignalUavIndex));
    nrdUavDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    device.getDevice()->CreateUnorderedAccessView(nrdNormalRoughnessTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdNormalRoughnessUavIndex));
    nrdUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    device.getDevice()->CreateUnorderedAccessView(nrdViewZTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdViewZUavIndex));
    nrdUavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device.getDevice()->CreateUnorderedAccessView(nrdMotionTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdMotionUavIndex));
    device.getDevice()->CreateUnorderedAccessView(nrdDiffuseFactorTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdDiffuseFactorUavIndex));
    device.getDevice()->CreateUnorderedAccessView(nrdSpecularFactorTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdSpecularFactorUavIndex));
#endif
#if ENGINE_ENABLE_DLSS
    auto createDlssUav = [&](ID3D12Resource* resource, DXGI_FORMAT format, uint32_t descriptor) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device.getDevice()->CreateUnorderedAccessView(resource, nullptr, &desc, device.getSrvUavCpuHandle(descriptor));
    };
    createDlssUav(dlssInputColorTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_dlssInputColorUavIndex);
    createDlssUav(dlssDepthTexture.Get(), DXGI_FORMAT_R32_FLOAT, m_dlssDepthUavIndex);
    createDlssUav(dlssMotionTexture.Get(), DXGI_FORMAT_R16G16_FLOAT, m_dlssMotionUavIndex);
    createDlssUav(dlssNormalRoughnessTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_dlssNormalRoughnessUavIndex);
    createDlssUav(dlssDiffuseAlbedoTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_dlssDiffuseAlbedoUavIndex);
    createDlssUav(dlssSpecularAlbedoTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_dlssSpecularAlbedoUavIndex);
    createDlssUav(dlssSpecularHitDistanceTexture.Get(), DXGI_FORMAT_R32_FLOAT, m_dlssSpecularHitDistanceUavIndex);
    createDlssUav(dlssDisplayTexture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, m_dlssDisplayUavIndex);
#endif

    // SRV for ImGui Viewport rendering
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device.getDevice()->CreateShaderResourceView(outputTexture.Get(), &srvDesc, device.getSrvUavCpuHandle(m_outputSrvIndex));

#if ENGINE_ENABLE_NRD
    D3D12_SHADER_RESOURCE_VIEW_DESC floatSrvDesc = {};
    floatSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    floatSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    floatSrvDesc.Texture2D.MipLevels = 1;
    floatSrvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device.getDevice()->CreateShaderResourceView(accumulationTexture.Get(), &floatSrvDesc, device.getSrvUavCpuHandle(m_accumulationSrvIndex));
    device.getDevice()->CreateShaderResourceView(nrdDiffuseOutputTexture.Get(), &floatSrvDesc, device.getSrvUavCpuHandle(m_nrdDiffuseOutputSrvIndex));
    device.getDevice()->CreateShaderResourceView(nrdDiffuseFactorTexture.Get(), &floatSrvDesc, device.getSrvUavCpuHandle(m_nrdDiffuseFactorSrvIndex));
    floatSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    device.getDevice()->CreateShaderResourceView(nrdViewZTexture.Get(), &floatSrvDesc, device.getSrvUavCpuHandle(m_nrdViewZSrvIndex));
    floatSrvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device.getDevice()->CreateShaderResourceView(nrdSpecularOutputTexture.Get(), &floatSrvDesc, device.getSrvUavCpuHandle(m_nrdSpecularOutputSrvIndex));
    device.getDevice()->CreateShaderResourceView(nrdSpecularFactorTexture.Get(), &floatSrvDesc, device.getSrvUavCpuHandle(m_nrdSpecularFactorSrvIndex));
#endif
#if ENGINE_ENABLE_DLSS
    D3D12_SHADER_RESOURCE_VIEW_DESC dlssOutputSrv = {};
    dlssOutputSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dlssOutputSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dlssOutputSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dlssOutputSrv.Texture2D.MipLevels = 1;
    device.getDevice()->CreateShaderResourceView(dlssOutputHdrTexture.Get(), &dlssOutputSrv,
                                                  device.getSrvUavCpuHandle(m_dlssOutputHdrSrvIndex));
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device.getDevice()->CreateShaderResourceView(dlssDisplayTexture.Get(), &srvDesc,
                                                  device.getSrvUavCpuHandle(m_dlssDisplaySrvIndex));
#endif

    m_outputTexture = std::move(outputTexture);
    m_accumulationTexture = std::move(accumulationTexture);
    m_historyTexture = std::move(historyTexture);
#if ENGINE_ENABLE_DLSS
    m_dlssInputColorTexture = std::move(dlssInputColorTexture);
    m_dlssDepthTexture = std::move(dlssDepthTexture);
    m_dlssMotionTexture = std::move(dlssMotionTexture);
    m_dlssNormalRoughnessTexture = std::move(dlssNormalRoughnessTexture);
    m_dlssDiffuseAlbedoTexture = std::move(dlssDiffuseAlbedoTexture);
    m_dlssSpecularAlbedoTexture = std::move(dlssSpecularAlbedoTexture);
    m_dlssSpecularHitDistanceTexture = std::move(dlssSpecularHitDistanceTexture);
    m_dlssOutputHdrTexture = std::move(dlssOutputHdrTexture);
    m_dlssDisplayTexture = std::move(dlssDisplayTexture);
    m_dlssOutputIsActive = false;
    m_dlssResetHistory = true;
    m_dlssFeatureNeedsCreation = true;
#endif
#if ENGINE_ENABLE_NRD
    m_nrdSignalTexture = std::move(nrdSignalTexture);
    m_nrdSpecularSignalTexture = std::move(nrdSpecularSignalTexture);
    m_nrdNormalRoughnessTexture = std::move(nrdNormalRoughnessTexture);
    m_nrdViewZTexture = std::move(nrdViewZTexture);
    m_nrdMotionTexture = std::move(nrdMotionTexture);
    m_nrdDiffuseFactorTexture = std::move(nrdDiffuseFactorTexture);
    m_nrdSpecularFactorTexture = std::move(nrdSpecularFactorTexture);
    m_nrdDiffuseOutputTexture = std::move(nrdDiffuseOutputTexture);
    m_nrdSpecularOutputTexture = std::move(nrdSpecularOutputTexture);
    m_nrdOutputsAreShaderResources = false;
#endif
    m_readbackBuffer = std::move(readbackBuffer);
    m_readbackRowPitch = readbackRowPitch;
    m_renderWidth = width;
    m_renderHeight = height;
    m_outputWidth = outputWidth;
    m_outputHeight = outputHeight;
    m_outputIsShaderResource = false;
#if ENGINE_ENABLE_DLSS
    m_dlssDisplayIsShaderResource = false;
#endif
    return true;
}

bool RayTracingPipeline::resizeOutput(D3D12Device& device, uint32_t renderWidth, uint32_t renderHeight,
                                      uint32_t outputWidth, uint32_t outputHeight) {
    if (outputWidth == 0) outputWidth = renderWidth;
    if (outputHeight == 0) outputHeight = renderHeight;
    if (renderWidth == m_renderWidth && renderHeight == m_renderHeight &&
        outputWidth == m_outputWidth && outputHeight == m_outputHeight) return true;
    if (!device.waitForGpu()) {
        std::cerr << "[DXR Error] Cannot resize output while prior GPU work failed to drain.\n";
        return false;
    }
#if ENGINE_ENABLE_DLSS
    releaseDlssFeature();
    m_dlssFeatureNeedsCreation = true;
    m_dlssFeatureFailed = false;
    m_dlssResetHistory = true;
    m_dlssOutputIsActive = false;
#endif
    if (!createOutputResources(device, renderWidth, renderHeight, outputWidth, outputHeight)) return false;
#if ENGINE_ENABLE_NRD
    if (m_nrd && !m_nrd->initialize(device, renderWidth, renderHeight)) {
        std::cerr << "[NRD Warning] Denoiser resize failed; direct ray-traced output remains available.\n";
    }
#endif
    return true;
}

bool RayTracingPipeline::dispatch(D3D12Device& device,
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
                                  DirectX::FXMMATRIX worldToView) {
    auto cmdList = device.getCommandList();
    const D3D12_GPU_DESCRIPTOR_HANDLE outputUav = device.getSrvUavGpuHandle(m_outputUavIndex);
    const uint32_t invalidDescriptor = std::numeric_limits<uint32_t>::max();
    if (!device.isFrameRecording() || !cmdList || !device.getSrvUavHeap() || !m_globalRootSignature ||
        !m_outputTexture || !m_accumulationTexture || !m_historyTexture ||
        !m_stateObject || !m_shaderBindingTable || tlasAddress == 0 || sceneCbAddress == 0 ||
        verticesAddress == 0 || indicesAddress == 0 || materialsAddress == 0 || instancesAddress == 0 ||
        textureArrayHandle.ptr == 0 ||
        outputUav.ptr == 0 || width == 0 || height == 0 ||
        m_defaultEnvironmentSrvIndex == std::numeric_limits<uint32_t>::max() ||
        width != m_renderWidth || height != m_renderHeight || m_outputUavIndex == invalidDescriptor ||
        m_accumulationUavIndex == invalidDescriptor || m_historyUavIndex == invalidDescriptor) {
        std::cerr << "[DXR Error] Refused ray dispatch with an invalid command list, resource address, descriptor, or extent.\n";
        return false;
    }
#if ENGINE_ENABLE_DLSS
    m_dlssOutputIsActive = false;
#endif

    if (m_outputIsShaderResource) {
        device.transitionBarrier(m_outputTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_outputIsShaderResource = false;
    }

    cmdList->SetComputeRootSignature(m_globalRootSignature.Get());
    cmdList->SetComputeRootDescriptorTable(0, device.getSrvUavGpuHandle(m_outputUavIndex));
    cmdList->SetComputeRootShaderResourceView(1, tlasAddress);
    cmdList->SetComputeRootConstantBufferView(2, sceneCbAddress);
    cmdList->SetComputeRootShaderResourceView(3, verticesAddress);
    cmdList->SetComputeRootShaderResourceView(4, indicesAddress);
    cmdList->SetComputeRootShaderResourceView(5, materialsAddress);
    cmdList->SetComputeRootShaderResourceView(6, instancesAddress);
    cmdList->SetComputeRootDescriptorTable(7, textureArrayHandle);
    const uint32_t activeEnvironmentSrvIndex = environmentSrvIndex == invalidDescriptor
        ? m_defaultEnvironmentSrvIndex : environmentSrvIndex;
    cmdList->SetComputeRootDescriptorTable(8, device.getSrvUavGpuHandle(activeEnvironmentSrvIndex));
    cmdList->SetPipelineState1(m_stateObject.Get());

    uint64_t rayGenSectionSize = alignTo(m_rayGenRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    uint64_t missSectionSize   = alignTo(m_missRecordSize * 2, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    D3D12_GPU_VIRTUAL_ADDRESS sbtAddress = m_shaderBindingTable->GetGPUVirtualAddress();

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.RayGenerationShaderRecord.StartAddress = sbtAddress;
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenRecordSize;

    dispatchDesc.MissShaderTable.StartAddress = sbtAddress + rayGenSectionSize;
    dispatchDesc.MissShaderTable.SizeInBytes = m_missRecordSize * 2;
    dispatchDesc.MissShaderTable.StrideInBytes = m_missRecordSize;

    dispatchDesc.HitGroupTable.StartAddress = sbtAddress + rayGenSectionSize + missSectionSize;
    dispatchDesc.HitGroupTable.SizeInBytes = m_hitGroupRecordSize;
    dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupRecordSize;

    dispatchDesc.Width = width;
    dispatchDesc.Height = height;
    dispatchDesc.Depth = 1;

    cmdList->DispatchRays(&dispatchDesc);

    device.uavBarrier(m_outputTexture.Get());
    device.uavBarrier(m_accumulationTexture.Get());
#if ENGINE_ENABLE_NRD
    // NRD reads these UAV-written guides on the same command list. Explicit
    // ordering is required because their resource state remains UAV.
    if (sceneData.enableNRD != 0 && sceneData.renderMode <= 1u) {
        device.uavBarrier(m_nrdSignalTexture.Get());
        device.uavBarrier(m_nrdSpecularSignalTexture.Get());
        device.uavBarrier(m_nrdNormalRoughnessTexture.Get());
        device.uavBarrier(m_nrdViewZTexture.Get());
        device.uavBarrier(m_nrdMotionTexture.Get());
        device.uavBarrier(m_nrdDiffuseFactorTexture.Get());
        device.uavBarrier(m_nrdSpecularFactorTexture.Get());
    }
#endif
#if ENGINE_ENABLE_DLSS
    if (sceneData.enableDLSS != 0 && sceneData.renderMode <= 1u) {
        device.uavBarrier(m_dlssInputColorTexture.Get());
        device.uavBarrier(m_dlssDepthTexture.Get());
        device.uavBarrier(m_dlssMotionTexture.Get());
        device.uavBarrier(m_dlssNormalRoughnessTexture.Get());
        device.uavBarrier(m_dlssDiffuseAlbedoTexture.Get());
        device.uavBarrier(m_dlssSpecularAlbedoTexture.Get());
        device.uavBarrier(m_dlssSpecularHitDistanceTexture.Get());
    }
#endif

    // Copy accumulation buffer to history buffer for next frame's TAA reprojection
    device.transitionBarrier(m_accumulationTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    device.transitionBarrier(m_historyTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(m_historyTexture.Get(), m_accumulationTexture.Get());
    device.transitionBarrier(m_accumulationTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    device.transitionBarrier(m_historyTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

#if ENGINE_ENABLE_NRD
    if (sceneData.enableNRD != 0 && sceneData.renderMode <= 1u && sceneData.enableDLSSRR == 0) {
        compositeNrd(device, sceneCbAddress, sceneData, viewToClip, worldToView);
    } else
#endif
    {
        device.transitionBarrier(m_outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_outputIsShaderResource = true;
    }
#if ENGINE_ENABLE_DLSS
    // Evaluate even when disabled so output status cannot remain stale after
    // the UI turns DLSS off or enters an unsupported scene/render mode.
    evaluateDlss(device, sceneData, viewToClip, worldToView);
#endif
    return true;
}

void RayTracingPipeline::compositeNrd(D3D12Device& device,
                                     D3D12_GPU_VIRTUAL_ADDRESS,
                                     const SceneCB& sceneData,
                                     DirectX::FXMMATRIX viewToClip,
                                     DirectX::FXMMATRIX worldToView) {
#if ENGINE_ENABLE_NRD
    if (!isNrdAvailable() || !m_nrd->denoise(device, sceneData, viewToClip, worldToView,
            m_nrdSignalTexture.Get(), m_nrdSpecularSignalTexture.Get(),
            m_nrdNormalRoughnessTexture.Get(), m_nrdViewZTexture.Get(), m_nrdMotionTexture.Get(),
            m_nrdDiffuseOutputTexture.Get(), m_nrdSpecularOutputTexture.Get(),
            m_nrdOutputsAreShaderResources)) {
        // Keep the raygen shader's current noisy frame visible if NRD rejects
        // a dispatch; the next frame can return to built-in accumulation.
        device.transitionBarrier(m_outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_outputIsShaderResource = true;
        return;
    }

    if (!m_nrdOutputsAreShaderResources) {
        device.transitionBarrier(m_nrdDiffuseOutputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        device.transitionBarrier(m_nrdSpecularOutputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_nrdOutputsAreShaderResources = true;
    }
    device.transitionBarrier(m_accumulationTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    device.transitionBarrier(m_nrdDiffuseFactorTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    device.transitionBarrier(m_nrdSpecularFactorTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    device.transitionBarrier(m_nrdViewZTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    auto* commandList = device.getCommandList();
    ID3D12DescriptorHeap* heaps[] = { device.getSrvUavHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(m_nrdCompositeRootSignature.Get());
    commandList->SetPipelineState(m_nrdCompositePipeline.Get());
    commandList->SetComputeRootDescriptorTable(0, device.getSrvUavGpuHandle(m_outputUavIndex));
#if ENGINE_ENABLE_DLSS
    commandList->SetComputeRootDescriptorTable(1, device.getSrvUavGpuHandle(m_dlssInputColorUavIndex));
    commandList->SetComputeRootDescriptorTable(2, device.getSrvUavGpuHandle(m_accumulationSrvIndex));
#else
    commandList->SetComputeRootDescriptorTable(1, device.getSrvUavGpuHandle(m_accumulationSrvIndex));
#endif
    const uint32_t settings[] = { std::bit_cast<uint32_t>(sceneData.exposure), sceneData.toneMappingMode };
#if ENGINE_ENABLE_DLSS
    commandList->SetComputeRoot32BitConstants(3, 2, settings, 0);
#else
    commandList->SetComputeRoot32BitConstants(2, 2, settings, 0);
#endif
    commandList->Dispatch((m_renderWidth + 7) / 8, (m_renderHeight + 7) / 8, 1);
    device.uavBarrier(m_outputTexture.Get());
#if ENGINE_ENABLE_DLSS
    device.uavBarrier(m_dlssInputColorTexture.Get());
#endif

    device.transitionBarrier(m_accumulationTexture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    device.transitionBarrier(m_nrdDiffuseFactorTexture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    device.transitionBarrier(m_nrdSpecularFactorTexture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    device.transitionBarrier(m_nrdViewZTexture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    device.transitionBarrier(m_outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_outputIsShaderResource = true;
#else
    (void)device;
    (void)sceneData;
    (void)viewToClip;
    (void)worldToView;
#endif
}

void RayTracingPipeline::copyOutputToReadback(D3D12Device& device) {
    if (!m_readbackBuffer || !m_outputTexture) return;
    ID3D12Resource* sourceTexture = m_outputTexture.Get();
    uint32_t copyWidth = m_renderWidth;
    uint32_t copyHeight = m_renderHeight;
    bool* sourceIsShaderResource = &m_outputIsShaderResource;
#if ENGINE_ENABLE_DLSS
    if (m_dlssOutputIsActive && m_dlssDisplayTexture) {
        sourceTexture = m_dlssDisplayTexture.Get();
        copyWidth = m_outputWidth;
        copyHeight = m_outputHeight;
        sourceIsShaderResource = &m_dlssDisplayIsShaderResource;
    }
#endif
    if (!device.isFrameRecording() || !device.getCommandList() || copyWidth == 0 || copyHeight == 0) return;
    device.transitionBarrier(sourceTexture,
        *sourceIsShaderResource ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_readbackBuffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = copyWidth;
    dst.PlacedFootprint.Footprint.Height = copyHeight;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = m_readbackRowPitch;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = sourceTexture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    device.getCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    device.transitionBarrier(sourceTexture, D3D12_RESOURCE_STATE_COPY_SOURCE,
        *sourceIsShaderResource ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_readbackWidth = copyWidth;
    m_readbackHeight = copyHeight;
}

bool RayTracingPipeline::saveReadbackToBmp(D3D12Device& device, const std::string& filename, std::string* failureReason) {
    if (!m_readbackBuffer) {
        if (failureReason) *failureReason = "renderer readback buffer was not allocated";
        char msg[] = "[saveReadbackToBmp] m_readbackBuffer is null!\n";
        OutputDebugStringA(msg);
        std::cout << msg;
        return false;
    }
    uint8_t* mappedData = nullptr;
    HRESULT hr = m_readbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedData));
    if (FAILED(hr)) {
        HRESULT reason = device.getDevice() ? device.getDevice()->GetDeviceRemovedReason() : S_OK;
        if (failureReason) {
            char detail[128];
            snprintf(detail, sizeof(detail), "readback Map failed (hr=0x%08X, device=0x%08X)",
                     static_cast<unsigned int>(hr), static_cast<unsigned int>(reason));
            *failureReason = detail;
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "[saveReadbackToBmp] Map failed hr=0x%08X (DeviceRemovedReason=0x%08X)\n",
                 static_cast<unsigned int>(hr), static_cast<unsigned int>(reason));
        OutputDebugStringA(msg);
        std::cout << msg;
        return false;
    }
    const uint32_t imageWidth = m_readbackWidth ? m_readbackWidth : m_renderWidth;
    const uint32_t imageHeight = m_readbackHeight ? m_readbackHeight : m_renderHeight;
    bool ok = ImageCapture::saveBmp(filename, imageWidth, imageHeight, mappedData, m_readbackRowPitch);
    m_readbackBuffer->Unmap(0, nullptr);
    if (!ok) {
        if (failureReason) *failureReason = "BMP output could not be opened or written";
        char msg[128];
        snprintf(msg, sizeof(msg), "[saveReadbackToBmp] saveBmp returned false for %s\n", filename.c_str());
        OutputDebugStringA(msg);
        std::cout << msg;
    }
    return ok;
}

} // namespace Engine
