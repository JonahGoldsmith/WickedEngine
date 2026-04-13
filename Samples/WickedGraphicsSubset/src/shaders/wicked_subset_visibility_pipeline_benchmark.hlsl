#include "wicked_subset_shader_compat.hlsli"

static const uint kPipelineWicked = 0u;
static const uint kPipelineTVB = 1u;
static const uint kPipelineEsoterica = 2u;
static const uint kMaxDispatchGroups = 65535u;

static const uint kMaxClusterVertices = 64u;
static const uint kMaxClusterTriangles = 124u;
static const uint kMaxClusterIndices = kMaxClusterTriangles * 3u;

#ifndef WICKED_SUBSET_BINDLESS
#define WICKED_SUBSET_BINDLESS 0
#endif

// DX12 DrawIndexedIndirectCount path expects a prefixed DrawID root constant.
// Metal also uses DXC (`__hlsl_dx_compiler`) but must keep the portable 20-byte layout.
#if defined(__hlsl_dx_compiler) && !defined(__spirv__) && !defined(__metal__)
static const uint kArgDrawIDOffset = 0u;
static const uint kArgIndexCountOffset = 4u;
static const uint kArgInstanceCountOffset = 8u;
static const uint kArgStartIndexOffset = 12u;
static const uint kArgBaseVertexOffset = 16u;
static const uint kArgStartInstanceOffset = 20u;
static const uint kIndirectArgStride = 24u;
static const uint kIndirectArgDWordCount = 6u;
#else
static const uint kArgIndexCountOffset = 0u;
static const uint kArgInstanceCountOffset = 4u;
static const uint kArgStartIndexOffset = 8u;
static const uint kArgBaseVertexOffset = 12u;
static const uint kArgStartInstanceOffset = 16u;
static const uint kIndirectArgStride = 20u;
static const uint kIndirectArgDWordCount = 5u;
#endif

groupshared uint gTVBVisibleTriCount;
groupshared uint gTVBDispatchValid;
groupshared uint gTVBDrawCommandIndex;
groupshared uint gTVBCoarseVisible;
groupshared uint gTVBTriVisible[kMaxClusterTriangles];
groupshared uint gTVBTriPrefix[kMaxClusterTriangles];
groupshared uint gTVBTriLocalV0[kMaxClusterTriangles];
groupshared uint gTVBTriLocalV1[kMaxClusterTriangles];
groupshared uint gTVBTriLocalV2[kMaxClusterTriangles];
groupshared uint gMeshCommandIndex;
groupshared uint gMeshLocalVertexCount;
groupshared uint gMeshLocalTriCount;
groupshared uint gMeshValid;
groupshared uint gMeshVisibleTriCount;
groupshared uint gMeshVisibleTriIndices[kMaxClusterTriangles];
groupshared float4 gMeshClipPositions[kMaxClusterVertices];

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

groupshared ClusterCommand gTVBCommand;
groupshared InstanceData gTVBInstance;
groupshared ClusterTemplate gTVBCluster;
groupshared uint gTVBLocalVertices[kMaxClusterVertices];
groupshared float4 gTVBClipPositions[kMaxClusterVertices];

#if !WICKED_SUBSET_BINDLESS
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
#endif
Texture2D<uint> gPrimitiveIDTexture : register(t11);
Texture2D<float> gDepthTexture : register(t12);
Texture2D<float> gHiZTexture : register(t13);

#if !WICKED_SUBSET_BINDLESS
RWStructuredBuffer<uint> gInstanceVisibleOut : register(u0);
RWStructuredBuffer<uint> gVisibleCommandIndicesOut : register(u1);
RWByteAddressBuffer gVisibleCountOut : register(u2);
RWByteAddressBuffer gVisibleArgsOut : register(u3);
RWByteAddressBuffer gTVBFilteredIndicesOut : register(u4);
RWByteAddressBuffer gTVBArgsOut : register(u5);
RWStructuredBuffer<uint> gTVBFilteredPrimitiveIDsOut : register(u6);
RWByteAddressBuffer gHashOut : register(u7);
RWByteAddressBuffer gMeshDispatchArgsOut : register(u8);
#endif
RWTexture2D<float> gHiZOut : register(u9);
#if WICKED_SUBSET_BINDLESS
#ifndef descriptor_index
#define descriptor_index(x) (max(0, (x)))
#endif
#if defined(__hlsl_dx_compiler) && !defined(__spirv__) && __SHADER_TARGET_MAJOR >= 6 && __SHADER_TARGET_MINOR >= 6
#define WICKED_SUBSET_TYPED_BINDLESS 1
#else
#define WICKED_SUBSET_TYPED_BINDLESS 0
#endif
#if WICKED_SUBSET_TYPED_BINDLESS
template<typename T>
struct BindlessResource
{
    T operator[](uint index) { return (T)ResourceDescriptorHeap[index]; }
};
static const BindlessResource<StructuredBuffer<float3> > bindless_vertices;
static const BindlessResource<StructuredBuffer<InstanceData> > bindless_instances;
static const BindlessResource<StructuredBuffer<ClusterCommand> > bindless_commands;
static const BindlessResource<StructuredBuffer<ClusterTemplate> > bindless_cluster_templates;
static const BindlessResource<StructuredBuffer<uint> > bindless_uint_buffers;
static const BindlessResource<ByteAddressBuffer> bindless_raw_buffers;
static const BindlessResource<RWStructuredBuffer<uint> > bindless_rwuint_buffers;
static const BindlessResource<RWByteAddressBuffer> bindless_rwbuffers;
// Vulkan/SPIR-V benchmark path uses untyped ByteAddressBuffer bindless access.
// This is more robust across laptop drivers than typed storage-buffer descriptor arrays.
#elif defined(__spirv__)
[[vk::binding(0, DESCRIPTOR_SET_BINDLESS_STORAGE_BUFFER)]] ByteAddressBuffer bindless_buffers[];
[[vk::binding(0, DESCRIPTOR_SET_BINDLESS_STORAGE_BUFFER)]] RWByteAddressBuffer bindless_rwbuffers[];
#else
ByteAddressBuffer bindless_buffers[] : register(space3);
RWByteAddressBuffer bindless_rwbuffers[] : register(space101);
#endif
struct BindlessPush
{
    int vertexBufferSRV;
    int instanceBufferSRV;
    int commandBufferSRV;
    int clusterTemplateBufferSRV;
    int templateVerticesBufferSRV;
    int templateTrianglesBufferSRV;
    int instanceVisibleSRV;
    int visibleCommandIndicesSRV;
    int sourceArgsSRV;
    int tvbFilteredPrimitiveIDsSRV;
    int visibleCountSRV;
    int instanceVisibleUAV;
    int visibleCommandIndicesUAV;
    int visibleCountUAV;
    int visibleArgsUAV;
    int tvbFilteredIndicesUAV;
    int tvbArgsUAV;
    int tvbFilteredPrimitiveIDsUAV;
    int hashUAV;
    int meshDispatchArgsUAV;
};
ConstantBuffer<BindlessPush> bindless : register(b1);
#endif

