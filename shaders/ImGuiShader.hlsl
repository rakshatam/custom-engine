// ImGuiShader.hlsl - Vertex and Pixel Shaders for Dear ImGui on DirectX 12

cbuffer VertexConstantBuffer : register(b0) {
    float4x4 ProjectionMatrix;
};

struct VS_INPUT {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

PS_INPUT VSMain(VS_INPUT input) {
    PS_INPUT output;
    output.pos = mul(ProjectionMatrix, float4(input.pos.xy, 0.0f, 1.0f));
    output.col = input.col;
    output.uv  = input.uv;
    return output;
}

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

float4 PSMain(PS_INPUT input) : SV_Target {
    return input.col * g_Texture.Sample(g_Sampler, input.uv);
}
