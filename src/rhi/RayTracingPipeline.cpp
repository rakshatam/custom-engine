#include "RayTracingPipeline.h"
#include "RayTracingPBR_Bytecode.h"
#include "NRDDenoiser.h"
#if ENGINE_ENABLE_NRD
#include "NrdComposite_Bytecode.h"
#endif
#include "../core/ImageCapture.h"
#include <bit>
#include <cstring>
#include <iterator>
#include <vector>

namespace Engine {

RayTracingPipeline::RayTracingPipeline() = default;
RayTracingPipeline::~RayTracingPipeline() = default;

static inline uint64_t alignTo(uint64_t size, uint64_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

bool RayTracingPipeline::init(D3D12Device& device, uint32_t renderWidth, uint32_t renderHeight) {
    if (!createRootSignature(device)) return false;
    if (!createPipelineState(device)) return false;
    if (!createShaderBindingTable(device)) return false;
    if (!createOutputResources(device, renderWidth, renderHeight)) return false;
#if ENGINE_ENABLE_NRD
    m_nrd = std::make_unique<NRDDenoiser>();
    if (createNrdCompositePipeline(device)) {
        m_nrd->initialize(device, renderWidth, renderHeight);
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

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = ENGINE_ENABLE_NRD ? 8 : 3; // Output, temporal buffers, then NRD signal guides.
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE srvTexRange = {};
    srvTexRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvTexRange.NumDescriptors = 1; // t5: g_Textures (Texture2DArray)
    srvTexRange.BaseShaderRegister = 5;
    srvTexRange.RegisterSpace = 0;
    srvTexRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[8] = {};

    // 0: UAV Table
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &uavRange;
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

    D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    samplerDesc.ShaderRegister = 0; // s0
    samplerDesc.RegisterSpace = 0;
    samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 8;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &samplerDesc;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, errorBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errorBlob))) {
        return false;
    }

    if (FAILED(device.getDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_globalRootSignature)))) {
        return false;
    }

    return true;
}

bool RayTracingPipeline::createNrdCompositePipeline(D3D12Device& device) {
#if ENGINE_ENABLE_NRD
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 4;
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
    // 76-byte RayPayload rounded up for the added primary non-diffuse signal.
    shaderConfig.MaxPayloadSizeInBytes = 80;
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
    pipelineConfig.MaxTraceRecursionDepth = 2; // Primary ray and one bounded specular reflection ray.

    subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[4].pDesc = &pipelineConfig;

    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = 5;
    stateObjectDesc.pSubobjects = subobjects;

    if (FAILED(device.getDevice()->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_stateObject)))) {
        return false;
    }

    m_stateObject.As(&m_stateObjectProperties);
    return true;
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

    m_shaderBindingTable = device.createBuffer(sbtSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

    uint8_t* mappedData = nullptr;
    m_shaderBindingTable->Map(0, nullptr, reinterpret_cast<void**>(&mappedData));

    // 1. RayGen Record
    void* rayGenId = m_stateObjectProperties->GetShaderIdentifier(L"RayGenShader");
    std::memcpy(mappedData, rayGenId, shaderIdSize);

    // 2. Miss Records (Primary Miss + Shadow Miss)
    uint8_t* missPtr = mappedData + rayGenSectionSize;
    void* missId = m_stateObjectProperties->GetShaderIdentifier(L"MissShader");
    std::memcpy(missPtr, missId, shaderIdSize);

    void* shadowMissId = m_stateObjectProperties->GetShaderIdentifier(L"ShadowMissShader");
    std::memcpy(missPtr + m_missRecordSize, shadowMissId, shaderIdSize);

    // 3. HitGroup Record
    uint8_t* hitGroupPtr = mappedData + rayGenSectionSize + missSectionSize;
    void* hitGroupId = m_stateObjectProperties->GetShaderIdentifier(L"HitGroup_PBR");
    std::memcpy(hitGroupPtr, hitGroupId, shaderIdSize);

    m_shaderBindingTable->Unmap(0, nullptr);
    return true;
}