cbuffer SceneCB : register(b0)
{
    row_major float4x4 viewProj;
    float4 projectionScale; // x = proj00, y = proj11
    float4 viewportSize;    // x = width, y = height
    uint activeCommandCount;
    uint activeInstanceCount;
    uint pipelineStyle;
    uint meshUseVisibleList;
    uint meshCommandOffset;
    uint hiZMipCount;
    uint hiZSourceMip;
    uint hiZEnabled;
    uint hiZValid;
    float cullPaddingPixels;
    float hiZOcclusionBias;
    float _scenePadding0;
    float _scenePadding1;
}

static const uint kVertexStrideBytes = 12u;
static const uint kInstanceStrideBytes = 112u;
static const uint kCommandStrideBytes = 16u;
static const uint kClusterTemplateStrideBytes = 48u;
static const uint kUIntStrideBytes = 4u;

float4 LoadFloat4(ByteAddressBuffer buffer, uint byteOffset)
{
    const uint4 raw = uint4(
        buffer.Load(byteOffset + 0u),
        buffer.Load(byteOffset + 4u),
        buffer.Load(byteOffset + 8u),
        buffer.Load(byteOffset + 12u));
    return asfloat(raw);
}

float3 LoadFloat3(ByteAddressBuffer buffer, uint byteOffset)
{
    const uint3 raw = uint3(
        buffer.Load(byteOffset + 0u),
        buffer.Load(byteOffset + 4u),
        buffer.Load(byteOffset + 8u));
    return asfloat(raw);
}

uint LoadUInt(ByteAddressBuffer buffer, uint byteOffset)
{
    return buffer.Load(byteOffset);
}

#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
StructuredBuffer<float3> VertexBufferSRV() { return bindless_vertices[descriptor_index(bindless.vertexBufferSRV)]; }
StructuredBuffer<InstanceData> InstanceBufferSRV() { return bindless_instances[descriptor_index(bindless.instanceBufferSRV)]; }
StructuredBuffer<ClusterCommand> CommandBufferSRV() { return bindless_commands[descriptor_index(bindless.commandBufferSRV)]; }
StructuredBuffer<ClusterTemplate> ClusterTemplateBufferSRV() { return bindless_cluster_templates[descriptor_index(bindless.clusterTemplateBufferSRV)]; }
StructuredBuffer<uint> TemplateVerticesBufferSRV() { return bindless_uint_buffers[descriptor_index(bindless.templateVerticesBufferSRV)]; }
StructuredBuffer<uint> TemplateTrianglesBufferSRV() { return bindless_uint_buffers[descriptor_index(bindless.templateTrianglesBufferSRV)]; }
StructuredBuffer<uint> InstanceVisibleBufferSRV() { return bindless_uint_buffers[descriptor_index(bindless.instanceVisibleSRV)]; }
StructuredBuffer<uint> VisibleCommandIndicesBufferSRV() { return bindless_uint_buffers[descriptor_index(bindless.visibleCommandIndicesSRV)]; }
StructuredBuffer<uint> TVBFilteredPrimitiveIDsBufferSRV() { return bindless_uint_buffers[descriptor_index(bindless.tvbFilteredPrimitiveIDsSRV)]; }
ByteAddressBuffer SourceArgsBufferSRV() { return bindless_raw_buffers[descriptor_index(bindless.sourceArgsSRV)]; }
ByteAddressBuffer VisibleCountBufferSRV() { return bindless_raw_buffers[descriptor_index(bindless.visibleCountSRV)]; }
RWStructuredBuffer<uint> InstanceVisibleBufferUAV() { return bindless_rwuint_buffers[descriptor_index(bindless.instanceVisibleUAV)]; }
RWStructuredBuffer<uint> VisibleCommandIndicesBufferUAV() { return bindless_rwuint_buffers[descriptor_index(bindless.visibleCommandIndicesUAV)]; }
RWStructuredBuffer<uint> TVBFilteredPrimitiveIDsBufferUAV() { return bindless_rwuint_buffers[descriptor_index(bindless.tvbFilteredPrimitiveIDsUAV)]; }
RWByteAddressBuffer VisibleCountBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.visibleCountUAV)]; }
RWByteAddressBuffer VisibleArgsBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.visibleArgsUAV)]; }
RWByteAddressBuffer TVBFilteredIndicesBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.tvbFilteredIndicesUAV)]; }
RWByteAddressBuffer HashBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.hashUAV)]; }
RWByteAddressBuffer MeshDispatchArgsBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.meshDispatchArgsUAV)]; }
#else
ByteAddressBuffer VertexBufferSRV() { return bindless_buffers[descriptor_index(bindless.vertexBufferSRV)]; }
ByteAddressBuffer InstanceBufferSRV() { return bindless_buffers[descriptor_index(bindless.instanceBufferSRV)]; }
ByteAddressBuffer CommandBufferSRV() { return bindless_buffers[descriptor_index(bindless.commandBufferSRV)]; }
ByteAddressBuffer ClusterTemplateBufferSRV() { return bindless_buffers[descriptor_index(bindless.clusterTemplateBufferSRV)]; }
ByteAddressBuffer TemplateVerticesBufferSRV() { return bindless_buffers[descriptor_index(bindless.templateVerticesBufferSRV)]; }
ByteAddressBuffer TemplateTrianglesBufferSRV() { return bindless_buffers[descriptor_index(bindless.templateTrianglesBufferSRV)]; }
ByteAddressBuffer InstanceVisibleBufferSRV() { return bindless_buffers[descriptor_index(bindless.instanceVisibleSRV)]; }
ByteAddressBuffer VisibleCommandIndicesBufferSRV() { return bindless_buffers[descriptor_index(bindless.visibleCommandIndicesSRV)]; }
ByteAddressBuffer SourceArgsBufferSRV() { return bindless_buffers[descriptor_index(bindless.sourceArgsSRV)]; }
ByteAddressBuffer TVBFilteredPrimitiveIDsBufferSRV() { return bindless_buffers[descriptor_index(bindless.tvbFilteredPrimitiveIDsSRV)]; }
ByteAddressBuffer VisibleCountBufferSRV() { return bindless_buffers[descriptor_index(bindless.visibleCountSRV)]; }
RWByteAddressBuffer InstanceVisibleBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.instanceVisibleUAV)]; }
RWByteAddressBuffer VisibleCommandIndicesBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.visibleCommandIndicesUAV)]; }
RWByteAddressBuffer VisibleCountBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.visibleCountUAV)]; }
RWByteAddressBuffer VisibleArgsBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.visibleArgsUAV)]; }
RWByteAddressBuffer TVBFilteredIndicesBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.tvbFilteredIndicesUAV)]; }
RWByteAddressBuffer TVBFilteredPrimitiveIDsBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.tvbFilteredPrimitiveIDsUAV)]; }
RWByteAddressBuffer HashBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.hashUAV)]; }
RWByteAddressBuffer MeshDispatchArgsBufferUAV() { return bindless_rwbuffers[descriptor_index(bindless.meshDispatchArgsUAV)]; }
#endif
#endif

