#include "wicked_subset_shader_compat.hlsli"

struct CubeVertexData
{
    float3 position;
    float3 color;
};

StructuredBuffer<CubeVertexData> gVertices : register(t0);

struct VSIn
{
    float3 position : POSITION;
    float3 color : COLOR0;
};

struct VSOut
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

cbuffer SceneCB : register(b0)
{
    row_major float4x4 viewProj;
    float timeSeconds;
    float3 _padding;
};

VSOut vs_main(VSIn input)
{
    VSOut output;
    output.position = mul(float4(input.position, 1.0), viewProj);
    output.color = input.color;
    return output;
}

float4 ps_main(VSOut input) : SV_Target0
{
    return float4(input.color, 1.0);
}

static const uint kCubeVertexCount = 36;
static const uint kCubePrimitiveCount = 12;
static const uint kMeshThreadCount = 32;

[outputtopology("triangle")]
[numthreads(kMeshThreadCount, 1, 1)]
void ms_main(
    in uint groupThreadID : SV_GroupThreadID,
    in uint3 groupID : SV_GroupID,
    out vertices VSOut verts[kCubeVertexCount],
    out indices uint3 triangles[kCubePrimitiveCount])
{
    SetMeshOutputCounts(kCubeVertexCount, kCubePrimitiveCount);

    const uint cubeIndex = groupID.x;
    const uint vertexBase = cubeIndex * kCubeVertexCount;

    for (uint localVertexIndex = groupThreadID; localVertexIndex < kCubeVertexCount; localVertexIndex += kMeshThreadCount)
    {
        const CubeVertexData v = gVertices[vertexBase + localVertexIndex];

        VSOut output;
        output.position = mul(float4(v.position, 1.0), viewProj);
        output.color = v.color;
        verts[localVertexIndex] = output;
    }

    for (uint triIndex = groupThreadID; triIndex < kCubePrimitiveCount; triIndex += kMeshThreadCount)
    {
        const uint baseVertex = triIndex * 3u;
        triangles[triIndex] = uint3(baseVertex + 0u, baseVertex + 1u, baseVertex + 2u);
    }
}
