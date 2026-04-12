#include "wicked_subset_shader_compat.hlsli"

static const uint kPipelineWicked = 0u;
static const uint kPipelineTVB = 1u;
static const uint kPipelineEsoterica = 2u;

static const uint kMaxClusterVertices = 64u;
static const uint kMaxClusterTriangles = 124u;
static const uint kMaxClusterIndices = kMaxClusterTriangles * 3u;

static const uint kArgIndexCountOffset = 0u;
static const uint kArgInstanceCountOffset = 4u;
static const uint kArgStartIndexOffset = 8u;
static const uint kArgBaseVertexOffset = 12u;
static const uint kArgStartInstanceOffset = 16u;
static const uint kIndirectArgStride = 20u;

groupshared uint gTVBVisibleTriCount;

struct InstanceData
{
    row_major float4x4 world;
    float4 color;
    float4 bounds; // xyz: world-space center, w: world-space radius
    float scale;
    float3 _padding0;
};

struct ClusterCommand
{
    uint clusterTemplateIndex;
    uint instanceIndex;
    uint primitiveBase;
    uint _padding0;
};

struct ClusterTemplate
{
    uint indexOffset;
    uint indexCount;
    uint baseVertex;
    uint primitiveOffset;

    uint localVertexOffset;
    uint localVertexCount;
    uint localTriOffset;
    uint localTriCount;

    float4 bounds; // xyz local center, w local radius
};

StructuredBuffer<float3> gVertices : register(t0);
StructuredBuffer<InstanceData> gInstances : register(t1);
StructuredBuffer<ClusterCommand> gCommands : register(t2);
StructuredBuffer<ClusterTemplate> gClusterTemplates : register(t3);
StructuredBuffer<uint> gTemplateVertices : register(t4);
StructuredBuffer<uint> gTemplatePackedTriangles : register(t5);
StructuredBuffer<uint> gInstanceVisible : register(t6);
StructuredBuffer<uint> gVisibleCommandIndices : register(t7);
ByteAddressBuffer gSourceArgs : register(t8);
StructuredBuffer<uint> gTVBFilteredPrimitiveIDs : register(t9);
ByteAddressBuffer gVisibleCount : register(t10);
Texture2D<uint> gPrimitiveIDTexture : register(t11);

RWStructuredBuffer<uint> gInstanceVisibleOut : register(u0);
RWStructuredBuffer<uint> gVisibleCommandIndicesOut : register(u1);
RWByteAddressBuffer gVisibleCountOut : register(u2);
RWByteAddressBuffer gVisibleArgsOut : register(u3);
RWByteAddressBuffer gTVBFilteredIndicesOut : register(u4);
RWByteAddressBuffer gTVBArgsOut : register(u5);
RWStructuredBuffer<uint> gTVBFilteredPrimitiveIDsOut : register(u6);
RWByteAddressBuffer gHashOut : register(u7);
RWByteAddressBuffer gMeshDispatchArgsOut : register(u8);

cbuffer SceneCB : register(b0)
{
    row_major float4x4 viewProj;
    float4 projectionScale; // x = proj00, y = proj11
    float4 viewportSize;    // x = width, y = height
    uint activeCommandCount;
    uint activeInstanceCount;
    uint pipelineStyle;
    uint scenarioMode;
    uint meshUseVisibleList;
    uint3 _padding0;
}

bool SphereVisible(float3 center, float radius)
{
    const float4 clip = mul(float4(center, 1.0), viewProj);
    const float w = abs(clip.w);
    if (w <= 1e-6)
        return false;

    const float radiusX = radius * abs(projectionScale.x);
    const float radiusY = radius * abs(projectionScale.y);
    if (clip.x > w + radiusX || clip.x < -w - radiusX)
        return false;
    if (clip.y > w + radiusY || clip.y < -w - radiusY)
        return false;

    return true;
}

uint PackTri(uint i0, uint i1, uint i2)
{
    return (i0 & 0xFFu) | ((i1 & 0xFFu) << 8u) | ((i2 & 0xFFu) << 16u);
}