RWByteAddressBuffer TVBArgsBufferUAV()
{
#if WICKED_SUBSET_BINDLESS
    return bindless_rwbuffers[descriptor_index(bindless.tvbArgsUAV)];
#else
    return gTVBArgsOut;
#endif
}

float3 LoadVertex(uint vertexIndex)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    return VertexBufferSRV()[vertexIndex];
#else
    return LoadFloat3(VertexBufferSRV(), vertexIndex * kVertexStrideBytes);
#endif
#else
    return gVertices[vertexIndex];
#endif
}

InstanceData LoadInstance(uint instanceIndex)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    return InstanceBufferSRV()[instanceIndex];
#else
    const uint base = instanceIndex * kInstanceStrideBytes;
    ByteAddressBuffer buffer = InstanceBufferSRV();
    InstanceData inst;
    inst.world[0] = LoadFloat4(buffer, base + 0u);
    inst.world[1] = LoadFloat4(buffer, base + 16u);
    inst.world[2] = LoadFloat4(buffer, base + 32u);
    inst.world[3] = LoadFloat4(buffer, base + 48u);
    inst.color = LoadFloat4(buffer, base + 64u);
    inst.bounds = LoadFloat4(buffer, base + 80u);
    inst.scale = asfloat(buffer.Load(base + 96u));
    inst._padding0 = asfloat(uint3(
        buffer.Load(base + 100u),
        buffer.Load(base + 104u),
        buffer.Load(base + 108u)));
    return inst;
#endif
#else
    return gInstances[instanceIndex];
#endif
}

ClusterCommand LoadClusterCommand(uint commandIndex)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    return CommandBufferSRV()[commandIndex];
#else
    const uint base = commandIndex * kCommandStrideBytes;
    ByteAddressBuffer buffer = CommandBufferSRV();
    ClusterCommand command;
    command.clusterTemplateIndex = buffer.Load(base + 0u);
    command.instanceIndex = buffer.Load(base + 4u);
    command.primitiveBase = buffer.Load(base + 8u);
    command._padding0 = buffer.Load(base + 12u);
    return command;
#endif
#else
    return gCommands[commandIndex];
#endif
}

ClusterTemplate LoadClusterTemplate(uint clusterTemplateIndex)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    return ClusterTemplateBufferSRV()[clusterTemplateIndex];
#else
    const uint base = clusterTemplateIndex * kClusterTemplateStrideBytes;
    ByteAddressBuffer buffer = ClusterTemplateBufferSRV();
    ClusterTemplate cluster;
    cluster.indexOffset = buffer.Load(base + 0u);
    cluster.indexCount = buffer.Load(base + 4u);
    cluster.baseVertex = buffer.Load(base + 8u);
    cluster.primitiveOffset = buffer.Load(base + 12u);
    cluster.localVertexOffset = buffer.Load(base + 16u);
    cluster.localVertexCount = buffer.Load(base + 20u);
    cluster.localTriOffset = buffer.Load(base + 24u);
    cluster.localTriCount = buffer.Load(base + 28u);
    cluster.bounds = LoadFloat4(buffer, base + 32u);
    return cluster;
#endif
#else
    return gClusterTemplates[clusterTemplateIndex];
#endif
}

uint LoadTemplateVertex(uint index)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    return TemplateVerticesBufferSRV()[index];
#else
    return LoadUInt(TemplateVerticesBufferSRV(), index * kUIntStrideBytes);
#endif
#else
    return gTemplateVertices[index];
#endif
}

uint LoadTemplatePackedTriangle(uint index)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    return TemplateTrianglesBufferSRV()[index];
#else
    return LoadUInt(TemplateTrianglesBufferSRV(), index * kUIntStrideBytes);
#endif
#else
    return gTemplatePackedTriangles[index];
#endif
}

