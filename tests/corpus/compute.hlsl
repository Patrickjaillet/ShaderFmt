// HLSL compute shader: resource declarations and numthreads attribute
RWStructuredBuffer<float4> gParticles : register(u0);
Texture2D gHeightMap : register(t0);
SamplerState gSampler : register(s0);

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    float4 p = gParticles[dtid.x];
    p.y += gHeightMap.SampleLevel(gSampler, p.xz, 0).r;
    gParticles[dtid.x] = p;
}