uint3 UnpackTri(uint packed)
{
    const uint i0 = packed & 0xFFu;
    const uint i1 = (packed >> 8u) & 0xFFu;
    const uint i2 = (packed >> 16u) & 0xFFu;
    return uint3(i0, i1, i2);
}

bool CullTriangle(float4 clip0, float4 clip1, float4 clip2)
{
    if (clip0.w <= 0.0 || clip1.w <= 0.0 || clip2.w <= 0.0)
    {
        return true;
    }

    if (clip0.x < -clip0.w && clip1.x < -clip1.w && clip2.x < -clip2.w)
        return true;
    if (clip0.x > clip0.w && clip1.x > clip1.w && clip2.x > clip2.w)
        return true;
    if (clip0.y < -clip0.w && clip1.y < -clip1.w && clip2.y < -clip2.w)
        return true;
    if (clip0.y > clip0.w && clip1.y > clip1.w && clip2.y > clip2.w)
        return true;
    if (clip0.z < 0.0 && clip1.z < 0.0 && clip2.z < 0.0)
        return true;
    if (clip0.z > clip0.w && clip1.z > clip1.w && clip2.z > clip2.w)
        return true;

    const float2 ndc0 = clip0.xy / clip0.w;
    const float2 ndc1 = clip1.xy / clip1.w;
    const float2 ndc2 = clip2.xy / clip2.w;

    // Backface cull in NDC space
    const float area = (ndc1.x - ndc0.x) * (ndc2.y - ndc0.y) - (ndc1.y - ndc0.y) * (ndc2.x - ndc0.x);
    if (area >= 0.0)
    {
        return true;
    }

    // Very small primitive culling in pixels
    const float2 mn = min(ndc0, min(ndc1, ndc2));
    const float2 mx = max(ndc0, max(ndc1, ndc2));
    const float2 extentPixels = (mx - mn) * 0.5 * viewportSize.xy;
    if (extentPixels.x < 0.75 && extentPixels.y < 0.75)
    {
        return true;
    }

    return false;
}

void StoreIndexedIndirectArg(RWByteAddressBuffer outBuffer, uint commandIndex, uint indexCount, uint startIndex, uint baseVertex, uint startInstance)
{
    const uint byteOffset = commandIndex * kIndirectArgStride;
    outBuffer.Store(byteOffset + kArgIndexCountOffset, indexCount);
    outBuffer.Store(byteOffset + kArgInstanceCountOffset, 1u);
    outBuffer.Store(byteOffset + kArgStartIndexOffset, startIndex);
    outBuffer.Store(byteOffset + kArgBaseVertexOffset, baseVertex);
    outBuffer.Store(byteOffset + kArgStartInstanceOffset, startInstance);
}

[numthreads(64, 1, 1)]
void cs_instance_filter(uint3 DTid : SV_DispatchThreadID)
{
    const uint instanceIndex = DTid.x;
    if (instanceIndex >= activeInstanceCount)
        return;

    gInstanceVisibleOut[instanceIndex] = 1u;
}

[numthreads(64, 1, 1)]
void cs_cluster_filter(uint3 DTid : SV_DispatchThreadID)
{
    const uint commandIndex = DTid.x;
    if (commandIndex >= activeCommandCount)
        return;

    const ClusterCommand command = gCommands[commandIndex];
    if (gInstanceVisible[command.instanceIndex] == 0u)
        return;

    if (scenarioMode != 0u)
    {
        // Deterministic high-culling mode: keep roughly 1/4 of clusters.
        uint h = commandIndex * 747796405u + 2891336453u;
        h = (h >> ((h >> 28u) + 4u)) ^ h;
        if ((h & 0x3u) != 0u)
            return;
    }

    uint dstIndex = 0u;
    gVisibleCountOut.InterlockedAdd(0u, 1u, dstIndex);
    gVisibleCommandIndicesOut[dstIndex] = commandIndex;
}