uint LoadInstanceVisible(uint index)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    return InstanceVisibleBufferSRV()[index];
#else
    return LoadUInt(InstanceVisibleBufferSRV(), index * kUIntStrideBytes);
#endif
#else
    return gInstanceVisible[index];
#endif
}

uint LoadVisibleCommandIndex(uint index)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    return VisibleCommandIndicesBufferSRV()[index];
#else
    return LoadUInt(VisibleCommandIndicesBufferSRV(), index * kUIntStrideBytes);
#endif
#else
    return gVisibleCommandIndices[index];
#endif
}

uint LoadSourceArgDWord(uint byteOffset)
{
#if WICKED_SUBSET_BINDLESS
    return SourceArgsBufferSRV().Load(byteOffset);
#else
    return gSourceArgs.Load(byteOffset);
#endif
}

uint LoadVisibleCountValue()
{
#if WICKED_SUBSET_BINDLESS
    return VisibleCountBufferSRV().Load(0u);
#else
    return gVisibleCount.Load(0u);
#endif
}

uint LoadTVBFilteredPrimitiveID(uint index)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    return TVBFilteredPrimitiveIDsBufferSRV()[index];
#else
    return LoadUInt(TVBFilteredPrimitiveIDsBufferSRV(), index * kUIntStrideBytes);
#endif
#else
    return gTVBFilteredPrimitiveIDs[index];
#endif
}

void StoreInstanceVisible(uint index, uint value)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    InstanceVisibleBufferUAV()[index] = value;
#else
    InstanceVisibleBufferUAV().Store(index * kUIntStrideBytes, value);
#endif
#else
    gInstanceVisibleOut[index] = value;
#endif
}

void StoreVisibleCommandIndex(uint index, uint value)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    VisibleCommandIndicesBufferUAV()[index] = value;
#else
    VisibleCommandIndicesBufferUAV().Store(index * kUIntStrideBytes, value);
#endif
#else
    gVisibleCommandIndicesOut[index] = value;
#endif
}

void StoreTVBFilteredPrimitiveID(uint index, uint value)
{
#if WICKED_SUBSET_BINDLESS
#if WICKED_SUBSET_TYPED_BINDLESS
    TVBFilteredPrimitiveIDsBufferUAV()[index] = value;
#else
    TVBFilteredPrimitiveIDsBufferUAV().Store(index * kUIntStrideBytes, value);
#endif
#else
    gTVBFilteredPrimitiveIDsOut[index] = value;
#endif
}

void StoreVisibleArgDWord(uint byteOffset, uint value)
{
#if WICKED_SUBSET_BINDLESS
    VisibleArgsBufferUAV().Store(byteOffset, value);
#else
    gVisibleArgsOut.Store(byteOffset, value);
#endif
}

void StoreTVBFilteredIndexDWord(uint byteOffset, uint value)
{
#if WICKED_SUBSET_BINDLESS
    TVBFilteredIndicesBufferUAV().Store(byteOffset, value);
#else
    gTVBFilteredIndicesOut.Store(byteOffset, value);
#endif
}

void StoreTVBArgDWord(uint byteOffset, uint value)
{
#if WICKED_SUBSET_BINDLESS
    TVBArgsBufferUAV().Store(byteOffset, value);
#else
    gTVBArgsOut.Store(byteOffset, value);
#endif
}

void InterlockedAddVisibleCount(uint value, out uint originalValue)
{
#if WICKED_SUBSET_BINDLESS
    VisibleCountBufferUAV().InterlockedAdd(0u, value, originalValue);
#else
    gVisibleCountOut.InterlockedAdd(0u, value, originalValue);
#endif
}

void StoreMeshDispatchArgDWord(uint byteOffset, uint value)
{
#if WICKED_SUBSET_BINDLESS
    MeshDispatchArgsBufferUAV().Store(byteOffset, value);
#else
    gMeshDispatchArgsOut.Store(byteOffset, value);
#endif
}

void InterlockedXorHash(uint value, out uint originalValue)
{
#if WICKED_SUBSET_BINDLESS
    HashBufferUAV().InterlockedXor(0u, value, originalValue);
#else
    gHashOut.InterlockedXor(0u, value, originalValue);
#endif
}

bool SphereVisible(float3 center, float radius)
{
    const float4 clip = mul(float4(center, 1.0), viewProj);
    const float w = abs(clip.w);
    if (w <= 1e-6)
        return false;

    const float radiusX = radius * abs(projectionScale.x);
    const float radiusY = radius * abs(projectionScale.y);
    const float padX = (2.0f * cullPaddingPixels / max(viewportSize.x, 1.0f)) * w;
    const float padY = (2.0f * cullPaddingPixels / max(viewportSize.y, 1.0f)) * w;
    if (clip.x > w + radiusX + padX || clip.x < -w - radiusX - padX)
        return false;
    if (clip.y > w + radiusY + padY || clip.y < -w - radiusY - padY)
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

bool CullTriangleMesh(float4 clip0, float4 clip1, float4 clip2)
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

    return false;
}

void StoreIndexedIndirectArg(RWByteAddressBuffer outBuffer, uint commandIndex, uint drawID, uint indexCount, uint startIndex, uint baseVertex, uint startInstance)
{
    const uint byteOffset = commandIndex * kIndirectArgStride;
#if defined(__hlsl_dx_compiler) && !defined(__spirv__) && !defined(__metal__)
    outBuffer.Store(byteOffset + kArgDrawIDOffset, drawID);
#endif
    outBuffer.Store(byteOffset + kArgIndexCountOffset, indexCount);
    outBuffer.Store(byteOffset + kArgInstanceCountOffset, 1u);
    outBuffer.Store(byteOffset + kArgStartIndexOffset, startIndex);
    outBuffer.Store(byteOffset + kArgBaseVertexOffset, baseVertex);
    outBuffer.Store(byteOffset + kArgStartInstanceOffset, startInstance);
}

float ComputeApproximateSphereNearDepth(float ndcDepth, float worldRadius, float clipW)
{
    const float depthBias = worldRadius / max(abs(clipW), 1e-4f);
    return saturate(ndcDepth - depthBias);
}

