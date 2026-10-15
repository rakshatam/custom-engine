#include "NRD.hlsli"

Texture2D<float4> g_DirectRadiance : register(t0);
Texture2D<float4> g_DenoisedDiffuse : register(t1);
Texture2D<float4> g_Albedo : register(t2);
Texture2D<float>  g_ViewZ : register(t3);
RWTexture2D<float4> g_Output : register(u0);

cbuffer CompositeSettings : register(b0) {
    float g_Exposure;
    uint  g_ToneMappingMode;
};

float3 applyACES(float3 x) {
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 toneMap(float3 hdr) {
    const float3 exposed = max(hdr, 0.0f) * g_Exposure;
    if (g_ToneMappingMode == 0) return applyACES(exposed);
    if (g_ToneMappingMode == 1) return exposed / (1.0f + exposed);
    return saturate(exposed);
}

float3 linearToSrgb(float3 linearColor) {
    const float3 low = linearColor * 12.92f;
    const float3 high = 1.055f * pow(max(linearColor, 0.0f), 1.0f / 2.4f) - 0.055f;
    return lerp(low, high, step(0.0031308f, linearColor));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint width, height;
    g_Output.GetDimensions(width, height);
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= width || pixel.y >= height) return;

    const float4 direct = g_DirectRadiance[pixel];
    const float viewZ = g_ViewZ[pixel];
    float3 linearColor = direct.rgb;
    if (viewZ < 100000.0f) {
        const float3 denoisedLighting = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(g_DenoisedDiffuse[pixel]).rgb;
        linearColor += denoisedLighting * g_Albedo[pixel].rgb;
    }

    g_Output[pixel] = float4(linearToSrgb(toneMap(linearColor)), 1.0f);
}
