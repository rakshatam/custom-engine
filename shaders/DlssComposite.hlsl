Texture2D<float4> g_DlssHdr : register(t0);
RWTexture2D<float4> g_Display : register(u0);

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

float3 linearToSrgb(float3 x) {
    const float3 low = x * 12.92f;
    const float3 high = 1.055f * pow(max(x, 0.0f), 1.0f / 2.4f) - 0.055f;
    return lerp(low, high, step(0.0031308f, x));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint width, height;
    g_Display.GetDimensions(width, height);
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= width || pixel.y >= height) return;

    const float3 hdr = max(g_DlssHdr[pixel].rgb, 0.0f) * max(g_Exposure, 0.0f);
    float3 mapped = hdr;
    if (g_ToneMappingMode == 0) mapped = applyACES(hdr);
    else if (g_ToneMappingMode == 1) mapped = hdr / (1.0f + hdr);
    else mapped = saturate(hdr);
    g_Display[pixel] = float4(linearToSrgb(mapped), 1.0f);
}