bool RayTracingPipeline::createOutputResources(D3D12Device& device, uint32_t width, uint32_t height) {
    m_renderWidth = width;
    m_renderHeight = height;

    m_outputTexture = device.createTexture2D(
        m_renderWidth, m_renderHeight,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );

    m_accumulationTexture = device.createTexture2D(
        m_renderWidth, m_renderHeight,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );

    m_historyTexture = device.createTexture2D(
        m_renderWidth, m_renderHeight,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );

#if ENGINE_ENABLE_NRD
    m_nrdSignalTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_nrdNormalRoughnessTexture = device.createTexture2D(width, height, DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_nrdViewZTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_nrdMotionTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_nrdAlbedoTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_nrdOutputTexture = device.createTexture2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!m_nrdSignalTexture || !m_nrdNormalRoughnessTexture || !m_nrdViewZTexture ||
        !m_nrdMotionTexture || !m_nrdAlbedoTexture || !m_nrdOutputTexture) return false;
#endif

    if (!m_descriptorsAllocated) {
        m_outputUavIndex = device.allocateSrvUavDescriptor();
        m_accumulationUavIndex = device.allocateSrvUavDescriptor();
        m_historyUavIndex = device.allocateSrvUavDescriptor();
#if ENGINE_ENABLE_NRD
        m_nrdSignalUavIndex = device.allocateSrvUavDescriptor();
        m_nrdNormalRoughnessUavIndex = device.allocateSrvUavDescriptor();
        m_nrdViewZUavIndex = device.allocateSrvUavDescriptor();
        m_nrdMotionUavIndex = device.allocateSrvUavDescriptor();
        m_nrdAlbedoUavIndex = device.allocateSrvUavDescriptor();
#endif
        m_outputSrvIndex = device.allocateSrvUavDescriptor();
#if ENGINE_ENABLE_NRD
        m_accumulationSrvIndex = device.allocateSrvUavDescriptor();
        m_nrdOutputSrvIndex = device.allocateSrvUavDescriptor();
        m_nrdAlbedoSrvIndex = device.allocateSrvUavDescriptor();
        m_nrdViewZSrvIndex = device.allocateSrvUavDescriptor();
#endif
        m_descriptorsAllocated = true;
    }

    // UAV u0 for final output
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc0 = {};
    uavDesc0.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc0.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device.getDevice()->CreateUnorderedAccessView(m_outputTexture.Get(), nullptr, &uavDesc0, device.getSrvUavCpuHandle(m_outputUavIndex));

    // UAV u1 for accumulation buffer
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc1 = {};
    uavDesc1.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uavDesc1.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device.getDevice()->CreateUnorderedAccessView(m_accumulationTexture.Get(), nullptr, &uavDesc1, device.getSrvUavCpuHandle(m_accumulationUavIndex));

    // UAV u2 for the temporal history and progressive accumulation.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc2 = {};
    uavDesc2.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uavDesc2.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device.getDevice()->CreateUnorderedAccessView(m_historyTexture.Get(), nullptr, &uavDesc2, device.getSrvUavCpuHandle(m_historyUavIndex));

#if ENGINE_ENABLE_NRD
    D3D12_UNORDERED_ACCESS_VIEW_DESC nrdUavDesc = {};
    nrdUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    nrdUavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device.getDevice()->CreateUnorderedAccessView(m_nrdSignalTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdSignalUavIndex));
    nrdUavDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    device.getDevice()->CreateUnorderedAccessView(m_nrdNormalRoughnessTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdNormalRoughnessUavIndex));
    nrdUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    device.getDevice()->CreateUnorderedAccessView(m_nrdViewZTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdViewZUavIndex));
    nrdUavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device.getDevice()->CreateUnorderedAccessView(m_nrdMotionTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdMotionUavIndex));
    device.getDevice()->CreateUnorderedAccessView(m_nrdAlbedoTexture.Get(), nullptr, &nrdUavDesc, device.getSrvUavCpuHandle(m_nrdAlbedoUavIndex));
#endif

    // SRV for ImGui Viewport rendering
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device.getDevice()->CreateShaderResourceView(m_outputTexture.Get(), &srvDesc, device.getSrvUavCpuHandle(m_outputSrvIndex));

#if ENGINE_ENABLE_NRD
    D3D12_SHADER_RESOURCE_VIEW_DESC floatSrvDesc = {};
    floatSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    floatSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    floatSrvDesc.Texture2D.MipLevels = 1;
    floatSrvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device.getDevice()->CreateShaderResourceView(m_accumulationTexture.Get(), &floatSrvDesc, device.getSrvUavCpuHandle(m_accumulationSrvIndex));
    device.getDevice()->CreateShaderResourceView(m_nrdOutputTexture.Get(), &floatSrvDesc, device.getSrvUavCpuHandle(m_nrdOutputSrvIndex));
    device.getDevice()->CreateShaderResourceView(m_nrdAlbedoTexture.Get(), &floatSrvDesc, device.getSrvUavCpuHandle(m_nrdAlbedoSrvIndex));
    floatSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    device.getDevice()->CreateShaderResourceView(m_nrdViewZTexture.Get(), &floatSrvDesc, device.getSrvUavCpuHandle(m_nrdViewZSrvIndex));
    m_nrdOutputIsShaderResource = false;
#endif

    m_readbackRowPitch = (m_renderWidth * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    m_readbackBuffer = device.createBuffer(
        static_cast<uint64_t>(m_readbackRowPitch) * m_renderHeight,
        D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST
    );

    return true;
}

void RayTracingPipeline::resizeOutput(D3D12Device& device, uint32_t renderWidth, uint32_t renderHeight) {
    if (renderWidth == m_renderWidth && renderHeight == m_renderHeight) return;
    device.waitForGpu();
    createOutputResources(device, renderWidth, renderHeight);
#if ENGINE_ENABLE_NRD
    if (m_nrd) m_nrd->initialize(device, renderWidth, renderHeight);
#endif
}