[numthreads(64, 1, 1)]
void cs_compact_visible_args(uint3 DTid : SV_DispatchThreadID)
{
    const uint visibleIndex = DTid.x;
    const uint visibleCount = gVisibleCount.Load(0u);
    if (visibleIndex >= visibleCount)
        return;

    const uint commandIndex = gVisibleCommandIndices[visibleIndex];
    const uint srcOffset = commandIndex * kIndirectArgStride;
    const uint dstOffset = visibleIndex * kIndirectArgStride;

    [unroll]
    for (uint i = 0u; i < 5u; ++i)
    {
        const uint value = gSourceArgs.Load(srcOffset + i * 4u);
        gVisibleArgsOut.Store(dstOffset + i * 4u, value);
    }
}

[numthreads(128, 1, 1)]
void cs_tvb_filter(uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
    const uint dispatchIndex = Gid.x;
    uint commandIndex = dispatchIndex;
    uint drawCommandIndex = dispatchIndex;

    if (pipelineStyle == kPipelineEsoterica)
    {
        const uint visibleCount = gVisibleCount.Load(0u);
        if (dispatchIndex >= visibleCount)
            return;
        commandIndex = gVisibleCommandIndices[dispatchIndex];
        drawCommandIndex = dispatchIndex;
    }
    else
    {
        if (dispatchIndex >= activeCommandCount)
            return;
    }

    if (GTid.x == 0u)
    {
        gTVBVisibleTriCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const ClusterCommand command = gCommands[commandIndex];
    const InstanceData inst = gInstances[command.instanceIndex];
    const ClusterTemplate cluster = gClusterTemplates[command.clusterTemplateIndex];

    if (GTid.x < cluster.localTriCount)
    {
        const uint packed = gTemplatePackedTriangles[cluster.localTriOffset + GTid.x];
        const uint3 localTri = UnpackTri(packed);

        const uint localVertex0 = gTemplateVertices[cluster.localVertexOffset + localTri.x];
        const uint localVertex1 = gTemplateVertices[cluster.localVertexOffset + localTri.y];
        const uint localVertex2 = gTemplateVertices[cluster.localVertexOffset + localTri.z];

        const uint vertexIndex0 = cluster.baseVertex + localVertex0;
        const uint vertexIndex1 = cluster.baseVertex + localVertex1;
        const uint vertexIndex2 = cluster.baseVertex + localVertex2;

        const float4 world0 = mul(float4(gVertices[vertexIndex0], 1.0), inst.world);
        const float4 world1 = mul(float4(gVertices[vertexIndex1], 1.0), inst.world);
        const float4 world2 = mul(float4(gVertices[vertexIndex2], 1.0), inst.world);

        const float4 clip0 = mul(world0, viewProj);
        const float4 clip1 = mul(world1, viewProj);
        const float4 clip2 = mul(world2, viewProj);

        const bool culled = CullTriangle(clip0, clip1, clip2);
        if (!culled)
        {
            uint dstTri = 0u;
            InterlockedAdd(gTVBVisibleTriCount, 1u, dstTri);

            const uint filteredIndexOffset = drawCommandIndex * kMaxClusterIndices + dstTri * 3u;
            gTVBFilteredIndicesOut.Store((filteredIndexOffset + 0u) * 4u, localVertex0);
            gTVBFilteredIndicesOut.Store((filteredIndexOffset + 1u) * 4u, localVertex1);
            gTVBFilteredIndicesOut.Store((filteredIndexOffset + 2u) * 4u, localVertex2);

            const uint primitiveOffset = drawCommandIndex * kMaxClusterTriangles + dstTri;
            gTVBFilteredPrimitiveIDsOut[primitiveOffset] = command.primitiveBase + GTid.x;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (GTid.x == 0u)
    {
        StoreIndexedIndirectArg(
            gTVBArgsOut,
            drawCommandIndex,
            gTVBVisibleTriCount * 3u,
            drawCommandIndex * kMaxClusterIndices,
            cluster.baseVertex,
            drawCommandIndex);
    }
}

[numthreads(1, 1, 1)]
void cs_write_mesh_dispatch_args(uint3 DTid : SV_DispatchThreadID)
{
    (void)DTid;
    const uint visibleCount = gVisibleCount.Load(0u);
    gMeshDispatchArgsOut.Store(0u, visibleCount);
    gMeshDispatchArgsOut.Store(4u, 1u);
    gMeshDispatchArgsOut.Store(8u, 1u);
}

[numthreads(8, 8, 1)]
void cs_hash_primitive_id(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= (uint)viewportSize.x || pixel.y >= (uint)viewportSize.y)
        return;

    const uint value = gPrimitiveIDTexture[pixel];
    const uint hashValue = value * 16777619u ^ (pixel.x * 73856093u) ^ (pixel.y * 19349663u);

    uint ignored = 0u;
    gHashOut.InterlockedXor(0u, hashValue, ignored);
}

struct VSOut
{
    float4 position : SV_Position;
    nointerpolation uint commandIndex : COMMANDINDEX;
    nointerpolation uint drawCommandIndex : DRAWCMDINDEX;
};

VSOut vs_indexed(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOut output;

    const uint drawCommandIndex = instanceID;
    uint commandIndex = drawCommandIndex;
    if (pipelineStyle == kPipelineEsoterica)
    {
        commandIndex = gVisibleCommandIndices[drawCommandIndex];
    }

    const ClusterCommand command = gCommands[commandIndex];
    const InstanceData inst = gInstances[command.instanceIndex];

    const float4 worldPos = mul(float4(gVertices[vertexID], 1.0), inst.world);
    output.position = mul(worldPos, viewProj);
    output.commandIndex = commandIndex;
    output.drawCommandIndex = drawCommandIndex;

    return output;
}

uint ps_indexed(VSOut input, uint primitiveID : SV_PrimitiveID) : SV_Target0
{
    if (pipelineStyle == kPipelineTVB || pipelineStyle == kPipelineEsoterica)
    {
        const uint primitiveOffset = input.drawCommandIndex * kMaxClusterTriangles + primitiveID;
        return gTVBFilteredPrimitiveIDs[primitiveOffset] + 1u;
    }

    const ClusterCommand command = gCommands[input.commandIndex];
    return command.primitiveBase + primitiveID + 1u;
}

struct MSVertexOut
{
    float4 position : SV_Position;
};

struct MSPrimitiveOut
{
    nointerpolation uint globalPrimitiveID : GLOBALPRIMITIVEID;
};

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void ms_clusters(
    uint groupThreadID : SV_GroupThreadID,
    uint3 groupID : SV_GroupID,
    out vertices MSVertexOut verts[kMaxClusterVertices],
    out indices uint3 tris[kMaxClusterTriangles],
    out primitives MSPrimitiveOut prims[kMaxClusterTriangles])
{
    uint commandIndex = groupID.x;

    if (meshUseVisibleList != 0u)
    {
        commandIndex = gVisibleCommandIndices[commandIndex];
    }

    const ClusterCommand command = gCommands[commandIndex];
    const ClusterTemplate cluster = gClusterTemplates[command.clusterTemplateIndex];
    const InstanceData inst = gInstances[command.instanceIndex];

    SetMeshOutputCounts(cluster.localVertexCount, cluster.localTriCount);

    for (uint localVertex = groupThreadID; localVertex < cluster.localVertexCount; localVertex += 32u)
    {
        const uint shapeVertex = gTemplateVertices[cluster.localVertexOffset + localVertex];
        const float4 worldPos = mul(float4(gVertices[cluster.baseVertex + shapeVertex], 1.0), inst.world);
        verts[localVertex].position = mul(worldPos, viewProj);
    }

    for (uint localTri = groupThreadID; localTri < cluster.localTriCount; localTri += 32u)
    {
        const uint packed = gTemplatePackedTriangles[cluster.localTriOffset + localTri];
        tris[localTri] = UnpackTri(packed);
        prims[localTri].globalPrimitiveID = command.primitiveBase + localTri + 1u;
    }
}

struct MeshPSIn
{
    float4 position : SV_Position;
    nointerpolation uint globalPrimitiveID : GLOBALPRIMITIVEID;
};

uint ps_mesh(MeshPSIn input) : SV_Target0
{
    return input.globalPrimitiveID;
}
