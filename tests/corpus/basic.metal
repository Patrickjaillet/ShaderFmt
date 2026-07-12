#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
};

vertex VertexOut vertex_main(VertexIn in [[stage_in]],
                              constant float4x4 &modelViewProj [[buffer(1)]]) {
    VertexOut out;
    out.position = modelViewProj * float4(in.position, 1.0);
    out.normal = in.normal;
    return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]],
                               constant float3 &lightDir [[buffer(0)]]) {
    float3 n = normalize(in.normal);
    float ndotl = max(dot(n, -lightDir), 0.0);
    // simple lambert term
    return float4(ndotl, ndotl, ndotl, 1.0);
}
