cbuffer PerFrame : register(b0) {
    float4x4 uViewProj;
    float3 uLightDir;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
};

float4 PSMain(VSOutput input) : SV_Target {
    float3 n = normalize(input.normal);
    float ndotl = max(dot(n, -uLightDir), 0.0);
    // simple lambert term
    return float4(ndotl, ndotl, ndotl, 1.0);
}