bool IsSphereOccludedByHiZ(float2 uvMin, float2 uvMax, float nearDepth)
{
    if (hiZEnabled == 0u || hiZValid == 0u || hiZMipCount == 0u)
        return false;

    const float2 spanPixels = max(uvMax - uvMin, float2(0.0f, 0.0f)) * viewportSize.xy;
    const float maxSpanPixels = max(spanPixels.x, spanPixels.y);

    // Tiny bounds tend to be unstable for depth-pyramid rejection; keep them.
    if (maxSpanPixels < 2.0f)
        return false;

    const uint maxMip = hiZMipCount - 1u;
    uint mip = (uint)floor(log2(max(maxSpanPixels, 1.0f)));
    mip = min(mip, maxMip);

    uint mipWidth = 1u;
    uint mipHeight = 1u;
    uint mipLevels = 1u;
    gHiZTexture.GetDimensions(mip, mipWidth, mipHeight, mipLevels);

    const uint maxX = mipWidth > 0u ? (mipWidth - 1u) : 0u;
    const uint maxY = mipHeight > 0u ? (mipHeight - 1u) : 0u;

    const float2 uvCenter = 0.5f * (uvMin + uvMax);
    const float2 uvSamples[5] = {
        uvMin,
        float2(uvMax.x, uvMin.y),
        float2(uvMin.x, uvMax.y),
        uvMax,
        uvCenter
    };

    float minDepth = 1.0f;
    [unroll]
    for (uint i = 0u; i < 5u; ++i)
    {
        const float2 uv = saturate(uvSamples[i]);
        const uint px = min((uint)(uv.x * (float)mipWidth), maxX);
        const uint py = min((uint)(uv.y * (float)mipHeight), maxY);
        minDepth = min(minDepth, gHiZTexture.Load(int3(px, py, mip)));
    }

    return nearDepth > (minDepth + hiZOcclusionBias);
}

[numthreads(64, 1, 1)]
void cs_instance_filter(uint3 DTid : SV_DispatchThreadID)
{
    const uint instanceIndex = DTid.x;
    if (instanceIndex >= activeInstanceCount)
        return;

    const InstanceData inst = LoadInstance(instanceIndex);
    const bool visible = SphereVisible(inst.bounds.xyz, inst.bounds.w);
    StoreInstanceVisible(instanceIndex, visible ? 1u : 0u);
}

[numthreads(64, 1, 1)]
void cs_cluster_filter(uint3 DTid : SV_DispatchThreadID)
{
    const uint commandIndex = DTid.x;
    if (commandIndex >= activeCommandCount)
        return;

    const ClusterCommand command = LoadClusterCommand(commandIndex);
    if (LoadInstanceVisible(command.instanceIndex) == 0u)
        return;

    const InstanceData inst = LoadInstance(command.instanceIndex);
    const ClusterTemplate cluster = LoadClusterTemplate(command.clusterTemplateIndex);

    const float3 localCenter = cluster.bounds.xyz;
    const float worldRadius = cluster.bounds.w * inst.scale;
    const float3 worldCenter = mul(float4(localCenter, 1.0), inst.world).xyz;

    if (!SphereVisible(worldCenter, worldRadius))
        return;

    const float4 clipCenter = mul(float4(worldCenter, 1.0), viewProj);
    if (clipCenter.w <= 1e-6f)
        return;

    const float invW = 1.0f / clipCenter.w;
    const float2 ndcCenter = clipCenter.xy * invW;
    const float ndcDepth = saturate(clipCenter.z * invW);
    const float2 ndcRadius = float2(
        worldRadius * abs(projectionScale.x) * abs(invW),
        worldRadius * abs(projectionScale.y) * abs(invW));

    float2 uvMin = (ndcCenter - ndcRadius) * 0.5f + 0.5f;
    float2 uvMax = (ndcCenter + ndcRadius) * 0.5f + 0.5f;
    const float2 uvPadding = float2(
        cullPaddingPixels / max(viewportSize.x, 1.0f),
        cullPaddingPixels / max(viewportSize.y, 1.0f));
    uvMin -= uvPadding;
    uvMax += uvPadding;
    uvMin = saturate(uvMin);
    uvMax = saturate(uvMax);

    if (uvMin.x < uvMax.x && uvMin.y < uvMax.y)
    {
        const float nearDepth = ComputeApproximateSphereNearDepth(ndcDepth, worldRadius, clipCenter.w);
        if (IsSphereOccludedByHiZ(uvMin, uvMax, nearDepth))
            return;
    }

    uint dstIndex = 0u;
    InterlockedAddVisibleCount(1u, dstIndex);
    StoreVisibleCommandIndex(dstIndex, commandIndex);

    const uint srcOffset = commandIndex * kIndirectArgStride;
    const uint dstOffset = dstIndex * kIndirectArgStride;

    [unroll]
    for (uint i = 0u; i < kIndirectArgDWordCount; ++i)
    {
        const uint value = LoadSourceArgDWord(srcOffset + i * 4u);
        StoreVisibleArgDWord(dstOffset + i * 4u, value);
    }
}

[numthreads(64, 1, 1)]
void cs_compact_visible_args(uint3 DTid : SV_DispatchThreadID)
{
    const uint visibleIndex = DTid.x;
    const uint visibleCount = LoadVisibleCountValue();
    if (visibleIndex >= visibleCount)
        return;

    const uint commandIndex = LoadVisibleCommandIndex(visibleIndex);
    const uint srcOffset = commandIndex * kIndirectArgStride;
    const uint dstOffset = visibleIndex * kIndirectArgStride;

    [unroll]
    for (uint i = 0u; i < kIndirectArgDWordCount; ++i)
    {
        const uint value = LoadSourceArgDWord(srcOffset + i * 4u);
        StoreVisibleArgDWord(dstOffset + i * 4u, value);
    }
}