void RayTracingPipeline::dispatch(D3D12Device& device,
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
                                  DirectX::FXMMATRIX worldToView) {
    auto cmdList = device.getCommandList();

    device.transitionBarrier(m_outputTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetComputeRootSignature(m_globalRootSignature.Get());
    cmdList->SetComputeRootDescriptorTable(0, device.getSrvUavGpuHandle(m_outputUavIndex));
    cmdList->SetComputeRootShaderResourceView(1, tlasAddress);
    cmdList->SetComputeRootConstantBufferView(2, sceneCbAddress);
    cmdList->SetComputeRootShaderResourceView(3, verticesAddress);
    cmdList->SetComputeRootShaderResourceView(4, indicesAddress);
    cmdList->SetComputeRootShaderResourceView(5, materialsAddress);
    cmdList->SetComputeRootShaderResourceView(6, instancesAddress);
    cmdList->SetComputeRootDescriptorTable(7, textureArrayHandle);

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
    if (sceneData.enableNRD != 0 && sceneData.renderMode == 0) {
        device.uavBarrier(m_nrdSignalTexture.Get());
        device.uavBarrier(m_nrdNormalRoughnessTexture.Get());
        device.uavBarrier(m_nrdViewZTexture.Get());
        device.uavBarrier(m_nrdMotionTexture.Get());
        device.uavBarrier(m_nrdAlbedoTexture.Get());
    }
#endif

    // Copy accumulation buffer to history buffer for next frame's TAA reprojection
    device.transitionBarrier(m_accumulationTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    device.transitionBarrier(m_historyTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(m_historyTexture.Get(), m_accumulationTexture.Get());
    device.transitionBarrier(m_accumulationTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    device.transitionBarrier(m_historyTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

#if ENGINE_ENABLE_NRD
    if (sceneData.enableNRD != 0 && sceneData.renderMode == 0) {
        compositeNrd(device, sceneCbAddress, sceneData, viewToClip, worldToView);
    } else
#endif
    {
        device.transitionBarrier(m_outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

void RayTracingPipeline::compositeNrd(D3D12Device& device,
                                     D3D12_GPU_VIRTUAL_ADDRESS,
                                     const SceneCB& sceneData,
                                     DirectX::FXMMATRIX viewToClip,
                                     DirectX::FXMMATRIX worldToView) {
#if ENGINE_ENABLE_NRD
    if (!isNrdAvailable() || !m_nrd->denoise(device, sceneData, viewToClip, worldToView,
            m_nrdSignalTexture.Get(), m_nrdNormalRoughnessTexture.Get(), m_nrdViewZTexture.Get(),
            m_nrdMotionTexture.Get(), m_nrdOutputTexture.Get(), m_nrdOutputIsShaderResource)) {
        // Keep the raygen shader's current noisy frame visible if NRD rejects
        // a dispatch; the next frame can return to built-in accumulation.
        device.transitionBarrier(m_outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return;
    }

    if (!m_nrdOutputIsShaderResource) {
        device.transitionBarrier(m_nrdOutputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_nrdOutputIsShaderResource = true;
    }
    device.transitionBarrier(m_accumulationTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    device.transitionBarrier(m_nrdAlbedoTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    device.transitionBarrier(m_nrdViewZTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    auto* commandList = device.getCommandList();
    ID3D12DescriptorHeap* heaps[] = { device.getSrvUavHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(m_nrdCompositeRootSignature.Get());
    commandList->SetPipelineState(m_nrdCompositePipeline.Get());
    commandList->SetComputeRootDescriptorTable(0, device.getSrvUavGpuHandle(m_outputUavIndex));
    commandList->SetComputeRootDescriptorTable(1, device.getSrvUavGpuHandle(m_accumulationSrvIndex));
    const uint32_t settings[] = { std::bit_cast<uint32_t>(sceneData.exposure), sceneData.toneMappingMode };
    commandList->SetComputeRoot32BitConstants(2, 2, settings, 0);
    commandList->Dispatch((m_renderWidth + 7) / 8, (m_renderHeight + 7) / 8, 1);
    device.uavBarrier(m_outputTexture.Get());

    device.transitionBarrier(m_accumulationTexture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    device.transitionBarrier(m_nrdAlbedoTexture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    device.transitionBarrier(m_nrdViewZTexture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    device.transitionBarrier(m_outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
#else
    (void)device;
    (void)sceneData;
    (void)viewToClip;
    (void)worldToView;
#endif
}

void RayTracingPipeline::copyOutputToReadback(D3D12Device& device) {
    if (!m_readbackBuffer || !m_outputTexture) return;

    device.transitionBarrier(m_outputTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_readbackBuffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = m_renderWidth;
    dst.PlacedFootprint.Footprint.Height = m_renderHeight;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = m_readbackRowPitch;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = m_outputTexture.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    device.getCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    device.transitionBarrier(m_outputTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
    bool ok = ImageCapture::saveBmp(filename, m_renderWidth, m_renderHeight, mappedData, m_readbackRowPitch);
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
