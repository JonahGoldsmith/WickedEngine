#include "wicked_subset_shader_compat.hlsli"

struct VSOut
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

cbuffer CameraCB : register(b0)
{
    row_major float4x4 viewProj;
};

struct CubePushConstants
{
    float4 color;
    float4 rotationAxisAngle;
};

WICKED_SUBSET_PUSHCONSTANT(gPush, CubePushConstants);

static const float3 kCubeVertices[36] =
{
    float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0),
    float3(-1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),

    float3( 1.0, -1.0, -1.0), float3(-1.0, -1.0, -1.0), float3(-1.0,  1.0, -1.0),
    float3( 1.0, -1.0, -1.0), float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0, -1.0),

    float3( 1.0, -1.0,  1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0),
    float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0),

    float3(-1.0, -1.0, -1.0), float3(-1.0, -1.0,  1.0), float3(-1.0,  1.0,  1.0),
    float3(-1.0, -1.0, -1.0), float3(-1.0,  1.0,  1.0), float3(-1.0,  1.0, -1.0),

    float3(-1.0,  1.0,  1.0), float3( 1.0,  1.0,  1.0), float3( 1.0,  1.0, -1.0),
    float3(-1.0,  1.0,  1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0),

    float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0, -1.0,  1.0),
    float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0,  1.0), float3(-1.0, -1.0,  1.0),
};

static const float3 kCubeNormals[36] =
{
    float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 1.0),
    float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 1.0),

    float3(0.0, 0.0, -1.0), float3(0.0, 0.0, -1.0), float3(0.0, 0.0, -1.0),
    float3(0.0, 0.0, -1.0), float3(0.0, 0.0, -1.0), float3(0.0, 0.0, -1.0),

    float3(1.0, 0.0, 0.0), float3(1.0, 0.0, 0.0), float3(1.0, 0.0, 0.0),
    float3(1.0, 0.0, 0.0), float3(1.0, 0.0, 0.0), float3(1.0, 0.0, 0.0),

    float3(-1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0),
    float3(-1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0),

    float3(0.0, 1.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 1.0, 0.0),
    float3(0.0, 1.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 1.0, 0.0),

    float3(0.0, -1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, -1.0, 0.0),
    float3(0.0, -1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, -1.0, 0.0),
};

float3 RotateAroundAxis(float3 v, float3 axis, float angle)
{
    const float s = sin(angle);
    const float c = cos(angle);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

VSOut vs_main(uint vertexId : SV_VertexID)
{
    VSOut output;

    const float axisLenSq = dot(gPush.rotationAxisAngle.xyz, gPush.rotationAxisAngle.xyz);
    const float3 axis = axisLenSq > 1e-6 ? normalize(gPush.rotationAxisAngle.xyz) : float3(0.0, 1.0, 0.0);
    const float angle = gPush.rotationAxisAngle.w;

    const float3 localPos = kCubeVertices[vertexId];
    const float3 worldPos = RotateAroundAxis(localPos, axis, angle);
    const float3 normal = normalize(RotateAroundAxis(kCubeNormals[vertexId], axis, angle));

    output.position = mul(float4(worldPos, 1.0), viewProj);

    const float3 lightDir = normalize(float3(0.35, 0.85, 0.25));
    const float lighting = 0.30 + 0.70 * saturate(dot(normal, lightDir));
    output.color = gPush.color.rgb * lighting;
    return output;
}

float4 ps_main(VSOut input) : SV_Target0
{
    return float4(input.color, gPush.color.a);
}