[numthreads(128, 1, 1)]
void cs_tvb_filter(uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
    const uint dispatchLinearIndex = Gid.x + Gid.y * kMaxDispatchGroups;
    const uint dispatchIndex = meshCommandOffset + dispatchLinearIndex;
    if (GTid.x == 0u)
    {
        gTVBVisibleTriCount = 0u;
        gTVBDispatchValid = 0u;
        gTVBCoarseVisible = 0u;
        gTVBDrawCommandIndex = dispatchIndex;

        uint commandIndex = dispatchIndex;
        if (pipelineStyle == kPipelineEsoterica)
        {
            const uint visibleCount = LoadVisibleCountValue();
            if (dispatchIndex < visibleCount)
            {
                commandIndex = LoadVisibleCommandIndex(dispatchIndex);
                gTVBDispatchValid = 1u;
            }
        }
        else
        {
            if (dispatchIndex < activeCommandCount)
            {
                gTVBDispatchValid = 1u;
            }
        }

        if (gTVBDispatchValid != 0u)
        {
            gTVBCommand = LoadClusterCommand(commandIndex);
            gTVBInstance = LoadInstance(gTVBCommand.instanceIndex);
            gTVBCluster = LoadClusterTemplate(gTVBCommand.clusterTemplateIndex);

            const float3 localCenter = gTVBCluster.bounds.xyz;
            const float worldRadius = gTVBCluster.bounds.w * gTVBInstance.scale;
            const float3 worldCenter = mul(float4(localCenter, 1.0), gTVBInstance.world).xyz;

            bool coarseVisible = SphereVisible(worldCenter, worldRadius);
            if (coarseVisible)
            {
                const float4 clipCenter = mul(float4(worldCenter, 1.0), viewProj);
                if (clipCenter.w <= 1e-6f)
                {
                    coarseVisible = false;
                }
                else
                {
                    const float invW = 1.0f / clipCenter.w;
                    const float2 ndcCenter = clipCenter.xy * invW;
                    const float ndcDepth = saturate(clipCenter.z * invW);
                    const float2 ndcRadius = float2(
                        worldRadius * abs(projectionScale.x) * abs(invW),
                        worldRadius * abs(projectionScale.y) * abs(invW));

                    float2 uvMin = (ndcCenter - ndcRadius) * 0.5f + 0.5f;
                    float2 uvMax = (ndcCenter + ndcRadius) * 0.5f + 0.5f;
                    const float2 uvPadding = float2(
                        cullPaddingPixels / max(viewportSize.x, 1.0f),
                        cullPaddingPixels / max(viewportSize.y, 1.0f));
                    uvMin -= uvPadding;
                    uvMax += uvPadding;
                    uvMin = saturate(uvMin);
                    uvMax = saturate(uvMax);

                    if (uvMin.x < uvMax.x && uvMin.y < uvMax.y)
                    {
                        const float nearDepth = ComputeApproximateSphereNearDepth(ndcDepth, worldRadius, clipCenter.w);
                        if (IsSphereOccludedByHiZ(uvMin, uvMax, nearDepth))
                        {
                            coarseVisible = false;
                        }
                    }
                }
            }

            gTVBCoarseVisible = coarseVisible ? 1u : 0u;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gTVBDispatchValid == 0u)
        return;

    if (gTVBCoarseVisible == 0u || gTVBCluster.localTriCount == 0u)
    {
        if (GTid.x == 0u)
        {
            StoreIndexedIndirectArg(
                TVBArgsBufferUAV(),
                gTVBDrawCommandIndex,
                gTVBDrawCommandIndex,
                0u,
                gTVBDrawCommandIndex * kMaxClusterIndices,
                gTVBCluster.baseVertex,
                gTVBDrawCommandIndex);
        }
        return;
    }

    if (GTid.x < gTVBCluster.localVertexCount)
    {
        const uint localVertex = LoadTemplateVertex(gTVBCluster.localVertexOffset + GTid.x);
        gTVBLocalVertices[GTid.x] = localVertex;

        const uint vertexIndex = gTVBCluster.baseVertex + localVertex;
        const float4 world = mul(float4(LoadVertex(vertexIndex), 1.0), gTVBInstance.world);
        gTVBClipPositions[GTid.x] = mul(world, viewProj);
    }
    GroupMemoryBarrierWithGroupSync();

    if (GTid.x < gTVBCluster.localTriCount)
    {
        const uint packed = LoadTemplatePackedTriangle(gTVBCluster.localTriOffset + GTid.x);
        const uint3 localTri = UnpackTri(packed);

        const uint localVertex0 = gTVBLocalVertices[localTri.x];
        const uint localVertex1 = gTVBLocalVertices[localTri.y];
        const uint localVertex2 = gTVBLocalVertices[localTri.z];
        gTVBTriLocalV0[GTid.x] = localVertex0;
        gTVBTriLocalV1[GTid.x] = localVertex1;
        gTVBTriLocalV2[GTid.x] = localVertex2;

        const float4 clip0 = gTVBClipPositions[localTri.x];
        const float4 clip1 = gTVBClipPositions[localTri.y];
        const float4 clip2 = gTVBClipPositions[localTri.z];

        const bool culled = CullTriangle(clip0, clip1, clip2);
        gTVBTriVisible[GTid.x] = culled ? 0u : 1u;
    }

    GroupMemoryBarrierWithGroupSync();

    if (GTid.x == 0u)
    {
        uint prefix = 0u;
        [loop]
        for (uint tri = 0u; tri < gTVBCluster.localTriCount; ++tri)
        {
            gTVBTriPrefix[tri] = prefix;
            prefix += gTVBTriVisible[tri];
        }
        gTVBVisibleTriCount = prefix;
    }

    GroupMemoryBarrierWithGroupSync();

    if (GTid.x < gTVBCluster.localTriCount && gTVBTriVisible[GTid.x] != 0u)
    {
        const uint dstTri = gTVBTriPrefix[GTid.x];
        const uint localVertex0 = gTVBTriLocalV0[GTid.x];
        const uint localVertex1 = gTVBTriLocalV1[GTid.x];
        const uint localVertex2 = gTVBTriLocalV2[GTid.x];

        const uint filteredIndexOffset = gTVBDrawCommandIndex * kMaxClusterIndices + dstTri * 3u;
        StoreTVBFilteredIndexDWord((filteredIndexOffset + 0u) * 4u, localVertex0);
        StoreTVBFilteredIndexDWord((filteredIndexOffset + 1u) * 4u, localVertex1);
        StoreTVBFilteredIndexDWord((filteredIndexOffset + 2u) * 4u, localVertex2);

        const uint primitiveOffset = gTVBDrawCommandIndex * kMaxClusterTriangles + dstTri;
        StoreTVBFilteredPrimitiveID(primitiveOffset, gTVBCommand.primitiveBase + GTid.x);
    }

    GroupMemoryBarrierWithGroupSync();

    if (GTid.x == 0u)
    {
        StoreIndexedIndirectArg(
            TVBArgsBufferUAV(),
            gTVBDrawCommandIndex,
            gTVBDrawCommandIndex,
            gTVBVisibleTriCount * 3u,
            gTVBDrawCommandIndex * kMaxClusterIndices,
            gTVBCluster.baseVertex,
            gTVBDrawCommandIndex);
    }
}

[numthreads(1, 1, 1)]
void cs_write_mesh_dispatch_args(uint3 DTid : SV_DispatchThreadID)
{
    (void)DTid;
    const uint visibleCount = LoadVisibleCountValue();
    const uint remaining = visibleCount > meshCommandOffset ? (visibleCount - meshCommandOffset) : 0u;
    const uint dispatchCountX = min(remaining, kMaxDispatchGroups);
    const uint dispatchCountY = max(1u, (remaining + kMaxDispatchGroups - 1u) / kMaxDispatchGroups);
    StoreMeshDispatchArgDWord(0u, dispatchCountX);
    StoreMeshDispatchArgDWord(4u, dispatchCountY);
    StoreMeshDispatchArgDWord(8u, 1u);
}

[numthreads(8, 8, 1)]
void cs_hiz_init(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= (uint)viewportSize.x || pixel.y >= (uint)viewportSize.y)
        return;

    const float depth = gDepthTexture.Load(int3(pixel, 0));
    gHiZOut[pixel] = depth;
}

[numthreads(8, 8, 1)]
void cs_hiz_downsample(uint3 DTid : SV_DispatchThreadID)
{
    uint srcWidth = 1u;
    uint srcHeight = 1u;
    uint srcLevels = 1u;
    gHiZTexture.GetDimensions(hiZSourceMip, srcWidth, srcHeight, srcLevels);

    const uint dstWidth = max(1u, srcWidth >> 1u);
    const uint dstHeight = max(1u, srcHeight >> 1u);
    const uint2 dstPixel = DTid.xy;
    if (dstPixel.x >= dstWidth || dstPixel.y >= dstHeight)
        return;

    const uint maxX = srcWidth > 0u ? (srcWidth - 1u) : 0u;
    const uint maxY = srcHeight > 0u ? (srcHeight - 1u) : 0u;
    const uint2 src00 = uint2(min(dstPixel.x * 2u, maxX), min(dstPixel.y * 2u, maxY));
    const uint2 src10 = uint2(min(src00.x + 1u, maxX), src00.y);
    const uint2 src01 = uint2(src00.x, min(src00.y + 1u, maxY));
    const uint2 src11 = uint2(src10.x, src01.y);

    const float d00 = gHiZTexture.Load(int3(src00, hiZSourceMip));
    const float d10 = gHiZTexture.Load(int3(src10, hiZSourceMip));
    const float d01 = gHiZTexture.Load(int3(src01, hiZSourceMip));
    const float d11 = gHiZTexture.Load(int3(src11, hiZSourceMip));
    gHiZOut[dstPixel] = min(min(d00, d10), min(d01, d11));
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
    InterlockedXorHash(hashValue, ignored);
}

struct VSOut
{
    float4 position : SV_Position;
    nointerpolation uint commandIndex : COMMANDINDEX;
    nointerpolation uint drawCommandIndex : DRAWCMDINDEX;
};

struct IndexedVSIn
{
    float3 position : POSITION;
    uint drawCommandIndex : COLOR0;
};

VSOut vs_indexed(IndexedVSIn input)
{
    VSOut output;

    const uint drawCommandIndex = input.drawCommandIndex;
    uint commandIndex = drawCommandIndex;
    if (pipelineStyle == kPipelineEsoterica)
    {
        commandIndex = LoadVisibleCommandIndex(drawCommandIndex);
    }

    const ClusterCommand command = LoadClusterCommand(commandIndex);
    const InstanceData inst = LoadInstance(command.instanceIndex);

    const float4 worldPos = mul(float4(input.position, 1.0), inst.world);
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
        return LoadTVBFilteredPrimitiveID(primitiveOffset) + 1u;
    }

    const ClusterCommand command = LoadClusterCommand(input.commandIndex);
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
    if (groupThreadID == 0u)
    {
        gMeshCommandIndex = 0u;
        gMeshLocalVertexCount = 0u;
        gMeshLocalTriCount = 0u;
        gMeshValid = 0u;
        gMeshVisibleTriCount = 0u;

        const uint dispatchCommandIndex = meshCommandOffset + groupID.x;
        if (dispatchCommandIndex < activeCommandCount)
        {
            uint commandIndex = dispatchCommandIndex;
            if (meshUseVisibleList != 0u)
            {
                const uint visibleCount = LoadVisibleCountValue();
                if (visibleCount > 0u)
                {
                    if (dispatchCommandIndex < visibleCount)
                    {
                        commandIndex = LoadVisibleCommandIndex(dispatchCommandIndex);
                        const ClusterCommand c = LoadClusterCommand(commandIndex);
                        const ClusterTemplate cl = LoadClusterTemplate(c.clusterTemplateIndex);
                        gMeshCommandIndex = commandIndex;
                        gMeshLocalVertexCount = cl.localVertexCount;
                        gMeshLocalTriCount = cl.localTriCount;
                        gMeshValid = (cl.localVertexCount > 0u && cl.localTriCount > 0u) ? 1u : 0u;
                    }
                }
                else
                {
                    // Safety fallback: if visible list is temporarily empty, fall back to direct command indexing.
                    // This avoids a full-frame mesh-path blackout while async history is warming up.
                    const ClusterCommand c = LoadClusterCommand(commandIndex);
                    const ClusterTemplate cl = LoadClusterTemplate(c.clusterTemplateIndex);
                    gMeshCommandIndex = commandIndex;
                    gMeshLocalVertexCount = cl.localVertexCount;
                    gMeshLocalTriCount = cl.localTriCount;
                    gMeshValid = (cl.localVertexCount > 0u && cl.localTriCount > 0u) ? 1u : 0u;
                }
            }
            else
            {
                const ClusterCommand c = LoadClusterCommand(commandIndex);
                const ClusterTemplate cl = LoadClusterTemplate(c.clusterTemplateIndex);
                gMeshCommandIndex = commandIndex;
                gMeshLocalVertexCount = cl.localVertexCount;
                gMeshLocalTriCount = cl.localTriCount;
                gMeshValid = 1u;
            }
        }

        if (gMeshValid != 0u)
        {
            if (gMeshLocalVertexCount == 0u || gMeshLocalTriCount == 0u)
            {
                gMeshValid = 0u;
            }
        }

    }
    GroupMemoryBarrierWithGroupSync();

    if (gMeshValid != 0u)
    {
        const ClusterCommand command = LoadClusterCommand(gMeshCommandIndex);
        const ClusterTemplate cluster = LoadClusterTemplate(command.clusterTemplateIndex);
        const InstanceData inst = LoadInstance(command.instanceIndex);

        for (uint localVertex = groupThreadID; localVertex < gMeshLocalVertexCount; localVertex += 32u)
        {
            const uint shapeVertex = LoadTemplateVertex(cluster.localVertexOffset + localVertex);
            const float4 worldPos = mul(float4(LoadVertex(cluster.baseVertex + shapeVertex), 1.0), inst.world);
            gMeshClipPositions[localVertex] = mul(worldPos, viewProj);
        }

        GroupMemoryBarrierWithGroupSync();

        for (uint localTri = groupThreadID; localTri < gMeshLocalTriCount; localTri += 32u)
        {
            const uint packed = LoadTemplatePackedTriangle(cluster.localTriOffset + localTri);
            const uint3 tri = UnpackTri(packed);
            const float4 clip0 = gMeshClipPositions[tri.x];
            const float4 clip1 = gMeshClipPositions[tri.y];
            const float4 clip2 = gMeshClipPositions[tri.z];
            const bool culled = CullTriangleMesh(clip0, clip1, clip2);
            if (!culled)
            {
                uint dstTri = 0u;
                InterlockedAdd(gMeshVisibleTriCount, 1u, dstTri);
                gMeshVisibleTriIndices[dstTri] = localTri;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();
    const uint outputVertexCount = (gMeshValid != 0u) ? gMeshLocalVertexCount : 0u;
    const uint outputTriCount = (gMeshValid != 0u) ? max(gMeshVisibleTriCount, 1u) : 0u;
    SetMeshOutputCounts(outputVertexCount, outputTriCount);

    if (gMeshValid == 0u)
        return;

    const ClusterCommand command = LoadClusterCommand(gMeshCommandIndex);
    const ClusterTemplate cluster = LoadClusterTemplate(command.clusterTemplateIndex);

    for (uint localVertex = groupThreadID; localVertex < gMeshLocalVertexCount; localVertex += 32u)
    {
        verts[localVertex].position = gMeshClipPositions[localVertex];
    }

    if (gMeshVisibleTriCount == 0u)
    {
        if (groupThreadID == 0u)
        {
            const uint packed = LoadTemplatePackedTriangle(cluster.localTriOffset);
            const uint localTri = 0u;
            tris[0u] = UnpackTri(packed);
            prims[0u].globalPrimitiveID = command.primitiveBase + localTri + 1u;
        }
        return;
    }

    for (uint visibleTri = groupThreadID; visibleTri < gMeshVisibleTriCount; visibleTri += 32u)
    {
        const uint localTri = gMeshVisibleTriIndices[visibleTri];
        const uint packed = LoadTemplatePackedTriangle(cluster.localTriOffset + localTri);
        tris[visibleTri] = UnpackTri(packed);
        prims[visibleTri].globalPrimitiveID = command.primitiveBase + localTri + 1u;
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

struct DebugVSOut
{
    float4 position : SV_Position;
};

DebugVSOut vs_debug_fullscreen(uint vertexID : SV_VertexID)
{
    DebugVSOut output;
    const float2 p = float2((vertexID == 2u) ? 3.0f : -1.0f, (vertexID == 1u) ? 3.0f : -1.0f);
    output.position = float4(p, 0.0f, 1.0f);
    return output;
}

float4 ps_debug_visualize(DebugVSOut input) : SV_Target0
{
    uint width = 0u;
    uint height = 0u;
    gPrimitiveIDTexture.GetDimensions(width, height);

    const uint2 pixel = uint2(input.position.xy);
    if (pixel.x >= width || pixel.y >= height)
    {
        return float4(0.03f, 0.04f, 0.06f, 1.0f);
    }

    const uint primitiveID = gPrimitiveIDTexture[pixel];
    if (primitiveID == 0u)
    {
        return float4(0.03f, 0.04f, 0.06f, 1.0f);
    }

    uint h = primitiveID * 1664525u + 1013904223u;
    h ^= h >> 16u;
    const float3 color = 0.25f + 0.75f * float3(
        (float)(h & 255u) / 255.0f,
        (float)((h >> 8u) & 255u) / 255.0f,
        (float)((h >> 16u) & 255u) / 255.0f);
    return float4(color, 1.0f);
}
