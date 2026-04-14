#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#endif
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#ifdef METAL
#undef METAL
#endif

#include "EngineConfig.h"

#ifndef WICKED_SUBSET_BACKEND_VULKAN
#define WICKED_SUBSET_BACKEND_VULKAN 0
#endif
#ifndef WICKED_SUBSET_BACKEND_DX12
#define WICKED_SUBSET_BACKEND_DX12 1
#endif
#ifndef WICKED_SUBSET_BACKEND_METAL
#define WICKED_SUBSET_BACKEND_METAL 2
#endif
#ifndef WICKED_SUBSET_BACKEND
#if defined(__APPLE__)
#define WICKED_SUBSET_BACKEND WICKED_SUBSET_BACKEND_METAL
#elif defined(_WIN32) && WI_ENGINECONFIG_SUBSET_USE_DX12
#define WICKED_SUBSET_BACKEND WICKED_SUBSET_BACKEND_DX12
#else
#define WICKED_SUBSET_BACKEND WICKED_SUBSET_BACKEND_VULKAN
#endif
#endif
#ifndef WICKED_SUBSET_USE_DX12
#define WICKED_SUBSET_USE_DX12 (WICKED_SUBSET_BACKEND == WICKED_SUBSET_BACKEND_DX12)
#endif
#ifndef WICKED_SUBSET_USE_METAL
#define WICKED_SUBSET_USE_METAL (WICKED_SUBSET_BACKEND == WICKED_SUBSET_BACKEND_METAL)
#endif
#ifndef WICKED_SUBSET_USE_VULKAN
#define WICKED_SUBSET_USE_VULKAN (WICKED_SUBSET_BACKEND == WICKED_SUBSET_BACKEND_VULKAN)
#endif

#include "wiGraphicsDevice.h"
#if WICKED_SUBSET_USE_METAL
#include "wiGraphicsDevice_Metal.h"
#elif WICKED_SUBSET_USE_DX12
#include "wiGraphicsDevice_DX12.h"
#else
#include "wiGraphicsDevice_Vulkan.h"
#endif
#include "wiShaderCompiler.h"

#include "platform_window_helpers.h"
#include "flecs.h"

#ifndef WICKED_SUBSET_VISIBILITY_BENCHMARK_SHADER_PATH
#define WICKED_SUBSET_VISIBILITY_BENCHMARK_SHADER_PATH ""
#endif

#ifndef WICKED_SUBSET_ENGINE_SHADER_DIR
#define WICKED_SUBSET_ENGINE_SHADER_DIR ""
#endif

#ifndef WI_ENGINECONFIG_SUBSET_SKIP_SDL_QUIT
#define WI_ENGINECONFIG_SUBSET_SKIP_SDL_QUIT 0
#endif

#if defined(WICKED_MMGR_ENABLED)
#include "../../../forge-mmgr/FluidStudios/MemoryManager/mmgr.h"
extern "C" bool WickedMMGRInitialize(const char* appName);
extern "C" void WickedMMGRShutdown(void);
#endif

namespace
{
using wi::BindFlag;
using wi::BlendState;
using wi::CommandList;
using wi::ComparisonFunc;
using wi::CullMode;
using wi::DepthStencilState;
using wi::DepthWriteMask;
using wi::Format;
using wi::GPUBarrier;
using wi::GPUBuffer;
using wi::GPUBufferDesc;
using wi::GPUQueryHeap;
using wi::GPUQueryHeapDesc;
using wi::InputClassification;
using wi::InputLayout;
using wi::PipelineState;
using wi::PipelineStateDesc;
using wi::PrimitiveTopology;
using wi::QUEUE_COMPUTE;
using wi::QUEUE_GRAPHICS;
using wi::QueueSyncPoint;
using wi::SubmissionToken;
using wi::RasterizerState;
using wi::RenderPassImage;
using wi::ResourceMiscFlag;
using wi::ResourceState;
using wi::Shader;
using wi::ShaderModel;
using wi::ShaderStage;
using wi::SubresourceType;
using wi::SwapChain;
using wi::SwapChainDesc;
using wi::SubmitDesc;
using wi::Texture;
using wi::TextureDesc;
using wi::Usage;
using wi::ValidationMode;

static constexpr uint32_t kMaxClusterVertices = 64u;
static constexpr uint32_t kMaxClusterTriangles = 124u;
static constexpr uint32_t kMaxClusterIndices = kMaxClusterTriangles * 3u;

static constexpr uint32_t kTimestampCount = 8u;
static constexpr uint32_t kTimestampFrameStart = 0u;
static constexpr uint32_t kTimestampCullStart = 1u;
static constexpr uint32_t kTimestampCullEnd = 2u;
static constexpr uint32_t kTimestampDrawStart = 3u;
static constexpr uint32_t kTimestampDrawEnd = 4u;
static constexpr uint32_t kTimestampHashStart = 5u;
static constexpr uint32_t kTimestampHashEnd = 6u;
static constexpr uint32_t kTimestampFrameEnd = 7u;

static constexpr uint32_t kArgByteStride = 20u;
static constexpr float kPi = 3.14159265358979323846f;
static constexpr uint32_t kMaxMeshDispatchGroups = 65535u;
static constexpr uint32_t kCullOutputSlotCount = 2u;
static constexpr float kSceneRenderScale = 0.75f;

uint32_t ComputeMipCount(uint32_t width, uint32_t height)
{
    uint32_t mips = 1u;
    uint32_t dim = std::max(width, height);
    while (dim > 1u)
    {
        dim >>= 1u;
        ++mips;
    }
    return mips;
}

#if defined(WICKED_MMGR_ENABLED)
void* SDLCALL SDLMmgrMalloc(size_t size)
{
    return mmgrAllocator(__FILE__, __LINE__, __FUNCTION__, m_alloc_malloc, sizeof(void*), size);
}

void* SDLCALL SDLMmgrCalloc(size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > ((std::numeric_limits<size_t>::max)() / nmemb))
    {
        return nullptr;
    }
    const size_t total = nmemb * size;
    void* memory = mmgrAllocator(__FILE__, __LINE__, __FUNCTION__, m_alloc_calloc, sizeof(void*), total);
    if (memory != nullptr && total > 0)
    {
        std::memset(memory, 0, total);
    }
    return memory;
}

void* SDLCALL SDLMmgrRealloc(void* mem, size_t size)
{
    return mmgrReallocator(__FILE__, __LINE__, __FUNCTION__, m_alloc_realloc, size, mem);
}

void SDLCALL SDLMmgrFree(void* mem)
{
    mmgrDeallocator(__FILE__, __LINE__, __FUNCTION__, m_alloc_free, mem);
}

bool InstallSDLMemoryOverrides()
{
    if (!WickedMMGRInitialize("wicked_subset_visibility_pipeline_benchmark"))
    {
        std::fprintf(stderr, "WickedMMGRInitialize failed\n");
        return false;
    }

    if (!SDL_SetMemoryFunctions(SDLMmgrMalloc, SDLMmgrCalloc, SDLMmgrRealloc, SDLMmgrFree))
    {
        std::fprintf(stderr, "SDL_SetMemoryFunctions failed: %s\n", SDL_GetError());
        return false;
    }

    return true;
}
#else
bool InstallSDLMemoryOverrides()
{
    return true;
}
#endif

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Mat4
{
    float m[16] = {};
};

Mat4 MatIdentity()
{
    Mat4 result = {};
    result.m[0] = 1.0f;
    result.m[5] = 1.0f;
    result.m[10] = 1.0f;
    result.m[15] = 1.0f;
    return result;
}

Mat4 MatMul(const Mat4& a, const Mat4& b)
{
    Mat4 result = {};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            result.m[row * 4 + col] =
                a.m[row * 4 + 0] * b.m[0 * 4 + col] +
                a.m[row * 4 + 1] * b.m[1 * 4 + col] +
                a.m[row * 4 + 2] * b.m[2 * 4 + col] +
                a.m[row * 4 + 3] * b.m[3 * 4 + col];
        }
    }
    return result;
}

Mat4 MatPerspectiveFovLH(float fovYRadians, float aspect, float zNear, float zFar)
{
    Mat4 result = {};
    const float yScale = 1.0f / std::tan(fovYRadians * 0.5f);
    const float xScale = yScale / std::max(1e-6f, aspect);
    result.m[0] = xScale;
    result.m[5] = yScale;
    result.m[10] = zFar / (zFar - zNear);
    result.m[11] = 1.0f;
    result.m[14] = (-zNear * zFar) / (zFar - zNear);
    return result;
}

float VecDot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 VecSub(const Vec3& a, const Vec3& b)
{
    return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}

Vec3 VecAdd(const Vec3& a, const Vec3& b)
{
    return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z };
}

Vec3 VecScale(const Vec3& v, float s)
{
    return Vec3{ v.x * s, v.y * s, v.z * s };
}

Vec3 VecCross(const Vec3& a, const Vec3& b)
{
    return Vec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

Vec3 VecNormalize(const Vec3& v)
{
    const float lenSq = VecDot(v, v);
    if (lenSq <= 1e-12f)
    {
        return Vec3{ 0.0f, 0.0f, 0.0f };
    }
    const float invLen = 1.0f / std::sqrt(lenSq);
    return Vec3{ v.x * invLen, v.y * invLen, v.z * invLen };
}

Vec3 BuildForwardFromYawPitch(float yawRadians, float pitchRadians)
{
    return VecNormalize(Vec3{
        std::cos(pitchRadians) * std::sin(yawRadians),
        std::sin(pitchRadians),
        std::cos(pitchRadians) * std::cos(yawRadians),
    });
}

Vec3 BuildRightFromForward(const Vec3& forward)
{
    return VecNormalize(VecCross(Vec3{ 0.0f, 1.0f, 0.0f }, forward));
}

Mat4 MatLookAtLH(const Vec3& eye, const Vec3& target, const Vec3& up)
{
    const Vec3 zAxis = VecNormalize(VecSub(target, eye));
    const Vec3 xAxis = VecNormalize(VecCross(up, zAxis));
    const Vec3 yAxis = VecCross(zAxis, xAxis);

    Mat4 result = MatIdentity();
    result.m[0] = xAxis.x;
    result.m[1] = yAxis.x;
    result.m[2] = zAxis.x;

    result.m[4] = xAxis.y;
    result.m[5] = yAxis.y;
    result.m[6] = zAxis.y;

    result.m[8] = xAxis.z;
    result.m[9] = yAxis.z;
    result.m[10] = zAxis.z;

    result.m[12] = -VecDot(xAxis, eye);
    result.m[13] = -VecDot(yAxis, eye);
    result.m[14] = -VecDot(zAxis, eye);
    return result;
}

Mat4 MatScaleTranslate(float scale, const Vec3& translation)
{
    Mat4 result = MatIdentity();
    result.m[0] = scale;
    result.m[5] = scale;
    result.m[10] = scale;
    result.m[12] = translation.x;
    result.m[13] = translation.y;
    result.m[14] = translation.z;
    return result;
}

Mat4 MatScaleRotateYTranslate(float scale, float yawRadians, const Vec3& translation)
{
    Mat4 result = MatIdentity();
    const float c = std::cos(yawRadians);
    const float s = std::sin(yawRadians);
    result.m[0] = c * scale;
    result.m[2] = -s * scale;
    result.m[5] = scale;
    result.m[8] = s * scale;
    result.m[10] = c * scale;
    result.m[12] = translation.x;
    result.m[13] = translation.y;
    result.m[14] = translation.z;
    return result;
}

Vec3 TransformPoint(const Mat4& m, const Vec3& p)
{
    Vec3 result = {};
    result.x = p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + m.m[12];
    result.y = p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + m.m[13];
    result.z = p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14];
    return result;
}

float Max3(float a, float b, float c)
{
    return std::max(a, std::max(b, c));
}

float Hash01(uint32_t x)
{
    x ^= x >> 17;
    x *= 0xED5AD4BBu;
    x ^= x >> 11;
    x *= 0xAC4C1B51u;
    x ^= x >> 15;
    x *= 0x31848BABu;
    x ^= x >> 14;
    return static_cast<float>(x) / static_cast<float>(std::numeric_limits<uint32_t>::max());
}

struct IndirectDrawArgsIndexedInstanced
{
    uint32_t IndexCountPerInstance = 0;
    uint32_t InstanceCount = 0;
    uint32_t StartIndexLocation = 0;
    int32_t BaseVertexLocation = 0;
    uint32_t StartInstanceLocation = 0;
};
static_assert(sizeof(IndirectDrawArgsIndexedInstanced) == 20, "IndirectDrawArgsIndexedInstanced ABI mismatch");

#if WICKED_SUBSET_USE_DX12
// DX12 DrawIndexedIndirectCount command signature prefixes a 32-bit root constant (DrawID).
struct DrawIndexedIndirectCountArgs
{
    uint32_t DrawID = 0;
    IndirectDrawArgsIndexedInstanced Draw = {};
};
static_assert(sizeof(DrawIndexedIndirectCountArgs) == 24, "DrawIndexedIndirectCountArgs ABI mismatch");
#else
using DrawIndexedIndirectCountArgs = IndirectDrawArgsIndexedInstanced;
#endif

struct IndirectDispatchArgs
{
    uint32_t ThreadGroupCountX = 0;
    uint32_t ThreadGroupCountY = 0;
    uint32_t ThreadGroupCountZ = 0;
};
static_assert(sizeof(IndirectDispatchArgs) == 12, "IndirectDispatchArgs ABI mismatch");

struct GPUInstanceData
{
    Mat4 world;
    float color[4] = {};
    float bounds[4] = {};
    float scale = 1.0f;
    float padding[3] = {};
};
static_assert(sizeof(GPUInstanceData) == 112, "GPUInstanceData ABI mismatch");

struct ECSInstanceLink
{
    uint32_t instanceIndex = 0;
};

struct ECSMotion
{
    uint32_t shapeIndex = 0;
    float scale = 1.0f;
    float color[4] = {};
    Vec3 basePosition = {};
    float yawRate = 0.0f;
    float orbitRadius = 0.0f;
    float orbitRate = 0.0f;
    float orbitPhase = 0.0f;
    float bobAmplitude = 0.0f;
    float bobRate = 0.0f;
    float bobPhase = 0.0f;
};

struct ECSDynamicState
{
    Vec3 position = {};
    float yaw = 0.0f;
    float elapsed = 0.0f;
};

struct GPUClusterCommand
{
    uint32_t clusterTemplateIndex = 0;
    uint32_t instanceIndex = 0;
    uint32_t primitiveBase = 0;
    uint32_t padding = 0;
};
static_assert(sizeof(GPUClusterCommand) == 16, "GPUClusterCommand ABI mismatch");

struct GPUClusterTemplate
{
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t baseVertex = 0;
    uint32_t primitiveOffset = 0;

    uint32_t localVertexOffset = 0;
    uint32_t localVertexCount = 0;
    uint32_t localTriOffset = 0;
    uint32_t localTriCount = 0;

    float bounds[4] = {};
};
static_assert(sizeof(GPUClusterTemplate) == 48, "GPUClusterTemplate ABI mismatch");

struct SceneCB
{
    float viewProj[16] = {};
    float projectionScale[4] = {};
    float viewportSize[4] = {};
    uint32_t activeCommandCount = 0;
    uint32_t activeInstanceCount = 0;
    uint32_t pipelineStyle = 0;
    uint32_t meshUseVisibleList = 0;
    uint32_t meshCommandOffset = 0;
    uint32_t hiZMipCount = 0;
    uint32_t hiZSourceMip = 0;
    uint32_t hiZEnabled = 0;
    uint32_t hiZValid = 0;
    float cullPaddingPixels = 0.0f;
    float hiZOcclusionBias = 0.0f;
    float padding[2] = {};
};

struct SubsetBindlessCB
{
    int32_t vertexBufferSRV = -1;
    int32_t instanceBufferSRV = -1;
    int32_t commandBufferSRV = -1;
    int32_t clusterTemplateBufferSRV = -1;
    int32_t templateVerticesBufferSRV = -1;
    int32_t templateTrianglesBufferSRV = -1;
    int32_t instanceVisibleSRV = -1;
    int32_t visibleCommandIndicesSRV = -1;
    int32_t sourceArgsSRV = -1;
    int32_t tvbFilteredPrimitiveIDsSRV = -1;
    int32_t visibleCountSRV = -1;
    int32_t instanceVisibleUAV = -1;
    int32_t visibleCommandIndicesUAV = -1;
    int32_t visibleCountUAV = -1;
    int32_t visibleArgsUAV = -1;
    int32_t tvbFilteredIndicesUAV = -1;
    int32_t tvbArgsUAV = -1;
    int32_t tvbFilteredPrimitiveIDsUAV = -1;
    int32_t hashUAV = -1;
    int32_t meshDispatchArgsUAV = -1;
};
static_assert(sizeof(SubsetBindlessCB) == 80, "SubsetBindlessCB ABI mismatch");

struct ShapeTemplate
{
    std::string name;
    uint32_t vertexOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t clusterStart = 0;
    uint32_t clusterCount = 0;
    Vec3 boundsCenter = {};
    float boundsRadius = 0.0f;
    float color[4] = {};
};

struct MeshShapeCPU
{
    std::string name;
    std::vector<Vec3> vertices;
    std::vector<uint32_t> indices;
    float color[4] = {};
};

struct TierPreset
{
    const char* name = "";
    uint64_t targetTriangles = 0;
};

enum class PipelineStyle : uint32_t
{
    Wicked = 0,
    TVB = 1,
    Esoterica = 2,
};

enum class SuiteMode : uint32_t
{
    Portable = 0,
    Mesh = 1,
};

enum class ScenarioMode : uint32_t
{
    AllVisible = 0,
    HighCulling = 1,
};

enum class BindingMode : uint32_t
{
    Bindful = 0,
    Bindless = 1,
};

struct ComboConfig
{
    SuiteMode suite = SuiteMode::Portable;
    PipelineStyle pipeline = PipelineStyle::Wicked;
    ScenarioMode scenario = ScenarioMode::AllVisible;
    uint32_t tierIndex = 0;
};

struct FrameMetrics
{
    double cpuMs = 0.0;
    double gpuCullMs = 0.0;
    double gpuDrawMs = 0.0;
    double gpuFrameMs = 0.0;
    uint32_t visibleCommands = 0;
    uint32_t hashValue = 0;
};

struct AggregateStats
{
    std::vector<double> cpuMs;
    std::vector<double> gpuCullMs;
    std::vector<double> gpuDrawMs;
    std::vector<double> gpuFrameMs;
    std::vector<uint32_t> visibleCommands;
    std::vector<uint32_t> hashValues;
};

static constexpr std::array<TierPreset, 8> kTierPresets = {
    TierPreset{ "500K", 500'000ull },
    TierPreset{ "1M", 1'000'000ull },
    TierPreset{ "2M", 2'000'000ull },
    TierPreset{ "5M", 5'000'000ull },
    TierPreset{ "10M", 10'000'000ull },
    TierPreset{ "20M", 20'000'000ull },
    TierPreset{ "100M", 100'000'000ull },
    TierPreset{ "150M", 150'000'000ull },
};
static constexpr double kTimedBenchmarkDurationSeconds = 30.0;

const char* PipelineName(PipelineStyle p)
{
    switch (p)
    {
        case PipelineStyle::Wicked:
            return "Wicked";
        case PipelineStyle::TVB:
            return "TVB";
        case PipelineStyle::Esoterica:
            return "Esoterica";
        default:
            return "Unknown";
    }
}

const char* SuiteName(SuiteMode s)
{
    return s == SuiteMode::Mesh ? "Mesh" : "Portable";
}

const char* ScenarioName(ScenarioMode s)
{
    return s == ScenarioMode::HighCulling ? "HighCulling" : "AllVisible";
}

const char* BindingModeName(BindingMode mode)
{
    return mode == BindingMode::Bindless ? "Bindless" : "Bindful";
}

const char* BackendName()
{
#if WICKED_SUBSET_USE_DX12
    return "DX12";
#elif WICKED_SUBSET_USE_METAL
    return "Metal";
#else
    return "Vulkan";
#endif
}

void* GetNativeWindowHandle(SDL_Window* window)
{
#if !defined(__APPLE__) && !defined(_WIN32)
    return window;
#else
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (props == 0)
    {
        return nullptr;
    }
#if defined(__APPLE__)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(_WIN32)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
#endif
}

void SetWorkingDirectoryToExecutableDir(const char* argv0)
{
    if (argv0 == nullptr || argv0[0] == '\0')
    {
        return;
    }

    std::error_code ec;
    std::filesystem::path exePath = std::filesystem::absolute(std::filesystem::path(argv0), ec);
    if (ec)
    {
        return;
    }

    std::filesystem::path dir = exePath.parent_path();
    if (dir.empty())
    {
        return;
    }

    std::filesystem::current_path(dir, ec);
    if (ec)
    {
        std::fprintf(stderr, "Warning: failed to set cwd to executable dir: %s\n", ec.message().c_str());
    }
}

MeshShapeCPU CreateCubeShape()
{
    MeshShapeCPU shape = {};
    shape.name = "Cube";
    shape.color[0] = 0.94f;
    shape.color[1] = 0.41f;
    shape.color[2] = 0.28f;
    shape.color[3] = 1.0f;

    shape.vertices = {
        Vec3{-1.0f, -1.0f, 1.0f}, Vec3{1.0f, -1.0f, 1.0f}, Vec3{1.0f, 1.0f, 1.0f}, Vec3{-1.0f, 1.0f, 1.0f},
        Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, -1.0f}, Vec3{-1.0f, 1.0f, -1.0f},
    };

    shape.indices = {
        0, 1, 2, 0, 2, 3,
        1, 5, 6, 1, 6, 2,
        5, 4, 7, 5, 7, 6,
        4, 0, 3, 4, 3, 7,
        3, 2, 6, 3, 6, 7,
        4, 5, 1, 4, 1, 0,
    };

    return shape;
}

MeshShapeCPU CreatePyramidShape()
{
    MeshShapeCPU shape = {};
    shape.name = "Pyramid";
    shape.color[0] = 0.94f;
    shape.color[1] = 0.84f;
    shape.color[2] = 0.27f;
    shape.color[3] = 1.0f;

    shape.vertices = {
        Vec3{-1.0f, -1.0f, -1.0f},
        Vec3{1.0f, -1.0f, -1.0f},
        Vec3{1.0f, -1.0f, 1.0f},
        Vec3{-1.0f, -1.0f, 1.0f},
        Vec3{0.0f, 1.0f, 0.0f},
    };

    shape.indices = {
        0, 1, 2, 0, 2, 3,
        0, 4, 1,
        1, 4, 2,
        2, 4, 3,
        3, 4, 0,
    };

    return shape;
}

MeshShapeCPU CreateSphereShape(uint32_t latitudeBands, uint32_t longitudeBands)
{
    MeshShapeCPU shape = {};
    shape.name = "Sphere";
    shape.color[0] = 0.28f;
    shape.color[1] = 0.82f;
    shape.color[2] = 0.49f;
    shape.color[3] = 1.0f;

    for (uint32_t lat = 0; lat <= latitudeBands; ++lat)
    {
        const float theta = static_cast<float>(lat) * kPi / static_cast<float>(latitudeBands);
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);

        for (uint32_t lon = 0; lon <= longitudeBands; ++lon)
        {
            const float phi = static_cast<float>(lon) * 2.0f * kPi / static_cast<float>(longitudeBands);
            const float sinPhi = std::sin(phi);
            const float cosPhi = std::cos(phi);
            shape.vertices.push_back(Vec3{
                cosPhi * sinTheta,
                cosTheta,
                sinPhi * sinTheta,
            });
        }
    }

    for (uint32_t lat = 0; lat < latitudeBands; ++lat)
    {
        for (uint32_t lon = 0; lon < longitudeBands; ++lon)
        {
            const uint32_t first = lat * (longitudeBands + 1u) + lon;
            const uint32_t second = first + longitudeBands + 1u;

            shape.indices.push_back(first);
            shape.indices.push_back(second);
            shape.indices.push_back(first + 1u);

            shape.indices.push_back(second);
            shape.indices.push_back(second + 1u);
            shape.indices.push_back(first + 1u);
        }
    }

    return shape;
}

MeshShapeCPU CreateCylinderShape(uint32_t segments)
{
    MeshShapeCPU shape = {};
    shape.name = "Cylinder";
    shape.color[0] = 0.28f;
    shape.color[1] = 0.55f;
    shape.color[2] = 0.91f;
    shape.color[3] = 1.0f;

    const float halfHeight = 1.0f;

    const uint32_t topCenter = 0;
    const uint32_t bottomCenter = 1;
    shape.vertices.push_back(Vec3{ 0.0f, halfHeight, 0.0f });
    shape.vertices.push_back(Vec3{ 0.0f, -halfHeight, 0.0f });

    for (uint32_t i = 0; i < segments; ++i)
    {
        const float a = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * kPi;
        const float x = std::cos(a);
        const float z = std::sin(a);
        shape.vertices.push_back(Vec3{ x, halfHeight, z });
        shape.vertices.push_back(Vec3{ x, -halfHeight, z });
    }

    for (uint32_t i = 0; i < segments; ++i)
    {
        const uint32_t next = (i + 1u) % segments;
        const uint32_t top0 = 2u + i * 2u;
        const uint32_t bot0 = top0 + 1u;
        const uint32_t top1 = 2u + next * 2u;
        const uint32_t bot1 = top1 + 1u;

        shape.indices.push_back(top0);
        shape.indices.push_back(bot0);
        shape.indices.push_back(bot1);
        shape.indices.push_back(top0);
        shape.indices.push_back(bot1);
        shape.indices.push_back(top1);

        shape.indices.push_back(topCenter);
        shape.indices.push_back(top1);
        shape.indices.push_back(top0);

        shape.indices.push_back(bottomCenter);
        shape.indices.push_back(bot0);
        shape.indices.push_back(bot1);
    }

    return shape;
}

MeshShapeCPU CreateTorusShape(uint32_t majorSegments, uint32_t minorSegments, float majorRadius, float minorRadius)
{
    MeshShapeCPU shape = {};
    shape.name = "Torus";
    shape.color[0] = 0.76f;
    shape.color[1] = 0.35f;
    shape.color[2] = 0.93f;
    shape.color[3] = 1.0f;

    for (uint32_t major = 0; major <= majorSegments; ++major)
    {
        const float u = static_cast<float>(major) / static_cast<float>(majorSegments);
        const float majorAngle = u * 2.0f * kPi;
        const float cosMajor = std::cos(majorAngle);
        const float sinMajor = std::sin(majorAngle);

        for (uint32_t minor = 0; minor <= minorSegments; ++minor)
        {
            const float v = static_cast<float>(minor) / static_cast<float>(minorSegments);
            const float minorAngle = v * 2.0f * kPi;
            const float cosMinor = std::cos(minorAngle);
            const float sinMinor = std::sin(minorAngle);

            const float ring = majorRadius + minorRadius * cosMinor;
            shape.vertices.push_back(Vec3{
                ring * cosMajor,
                minorRadius * sinMinor,
                ring * sinMajor,
            });
        }
    }

    const uint32_t rowStride = minorSegments + 1u;
    for (uint32_t major = 0; major < majorSegments; ++major)
    {
        for (uint32_t minor = 0; minor < minorSegments; ++minor)
        {
            const uint32_t i0 = major * rowStride + minor;
            const uint32_t i1 = i0 + 1u;
            const uint32_t i2 = i0 + rowStride;
            const uint32_t i3 = i2 + 1u;

            shape.indices.push_back(i0);
            shape.indices.push_back(i2);
            shape.indices.push_back(i1);

            shape.indices.push_back(i1);
            shape.indices.push_back(i2);
            shape.indices.push_back(i3);
        }
    }

    return shape;
}

class WickedVisibilityPipelineBenchmark
{
public:
    bool Initialize()
    {
        SDL_Log("[WickedVisibilityPipelineBenchmark] Initialize begin");

        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }

        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__APPLE__) && WICKED_SUBSET_USE_METAL
        flags = static_cast<SDL_WindowFlags>(flags | SDL_WINDOW_METAL);
#endif
        window_ = SDL_CreateWindow("Wicked Visibility Pipeline Benchmark", 1440, 900, flags);
        if (window_ == nullptr)
        {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return false;
        }

#if WICKED_SUBSET_USE_VULKAN && !defined(_WIN32)
        nativeWindow_ = window_;
#else
        nativeWindow_ = GetNativeWindowHandle(window_);
        if (nativeWindow_ == nullptr)
        {
            std::fprintf(stderr, "Failed to get native window handle from SDL3 window\n");
            return false;
        }
#endif

#if defined(__APPLE__) && WICKED_SUBSET_USE_METAL
        metalView_ = SDL_Metal_CreateView(window_);
        if (metalView_ == nullptr)
        {
            std::fprintf(stderr, "SDL_Metal_CreateView failed: %s\n", SDL_GetError());
            return false;
        }
        WICConfigureMetalLayerForUncapped(SDL_Metal_GetLayer(metalView_));
#endif

        device_ =
#if WICKED_SUBSET_USE_METAL
#if defined(__APPLE__)
            std::make_unique<wi::GraphicsDevice_Metal>(ValidationMode::Verbose, wi::GPUPreference::Discrete);
#else
#error "WICKED_SUBSET_BACKEND=Metal requires an Apple build."
#endif
#elif WICKED_SUBSET_USE_DX12
#if defined(_WIN32)
            std::make_unique<wi::GraphicsDevice_DX12>(ValidationMode::Verbose, wi::GPUPreference::Discrete);
#else
#error "WICKED_SUBSET_BACKEND=DX12 requires a Windows build."
#endif
#else
            std::make_unique<wi::GraphicsDevice_Vulkan>((wi::platform::window_type)nativeWindow_, ValidationMode::Verbose, wi::GPUPreference::Discrete);
#endif

        if (device_ == nullptr)
        {
            std::fprintf(stderr, "Failed to create Wicked graphics device\n");
            return false;
        }

        supportsMeshShaders_ = device_->CheckCapability(wi::MESH_SHADER);

        if (!CreateSwapchain())
        {
            return false;
        }
        if (!CreatePipelines())
        {
            return false;
        }
        if (!BuildSceneAndBuffers())
        {
            return false;
        }
        if (!CreateTimingResources())
        {
            return false;
        }

        BuildComboList();
        autoRun_ = false;
        autoRunComboIndex_ = 0;
        ComboConfig initialCombo = {};
        initialCombo.suite = activeSuite_;
        initialCombo.pipeline = activePipeline_;
        initialCombo.scenario = activeScenario_;
        initialCombo.tierIndex = std::min(activeTier_, static_cast<uint32_t>(kTierPresets.size() - 1u));
        ApplyCombo(initialCombo);
        ResetCameraToScene();
        LogStartupControls();
        LogRenderPathSummary();
        LogActiveTier();

        SDL_Log(
            "[WickedVisibilityPipelineBenchmark] initialized | backend=%s | mesh=%s | async_compute=%s | token_mode=%s | binding=%s | instances=%u | commands=%u | auto-run=%s | camera=fly",
            BackendName(),
            supportsMeshShaders_ ? "yes" : "no",
            asyncComputeEnabled_ ? "yes" : "no",
            tokenSubmissionEnabled_ ? "yes" : "no",
            BindingModeName(activeBindingMode_),
            totalInstanceCount_,
            totalCommandCount_,
            autoRun_ ? "on" : "off");

        return true;
    }

    void Run()
    {
        bool running = true;
        uint64_t prevTick = SDL_GetPerformanceCounter();
        const uint64_t perfFreq = SDL_GetPerformanceFrequency();
        runPerfFrequency_ = perfFreq;
        runStartTick_ = prevTick;
        runEndTick_ = prevTick;
        runFrameCount_ = 0;
        runCpuMsAccum_ = 0.0;

        while (running)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT)
                {
                    running = false;
                }
                else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED)
                {
                    if (!RecreateSwapchain())
                    {
                        running = false;
                    }
                }
                else if (event.type == SDL_EVENT_KEY_DOWN && event.key.down && !event.key.repeat)
                {
                    HandleKey(event.key.key);
                }
                else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.down && event.button.button == SDL_BUTTON_RIGHT)
                {
                    mouseLookActive_ = true;
                    SDL_SetWindowRelativeMouseMode(window_, true);
                }
                else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT)
                {
                    mouseLookActive_ = false;
                    SDL_SetWindowRelativeMouseMode(window_, false);
                }
                else if (event.type == SDL_EVENT_MOUSE_MOTION && mouseLookActive_)
                {
                    cameraYaw_ += event.motion.xrel * mouseLookSensitivity_;
                    cameraPitch_ -= event.motion.yrel * mouseLookSensitivity_;
                    cameraPitch_ = std::clamp(cameraPitch_, -1.54f, 1.54f);
                }
            }

            if (!running)
            {
                break;
            }

            const uint64_t nowTick = SDL_GetPerformanceCounter();
            const double rawDt = static_cast<double>(nowTick - prevTick) / static_cast<double>(perfFreq);
            prevTick = nowTick;
            const float dt = std::min(static_cast<float>(rawDt), 1.0f / 15.0f);

            FrameMetrics metrics = {};
            if (!RenderFrame(dt, &metrics))
            {
                running = false;
                break;
            }
            ++runFrameCount_;
            runCpuMsAccum_ += metrics.cpuMs;
            runEndTick_ = SDL_GetPerformanceCounter();

            if (autoRun_)
            {
                StepAutoRun(metrics);
            }

            if (timedBenchmarkMode_ && runPerfFrequency_ > 0)
            {
                const uint64_t nowTickTimed = SDL_GetPerformanceCounter();
                const double elapsedSeconds = static_cast<double>(nowTickTimed - timedBenchmarkStartTick_) /
                                              static_cast<double>(runPerfFrequency_);
                if (elapsedSeconds >= kTimedBenchmarkDurationSeconds)
                {
                    timedBenchmarkMode_ = false;
                    autoRun_ = false;
                    requestQuit_ = true;
                    mouseLookActive_ = false;
                    SDL_SetWindowRelativeMouseMode(window_, false);
                    SDL_Log(
                        "[WickedVisibilityPipelineBenchmark] timed benchmark reached %.1fs, closing application",
                        kTimedBenchmarkDurationSeconds);
                    running = false;
                    continue;
                }
            }

            UpdateWindowTitle(metrics);
        }

        runEndTick_ = SDL_GetPerformanceCounter();
    }

    void LogAverageFramerateSummary() const
    {
        if (runFrameCount_ == 0)
        {
            SDL_Log("[WickedVisibilityPipelineBenchmark] average framerate unavailable (no rendered frames)");
            return;
        }

        const double avgCpuMs = runCpuMsAccum_ / static_cast<double>(runFrameCount_);
        const double avgFpsCpu = avgCpuMs > 0.0 ? (1000.0 / avgCpuMs) : 0.0;

        double wallSeconds = 0.0;
        if (runPerfFrequency_ > 0 && runEndTick_ >= runStartTick_)
        {
            wallSeconds = static_cast<double>(runEndTick_ - runStartTick_) / static_cast<double>(runPerfFrequency_);
        }
        const double avgFpsWall = wallSeconds > 0.0 ? (static_cast<double>(runFrameCount_) / wallSeconds) : avgFpsCpu;

        SDL_Log(
            "[WickedVisibilityPipelineBenchmark] average framerate | frames=%llu | avg_fps=%.2f | avg_cpu_fps=%.2f | avg_cpu_ms=%.3f | runtime=%.2fs",
            static_cast<unsigned long long>(runFrameCount_),
            avgFpsWall,
            avgFpsCpu,
            avgCpuMs,
            wallSeconds);
    }

    void Shutdown()
    {
        SDL_Log("[WickedVisibilityPipelineBenchmark] Shutdown begin");

        if (window_ != nullptr)
        {
            SDL_SetWindowRelativeMouseMode(window_, false);
        }

        if (device_ != nullptr)
        {
            device_->WaitForGPU();
        }

        LogAverageFramerateSummary();

        DestroyResources();

        pipelineIndexed_ = {};
        pipelineMesh_ = {};
        pipelineIndexedBindless_ = {};
        pipelineMeshBindless_ = {};
        pipelinePresent_ = {};
        csInstanceFilter_ = {};
        csInstanceFilterBindless_ = {};
        csClusterFilter_ = {};
        csClusterFilterBindless_ = {};
        csCompactArgs_ = {};
        csTVBFilter_ = {};
        csTVBFilterBindless_ = {};
        csHiZInit_ = {};
        csHiZDownsample_ = {};
        csHash_ = {};
        csHashBindless_ = {};
        csMeshArgs_ = {};
        csMeshArgsBindless_ = {};

        vsIndexed_ = {};
        vsIndexedBindless_ = {};
        msCluster_ = {};
        msClusterBindless_ = {};
        psIndexed_ = {};
        psIndexedBindless_ = {};
        psMesh_ = {};
        psMeshBindless_ = {};
        vsPresent_ = {};
        psPresent_ = {};
        bindlessShadersAvailable_ = false;
        activeBindingMode_ = BindingMode::Bindful;

        wi::DestroyInputLayout(indexedInputLayout_);

        swapchain_ = {};
        device_.reset();

#if defined(__APPLE__) && WICKED_SUBSET_USE_METAL
        if (metalView_ != nullptr)
        {
            SDL_Metal_DestroyView(metalView_);
            metalView_ = nullptr;
        }
#endif

        if (window_ != nullptr)
        {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }

#if defined(WICKED_MMGR_ENABLED) && WI_ENGINECONFIG_SUBSET_SKIP_SDL_QUIT
        SDL_Log("[WickedVisibilityPipelineBenchmark] SDL_Quit skipped (MMGR+config)");
#else
        SDL_Quit();
#endif
        SDL_Log("[WickedVisibilityPipelineBenchmark] Shutdown end");
    }

private:
    static double Average(const std::vector<double>& values)
    {
        if (values.empty())
        {
            return 0.0;
        }
        double sum = 0.0;
        for (double v : values)
        {
            sum += v;
        }
        return sum / static_cast<double>(values.size());
    }

    static double Percentile95(std::vector<double> values)
    {
        if (values.empty())
        {
            return 0.0;
        }
        std::sort(values.begin(), values.end());
        const size_t idx = static_cast<size_t>(std::floor(0.95 * static_cast<double>(values.size() - 1)));
        return values[idx];
    }

    static uint32_t MedianU32(std::vector<uint32_t> values)
    {
        if (values.empty())
        {
            return 0;
        }
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    bool RecreateSwapchain()
    {
        if (device_ == nullptr)
        {
            return false;
        }

        device_->WaitForGPU();
        DestroyRenderTargets();
        return CreateSwapchain();
    }

    bool CreateSwapchain()
    {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        if (width <= 0 || height <= 0)
        {
            return false;
        }

        SwapChainDesc desc = {};
        desc.width = static_cast<uint32_t>(width);
        desc.height = static_cast<uint32_t>(height);
        desc.buffer_count = 2;
        desc.format = Format::B8G8R8A8_UNORM;
        desc.vsync = false;
        desc.clear_color[0] = 0.05f;
        desc.clear_color[1] = 0.07f;
        desc.clear_color[2] = 0.10f;
        desc.clear_color[3] = 1.0f;

        if (!device_->CreateSwapChain(&desc, (wi::platform::window_type)nativeWindow_, &swapchain_))
        {
            std::fprintf(stderr, "CreateSwapChain failed\n");
            return false;
        }

        return CreateRenderTargets();
    }

    bool CreateRenderTargets()
    {
        if (swapchain_.desc.width == 0 || swapchain_.desc.height == 0)
        {
            return false;
        }

        TextureDesc idDesc = {};
        idDesc.width = swapchain_.desc.width;
        idDesc.height = swapchain_.desc.height;
        idDesc.format = Format::R32_UINT;
        idDesc.bind_flags = BindFlag::RENDER_TARGET | BindFlag::BIND_SHADER_RESOURCE;
        idDesc.layout = ResourceState::SHADER_RESOURCE;
        if (!device_->CreateTexture(&idDesc, nullptr, &primitiveIDTexture_))
        {
            std::fprintf(stderr, "CreateTexture(primitiveIDTexture) failed\n");
            return false;
        }

        TextureDesc depthDesc = {};
        depthDesc.width = swapchain_.desc.width;
        depthDesc.height = swapchain_.desc.height;
        depthDesc.format = Format::D32_FLOAT;
        depthDesc.bind_flags = BindFlag::DEPTH_STENCIL | BindFlag::BIND_SHADER_RESOURCE;
        depthDesc.clear.depth_stencil.depth = 1.0f;
        depthDesc.layout = ResourceState::SHADER_RESOURCE;
        if (!device_->CreateTexture(&depthDesc, nullptr, &depthTexture_))
        {
            std::fprintf(stderr, "CreateTexture(depthTexture) failed\n");
            return false;
        }

        TextureDesc hiZDesc = {};
        hiZDesc.width = swapchain_.desc.width;
        hiZDesc.height = swapchain_.desc.height;
        hiZDesc.format = Format::R32_FLOAT;
        hiZDesc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        hiZDesc.layout = ResourceState::SHADER_RESOURCE;
        hiZDesc.mip_levels = ComputeMipCount(hiZDesc.width, hiZDesc.height);
        if (!device_->CreateTexture(&hiZDesc, nullptr, &hiZTexture_))
        {
            std::fprintf(stderr, "CreateTexture(hiZTexture) failed\n");
            return false;
        }
        device_->CreateMipgenSubresources(hiZTexture_);
        hiZMipCount_ = hiZDesc.mip_levels;
        hiZOcclusionValid_ = false;

        device_->SetName(&primitiveIDTexture_, "subset_visibility_benchmark_primitive_id");
        device_->SetName(&depthTexture_, "subset_visibility_benchmark_depth");
        device_->SetName(&hiZTexture_, "subset_visibility_benchmark_hiz");

        return true;
    }

    void DestroyRenderTargets()
    {
        primitiveIDTexture_ = {};
        depthTexture_ = {};
        hiZTexture_ = {};
        hiZMipCount_ = 1u;
        hiZOcclusionValid_ = false;
    }

    bool CompileShader(ShaderStage stage, const char* entrypoint, Shader* shader, bool bindless = false)
    {
        wi::shadercompiler::CompilerInput input;
        input.flags = wi::shadercompiler::Flags::STRIP_REFLECTION;
        input.format = device_->GetShaderFormat();
        input.stage = stage;
        input.minshadermodel = ShaderModel::SM_6_6;
        input.entrypoint = entrypoint;
        input.shadersourcefilename = WICKED_SUBSET_VISIBILITY_BENCHMARK_SHADER_PATH;
        input.include_directories.push_back(std::filesystem::path(WICKED_SUBSET_VISIBILITY_BENCHMARK_SHADER_PATH).parent_path().string());
        if (WICKED_SUBSET_ENGINE_SHADER_DIR[0] != '\0')
        {
            input.include_directories.push_back(WICKED_SUBSET_ENGINE_SHADER_DIR);
        }
        if (bindless)
        {
            input.defines.push_back("WICKED_SUBSET_BINDLESS=1");
        }

        wi::shadercompiler::CompilerOutput output;
        wi::shadercompiler::Compile(input, output);
        if (!output.IsValid())
        {
            std::fprintf(
                stderr,
                "Shader compile failed (%s, binding=%s): %s\n",
                entrypoint,
                bindless ? "bindless" : "bindful",
                output.error_message.c_str());
            return false;
        }

        return device_->CreateShader(stage, output.shaderdata, output.shadersize, shader, entrypoint);
    }

    bool CreatePipelines()
    {
        wi::InitRasterizerState(rasterState_);
        rasterState_.cull_mode = CullMode::CULL_NONE;
        rasterState_.depth_clip_enable = true;

        wi::InitDepthStencilState(depthStencilState_);
        depthStencilState_.depth_enable = true;
        depthStencilState_.depth_write_mask = DepthWriteMask::ALL;
        depthStencilState_.depth_func = ComparisonFunc::LESS_EQUAL;

        wi::InitBlendState(blendState_);
        blendState_.alpha_to_coverage_enable = false;
        blendState_.render_target[0].blend_enable = false;

        wi::DestroyInputLayout(indexedInputLayout_);
        arrsetlen(indexedInputLayout_.elements, 2);
        for (size_t i = 0; i < 2; ++i)
        {
            wi::InitInputLayoutElement(indexedInputLayout_.elements[i]);
        }
        indexedInputLayout_.elements[0].semantic_name = wi::CloneCString("POSITION");
        indexedInputLayout_.elements[0].semantic_index = 0;
        indexedInputLayout_.elements[0].format = Format::R32G32B32_FLOAT;
        indexedInputLayout_.elements[0].input_slot = 0;
        indexedInputLayout_.elements[0].aligned_byte_offset = 0;
        indexedInputLayout_.elements[0].input_slot_class = InputClassification::PER_VERTEX_DATA;

        indexedInputLayout_.elements[1].semantic_name = wi::CloneCString("COLOR");
        indexedInputLayout_.elements[1].semantic_index = 0;
        indexedInputLayout_.elements[1].format = Format::R32_UINT;
        indexedInputLayout_.elements[1].input_slot = 1;
        indexedInputLayout_.elements[1].aligned_byte_offset = 0;
        indexedInputLayout_.elements[1].input_slot_class = InputClassification::PER_INSTANCE_DATA;

        if (!CompileShader(ShaderStage::VS, "vs_indexed", &vsIndexed_))
            return false;
        if (!CompileShader(ShaderStage::PS, "ps_indexed", &psIndexed_))
            return false;
        if (!CompileShader(ShaderStage::VS, "vs_debug_fullscreen", &vsPresent_))
            return false;
        if (!CompileShader(ShaderStage::PS, "ps_debug_visualize", &psPresent_))
            return false;
        if (!CompileShader(ShaderStage::CS, "cs_instance_filter", &csInstanceFilter_))
            return false;
        if (!CompileShader(ShaderStage::CS, "cs_cluster_filter", &csClusterFilter_))
            return false;
        if (!CompileShader(ShaderStage::CS, "cs_compact_visible_args", &csCompactArgs_))
            return false;
        if (!CompileShader(ShaderStage::CS, "cs_tvb_filter", &csTVBFilter_))
            return false;
        if (!CompileShader(ShaderStage::CS, "cs_hiz_init", &csHiZInit_))
            return false;
        if (!CompileShader(ShaderStage::CS, "cs_hiz_downsample", &csHiZDownsample_))
            return false;
        if (!CompileShader(ShaderStage::CS, "cs_hash_primitive_id", &csHash_))
            return false;
        if (!CompileShader(ShaderStage::CS, "cs_write_mesh_dispatch_args", &csMeshArgs_))
            return false;

        PipelineStateDesc pso = {};
        pso.vs = &vsIndexed_;
        pso.ps = &psIndexed_;
        pso.il = &indexedInputLayout_;
        pso.rs = &rasterState_;
        pso.dss = &depthStencilState_;
        pso.bs = &blendState_;
        pso.pt = PrimitiveTopology::TRIANGLELIST;
        if (!device_->CreatePipelineState(&pso, &pipelineIndexed_))
        {
            std::fprintf(stderr, "CreatePipelineState(indexed) failed\n");
            return false;
        }

        PipelineStateDesc presentPSO = {};
        presentPSO.vs = &vsPresent_;
        presentPSO.ps = &psPresent_;
        presentPSO.rs = &rasterState_;
        presentPSO.bs = &blendState_;
        presentPSO.pt = PrimitiveTopology::TRIANGLELIST;
        if (!device_->CreatePipelineState(&presentPSO, &pipelinePresent_))
        {
            std::fprintf(stderr, "CreatePipelineState(present) failed\n");
            return false;
        }

        if (supportsMeshShaders_)
        {
            if (!CompileShader(ShaderStage::MS, "ms_clusters", &msCluster_))
                return false;
            if (!CompileShader(ShaderStage::PS, "ps_mesh", &psMesh_))
                return false;

            PipelineStateDesc meshPSO = {};
            meshPSO.ms = &msCluster_;
            meshPSO.ps = &psMesh_;
            meshPSO.rs = &rasterState_;
            meshPSO.dss = &depthStencilState_;
            meshPSO.bs = &blendState_;
            meshPSO.pt = PrimitiveTopology::TRIANGLELIST;
            if (!device_->CreatePipelineState(&meshPSO, &pipelineMesh_))
            {
                std::fprintf(stderr, "CreatePipelineState(mesh) failed\n");
                return false;
            }
        }

        bindlessShadersAvailable_ = false;
        bool bindlessReady = true;
        bindlessReady &= CompileShader(ShaderStage::VS, "vs_indexed", &vsIndexedBindless_, true);
        bindlessReady &= CompileShader(ShaderStage::PS, "ps_indexed", &psIndexedBindless_, true);
        bindlessReady &= CompileShader(ShaderStage::CS, "cs_instance_filter", &csInstanceFilterBindless_, true);
        bindlessReady &= CompileShader(ShaderStage::CS, "cs_cluster_filter", &csClusterFilterBindless_, true);
        bindlessReady &= CompileShader(ShaderStage::CS, "cs_tvb_filter", &csTVBFilterBindless_, true);
        bindlessReady &= CompileShader(ShaderStage::CS, "cs_hash_primitive_id", &csHashBindless_, true);
        bindlessReady &= CompileShader(ShaderStage::CS, "cs_write_mesh_dispatch_args", &csMeshArgsBindless_, true);
        if (supportsMeshShaders_)
        {
            bindlessReady &= CompileShader(ShaderStage::MS, "ms_clusters", &msClusterBindless_, true);
            bindlessReady &= CompileShader(ShaderStage::PS, "ps_mesh", &psMeshBindless_, true);
        }

        if (bindlessReady)
        {
            PipelineStateDesc bindlessPSO = {};
            bindlessPSO.vs = &vsIndexedBindless_;
            bindlessPSO.ps = &psIndexedBindless_;
            bindlessPSO.il = &indexedInputLayout_;
            bindlessPSO.rs = &rasterState_;
            bindlessPSO.dss = &depthStencilState_;
            bindlessPSO.bs = &blendState_;
            bindlessPSO.pt = PrimitiveTopology::TRIANGLELIST;
            bindlessReady &= device_->CreatePipelineState(&bindlessPSO, &pipelineIndexedBindless_);

            if (supportsMeshShaders_)
            {
                PipelineStateDesc meshBindlessPSO = {};
                meshBindlessPSO.ms = &msClusterBindless_;
                meshBindlessPSO.ps = &psMeshBindless_;
                meshBindlessPSO.rs = &rasterState_;
                meshBindlessPSO.dss = &depthStencilState_;
                meshBindlessPSO.bs = &blendState_;
                meshBindlessPSO.pt = PrimitiveTopology::TRIANGLELIST;
                bindlessReady &= device_->CreatePipelineState(&meshBindlessPSO, &pipelineMeshBindless_);
            }
        }

        bindlessShadersAvailable_ = bindlessReady;
        if (!bindlessShadersAvailable_)
        {
            activeBindingMode_ = BindingMode::Bindful;
            SDL_Log("[WickedVisibilityPipelineBenchmark] bindless shader variant unavailable; using bindful mode");
        }
        else
        {
            SDL_Log("[WickedVisibilityPipelineBenchmark] bindless shader variant ready (toggle with [K])");
        }

        return true;
    }

    bool BuildSceneAndBuffers()
    {
        std::vector<MeshShapeCPU> meshShapes;
        meshShapes.push_back(CreateCubeShape());
        meshShapes.push_back(CreatePyramidShape());
        meshShapes.push_back(CreateSphereShape(24, 24));
        meshShapes.push_back(CreateCylinderShape(32));
        meshShapes.push_back(CreateTorusShape(40, 24, 1.4f, 0.45f));

        shapeTemplates_.clear();
        vertices_.clear();
        clusterTemplates_.clear();
        templateVertices_.clear();
        templatePackedTriangles_.clear();
        clusterDrawIndices_.clear();

        uint32_t globalVertexOffset = 0;
        for (const MeshShapeCPU& mesh : meshShapes)
        {
            ShapeTemplate shape = {};
            shape.name = mesh.name;
            shape.vertexOffset = globalVertexOffset;
            shape.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
            shape.triangleCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
            shape.clusterStart = static_cast<uint32_t>(clusterTemplates_.size());
            std::memcpy(shape.color, mesh.color, sizeof(shape.color));

            Vec3 minP = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
            Vec3 maxP = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
            for (const Vec3& p : mesh.vertices)
            {
                minP.x = std::min(minP.x, p.x);
                minP.y = std::min(minP.y, p.y);
                minP.z = std::min(minP.z, p.z);
                maxP.x = std::max(maxP.x, p.x);
                maxP.y = std::max(maxP.y, p.y);
                maxP.z = std::max(maxP.z, p.z);
            }
            shape.boundsCenter = Vec3{
                0.5f * (minP.x + maxP.x),
                0.5f * (minP.y + maxP.y),
                0.5f * (minP.z + maxP.z),
            };
            const Vec3 ext = Vec3{
                maxP.x - shape.boundsCenter.x,
                maxP.y - shape.boundsCenter.y,
                maxP.z - shape.boundsCenter.z,
            };
            shape.boundsRadius = std::sqrt(VecDot(ext, ext));

            vertices_.insert(vertices_.end(), mesh.vertices.begin(), mesh.vertices.end());

            const uint32_t triCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
            uint32_t triCursor = 0;
            while (triCursor < triCount)
            {
                const uint32_t clusterTriStart = triCursor;

                std::vector<uint32_t> localVertexRemap;
                localVertexRemap.reserve(kMaxClusterVertices);

                GPUClusterTemplate cluster = {};
                cluster.baseVertex = shape.vertexOffset;
                cluster.indexOffset = static_cast<uint32_t>(clusterDrawIndices_.size());
                cluster.localVertexOffset = static_cast<uint32_t>(templateVertices_.size());
                cluster.localTriOffset = static_cast<uint32_t>(templatePackedTriangles_.size());
                cluster.primitiveOffset = clusterTriStart;

                while (triCursor < triCount && cluster.localTriCount < kMaxClusterTriangles)
                {
                    const uint32_t i0 = mesh.indices[(triCursor * 3u) + 0u];
                    const uint32_t i1 = mesh.indices[(triCursor * 3u) + 1u];
                    const uint32_t i2 = mesh.indices[(triCursor * 3u) + 2u];

                    auto findLocalIndex = [&](uint32_t shapeVertex) -> int32_t {
                        for (uint32_t i = 0; i < localVertexRemap.size(); ++i)
                        {
                            if (localVertexRemap[i] == shapeVertex)
                            {
                                return static_cast<int32_t>(i);
                            }
                        }
                        return -1;
                    };

                    const int32_t l0 = findLocalIndex(i0);
                    const int32_t l1 = findLocalIndex(i1);
                    const int32_t l2 = findLocalIndex(i2);

                    uint32_t newVertices = 0;
                    if (l0 < 0)
                        ++newVertices;
                    if (l1 < 0)
                        ++newVertices;
                    if (l2 < 0)
                        ++newVertices;

                    if (cluster.localTriCount > 0 && (cluster.localVertexCount + newVertices) > kMaxClusterVertices)
                    {
                        break;
                    }

                    auto pushLocal = [&](uint32_t shapeVertex) -> uint32_t {
                        const int32_t existing = findLocalIndex(shapeVertex);
                        if (existing >= 0)
                        {
                            return static_cast<uint32_t>(existing);
                        }
                        const uint32_t newIdx = static_cast<uint32_t>(localVertexRemap.size());
                        localVertexRemap.push_back(shapeVertex);
                        templateVertices_.push_back(shapeVertex);
                        ++cluster.localVertexCount;
                        return newIdx;
                    };

                    const uint32_t local0 = pushLocal(i0);
                    const uint32_t local1 = pushLocal(i1);
                    const uint32_t local2 = pushLocal(i2);

                    templatePackedTriangles_.push_back((local0 & 0xFFu) | ((local1 & 0xFFu) << 8u) | ((local2 & 0xFFu) << 16u));

                    clusterDrawIndices_.push_back(i0);
                    clusterDrawIndices_.push_back(i1);
                    clusterDrawIndices_.push_back(i2);

                    ++cluster.localTriCount;
                    triCursor++;
                }

                cluster.indexCount = cluster.localTriCount * 3u;

                Vec3 localMin = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
                Vec3 localMax = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
                for (uint32_t local = 0; local < cluster.localVertexCount; ++local)
                {
                    const uint32_t shapeVertex = templateVertices_[cluster.localVertexOffset + local];
                    const Vec3 p = mesh.vertices[shapeVertex];
                    localMin.x = std::min(localMin.x, p.x);
                    localMin.y = std::min(localMin.y, p.y);
                    localMin.z = std::min(localMin.z, p.z);
                    localMax.x = std::max(localMax.x, p.x);
                    localMax.y = std::max(localMax.y, p.y);
                    localMax.z = std::max(localMax.z, p.z);
                }
                const Vec3 clusterCenter = {
                    0.5f * (localMin.x + localMax.x),
                    0.5f * (localMin.y + localMax.y),
                    0.5f * (localMin.z + localMax.z),
                };
                float clusterRadius = 0.0f;
                for (uint32_t local = 0; local < cluster.localVertexCount; ++local)
                {
                    const uint32_t shapeVertex = templateVertices_[cluster.localVertexOffset + local];
                    const Vec3 p = mesh.vertices[shapeVertex];
                    const Vec3 delta = VecSub(p, clusterCenter);
                    clusterRadius = std::max(clusterRadius, std::sqrt(VecDot(delta, delta)));
                }
                cluster.bounds[0] = clusterCenter.x;
                cluster.bounds[1] = clusterCenter.y;
                cluster.bounds[2] = clusterCenter.z;
                cluster.bounds[3] = clusterRadius;

                clusterTemplates_.push_back(cluster);
            }

            shape.clusterCount = static_cast<uint32_t>(clusterTemplates_.size()) - shape.clusterStart;
            shapeTemplates_.push_back(shape);

            globalVertexOffset += shape.vertexCount;
        }

        BuildInstancesAndCommands();
        BuildTierMappings();

        return CreateSceneGPUResources();
    }

    void BuildInstancesAndCommands()
    {
        instances_.clear();
        commands_.clear();
        baseArgs_.clear();

        ecsWorld_ = std::make_unique<flecs::world>();

        const uint64_t targetTriangles = kTierPresets.back().targetTriangles;

        std::vector<uint32_t> shapeSequence;
        shapeSequence.reserve(16384);

        uint64_t triAccum = 0;
        uint32_t sequenceCursor = 0;
        while (triAccum < targetTriangles)
        {
            const uint32_t shapeIndex = sequenceCursor % static_cast<uint32_t>(shapeTemplates_.size());
            shapeSequence.push_back(shapeIndex);
            triAccum += shapeTemplates_[shapeIndex].triangleCount;
            ++sequenceCursor;
        }

        const uint32_t instanceCount = static_cast<uint32_t>(shapeSequence.size());
        const uint32_t side = static_cast<uint32_t>(std::ceil(std::cbrt(static_cast<double>(std::max(1u, instanceCount)))));
        const float spacing = 5.2f * kSceneRenderScale;
        const float half = static_cast<float>(side - 1u) * 0.5f;
        sceneExtent_ = std::max(20.0f, half * spacing);

        uint32_t primitiveBase = 0;
        for (uint32_t instanceIndex = 0; instanceIndex < instanceCount; ++instanceIndex)
        {
            const uint32_t shapeIndex = shapeSequence[instanceIndex];
            const ShapeTemplate& shape = shapeTemplates_[shapeIndex];

            const uint32_t x = instanceIndex % side;
            const uint32_t y = (instanceIndex / side) % side;
            const uint32_t z = instanceIndex / (side * side);

            const float jitterX = (Hash01(instanceIndex * 7u + 13u) - 0.5f) * 0.35f * kSceneRenderScale;
            const float jitterY = (Hash01(instanceIndex * 13u + 5u) - 0.5f) * 0.35f * kSceneRenderScale;
            const float jitterZ = (Hash01(instanceIndex * 17u + 9u) - 0.5f) * 0.35f * kSceneRenderScale;

            const Vec3 translation = {
                (static_cast<float>(x) - half) * spacing + jitterX,
                (static_cast<float>(y) - half) * spacing * 0.55f + jitterY,
                (static_cast<float>(z) - half) * spacing + jitterZ,
            };

            const float scale = (0.75f + Hash01(instanceIndex * 31u + 7u) * 0.65f) * kSceneRenderScale;

            const float initialYaw = Hash01(instanceIndex * 29u + 41u) * (2.0f * kPi);
            const float yawRate = (0.35f + 1.25f * Hash01(instanceIndex * 89u + 17u)) *
                                  (Hash01(instanceIndex * 97u + 23u) > 0.5f ? 1.0f : -1.0f);
            const float orbitRadius = (0.12f + 0.34f * Hash01(instanceIndex * 67u + 31u)) * kSceneRenderScale;
            const float orbitRate = 0.25f + 0.65f * Hash01(instanceIndex * 71u + 7u);
            const float orbitPhase = Hash01(instanceIndex * 79u + 37u) * (2.0f * kPi);
            const float bobAmplitude = (0.08f + 0.28f * Hash01(instanceIndex * 83u + 29u)) * kSceneRenderScale;
            const float bobRate = 0.85f + 1.15f * Hash01(instanceIndex * 101u + 19u);
            const float bobPhase = Hash01(instanceIndex * 109u + 11u) * (2.0f * kPi);

            Vec3 animatedTranslation = translation;
            animatedTranslation.x += std::cos(orbitPhase) * orbitRadius;
            animatedTranslation.y += std::sin(bobPhase) * bobAmplitude;
            animatedTranslation.z += std::sin(orbitPhase) * orbitRadius;

            GPUInstanceData instance = {};
            instance.world = MatScaleRotateYTranslate(scale, initialYaw, animatedTranslation);
            instance.color[0] = shape.color[0] * (0.8f + 0.2f * Hash01(instanceIndex * 43u + 1u));
            instance.color[1] = shape.color[1] * (0.8f + 0.2f * Hash01(instanceIndex * 47u + 3u));
            instance.color[2] = shape.color[2] * (0.8f + 0.2f * Hash01(instanceIndex * 53u + 11u));
            instance.color[3] = 1.0f;

            const Vec3 worldCenter = TransformPoint(instance.world, shape.boundsCenter);
            instance.bounds[0] = worldCenter.x;
            instance.bounds[1] = worldCenter.y;
            instance.bounds[2] = worldCenter.z;
            instance.bounds[3] = shape.boundsRadius * scale;
            instance.scale = scale;
            instances_.push_back(instance);

            if (ecsWorld_)
            {
                ECSMotion motion = {};
                motion.shapeIndex = shapeIndex;
                motion.scale = scale;
                std::memcpy(motion.color, instance.color, sizeof(motion.color));
                motion.basePosition = translation;
                motion.yawRate = yawRate;
                motion.orbitRadius = orbitRadius;
                motion.orbitRate = orbitRate;
                motion.orbitPhase = orbitPhase;
                motion.bobAmplitude = bobAmplitude;
                motion.bobRate = bobRate;
                motion.bobPhase = bobPhase;

                ECSDynamicState dynamicState = {};
                dynamicState.position = animatedTranslation;
                dynamicState.yaw = initialYaw;
                dynamicState.elapsed = 0.0f;

                ecsWorld_->entity()
                    .set<ECSInstanceLink>({ instanceIndex })
                    .set<ECSMotion>(motion)
                    .set<ECSDynamicState>(dynamicState);
            }

            for (uint32_t localCluster = 0; localCluster < shape.clusterCount; ++localCluster)
            {
                const uint32_t clusterIndex = shape.clusterStart + localCluster;
                const GPUClusterTemplate& cluster = clusterTemplates_[clusterIndex];

                GPUClusterCommand command = {};
                command.clusterTemplateIndex = clusterIndex;
                command.instanceIndex = instanceIndex;
                command.primitiveBase = primitiveBase + cluster.primitiveOffset;
                commands_.push_back(command);

                DrawIndexedIndirectCountArgs args = {};
#if WICKED_SUBSET_USE_DX12
                args.DrawID = static_cast<uint32_t>(commands_.size() - 1u);
                args.Draw.IndexCountPerInstance = cluster.indexCount;
                args.Draw.InstanceCount = 1;
                args.Draw.StartIndexLocation = cluster.indexOffset;
                args.Draw.BaseVertexLocation = static_cast<int32_t>(cluster.baseVertex);
                args.Draw.StartInstanceLocation = static_cast<uint32_t>(commands_.size() - 1u);
#else
                args.IndexCountPerInstance = cluster.indexCount;
                args.InstanceCount = 1;
                args.StartIndexLocation = cluster.indexOffset;
                args.BaseVertexLocation = static_cast<int32_t>(cluster.baseVertex);
                args.StartInstanceLocation = static_cast<uint32_t>(commands_.size() - 1u);
#endif
                baseArgs_.push_back(args);
            }

            primitiveBase += shape.triangleCount;
        }

        totalInstanceCount_ = static_cast<uint32_t>(instances_.size());
        totalCommandCount_ = static_cast<uint32_t>(commands_.size());
        totalPrimitiveCount_ = primitiveBase;

        drawCommandIndices_.resize(totalCommandCount_);
        for (uint32_t i = 0; i < totalCommandCount_; ++i)
        {
            drawCommandIndices_[i] = i;
        }

        ecsSnapshotScratch_.resize(totalInstanceCount_);
    }

    void BuildTierMappings()
    {
        tierActiveCommandCount_.fill(0u);
        tierActiveInstanceCount_.fill(0u);

        for (uint32_t tier = 0; tier < kTierPresets.size(); ++tier)
        {
            const uint64_t target = kTierPresets[tier].targetTriangles;
            uint32_t activeCommands = 0;
            uint32_t activeInstances = 0;

            for (uint32_t commandIndex = 0; commandIndex < commands_.size(); ++commandIndex)
            {
                const GPUClusterCommand& command = commands_[commandIndex];
                const GPUClusterTemplate& cluster = clusterTemplates_[command.clusterTemplateIndex];
                const uint64_t commandEnd = static_cast<uint64_t>(command.primitiveBase) + static_cast<uint64_t>(cluster.localTriCount);

                if (commandEnd <= target || activeCommands == 0)
                {
                    activeCommands = commandIndex + 1u;
                    activeInstances = std::max(activeInstances, command.instanceIndex + 1u);
                }
                else
                {
                    break;
                }
            }

            tierActiveCommandCount_[tier] = activeCommands;
            tierActiveInstanceCount_[tier] = activeInstances;
        }
    }

    void UpdateECSWorld(float dt)
    {
        if (!ecsMotionEnabled_ || !ecsWorld_)
        {
            return;
        }

        const float step = std::min(dt, 1.0f / 20.0f);
        ecsWorld_->each([step](ECSMotion& motion, ECSDynamicState& dynamicState) {
            dynamicState.elapsed += step;
            dynamicState.yaw += motion.yawRate * step;

            const float orbitAngle = dynamicState.elapsed * motion.orbitRate + motion.orbitPhase;
            dynamicState.position.x = motion.basePosition.x + std::cos(orbitAngle) * motion.orbitRadius;
            dynamicState.position.y =
                motion.basePosition.y +
                std::sin(dynamicState.elapsed * motion.bobRate + motion.bobPhase) * motion.bobAmplitude;
            dynamicState.position.z = motion.basePosition.z + std::sin(orbitAngle) * motion.orbitRadius;
        });
    }

    bool BuildECSInstanceSnapshot(uint32_t instanceCount)
    {
        if (!ecsWorld_)
        {
            return false;
        }

        if (ecsSnapshotScratch_.size() < instanceCount)
        {
            ecsSnapshotScratch_.resize(instanceCount);
        }

        bool wroteAny = false;
        ecsWorld_->each([&](const ECSInstanceLink& link, const ECSMotion& motion, const ECSDynamicState& dynamicState) {
            if (link.instanceIndex >= instanceCount || motion.shapeIndex >= shapeTemplates_.size())
            {
                return;
            }

            const ShapeTemplate& shape = shapeTemplates_[motion.shapeIndex];
            GPUInstanceData& instance = ecsSnapshotScratch_[link.instanceIndex];
            instance.world = MatScaleRotateYTranslate(motion.scale, dynamicState.yaw, dynamicState.position);
            std::memcpy(instance.color, motion.color, sizeof(instance.color));

            const Vec3 worldCenter = TransformPoint(instance.world, shape.boundsCenter);
            instance.bounds[0] = worldCenter.x;
            instance.bounds[1] = worldCenter.y;
            instance.bounds[2] = worldCenter.z;
            instance.bounds[3] = shape.boundsRadius * motion.scale;
            instance.scale = motion.scale;
            wroteAny = true;
        });

        return wroteAny;
    }

    bool CreateSceneGPUResources()
    {
        DestroySceneGPUResources();

        GPUBufferDesc desc = {};

        desc.usage = Usage::DEFAULT;
        desc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_VERTEX_BUFFER;
        desc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
        desc.stride = sizeof(Vec3);
        desc.size = static_cast<uint64_t>(vertices_.size() * sizeof(Vec3));
        if (!device_->CreateBuffer(&desc, vertices_.data(), &vertexBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(vertexBuffer) failed\n");
            return false;
        }

        desc.stride = sizeof(GPUInstanceData);
        desc.size = static_cast<uint64_t>(instances_.size() * sizeof(GPUInstanceData));
        if (!device_->CreateBuffer(&desc, instances_.data(), &instanceBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(instanceBuffer) failed\n");
            return false;
        }

        desc.stride = sizeof(GPUClusterCommand);
        desc.size = static_cast<uint64_t>(commands_.size() * sizeof(GPUClusterCommand));
        if (!device_->CreateBuffer(&desc, commands_.data(), &commandBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(commandBuffer) failed\n");
            return false;
        }

        desc.stride = sizeof(GPUClusterTemplate);
        desc.size = static_cast<uint64_t>(clusterTemplates_.size() * sizeof(GPUClusterTemplate));
        if (!device_->CreateBuffer(&desc, clusterTemplates_.data(), &clusterTemplateBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(clusterTemplateBuffer) failed\n");
            return false;
        }

        desc.stride = sizeof(uint32_t);
        desc.size = static_cast<uint64_t>(templateVertices_.size() * sizeof(uint32_t));
        if (!device_->CreateBuffer(&desc, templateVertices_.data(), &templateVerticesBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(templateVerticesBuffer) failed\n");
            return false;
        }

        desc.size = static_cast<uint64_t>(templatePackedTriangles_.size() * sizeof(uint32_t));
        if (!device_->CreateBuffer(&desc, templatePackedTriangles_.data(), &templateTrianglesBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(templateTrianglesBuffer) failed\n");
            return false;
        }

        desc.bind_flags = BindFlag::BIND_INDEX_BUFFER | BindFlag::BIND_SHADER_RESOURCE;
        desc.misc_flags = ResourceMiscFlag::BUFFER_RAW;
        desc.stride = 0;
        desc.size = static_cast<uint64_t>(clusterDrawIndices_.size() * sizeof(uint32_t));
        if (!device_->CreateBuffer(&desc, clusterDrawIndices_.data(), &clusterIndexBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(clusterIndexBuffer) failed\n");
            return false;
        }

        GPUBufferDesc drawCommandVBDesc = {};
        drawCommandVBDesc.usage = Usage::DEFAULT;
        // This buffer is consumed as a vertex stream only; don't request SRV view creation.
        drawCommandVBDesc.bind_flags = BindFlag::BIND_VERTEX_BUFFER;
        drawCommandVBDesc.misc_flags = ResourceMiscFlag::RESOURCE_MISC_NONE;
        drawCommandVBDesc.stride = sizeof(uint32_t);
        drawCommandVBDesc.size = static_cast<uint64_t>(drawCommandIndices_.size()) * sizeof(uint32_t);
        if (!device_->CreateBuffer(&drawCommandVBDesc, drawCommandIndices_.data(), &drawCommandIndexBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(drawCommandIndexBuffer) failed\n");
            return false;
        }

        desc.bind_flags = BindFlag::BIND_SHADER_RESOURCE;
        desc.misc_flags = ResourceMiscFlag::INDIRECT_ARGS | ResourceMiscFlag::BUFFER_RAW;
        desc.size = static_cast<uint64_t>(baseArgs_.size() * sizeof(DrawIndexedIndirectCountArgs));
        if (!device_->CreateBuffer(&desc, baseArgs_.data(), &baseArgsBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(baseArgsBuffer) failed\n");
            return false;
        }

        desc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        for (uint32_t slot = 0; slot < kCullOutputSlotCount; ++slot)
        {
            if (!device_->CreateBuffer(&desc, nullptr, VisibleArgsBuffer(slot)))
            {
                std::fprintf(stderr, "CreateBuffer(visibleArgsBuffer[%u]) failed\n", slot);
                return false;
            }
            if (!device_->CreateBuffer(&desc, nullptr, TVBArgsBuffer(slot)))
            {
                std::fprintf(stderr, "CreateBuffer(tvbArgsBuffer[%u]) failed\n", slot);
                return false;
            }
        }

        GPUBufferDesc countDesc = {};
        countDesc.usage = Usage::DEFAULT;
        countDesc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        countDesc.misc_flags = ResourceMiscFlag::INDIRECT_ARGS | ResourceMiscFlag::BUFFER_RAW;
        countDesc.size = sizeof(uint32_t);

        uint32_t initialCount = tierActiveCommandCount_[activeTier_];
        if (!device_->CreateBuffer(&countDesc, &initialCount, &baseCountBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(baseCountBuffer) failed\n");
            return false;
        }

        uint32_t zero = 0;
        for (uint32_t slot = 0; slot < kCullOutputSlotCount; ++slot)
        {
            if (!device_->CreateBuffer(&countDesc, &zero, VisibleCountBuffer(slot)))
            {
                std::fprintf(stderr, "CreateBuffer(visibleCountBuffer[%u]) failed\n", slot);
                return false;
            }
        }

        GPUBufferDesc dispatchDesc = {};
        dispatchDesc.usage = Usage::DEFAULT;
        dispatchDesc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        dispatchDesc.misc_flags = ResourceMiscFlag::INDIRECT_ARGS | ResourceMiscFlag::BUFFER_RAW;
        dispatchDesc.size = sizeof(IndirectDispatchArgs);
        const IndirectDispatchArgs initialDispatch = { 0u, 1u, 1u };
        for (uint32_t slot = 0; slot < kCullOutputSlotCount; ++slot)
        {
            if (!device_->CreateBuffer(&dispatchDesc, &initialDispatch, MeshDispatchArgsBuffer(slot)))
            {
                std::fprintf(stderr, "CreateBuffer(meshDispatchArgsBuffer[%u]) failed\n", slot);
                return false;
            }
        }

        GPUBufferDesc visibilityDesc = {};
        visibilityDesc.usage = Usage::DEFAULT;
        visibilityDesc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        visibilityDesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
        visibilityDesc.stride = sizeof(uint32_t);
        visibilityDesc.size = static_cast<uint64_t>(totalCommandCount_) * sizeof(uint32_t);
        for (uint32_t slot = 0; slot < kCullOutputSlotCount; ++slot)
        {
            if (!device_->CreateBuffer(&visibilityDesc, nullptr, VisibleCommandIndicesBuffer(slot)))
            {
                std::fprintf(stderr, "CreateBuffer(visibleCommandIndicesBuffer[%u]) failed\n", slot);
                return false;
            }
        }

        visibilityDesc.size = static_cast<uint64_t>(totalInstanceCount_) * sizeof(uint32_t);
        if (!device_->CreateBuffer(&visibilityDesc, nullptr, &instanceVisibleBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(instanceVisibleBuffer) failed\n");
            return false;
        }

        GPUBufferDesc tvbIndexDesc = {};
        tvbIndexDesc.usage = Usage::DEFAULT;
        tvbIndexDesc.bind_flags = BindFlag::BIND_INDEX_BUFFER | BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        tvbIndexDesc.misc_flags = ResourceMiscFlag::BUFFER_RAW;
        tvbIndexDesc.size = static_cast<uint64_t>(totalCommandCount_) * static_cast<uint64_t>(kMaxClusterIndices) * sizeof(uint32_t);
        for (uint32_t slot = 0; slot < kCullOutputSlotCount; ++slot)
        {
            if (!device_->CreateBuffer(&tvbIndexDesc, nullptr, TVBFilteredIndexBuffer(slot)))
            {
                std::fprintf(stderr, "CreateBuffer(tvbFilteredIndexBuffer[%u]) failed\n", slot);
                return false;
            }
        }

        GPUBufferDesc tvbPrimDesc = {};
        tvbPrimDesc.usage = Usage::DEFAULT;
        tvbPrimDesc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        tvbPrimDesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
        tvbPrimDesc.stride = sizeof(uint32_t);
        tvbPrimDesc.size = static_cast<uint64_t>(totalCommandCount_) * static_cast<uint64_t>(kMaxClusterTriangles) * sizeof(uint32_t);
        for (uint32_t slot = 0; slot < kCullOutputSlotCount; ++slot)
        {
            if (!device_->CreateBuffer(&tvbPrimDesc, nullptr, TVBFilteredPrimitiveIDBuffer(slot)))
            {
                std::fprintf(stderr, "CreateBuffer(tvbFilteredPrimitiveIDBuffer[%u]) failed\n", slot);
                return false;
            }
        }

        GPUBufferDesc hashDesc = {};
        hashDesc.usage = Usage::DEFAULT;
        hashDesc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        hashDesc.misc_flags = ResourceMiscFlag::BUFFER_RAW;
        hashDesc.size = sizeof(uint32_t);
        if (!device_->CreateBuffer(&hashDesc, &zero, &hashBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(hashBuffer) failed\n");
            return false;
        }

        GPUBufferDesc hashReadbackDesc = {};
        hashReadbackDesc.usage = Usage::READBACK;
        hashReadbackDesc.size = sizeof(uint32_t);
        for (uint32_t i = 0; i < wi::GraphicsDevice::GetBufferCount(); ++i)
        {
            if (!device_->CreateBuffer(&hashReadbackDesc, nullptr, &hashReadback_[i]))
            {
                std::fprintf(stderr, "CreateBuffer(hashReadback) failed\n");
                return false;
            }
            if (!device_->CreateBuffer(&hashReadbackDesc, nullptr, &visibleCountReadback_[i]))
            {
                std::fprintf(stderr, "CreateBuffer(visibleCountReadback) failed\n");
                return false;
            }
        }

        device_->SetName(&vertexBuffer_, "subset_visibility_benchmark_vertices");
        device_->SetName(&instanceBuffer_, "subset_visibility_benchmark_instances");
        device_->SetName(&commandBuffer_, "subset_visibility_benchmark_commands");
        device_->SetName(&clusterTemplateBuffer_, "subset_visibility_benchmark_clusters");
        device_->SetName(&clusterIndexBuffer_, "subset_visibility_benchmark_cluster_indices");
        device_->SetName(&drawCommandIndexBuffer_, "subset_visibility_benchmark_draw_command_ids");
        device_->SetName(&baseArgsBuffer_, "subset_visibility_benchmark_base_args");
        device_->SetName(&baseCountBuffer_, "subset_visibility_benchmark_base_count");
        device_->SetName(&instanceVisibleBuffer_, "subset_visibility_benchmark_instance_visible");
        device_->SetName(&hashBuffer_, "subset_visibility_benchmark_hash");
        for (uint32_t slot = 0; slot < kCullOutputSlotCount; ++slot)
        {
            char name[128] = {};
            std::snprintf(name, sizeof(name), "subset_visibility_benchmark_visible_args_slot%u", slot);
            device_->SetName(VisibleArgsBuffer(slot), name);
            std::snprintf(name, sizeof(name), "subset_visibility_benchmark_tvb_args_slot%u", slot);
            device_->SetName(TVBArgsBuffer(slot), name);
            std::snprintf(name, sizeof(name), "subset_visibility_benchmark_visible_count_slot%u", slot);
            device_->SetName(VisibleCountBuffer(slot), name);
            std::snprintf(name, sizeof(name), "subset_visibility_benchmark_mesh_dispatch_args_slot%u", slot);
            device_->SetName(MeshDispatchArgsBuffer(slot), name);
            std::snprintf(name, sizeof(name), "subset_visibility_benchmark_visible_commands_slot%u", slot);
            device_->SetName(VisibleCommandIndicesBuffer(slot), name);
            std::snprintf(name, sizeof(name), "subset_visibility_benchmark_tvb_indices_slot%u", slot);
            device_->SetName(TVBFilteredIndexBuffer(slot), name);
            std::snprintf(name, sizeof(name), "subset_visibility_benchmark_tvb_primitive_ids_slot%u", slot);
            device_->SetName(TVBFilteredPrimitiveIDBuffer(slot), name);
        }

        ResetTransientBufferStates();
        return true;
    }

    void DestroySceneGPUResources()
    {
        vertexBuffer_ = {};
        instanceBuffer_ = {};
        commandBuffer_ = {};
        clusterTemplateBuffer_ = {};
        templateVerticesBuffer_ = {};
        templateTrianglesBuffer_ = {};
        clusterIndexBuffer_ = {};
        drawCommandIndexBuffer_ = {};
        baseArgsBuffer_ = {};
        for (GPUBuffer& b : visibleArgsBuffers_)
        {
            b = {};
        }
        for (GPUBuffer& b : tvbArgsBuffers_)
        {
            b = {};
        }
        baseCountBuffer_ = {};
        for (GPUBuffer& b : visibleCountBuffers_)
        {
            b = {};
        }
        for (GPUBuffer& b : meshDispatchArgsBuffers_)
        {
            b = {};
        }
        for (GPUBuffer& b : visibleCommandIndicesBuffers_)
        {
            b = {};
        }
        instanceVisibleBuffer_ = {};
        for (GPUBuffer& b : tvbFilteredIndexBuffers_)
        {
            b = {};
        }
        for (GPUBuffer& b : tvbFilteredPrimitiveIDBuffers_)
        {
            b = {};
        }
        hashBuffer_ = {};
        for (GPUBuffer& b : hashReadback_)
        {
            b = {};
        }
        for (GPUBuffer& b : visibleCountReadback_)
        {
            b = {};
        }
    }

    bool CreateTimingResources()
    {
        GPUQueryHeapDesc queryDesc = {};
        queryDesc.type = wi::GpuQueryType::TIMESTAMP;
        queryDesc.query_count = kTimestampCount;
        if (!device_->CreateQueryHeap(&queryDesc, &timestampQueryHeap_))
        {
            std::fprintf(stderr, "CreateQueryHeap(timestamp) failed\n");
            return false;
        }

        GPUBufferDesc readbackDesc = {};
        readbackDesc.usage = Usage::READBACK;
        readbackDesc.size = static_cast<uint64_t>(kTimestampCount) * sizeof(uint64_t);

        for (uint32_t i = 0; i < wi::GraphicsDevice::GetBufferCount(); ++i)
        {
            if (!device_->CreateBuffer(&readbackDesc, nullptr, &timestampReadback_[i]))
            {
                std::fprintf(stderr, "CreateBuffer(timestampReadback) failed\n");
                return false;
            }
            timestampReady_[i] = false;
            hashReady_[i] = false;
            visibleCountReady_[i] = false;
        }

        return true;
    }

    void DestroyTimingResources()
    {
        timestampQueryHeap_ = {};
        for (GPUBuffer& b : timestampReadback_)
        {
            b = {};
        }
    }

    void DestroyResources()
    {
        DestroyRenderTargets();
        DestroySceneGPUResources();
        DestroyTimingResources();
        ecsSnapshotScratch_.clear();
        ecsWorld_.reset();
    }

    void BuildComboList()
    {
        combos_.clear();

        std::array<SuiteMode, 2> suites = { SuiteMode::Portable, SuiteMode::Mesh };
        std::array<PipelineStyle, 3> pipelines = { PipelineStyle::Wicked, PipelineStyle::TVB, PipelineStyle::Esoterica };
        std::array<ScenarioMode, 2> scenarios = { ScenarioMode::AllVisible, ScenarioMode::HighCulling };

        for (uint32_t tier = 0; tier < kTierPresets.size(); ++tier)
        {
            for (ScenarioMode scenario : scenarios)
            {
                for (SuiteMode suite : suites)
                {
                    if (suite == SuiteMode::Mesh && !supportsMeshShaders_)
                    {
                        continue;
                    }
                    for (PipelineStyle pipeline : pipelines)
                    {
                        ComboConfig combo = {};
                        combo.suite = suite;
                        combo.pipeline = pipeline;
                        combo.scenario = scenario;
                        combo.tierIndex = tier;
                        combos_.push_back(combo);
                    }
                }
            }
        }

        if (combos_.empty())
        {
            ComboConfig fallback = {};
            combos_.push_back(fallback);
        }
    }

    void ApplyCombo(const ComboConfig& combo)
    {
        activeSuite_ = combo.suite;
        activePipeline_ = combo.pipeline;
        activeScenario_ = combo.scenario;
        activeTier_ = combo.tierIndex;

        activeCommandCount_ = tierActiveCommandCount_[activeTier_];
        activeInstanceCount_ = tierActiveInstanceCount_[activeTier_];

        countsDirty_ = true;
        ResetCullFrameOverlap();

        SDL_Log(
            "[WickedVisibilityPipelineBenchmark] combo -> suite=%s | pipeline=%s | scenario=%s | tier=%s | binding=%s | commands=%u | instances=%u",
            SuiteName(activeSuite_),
            PipelineName(activePipeline_),
            ScenarioName(activeScenario_),
            kTierPresets[activeTier_].name,
            BindingModeName(activeBindingMode_),
            activeCommandCount_,
            activeInstanceCount_);
    }

    bool IsAsyncCullActiveForCurrentMode() const
    {
        return asyncComputeEnabled_;
    }

    void ResetCullFrameOverlap()
    {
        cullHistoryValid_ = false;
        cullReadSlot_ = 0u;
        cullWriteSlot_ = 1u;
        cullCompletionTokens_ = {};
        lastGraphicsCompletionToken_ = {};
    }

    GPUBuffer* VisibleArgsBuffer(uint32_t slot)
    {
        return &visibleArgsBuffers_[slot % kCullOutputSlotCount];
    }
    GPUBuffer* TVBArgsBuffer(uint32_t slot)
    {
        return &tvbArgsBuffers_[slot % kCullOutputSlotCount];
    }
    GPUBuffer* VisibleCountBuffer(uint32_t slot)
    {
        return &visibleCountBuffers_[slot % kCullOutputSlotCount];
    }
    GPUBuffer* MeshDispatchArgsBuffer(uint32_t slot)
    {
        return &meshDispatchArgsBuffers_[slot % kCullOutputSlotCount];
    }
    GPUBuffer* VisibleCommandIndicesBuffer(uint32_t slot)
    {
        return &visibleCommandIndicesBuffers_[slot % kCullOutputSlotCount];
    }
    GPUBuffer* TVBFilteredIndexBuffer(uint32_t slot)
    {
        return &tvbFilteredIndexBuffers_[slot % kCullOutputSlotCount];
    }
    GPUBuffer* TVBFilteredPrimitiveIDBuffer(uint32_t slot)
    {
        return &tvbFilteredPrimitiveIDBuffers_[slot % kCullOutputSlotCount];
    }

    ResourceState* VisibleArgsBufferState(uint32_t slot)
    {
        return &visibleArgsBufferStates_[slot % kCullOutputSlotCount];
    }
    ResourceState* TVBArgsBufferState(uint32_t slot)
    {
        return &tvbArgsBufferStates_[slot % kCullOutputSlotCount];
    }
    ResourceState* VisibleCountBufferState(uint32_t slot)
    {
        return &visibleCountBufferStates_[slot % kCullOutputSlotCount];
    }
    ResourceState* MeshDispatchArgsBufferState(uint32_t slot)
    {
        return &meshDispatchArgsBufferStates_[slot % kCullOutputSlotCount];
    }
    ResourceState* VisibleCommandIndicesBufferState(uint32_t slot)
    {
        return &visibleCommandIndicesBufferStates_[slot % kCullOutputSlotCount];
    }
    ResourceState* TVBFilteredIndexBufferState(uint32_t slot)
    {
        return &tvbFilteredIndexBufferStates_[slot % kCullOutputSlotCount];
    }
    ResourceState* TVBFilteredPrimitiveIDBufferState(uint32_t slot)
    {
        return &tvbFilteredPrimitiveIDBufferStates_[slot % kCullOutputSlotCount];
    }

    void ResetTransientBufferStates()
    {
        vertexBufferState_ = ResourceState::SHADER_RESOURCE | ResourceState::VERTEX_BUFFER;
        instanceBufferState_ = ResourceState::SHADER_RESOURCE | ResourceState::VERTEX_BUFFER;
        drawCommandIndexBufferState_ = ResourceState::VERTEX_BUFFER;
        clusterIndexBufferState_ = ResourceState::INDEX_BUFFER;

        baseCountBufferState_ = ResourceState::INDIRECT_ARGUMENT;
        hashBufferState_ = ResourceState::UNORDERED_ACCESS;

        instanceVisibleBufferState_ = ResourceState::UNORDERED_ACCESS;
        for (uint32_t slot = 0; slot < kCullOutputSlotCount; ++slot)
        {
            visibleCountBufferStates_[slot] = ResourceState::UNORDERED_ACCESS;
            meshDispatchArgsBufferStates_[slot] = ResourceState::UNORDERED_ACCESS;
            visibleCommandIndicesBufferStates_[slot] = ResourceState::UNORDERED_ACCESS;
            visibleArgsBufferStates_[slot] = ResourceState::UNORDERED_ACCESS;
            tvbFilteredIndexBufferStates_[slot] = ResourceState::UNORDERED_ACCESS;
            tvbArgsBufferStates_[slot] = ResourceState::UNORDERED_ACCESS;
            tvbFilteredPrimitiveIDBufferStates_[slot] = ResourceState::UNORDERED_ACCESS;
        }
        ResetCullFrameOverlap();
    }

    void TransitionBufferState(const GPUBuffer* buffer, ResourceState* currentState, ResourceState newState, CommandList cmd)
    {
        if (buffer == nullptr || currentState == nullptr || *currentState == newState)
        {
            return;
        }
        device_->Barrier(wi::wiGraphicsCreateGPUBarrierBuffer(buffer, *currentState, newState), cmd);
        *currentState = newState;
    }

    void PrepareDrawBufferStates(CommandList cmd, uint32_t drawSlot)
    {
        TransitionBufferState(&instanceBuffer_, &instanceBufferState_, ResourceState::SHADER_RESOURCE, cmd);

        GPUBuffer* visibleCommandIndicesBuffer = VisibleCommandIndicesBuffer(drawSlot);
        GPUBuffer* visibleCountBuffer = VisibleCountBuffer(drawSlot);
        GPUBuffer* visibleArgsBuffer = VisibleArgsBuffer(drawSlot);
        GPUBuffer* tvbFilteredIndexBuffer = TVBFilteredIndexBuffer(drawSlot);
        GPUBuffer* tvbArgsBuffer = TVBArgsBuffer(drawSlot);
        GPUBuffer* tvbFilteredPrimitiveIDBuffer = TVBFilteredPrimitiveIDBuffer(drawSlot);

        const bool meshPath = activeSuite_ == SuiteMode::Mesh && supportsMeshShaders_ && activePipeline_ != PipelineStyle::TVB;
        if (meshPath)
        {
            TransitionBufferState(&vertexBuffer_, &vertexBufferState_, ResourceState::SHADER_RESOURCE | ResourceState::VERTEX_BUFFER, cmd);
            TransitionBufferState(visibleCommandIndicesBuffer, VisibleCommandIndicesBufferState(drawSlot), ResourceState::SHADER_RESOURCE, cmd);
            TransitionBufferState(
                visibleCountBuffer,
                VisibleCountBufferState(drawSlot),
                ResourceState::SHADER_RESOURCE | ResourceState::INDIRECT_ARGUMENT,
                cmd);
            return;
        }

        TransitionBufferState(&vertexBuffer_, &vertexBufferState_, ResourceState::SHADER_RESOURCE | ResourceState::VERTEX_BUFFER, cmd);
        TransitionBufferState(&drawCommandIndexBuffer_, &drawCommandIndexBufferState_, ResourceState::VERTEX_BUFFER, cmd);

        if (activePipeline_ == PipelineStyle::TVB)
        {
            TransitionBufferState(tvbFilteredIndexBuffer, TVBFilteredIndexBufferState(drawSlot), ResourceState::INDEX_BUFFER, cmd);
            TransitionBufferState(tvbArgsBuffer, TVBArgsBufferState(drawSlot), ResourceState::INDIRECT_ARGUMENT, cmd);
            TransitionBufferState(tvbFilteredPrimitiveIDBuffer, TVBFilteredPrimitiveIDBufferState(drawSlot), ResourceState::SHADER_RESOURCE, cmd);
            TransitionBufferState(&baseCountBuffer_, &baseCountBufferState_, ResourceState::INDIRECT_ARGUMENT, cmd);
        }
        else if (activePipeline_ == PipelineStyle::Esoterica)
        {
            TransitionBufferState(tvbFilteredIndexBuffer, TVBFilteredIndexBufferState(drawSlot), ResourceState::INDEX_BUFFER, cmd);
            TransitionBufferState(tvbArgsBuffer, TVBArgsBufferState(drawSlot), ResourceState::INDIRECT_ARGUMENT, cmd);
            TransitionBufferState(tvbFilteredPrimitiveIDBuffer, TVBFilteredPrimitiveIDBufferState(drawSlot), ResourceState::SHADER_RESOURCE, cmd);
            TransitionBufferState(visibleCommandIndicesBuffer, VisibleCommandIndicesBufferState(drawSlot), ResourceState::SHADER_RESOURCE, cmd);
            TransitionBufferState(
                visibleCountBuffer,
                VisibleCountBufferState(drawSlot),
                ResourceState::SHADER_RESOURCE | ResourceState::INDIRECT_ARGUMENT,
                cmd);
        }
        else
        {
            TransitionBufferState(&clusterIndexBuffer_, &clusterIndexBufferState_, ResourceState::INDEX_BUFFER, cmd);
            TransitionBufferState(visibleArgsBuffer, VisibleArgsBufferState(drawSlot), ResourceState::INDIRECT_ARGUMENT, cmd);
            TransitionBufferState(
                visibleCountBuffer,
                VisibleCountBufferState(drawSlot),
                ResourceState::SHADER_RESOURCE | ResourceState::INDIRECT_ARGUMENT,
                cmd);
        }
    }

    void ResetAggregate(AggregateStats* stats)
    {
        stats->cpuMs.clear();
        stats->gpuCullMs.clear();
        stats->gpuDrawMs.clear();
        stats->gpuFrameMs.clear();
        stats->visibleCommands.clear();
        stats->hashValues.clear();
    }

    bool IsBenchmarkParityLockedKey(SDL_Keycode key) const
    {
        switch (key)
        {
            case SDLK_1:
            case SDLK_2:
            case SDLK_3:
            case SDLK_M:
            case SDLK_UP:
            case SDLK_DOWN:
            case SDLK_RIGHTBRACKET:
            case SDLK_LEFTBRACKET:
            case SDLK_PAGEUP:
            case SDLK_PAGEDOWN:
            case SDLK_EQUALS:
            case SDLK_MINUS:
            case SDLK_J:
            case SDLK_T:
            case SDLK_V:
            case SDLK_O:
            case SDLK_COMMA:
            case SDLK_PERIOD:
            case SDLK_SEMICOLON:
            case SDLK_APOSTROPHE:
            case SDLK_K:
            case SDLK_Y:
                return true;
            default:
                return false;
        }
    }

    void ResetCameraForBenchmarkParity()
    {
        ResetCameraToScene();
        // Match the benchmark-facing pose: look left, then back up for scene framing.
        cameraYaw_ = -0.55f;
        const Vec3 forward = BuildForwardFromYawPitch(cameraYaw_, cameraPitch_);
        const float backoff = std::max(sceneExtent_ * 0.45f, 10.0f);
        cameraPosition_ = VecAdd(cameraPosition_, VecScale(forward, -backoff));
    }

    void ApplyBenchmarkParityPreset(bool forceLog)
    {
        bool changed = false;
        bool resetOverlap = false;
        bool fallbackMeshPath = false;
        bool fallbackBindless = false;

        if (!ecsMotionEnabled_)
        {
            ecsMotionEnabled_ = true;
            changed = true;
        }
        if (!validationEnabled_)
        {
            validationEnabled_ = true;
            changed = true;
        }
        if (!hiZOcclusionEnabled_)
        {
            hiZOcclusionEnabled_ = true;
            changed = true;
        }
        if (std::fabs(cullPaddingPixels_ - 6.0f) > 0.0001f)
        {
            cullPaddingPixels_ = 6.0f;
            changed = true;
        }
        if (std::fabs(hiZOcclusionBias_ - 0.0025f) > 0.000001f)
        {
            hiZOcclusionBias_ = 0.0025f;
            changed = true;
        }
        if (activePipeline_ != PipelineStyle::Wicked)
        {
            activePipeline_ = PipelineStyle::Wicked;
            resetOverlap = true;
            changed = true;
        }
        const SuiteMode paritySuite = supportsMeshShaders_ ? SuiteMode::Mesh : SuiteMode::Portable;
        if (activeSuite_ != paritySuite)
        {
            activeSuite_ = paritySuite;
            resetOverlap = true;
            changed = true;
        }
        fallbackMeshPath = !supportsMeshShaders_;

        const BindingMode parityBinding = bindlessShadersAvailable_ ? BindingMode::Bindless : BindingMode::Bindful;
        if (activeBindingMode_ != parityBinding)
        {
            activeBindingMode_ = parityBinding;
            resetOverlap = true;
            changed = true;
        }
        fallbackBindless = !bindlessShadersAvailable_;
        if (!asyncComputeEnabled_)
        {
            asyncComputeEnabled_ = true;
            resetOverlap = true;
            changed = true;
        }
        if (!tokenSubmissionEnabled_)
        {
            tokenSubmissionEnabled_ = true;
            resetOverlap = true;
            changed = true;
        }
        uint32_t parityTierIndex = activeTier_;
        for (uint32_t i = 0; i < static_cast<uint32_t>(kTierPresets.size()); ++i)
        {
            if (kTierPresets[i].targetTriangles == 150'000'000ull)
            {
                parityTierIndex = i;
                break;
            }
        }
        if (activeTier_ != parityTierIndex)
        {
            activeTier_ = parityTierIndex;
            activeCommandCount_ = tierActiveCommandCount_[activeTier_];
            activeInstanceCount_ = tierActiveInstanceCount_[activeTier_];
            countsDirty_ = true;
            resetOverlap = true;
            changed = true;
        }

        if (resetOverlap)
        {
            ResetCullFrameOverlap();
        }

        if (forceLog || changed)
        {
            if (forceLog && fallbackMeshPath)
            {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "[WickedVisibilityPipelineBenchmark] parity requested Mesh path but mesh shaders are unsupported on this backend; using Portable fallback");
            }
            if (forceLog && fallbackBindless)
            {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "[WickedVisibilityPipelineBenchmark] parity requested Bindless path but bindless shaders are unavailable; using Bindful fallback");
            }
            SDL_Log(
                "[WickedVisibilityPipelineBenchmark] benchmark parity preset applied "
                "| suite=%s pipeline=%s binding=%s async=on tokens=on flecs=on hiz=on padding=%.2f bias=%.6f validation=on tier=%s",
                SuiteName(activeSuite_),
                PipelineName(activePipeline_),
                BindingModeName(activeBindingMode_),
                cullPaddingPixels_,
                hiZOcclusionBias_,
                kTierPresets[activeTier_].name);
        }
    }

    void HandleKey(SDL_Keycode key)
    {
        if (key == SDLK_ESCAPE)
        {
            requestQuit_ = true;
            return;
        }

        if (key == SDLK_SPACE)
        {
            autoRun_ = !autoRun_;
            timedBenchmarkMode_ = false;
            if (autoRun_)
            {
                autoRunComboIndex_ = 0;
                autoRunFrame_ = 0;
                ResetAggregate(&autoRunStats_);
                ApplyCombo(combos_[autoRunComboIndex_]);
                mouseLookActive_ = false;
                SDL_SetWindowRelativeMouseMode(window_, false);
            }
            SDL_Log("[WickedVisibilityPipelineBenchmark] auto-run %s", autoRun_ ? "enabled" : "disabled");
            return;
        }

        if (key == SDLK_R || key == SDLK_B)
        {
            autoRun_ = true;
            timedBenchmarkMode_ = (key == SDLK_B);
            timedBenchmarkStartTick_ = timedBenchmarkMode_ ? SDL_GetPerformanceCounter() : 0;
            autoRunComboIndex_ = 0;
            autoRunFrame_ = 0;
            ResetAggregate(&autoRunStats_);
            ApplyCombo(combos_[autoRunComboIndex_]);
            mouseLookActive_ = false;
            SDL_SetWindowRelativeMouseMode(window_, false);
            ResetCameraToScene();
            if (timedBenchmarkMode_)
            {
                SDL_Log(
                    "[WickedVisibilityPipelineBenchmark] timed benchmark started (%.0fs) and will close automatically",
                    kTimedBenchmarkDurationSeconds);
            }
            else
            {
                SDL_Log("[WickedVisibilityPipelineBenchmark] benchmark sweep started");
            }
            return;
        }

        if (key == SDLK_U)
        {
            benchmarkParityMode_ = !benchmarkParityMode_;
            if (benchmarkParityMode_)
            {
                ApplyBenchmarkParityPreset(true);
                if (autoRun_ || timedBenchmarkMode_)
                {
                    autoRun_ = false;
                    timedBenchmarkMode_ = false;
                    timedBenchmarkStartTick_ = 0;
                    SDL_Log("[WickedVisibilityPipelineBenchmark] auto-run disabled by benchmark parity mode");
                }
                timedBenchmarkMode_ = true;
                timedBenchmarkStartTick_ = SDL_GetPerformanceCounter();
                mouseLookActive_ = false;
                SDL_SetWindowRelativeMouseMode(window_, false);
                ResetCameraForBenchmarkParity();
                SDL_Log(
                    "[WickedVisibilityPipelineBenchmark] benchmark parity mode -> enabled (Mesh+Wicked+Bindless+150M, auto-close %.0fs)",
                    kTimedBenchmarkDurationSeconds);
            }
            else
            {
                timedBenchmarkMode_ = false;
                timedBenchmarkStartTick_ = 0;
                SDL_Log("[WickedVisibilityPipelineBenchmark] benchmark parity mode -> disabled");
            }
            return;
        }

        if (benchmarkParityMode_ && IsBenchmarkParityLockedKey(key))
        {
            SDL_Log("[WickedVisibilityPipelineBenchmark] benchmark parity mode locks this key ([U] disables parity)");
            return;
        }

        bool changed = false;
        bool tierChanged = false;

        if (key == SDLK_1)
        {
            activePipeline_ = PipelineStyle::Wicked;
            changed = true;
        }
        else if (key == SDLK_2)
        {
            activePipeline_ = PipelineStyle::TVB;
            changed = true;
        }
        else if (key == SDLK_3)
        {
            activePipeline_ = PipelineStyle::Esoterica;
            changed = true;
        }
        else if (key == SDLK_M)
        {
            if (supportsMeshShaders_)
            {
                activeSuite_ = activeSuite_ == SuiteMode::Portable ? SuiteMode::Mesh : SuiteMode::Portable;
                changed = true;
            }
        }
        else if (key == SDLK_Z)
        {
            activeScenario_ = ScenarioMode::AllVisible;
            changed = true;
        }
        else if (key == SDLK_X)
        {
            activeScenario_ = ScenarioMode::HighCulling;
            changed = true;
        }
        else if (key == SDLK_UP || key == SDLK_RIGHTBRACKET || key == SDLK_PAGEUP || key == SDLK_EQUALS)
        {
            activeTier_ = (activeTier_ + 1u) % static_cast<uint32_t>(kTierPresets.size());
            changed = true;
            tierChanged = true;
        }
        else if (key == SDLK_DOWN || key == SDLK_LEFTBRACKET || key == SDLK_PAGEDOWN || key == SDLK_MINUS)
        {
            activeTier_ = (activeTier_ + static_cast<uint32_t>(kTierPresets.size()) - 1u) % static_cast<uint32_t>(kTierPresets.size());
            changed = true;
            tierChanged = true;
        }
        else if (key == SDLK_C)
        {
            ResetCameraToScene();
            SDL_Log("[WickedVisibilityPipelineBenchmark] camera reset near scene center");
            return;
        }
        else if (key == SDLK_V)
        {
            validationEnabled_ = !validationEnabled_;
            SDL_Log("[WickedVisibilityPipelineBenchmark] validation/hash pass -> %s", validationEnabled_ ? "enabled" : "disabled");
            return;
        }
        else if (key == SDLK_J)
        {
            asyncComputeEnabled_ = !asyncComputeEnabled_;
            ResetCullFrameOverlap();
            SDL_Log(
                "[WickedVisibilityPipelineBenchmark] async compute cull queue -> requested=%s effective=%s",
                asyncComputeEnabled_ ? "enabled" : "disabled",
                IsAsyncCullActiveForCurrentMode() ? "enabled" : "disabled");
            return;
        }
        else if (key == SDLK_T)
        {
            tokenSubmissionEnabled_ = !tokenSubmissionEnabled_;
            ResetCullFrameOverlap();
            SDL_Log(
                "[WickedVisibilityPipelineBenchmark] tokenized submission mode -> %s",
                tokenSubmissionEnabled_ ? "enabled" : "disabled");
            return;
        }
        else if (key == SDLK_O)
        {
            hiZOcclusionEnabled_ = !hiZOcclusionEnabled_;
            SDL_Log("[WickedVisibilityPipelineBenchmark] Hi-Z occlusion -> %s", hiZOcclusionEnabled_ ? "enabled" : "disabled");
            return;
        }
        else if (key == SDLK_COMMA)
        {
            cullPaddingPixels_ = std::max(0.0f, cullPaddingPixels_ - 1.0f);
            SDL_Log("[WickedVisibilityPipelineBenchmark] cull padding -> %.2f px", cullPaddingPixels_);
            return;
        }
        else if (key == SDLK_PERIOD)
        {
            cullPaddingPixels_ = std::min(64.0f, cullPaddingPixels_ + 1.0f);
            SDL_Log("[WickedVisibilityPipelineBenchmark] cull padding -> %.2f px", cullPaddingPixels_);
            return;
        }
        else if (key == SDLK_SEMICOLON)
        {
            hiZOcclusionBias_ = std::max(0.0f, hiZOcclusionBias_ - 0.0005f);
            SDL_Log("[WickedVisibilityPipelineBenchmark] Hi-Z occlusion bias -> %.6f", hiZOcclusionBias_);
            return;
        }
        else if (key == SDLK_APOSTROPHE)
        {
            hiZOcclusionBias_ = std::min(0.05f, hiZOcclusionBias_ + 0.0005f);
            SDL_Log("[WickedVisibilityPipelineBenchmark] Hi-Z occlusion bias -> %.6f", hiZOcclusionBias_);
            return;
        }
        else if (key == SDLK_K)
        {
            if (!bindlessShadersAvailable_)
            {
                SDL_Log("[WickedVisibilityPipelineBenchmark] bindless toggle unavailable (variant failed to compile)");
                return;
            }
            activeBindingMode_ = activeBindingMode_ == BindingMode::Bindful ? BindingMode::Bindless : BindingMode::Bindful;
            SDL_Log("[WickedVisibilityPipelineBenchmark] resource binding mode -> %s", BindingModeName(activeBindingMode_));
            return;
        }
        else if (key == SDLK_Y)
        {
            ecsMotionEnabled_ = !ecsMotionEnabled_;
            SDL_Log("[WickedVisibilityPipelineBenchmark] flecs motion update -> %s", ecsMotionEnabled_ ? "enabled" : "disabled");
            return;
        }
        else if (key == SDLK_P)
        {
            SDL_Log(
                "[WickedVisibilityPipelineBenchmark] snapshot | suite=%s pipeline=%s scenario=%s tier=%s binding=%s cmd=%u inst=%u async=%s tokens=%s hiz=%s flecs_motion=%s parity=%s cpu=%.3fms cull=%.3fms draw=%.3fms frame=%.3fms visible=%u hash=%u",
                SuiteName(activeSuite_),
                PipelineName(activePipeline_),
                ScenarioName(activeScenario_),
                kTierPresets[activeTier_].name,
                BindingModeName(activeBindingMode_),
                activeCommandCount_,
                activeInstanceCount_,
                IsAsyncCullActiveForCurrentMode() ? "on" : "off",
                tokenSubmissionEnabled_ ? "on" : "off",
                hiZOcclusionEnabled_ ? "on" : "off",
                ecsMotionEnabled_ ? "on" : "off",
                benchmarkParityMode_ ? "on" : "off",
                latestMetrics_.cpuMs,
                latestMetrics_.gpuCullMs,
                latestMetrics_.gpuDrawMs,
                latestMetrics_.gpuFrameMs,
                latestMetrics_.visibleCommands,
                latestMetrics_.hashValue);
            return;
        }
        else if (key == SDLK_H)
        {
            LogStartupControls();
            LogRenderPathSummary();
            LogActiveTier();
            return;
        }

        if (!changed)
        {
            return;
        }

        if (autoRun_ || timedBenchmarkMode_)
        {
            autoRun_ = false;
            timedBenchmarkMode_ = false;
            timedBenchmarkStartTick_ = 0;
            SDL_Log("[WickedVisibilityPipelineBenchmark] auto-run disabled by manual override");
        }

        activeCommandCount_ = tierActiveCommandCount_[activeTier_];
        activeInstanceCount_ = tierActiveInstanceCount_[activeTier_];
        countsDirty_ = true;
        ResetCullFrameOverlap();

        if (tierChanged)
        {
            LogActiveTier();
        }
    }

    void UpdateCamera(float dt)
    {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys == nullptr)
        {
            return;
        }

        const Vec3 forward = BuildForwardFromYawPitch(cameraYaw_, cameraPitch_);
        const Vec3 right = BuildRightFromForward(forward);
        const Vec3 worldUp = Vec3{ 0.0f, 1.0f, 0.0f };

        Vec3 move = {};
        if (keys[SDL_SCANCODE_W])
        {
            move = VecAdd(move, forward);
        }
        if (keys[SDL_SCANCODE_S])
        {
            move = VecSub(move, forward);
        }
        if (keys[SDL_SCANCODE_D])
        {
            move = VecAdd(move, right);
        }
        if (keys[SDL_SCANCODE_A])
        {
            move = VecSub(move, right);
        }
        if (keys[SDL_SCANCODE_E])
        {
            move = VecAdd(move, worldUp);
        }
        if (keys[SDL_SCANCODE_Q])
        {
            move = VecSub(move, worldUp);
        }

        const float speedMul = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) ? 3.0f : 1.0f;
        const float speed = cameraMoveSpeed_ * speedMul;
        const Vec3 moveDir = VecNormalize(move);
        cameraPosition_ = VecAdd(cameraPosition_, VecScale(moveDir, speed * dt));
    }

    void ResetCameraToScene()
    {
        const float distance = std::max(sceneExtent_ * 0.68f, 20.0f);
        cameraPosition_ = Vec3{ 0.0f, std::max(sceneExtent_ * 0.16f, 3.8f), -distance };
        cameraYaw_ = 0.0f;
        cameraPitch_ = -0.18f;
    }

    void LogStartupControls() const
    {
        SDL_Log("[WickedVisibilityPipelineBenchmark] Controls:");
        SDL_Log("  [R] start benchmark auto-run sweep | [B] start timed benchmark (30s auto-close) | [SPACE] toggle auto-run");
        SDL_Log("  [C] reset camera near center");
        SDL_Log("  Camera: hold RMB to look, WASD move, Q/E vertical, Shift boost");
        SDL_Log("  [1/2/3] pipeline Wicked/TVB/Esoterica | [M] suite Portable/Mesh");
        SDL_Log("  [Z/X] scenario AllVisible/HighCulling | [UP/DOWN] or [[/]] or [PgUp/PgDn] triangle tier");
        SDL_Log("  [U] toggle benchmark parity run (locks Mesh+Wicked+Bindless+150M + async/tokens/hiz/padding/bias/flecs/validation, auto-closes in 30s)");
        SDL_Log("  [J] toggle async compute cull queue | [T] toggle tokenized submission mode");
        SDL_Log("  [O] toggle Hi-Z occlusion | [;/' ] decrease/increase Hi-Z occlusion bias | [V] toggle validation/hash pass");
        SDL_Log("  [,/.] decrease/increase cull padding pixels");
        SDL_Log("  [K] toggle bindful/bindless resource mode | [Y] toggle Flecs motion update");
        SDL_Log("  [P] print current perf snapshot | [H] print controls and path summary");
    }

    void LogRenderPathSummary() const
    {
        SDL_Log("[WickedVisibilityPipelineBenchmark] Render path summary:");
        SDL_Log("  Wicked: instance+cluster (sphere+Hi-Z) cull -> fused visible arg compaction -> indexed cluster draw");
        SDL_Log("  TVB: triangle visibility filtering with Hi-Z coarse reject -> indexed draw from TVB filtered index stream");
        SDL_Log("  Esoterica: instance+cluster (sphere+Hi-Z) cull -> TVB triangle filtering on visible clusters");
        SDL_Log("  Shared backbone: GPU instance cull, cluster cull, TVB filtering, and Hi-Z depth pyramid build");
        SDL_Log("  Motion: Flecs ECS updates per-instance orbit+bob+yaw, then updates the GPU instance buffer each frame");
        SDL_Log("  Queueing: async mode uses one-frame-lag ping-pong so frame N draw can overlap frame N+1 cull");
        SDL_Log("  Queue sync: [T] token mode removes forced end-of-frame cross-queue waits and uses explicit slot-token waits");
        SDL_Log("  Parity: [U] locks Mesh+Wicked+Bindless at 150M and starts a 30s timed run");
        SDL_Log("  Resource binding: [K] toggles bindful slots vs bindless descriptor-index CB");
    }

    void LogActiveTier() const
    {
        SDL_Log(
            "[WickedVisibilityPipelineBenchmark] tier=%s (targetTriangles=%llu) activeCommands=%u activeInstances=%u",
            kTierPresets[activeTier_].name,
            static_cast<unsigned long long>(kTierPresets[activeTier_].targetTriangles),
            activeCommandCount_,
            activeInstanceCount_);
    }

    bool UseBindlessMode() const
    {
        return activeBindingMode_ == BindingMode::Bindless && bindlessShadersAvailable_;
    }

    SubsetBindlessCB BuildBindlessCB(uint32_t slot)
    {
        SubsetBindlessCB cb = {};
        GPUBuffer* visibleCommandIndicesBuffer = VisibleCommandIndicesBuffer(slot);
        GPUBuffer* visibleCountBuffer = VisibleCountBuffer(slot);
        GPUBuffer* visibleArgsBuffer = VisibleArgsBuffer(slot);
        GPUBuffer* tvbFilteredIndexBuffer = TVBFilteredIndexBuffer(slot);
        GPUBuffer* tvbArgsBuffer = TVBArgsBuffer(slot);
        GPUBuffer* tvbFilteredPrimitiveIDBuffer = TVBFilteredPrimitiveIDBuffer(slot);
        GPUBuffer* meshDispatchArgsBuffer = MeshDispatchArgsBuffer(slot);

        cb.vertexBufferSRV = device_->GetDescriptorIndex(&vertexBuffer_, SubresourceType::SRV);
        cb.instanceBufferSRV = device_->GetDescriptorIndex(&instanceBuffer_, SubresourceType::SRV);
        cb.commandBufferSRV = device_->GetDescriptorIndex(&commandBuffer_, SubresourceType::SRV);
        cb.clusterTemplateBufferSRV = device_->GetDescriptorIndex(&clusterTemplateBuffer_, SubresourceType::SRV);
        cb.templateVerticesBufferSRV = device_->GetDescriptorIndex(&templateVerticesBuffer_, SubresourceType::SRV);
        cb.templateTrianglesBufferSRV = device_->GetDescriptorIndex(&templateTrianglesBuffer_, SubresourceType::SRV);
        cb.instanceVisibleSRV = device_->GetDescriptorIndex(&instanceVisibleBuffer_, SubresourceType::SRV);
        cb.visibleCommandIndicesSRV = device_->GetDescriptorIndex(visibleCommandIndicesBuffer, SubresourceType::SRV);
        cb.sourceArgsSRV = device_->GetDescriptorIndex(&baseArgsBuffer_, SubresourceType::SRV);
        cb.tvbFilteredPrimitiveIDsSRV = device_->GetDescriptorIndex(tvbFilteredPrimitiveIDBuffer, SubresourceType::SRV);
        cb.visibleCountSRV = device_->GetDescriptorIndex(visibleCountBuffer, SubresourceType::SRV);

        cb.instanceVisibleUAV = device_->GetDescriptorIndex(&instanceVisibleBuffer_, SubresourceType::UAV);
        cb.visibleCommandIndicesUAV = device_->GetDescriptorIndex(visibleCommandIndicesBuffer, SubresourceType::UAV);
        cb.visibleCountUAV = device_->GetDescriptorIndex(visibleCountBuffer, SubresourceType::UAV);
        cb.visibleArgsUAV = device_->GetDescriptorIndex(visibleArgsBuffer, SubresourceType::UAV);
        cb.tvbFilteredIndicesUAV = device_->GetDescriptorIndex(tvbFilteredIndexBuffer, SubresourceType::UAV);
        cb.tvbArgsUAV = device_->GetDescriptorIndex(tvbArgsBuffer, SubresourceType::UAV);
        cb.tvbFilteredPrimitiveIDsUAV = device_->GetDescriptorIndex(tvbFilteredPrimitiveIDBuffer, SubresourceType::UAV);
        cb.hashUAV = device_->GetDescriptorIndex(&hashBuffer_, SubresourceType::UAV);
        cb.meshDispatchArgsUAV = device_->GetDescriptorIndex(meshDispatchArgsBuffer, SubresourceType::UAV);

        return cb;
    }

    bool ValidateBindlessCB(const SubsetBindlessCB& cb)
    {
        struct IndexField
        {
            const char* name;
            int32_t value;
        };
        const IndexField fields[] = {
            { "vertexBufferSRV", cb.vertexBufferSRV },
            { "instanceBufferSRV", cb.instanceBufferSRV },
            { "commandBufferSRV", cb.commandBufferSRV },
            { "clusterTemplateBufferSRV", cb.clusterTemplateBufferSRV },
            { "templateVerticesBufferSRV", cb.templateVerticesBufferSRV },
            { "templateTrianglesBufferSRV", cb.templateTrianglesBufferSRV },
            { "instanceVisibleSRV", cb.instanceVisibleSRV },
            { "visibleCommandIndicesSRV", cb.visibleCommandIndicesSRV },
            { "sourceArgsSRV", cb.sourceArgsSRV },
            { "tvbFilteredPrimitiveIDsSRV", cb.tvbFilteredPrimitiveIDsSRV },
            { "visibleCountSRV", cb.visibleCountSRV },
            { "instanceVisibleUAV", cb.instanceVisibleUAV },
            { "visibleCommandIndicesUAV", cb.visibleCommandIndicesUAV },
            { "visibleCountUAV", cb.visibleCountUAV },
            { "visibleArgsUAV", cb.visibleArgsUAV },
            { "tvbFilteredIndicesUAV", cb.tvbFilteredIndicesUAV },
            { "tvbArgsUAV", cb.tvbArgsUAV },
            { "tvbFilteredPrimitiveIDsUAV", cb.tvbFilteredPrimitiveIDsUAV },
            { "hashUAV", cb.hashUAV },
            { "meshDispatchArgsUAV", cb.meshDispatchArgsUAV },
        };

        bool valid = true;
        std::string missing;
        for (const IndexField& field : fields)
        {
            if (field.value < 0)
            {
                valid = false;
                if (!missing.empty())
                {
                    missing += ", ";
                }
                missing += field.name;
            }
        }

        if (!valid && !bindlessDescriptorFailureLogged_)
        {
            bindlessDescriptorFailureLogged_ = true;
#if WICKED_SUBSET_USE_VULKAN
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "[WickedVisibilityPipelineBenchmark] disabling bindless mode at runtime "
                "because descriptor indices are missing (%s). "
                "This adapter likely exhausted Vulkan bindless descriptor capacity for this workload.",
                missing.c_str());
#else
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "[WickedVisibilityPipelineBenchmark] disabling bindless mode at runtime "
                "because descriptor indices are missing (%s).",
                missing.c_str());
#endif
        }

        return valid;
    }

    bool RenderFrame(float dt, FrameMetrics* outMetrics)
    {
        if (requestQuit_)
        {
            return false;
        }

        if (benchmarkParityMode_)
        {
            ApplyBenchmarkParityPreset(false);
        }

        const uint64_t perfFreq = SDL_GetPerformanceFrequency();
        const uint64_t cpuBegin = SDL_GetPerformanceCounter();

        sceneTime_ += dt;
        UpdateECSWorld(dt);

        const float aspect = static_cast<float>(std::max(1u, swapchain_.desc.width)) /
                             static_cast<float>(std::max(1u, swapchain_.desc.height));

        UpdateCamera(dt);
        const Vec3 forward = BuildForwardFromYawPitch(cameraYaw_, cameraPitch_);
        const Vec3 eye = cameraPosition_;
        const Vec3 target = VecAdd(cameraPosition_, forward);
        const float fov = activeScenario_ == ScenarioMode::HighCulling ? 0.74f : 0.90f;

        const Mat4 view = MatLookAtLH(eye, target, Vec3{ 0.0f, 1.0f, 0.0f });
        const Mat4 proj = MatPerspectiveFovLH(fov, aspect, 0.05f, std::max(sceneExtent_ * 12.0f, 300.0f));
        const Mat4 viewProj = MatMul(view, proj);

        SceneCB sceneCB = {};
        std::memcpy(sceneCB.viewProj, viewProj.m, sizeof(sceneCB.viewProj));
        sceneCB.projectionScale[0] = proj.m[0];
        sceneCB.projectionScale[1] = proj.m[5];
        sceneCB.viewportSize[0] = static_cast<float>(swapchain_.desc.width);
        sceneCB.viewportSize[1] = static_cast<float>(swapchain_.desc.height);
        sceneCB.activeCommandCount = activeCommandCount_;
        sceneCB.activeInstanceCount = activeInstanceCount_;
        sceneCB.pipelineStyle = static_cast<uint32_t>(activePipeline_);
        sceneCB.meshUseVisibleList = (activePipeline_ != PipelineStyle::TVB && activeSuite_ == SuiteMode::Mesh) ? 1u : 0u;
        sceneCB.meshCommandOffset = 0u;
        sceneCB.hiZMipCount = hiZMipCount_;
        sceneCB.hiZSourceMip = 0u;
        sceneCB.hiZEnabled = hiZOcclusionEnabled_ ? 1u : 0u;
        sceneCB.hiZValid = hiZOcclusionValid_ ? 1u : 0u;
        sceneCB.cullPaddingPixels = cullPaddingPixels_;
        sceneCB.hiZOcclusionBias = hiZOcclusionBias_;

        FrameMetrics metrics = {};
        const uint32_t frameIndex = device_->GetBufferIndex();
        ReadTimingAndCounters(frameIndex, &metrics);

        const bool useAsyncComputeForCull = IsAsyncCullActiveForCurrentMode();
        uint32_t drawSlot = 0u;
        uint32_t cullSlot = 0u;
        if (useAsyncComputeForCull)
        {
            // One-frame-lag pipeline:
            // draw reads the previously produced slot while async compute writes the next slot.
            if (cullHistoryValid_)
            {
                drawSlot = cullReadSlot_;
                cullSlot = cullWriteSlot_;
            }
            else
            {
                drawSlot = cullWriteSlot_;
                cullSlot = cullWriteSlot_;
            }
        }
        SubmissionToken explicitSubmitDependencies[3] = {};
        uint32_t explicitSubmitDependencyCount = 0;
        auto appendSubmitDependency = [&](const SubmissionToken& dependency) {
            if (!dependency.IsValid() || explicitSubmitDependencyCount >= arraysize(explicitSubmitDependencies))
                return;
            for (uint32_t i = 0; i < explicitSubmitDependencyCount; ++i)
            {
                if (explicitSubmitDependencies[i].queue_mask != dependency.queue_mask)
                    continue;
                bool same = true;
                for (uint32_t q = 0; q < wi::QUEUE_COUNT; ++q)
                {
                    if (explicitSubmitDependencies[i].values[q] != dependency.values[q])
                    {
                        same = false;
                        break;
                    }
                }
                if (same)
                    return;
            }
            explicitSubmitDependencies[explicitSubmitDependencyCount++] = dependency;
        };
        if (useAsyncComputeForCull && tokenSubmissionEnabled_ && cullHistoryValid_)
        {
            appendSubmitDependency(cullCompletionTokens_[drawSlot]);
            // With a 2-slot cull ring, the next async-cull write slot was read by prior-frame draw.
            // Waiting on previous graphics completion avoids write-after-read hazards (flicker).
            appendSubmitDependency(lastGraphicsCompletionToken_);
        }
        if (useAsyncComputeForCull && tokenSubmissionEnabled_ && hiZOcclusionEnabled_ && hiZOcclusionValid_)
        {
            appendSubmitDependency(lastGraphicsCompletionToken_);
        }

        GPUBuffer* drawVisibleCountBuffer = VisibleCountBuffer(drawSlot);
        ResourceState* drawVisibleCountState = VisibleCountBufferState(drawSlot);
        GPUBuffer* cullVisibleCountBuffer = VisibleCountBuffer(cullSlot);
        ResourceState* cullVisibleCountState = VisibleCountBufferState(cullSlot);

        CommandList cmdCull = {};
        if (useAsyncComputeForCull)
        {
            cmdCull = device_->BeginCommandList(QUEUE_COMPUTE);
        }
        CommandList cmdGraphics = device_->BeginCommandList(QUEUE_GRAPHICS);
        if (useAsyncComputeForCull)
        {
            // keep compute cull on its own queue
        }
        else
        {
            cmdCull = cmdGraphics;
        }

        if (ecsMotionEnabled_ && BuildECSInstanceSnapshot(activeInstanceCount_))
        {
            const uint64_t instanceUploadBytes = static_cast<uint64_t>(activeInstanceCount_) * sizeof(GPUInstanceData);
            if (instanceUploadBytes > 0u)
            {
                TransitionBufferState(&instanceBuffer_, &instanceBufferState_, ResourceState::COPY_DST, cmdCull);
                device_->UpdateBuffer(&instanceBuffer_, ecsSnapshotScratch_.data(), cmdCull, instanceUploadBytes, 0);
                TransitionBufferState(&instanceBuffer_, &instanceBufferState_, ResourceState::SHADER_RESOURCE_COMPUTE, cmdCull);
            }
        }

        if (countsDirty_)
        {
            TransitionBufferState(&baseCountBuffer_, &baseCountBufferState_, ResourceState::COPY_DST, cmdGraphics);
            device_->UpdateBuffer(&baseCountBuffer_, &activeCommandCount_, cmdGraphics, sizeof(activeCommandCount_), 0);
            TransitionBufferState(&baseCountBuffer_, &baseCountBufferState_, ResourceState::INDIRECT_ARGUMENT, cmdGraphics);
            countsDirty_ = false;
        }

        TransitionBufferState(cullVisibleCountBuffer, cullVisibleCountState, ResourceState::UNORDERED_ACCESS, cmdCull);
        device_->ClearUAV(cullVisibleCountBuffer, 0u, cmdCull);
        TransitionBufferState(&hashBuffer_, &hashBufferState_, ResourceState::UNORDERED_ACCESS, cmdGraphics);
        device_->ClearUAV(&hashBuffer_, 0u, cmdGraphics);

        device_->QueryReset(&timestampQueryHeap_, 0, kTimestampCount, cmdGraphics);
        device_->QueryEnd(&timestampQueryHeap_, kTimestampFrameStart, cmdGraphics);

        if (!useAsyncComputeForCull)
        {
            device_->QueryEnd(&timestampQueryHeap_, kTimestampCullStart, cmdGraphics);
            ExecuteCullStage(sceneCB, cmdCull, cullSlot);
            device_->QueryEnd(&timestampQueryHeap_, kTimestampCullEnd, cmdGraphics);
        }
        else
        {
            ExecuteCullStage(sceneCB, cmdCull, cullSlot);
            if (!cullHistoryValid_)
            {
                // Prime first async frame so draw has valid data before one-frame-lag overlap begins.
                device_->WaitCommandList(cmdGraphics, cmdCull);
            }

            // Cull timestamps are graphics-queue-local in this benchmark path.
            // Keep them valid and deterministic when cull executes on async compute.
            device_->QueryEnd(&timestampQueryHeap_, kTimestampCullStart, cmdGraphics);
            device_->QueryEnd(&timestampQueryHeap_, kTimestampCullEnd, cmdGraphics);
        }
        if (activePipeline_ != PipelineStyle::TVB)
        {
            TransitionBufferState(
                drawVisibleCountBuffer,
                drawVisibleCountState,
                ResourceState::COPY_SRC,
                cmdGraphics);
            device_->CopyBuffer(&visibleCountReadback_[frameIndex], 0, drawVisibleCountBuffer, 0, sizeof(uint32_t), cmdGraphics);
        }
        else
        {
            visibleCountReady_[frameIndex] = false;
        }

        device_->QueryEnd(&timestampQueryHeap_, kTimestampDrawStart, cmdGraphics);
        PrepareDrawBufferStates(cmdGraphics, drawSlot);
        ExecuteDrawStage(sceneCB, cmdGraphics, drawSlot);
        device_->QueryEnd(&timestampQueryHeap_, kTimestampDrawEnd, cmdGraphics);

        if (useAsyncComputeForCull && tokenSubmissionEnabled_ && hiZOcclusionEnabled_)
        {
            // Hi-Z build writes the same texture async cull reads. Keep draw/cull overlap,
            // but wait before rebuilding Hi-Z to avoid read/write races.
            device_->WaitCommandList(cmdGraphics, cmdCull);
        }
        ExecuteHiZBuildStage(sceneCB, cmdGraphics);

        device_->QueryEnd(&timestampQueryHeap_, kTimestampHashStart, cmdGraphics);
        ExecuteHashStage(sceneCB, cmdGraphics, frameIndex, drawSlot);
        device_->QueryEnd(&timestampQueryHeap_, kTimestampHashEnd, cmdGraphics);

        PrepareAsyncCullBaselineStates(cmdGraphics, drawSlot);
        ExecutePresentStage(cmdGraphics);

        device_->QueryEnd(&timestampQueryHeap_, kTimestampFrameEnd, cmdGraphics);

        device_->QueryResolve(&timestampQueryHeap_, 0, kTimestampCount, &timestampReadback_[frameIndex], 0, cmdGraphics);

        SubmissionToken submitToken = {};
        if (tokenSubmissionEnabled_)
        {
            SubmitDesc submitDesc = {};
            submitDesc.submission_dependencies = explicitSubmitDependencies;
            submitDesc.submission_dependency_count = explicitSubmitDependencyCount;
            submitToken = device_->SubmitCommandListsEx(submitDesc);
            const QueueSyncPoint graphicsPoint = submitToken.Get(QUEUE_GRAPHICS);
            if (graphicsPoint.IsValid())
            {
                lastGraphicsCompletionToken_ = {};
                lastGraphicsCompletionToken_.Merge(graphicsPoint);
            }
        }
        else
        {
            SubmitDesc submitDesc = {};
            submitDesc.submission_dependencies = explicitSubmitDependencies;
            submitDesc.submission_dependency_count = explicitSubmitDependencyCount;
            submitToken = device_->SubmitCommandListsEx(submitDesc);
        }

        if (useAsyncComputeForCull)
        {
            if (tokenSubmissionEnabled_)
            {
                SubmissionToken cullToken = {};
                QueueSyncPoint cullPoint = submitToken.Get(QUEUE_COMPUTE);
                if (!cullPoint.IsValid())
                {
                    cullPoint = submitToken.Get(QUEUE_GRAPHICS);
                }
                cullToken.Merge(cullPoint);
                cullCompletionTokens_[cullSlot] = cullToken;
            }
            cullHistoryValid_ = true;
            cullReadSlot_ = cullSlot;
            cullWriteSlot_ = cullSlot ^ 1u;
        }
        else
        {
            ResetCullFrameOverlap();
        }

        timestampReady_[frameIndex] = true;
        hashReady_[frameIndex] = true;
        if (activePipeline_ != PipelineStyle::TVB)
        {
            visibleCountReady_[frameIndex] = true;
        }

        const uint64_t cpuEnd = SDL_GetPerformanceCounter();
        metrics.cpuMs = perfFreq > 0 ? (1000.0 * static_cast<double>(cpuEnd - cpuBegin) / static_cast<double>(perfFreq)) : 0.0;
        latestMetrics_ = metrics;

        if (outMetrics != nullptr)
        {
            *outMetrics = metrics;
        }

        return true;
    }

    void ExecuteCullStage(const SceneCB& sceneCB, CommandList cmd, uint32_t cullSlot)
    {
        const Shader* csInstanceFilter = UseBindlessMode() ? &csInstanceFilterBindless_ : &csInstanceFilter_;
        const Shader* csClusterFilter = UseBindlessMode() ? &csClusterFilterBindless_ : &csClusterFilter_;
        const Shader* csTVBFilter = UseBindlessMode() ? &csTVBFilterBindless_ : &csTVBFilter_;
        const Shader* csMeshArgs = UseBindlessMode() ? &csMeshArgsBindless_ : &csMeshArgs_;

        GPUBuffer* visibleCommandIndicesBuffer = VisibleCommandIndicesBuffer(cullSlot);
        GPUBuffer* visibleCountBuffer = VisibleCountBuffer(cullSlot);
        GPUBuffer* meshDispatchArgsBuffer = MeshDispatchArgsBuffer(cullSlot);
        GPUBuffer* visibleArgsBuffer = VisibleArgsBuffer(cullSlot);
        GPUBuffer* tvbFilteredIndexBuffer = TVBFilteredIndexBuffer(cullSlot);
        GPUBuffer* tvbArgsBuffer = TVBArgsBuffer(cullSlot);
        GPUBuffer* tvbFilteredPrimitiveIDBuffer = TVBFilteredPrimitiveIDBuffer(cullSlot);

        const bool requiresTVBFiltering = (activePipeline_ == PipelineStyle::TVB) ||
                                          (activePipeline_ == PipelineStyle::Esoterica && activeSuite_ == SuiteMode::Portable);

        TransitionBufferState(&vertexBuffer_, &vertexBufferState_, ResourceState::SHADER_RESOURCE | ResourceState::VERTEX_BUFFER, cmd);
        TransitionBufferState(&instanceBuffer_, &instanceBufferState_, ResourceState::SHADER_RESOURCE_COMPUTE, cmd);
        TransitionBufferState(&instanceVisibleBuffer_, &instanceVisibleBufferState_, ResourceState::UNORDERED_ACCESS, cmd);
        TransitionBufferState(visibleCommandIndicesBuffer, VisibleCommandIndicesBufferState(cullSlot), ResourceState::UNORDERED_ACCESS, cmd);
        TransitionBufferState(visibleCountBuffer, VisibleCountBufferState(cullSlot), ResourceState::UNORDERED_ACCESS, cmd);
        TransitionBufferState(visibleArgsBuffer, VisibleArgsBufferState(cullSlot), ResourceState::UNORDERED_ACCESS, cmd);
        if (requiresTVBFiltering)
        {
            TransitionBufferState(tvbFilteredIndexBuffer, TVBFilteredIndexBufferState(cullSlot), ResourceState::UNORDERED_ACCESS, cmd);
            TransitionBufferState(tvbArgsBuffer, TVBArgsBufferState(cullSlot), ResourceState::UNORDERED_ACCESS, cmd);
            TransitionBufferState(tvbFilteredPrimitiveIDBuffer, TVBFilteredPrimitiveIDBufferState(cullSlot), ResourceState::UNORDERED_ACCESS, cmd);
            TransitionBufferState(meshDispatchArgsBuffer, MeshDispatchArgsBufferState(cullSlot), ResourceState::UNORDERED_ACCESS, cmd);
        }

        device_->BindDynamicConstantBuffer(sceneCB, 0, cmd);
        BindCommonResources(cmd, cullSlot);
        device_->BindResource(&hiZTexture_, 13, cmd);

        if (activePipeline_ == PipelineStyle::TVB)
        {
            device_->BindComputeShader(csTVBFilter, cmd);
            for (uint32_t dispatchBase = 0; dispatchBase < activeCommandCount_; dispatchBase += kMaxMeshDispatchGroups)
            {
                const uint32_t dispatchCount = std::min(kMaxMeshDispatchGroups, activeCommandCount_ - dispatchBase);
                SceneCB tvbCB = sceneCB;
                tvbCB.meshCommandOffset = dispatchBase;
                device_->BindDynamicConstantBuffer(tvbCB, 0, cmd);
                device_->Dispatch(std::max(1u, dispatchCount), 1, 1, cmd);
            }
            device_->Barrier(cmd);
            return;
        }

        device_->BindComputeShader(csInstanceFilter, cmd);
        const uint32_t instanceGroups = (activeInstanceCount_ + 63u) / 64u;
        device_->Dispatch(std::max(1u, instanceGroups), 1, 1, cmd);
        device_->Barrier(cmd);
        TransitionBufferState(&instanceVisibleBuffer_, &instanceVisibleBufferState_, ResourceState::SHADER_RESOURCE_COMPUTE, cmd);

        device_->BindComputeShader(csClusterFilter, cmd);
        const uint32_t commandGroups = (activeCommandCount_ + 63u) / 64u;
        device_->Dispatch(std::max(1u, commandGroups), 1, 1, cmd);
        device_->Barrier(cmd);
        TransitionBufferState(visibleCommandIndicesBuffer, VisibleCommandIndicesBufferState(cullSlot), ResourceState::SHADER_RESOURCE_COMPUTE, cmd);
        TransitionBufferState(visibleCountBuffer, VisibleCountBufferState(cullSlot), ResourceState::SHADER_RESOURCE_COMPUTE, cmd);

        if (activePipeline_ == PipelineStyle::Esoterica && activeSuite_ == SuiteMode::Portable)
        {
            SceneCB tvbCB = sceneCB;
            tvbCB.meshCommandOffset = 0u;
            device_->BindDynamicConstantBuffer(tvbCB, 0, cmd);

            device_->BindComputeShader(csMeshArgs, cmd);
            device_->Dispatch(1, 1, 1, cmd);
            device_->Barrier(cmd);

            TransitionBufferState(meshDispatchArgsBuffer, MeshDispatchArgsBufferState(cullSlot), ResourceState::INDIRECT_ARGUMENT, cmd);

            device_->BindComputeShader(csTVBFilter, cmd);
            device_->BindDynamicConstantBuffer(tvbCB, 0, cmd);
            device_->DispatchIndirect(meshDispatchArgsBuffer, 0, cmd);

            TransitionBufferState(meshDispatchArgsBuffer, MeshDispatchArgsBufferState(cullSlot), ResourceState::UNORDERED_ACCESS, cmd);
            device_->Barrier(cmd);
        }
    }

    void PrepareAsyncCullBaselineStates(CommandList cmd, uint32_t slot)
    {
        // Keep resources that async cull can touch in compute-compatible states.
        // This avoids compute-queue transitions from graphics-only states (for example: INDEX).
        TransitionBufferState(&vertexBuffer_, &vertexBufferState_, ResourceState::SHADER_RESOURCE | ResourceState::VERTEX_BUFFER, cmd);
        TransitionBufferState(&instanceBuffer_, &instanceBufferState_, ResourceState::SHADER_RESOURCE_COMPUTE, cmd);
        TransitionBufferState(VisibleCommandIndicesBuffer(slot), VisibleCommandIndicesBufferState(slot), ResourceState::UNORDERED_ACCESS, cmd);
        TransitionBufferState(VisibleCountBuffer(slot), VisibleCountBufferState(slot), ResourceState::UNORDERED_ACCESS, cmd);
        TransitionBufferState(MeshDispatchArgsBuffer(slot), MeshDispatchArgsBufferState(slot), ResourceState::UNORDERED_ACCESS, cmd);
        TransitionBufferState(VisibleArgsBuffer(slot), VisibleArgsBufferState(slot), ResourceState::UNORDERED_ACCESS, cmd);
        TransitionBufferState(TVBFilteredIndexBuffer(slot), TVBFilteredIndexBufferState(slot), ResourceState::UNORDERED_ACCESS, cmd);
        TransitionBufferState(TVBArgsBuffer(slot), TVBArgsBufferState(slot), ResourceState::UNORDERED_ACCESS, cmd);
        TransitionBufferState(TVBFilteredPrimitiveIDBuffer(slot), TVBFilteredPrimitiveIDBufferState(slot), ResourceState::UNORDERED_ACCESS, cmd);
    }

    void ExecuteDrawStage(const SceneCB& sceneCB, CommandList cmd, uint32_t drawSlot)
    {
        GPUBuffer* visibleCountBuffer = VisibleCountBuffer(drawSlot);
        GPUBuffer* visibleArgsBuffer = VisibleArgsBuffer(drawSlot);
        GPUBuffer* tvbArgsBuffer = TVBArgsBuffer(drawSlot);
        GPUBuffer* tvbFilteredIndexBuffer = TVBFilteredIndexBuffer(drawSlot);

        RenderPassImage rp[] = {
            wi::wiGraphicsCreateRenderPassImageRenderTarget(
                &primitiveIDTexture_,
                RenderPassImage::LoadOp::CLEAR,
                RenderPassImage::StoreOp::STORE,
                ResourceState::SHADER_RESOURCE,
                ResourceState::SHADER_RESOURCE),
            wi::wiGraphicsCreateRenderPassImageDepthStencil(
                &depthTexture_,
                RenderPassImage::LoadOp::CLEAR,
                RenderPassImage::StoreOp::STORE,
                ResourceState::SHADER_RESOURCE,
                ResourceState::DEPTHSTENCIL,
                ResourceState::SHADER_RESOURCE),
        };

        device_->RenderPassBegin(rp, static_cast<uint32_t>(std::size(rp)), cmd);

        wi::Viewport vp;
        vp.top_left_x = 0.0f;
        vp.top_left_y = 0.0f;
        vp.width = static_cast<float>(swapchain_.desc.width);
        vp.height = static_cast<float>(swapchain_.desc.height);
        vp.min_depth = 0.0f;
        vp.max_depth = 1.0f;

        wi::Rect scissor;
        wiGraphicsRectFromViewport(&scissor, &vp);
        device_->BindViewports(1, &vp, cmd);
        device_->BindScissorRects(1, &scissor, cmd);

        if (activeSuite_ == SuiteMode::Mesh && supportsMeshShaders_ && activePipeline_ != PipelineStyle::TVB)
        {
            const PipelineState* meshPipeline = UseBindlessMode() ? &pipelineMeshBindless_ : &pipelineMesh_;
            device_->BindPipelineState(meshPipeline, cmd);
            BindCommonResources(cmd, drawSlot);

            for (uint32_t dispatchBase = 0; dispatchBase < activeCommandCount_; dispatchBase += kMaxMeshDispatchGroups)
            {
                const uint32_t dispatchCount = std::min(kMaxMeshDispatchGroups, activeCommandCount_ - dispatchBase);
                SceneCB meshCB = sceneCB;
                meshCB.meshCommandOffset = dispatchBase;
                device_->BindDynamicConstantBuffer(meshCB, 0, cmd);
                device_->DispatchMesh(dispatchCount, 1, 1, cmd);
            }
        }
        else
        {
            const PipelineState* indexedPipeline = UseBindlessMode() ? &pipelineIndexedBindless_ : &pipelineIndexed_;
            device_->BindPipelineState(indexedPipeline, cmd);
            device_->BindDynamicConstantBuffer(sceneCB, 0, cmd);
            BindCommonResources(cmd, drawSlot);

            const GPUBuffer* vbs[] = {
                &vertexBuffer_,
                &drawCommandIndexBuffer_,
            };
            const uint32_t strides[] = {
                sizeof(Vec3),
                sizeof(uint32_t),
            };
            const uint64_t offsets[] = {
                0,
                0,
            };
            device_->BindVertexBuffers(vbs, 0, 2, strides, offsets, cmd);

            if (activePipeline_ == PipelineStyle::TVB)
            {
                device_->BindIndexBuffer(tvbFilteredIndexBuffer, wi::IndexBufferFormat::UINT32, 0, cmd);
                device_->DrawIndexedInstancedIndirectCount(tvbArgsBuffer, 0, &baseCountBuffer_, 0, activeCommandCount_, cmd);
            }
            else if (activePipeline_ == PipelineStyle::Esoterica)
            {
                device_->BindIndexBuffer(tvbFilteredIndexBuffer, wi::IndexBufferFormat::UINT32, 0, cmd);
                device_->DrawIndexedInstancedIndirectCount(tvbArgsBuffer, 0, visibleCountBuffer, 0, activeCommandCount_, cmd);
            }
            else
            {
                device_->BindIndexBuffer(&clusterIndexBuffer_, wi::IndexBufferFormat::UINT32, 0, cmd);
                device_->DrawIndexedInstancedIndirectCount(visibleArgsBuffer, 0, visibleCountBuffer, 0, activeCommandCount_, cmd);
            }
        }

        device_->RenderPassEnd(cmd);
    }

    void ExecuteHiZBuildStage(const SceneCB& sceneCB, CommandList cmd)
    {
        if (!hiZOcclusionEnabled_ || hiZMipCount_ == 0u)
        {
            return;
        }

        SceneCB hiZCB = sceneCB;
        hiZCB.hiZSourceMip = 0u;
        device_->BindDynamicConstantBuffer(hiZCB, 0, cmd);
        device_->BindResource(&depthTexture_, 12, cmd);
        device_->BindResource(&hiZTexture_, 13, cmd);
        device_->BindComputeShader(&csHiZInit_, cmd);
        device_->BindUAV(&hiZTexture_, 9, cmd, 0);
        device_->Dispatch(
            std::max(1u, (swapchain_.desc.width + 7u) / 8u),
            std::max(1u, (swapchain_.desc.height + 7u) / 8u),
            1,
            cmd);
        device_->Barrier(cmd);

        device_->BindComputeShader(&csHiZDownsample_, cmd);
        for (uint32_t mip = 1u; mip < hiZMipCount_; ++mip)
        {
            hiZCB.hiZSourceMip = mip - 1u;
            device_->BindDynamicConstantBuffer(hiZCB, 0, cmd);
            device_->BindUAV(&hiZTexture_, 9, cmd, static_cast<int>(mip));

            const uint32_t mipWidth = std::max(1u, swapchain_.desc.width >> mip);
            const uint32_t mipHeight = std::max(1u, swapchain_.desc.height >> mip);
            device_->Dispatch(
                std::max(1u, (mipWidth + 7u) / 8u),
                std::max(1u, (mipHeight + 7u) / 8u),
                1,
                cmd);
            device_->Barrier(cmd);
        }

        hiZOcclusionValid_ = true;
    }

    void ExecuteHashStage(const SceneCB& sceneCB, CommandList cmd, uint32_t frameIndex, uint32_t drawSlot)
    {
        if (!validationEnabled_)
        {
            return;
        }

        device_->BindDynamicConstantBuffer(sceneCB, 0, cmd);
        BindCommonResources(cmd, drawSlot);

        const Shader* csHash = UseBindlessMode() ? &csHashBindless_ : &csHash_;
        device_->BindComputeShader(csHash, cmd);

        const uint32_t groupCountX = (swapchain_.desc.width + 7u) / 8u;
        const uint32_t groupCountY = (swapchain_.desc.height + 7u) / 8u;
        device_->Dispatch(groupCountX, groupCountY, 1, cmd);
        device_->Barrier(cmd);

        TransitionBufferState(&hashBuffer_, &hashBufferState_, ResourceState::COPY_SRC, cmd);
        device_->CopyBuffer(&hashReadback_[frameIndex], 0, &hashBuffer_, 0, sizeof(uint32_t), cmd);
        TransitionBufferState(&hashBuffer_, &hashBufferState_, ResourceState::UNORDERED_ACCESS, cmd);
    }

    void ExecutePresentStage(CommandList cmd)
    {
        device_->RenderPassBegin(&swapchain_, cmd);

        wi::Viewport vp;
        vp.top_left_x = 0.0f;
        vp.top_left_y = 0.0f;
        vp.width = static_cast<float>(swapchain_.desc.width);
        vp.height = static_cast<float>(swapchain_.desc.height);
        vp.min_depth = 0.0f;
        vp.max_depth = 1.0f;

        wi::Rect scissor;
        wiGraphicsRectFromViewport(&scissor, &vp);
        device_->BindViewports(1, &vp, cmd);
        device_->BindScissorRects(1, &scissor, cmd);

        device_->BindPipelineState(&pipelinePresent_, cmd);
        device_->BindResource(&primitiveIDTexture_, 11, cmd);
        device_->Draw(3, 0, cmd);

        device_->RenderPassEnd(cmd);
    }

    void BindCommonResources(CommandList cmd, uint32_t slot)
    {
        GPUBuffer* visibleCommandIndicesBuffer = VisibleCommandIndicesBuffer(slot);
        GPUBuffer* visibleCountBuffer = VisibleCountBuffer(slot);
        GPUBuffer* visibleArgsBuffer = VisibleArgsBuffer(slot);
        GPUBuffer* tvbFilteredIndexBuffer = TVBFilteredIndexBuffer(slot);
        GPUBuffer* tvbArgsBuffer = TVBArgsBuffer(slot);
        GPUBuffer* tvbFilteredPrimitiveIDBuffer = TVBFilteredPrimitiveIDBuffer(slot);
        GPUBuffer* meshDispatchArgsBuffer = MeshDispatchArgsBuffer(slot);

        if (UseBindlessMode())
        {
            const SubsetBindlessCB bindlessCB = BuildBindlessCB(slot);
            if (!ValidateBindlessCB(bindlessCB))
            {
                activeBindingMode_ = BindingMode::Bindful;
            }
            else
            {
                device_->BindDynamicConstantBuffer(bindlessCB, 1, cmd);
                device_->BindResource(&primitiveIDTexture_, 11, cmd);
                return;
            }
        }

        device_->BindResource(&vertexBuffer_, 0, cmd);
        device_->BindResource(&instanceBuffer_, 1, cmd);
        device_->BindResource(&commandBuffer_, 2, cmd);
        device_->BindResource(&clusterTemplateBuffer_, 3, cmd);
        device_->BindResource(&templateVerticesBuffer_, 4, cmd);
        device_->BindResource(&templateTrianglesBuffer_, 5, cmd);
        device_->BindResource(&instanceVisibleBuffer_, 6, cmd);
        device_->BindResource(visibleCommandIndicesBuffer, 7, cmd);
        device_->BindResource(&baseArgsBuffer_, 8, cmd);
        device_->BindResource(tvbFilteredPrimitiveIDBuffer, 9, cmd);
        device_->BindResource(visibleCountBuffer, 10, cmd);
        device_->BindResource(&primitiveIDTexture_, 11, cmd);

        device_->BindUAV(&instanceVisibleBuffer_, 0, cmd);
        device_->BindUAV(visibleCommandIndicesBuffer, 1, cmd);
        device_->BindUAV(visibleCountBuffer, 2, cmd);
        device_->BindUAV(visibleArgsBuffer, 3, cmd);
        device_->BindUAV(tvbFilteredIndexBuffer, 4, cmd);
        device_->BindUAV(tvbArgsBuffer, 5, cmd);
        device_->BindUAV(tvbFilteredPrimitiveIDBuffer, 6, cmd);
        device_->BindUAV(&hashBuffer_, 7, cmd);
        device_->BindUAV(meshDispatchArgsBuffer, 8, cmd);
    }

    void ReadTimingAndCounters(uint32_t frameIndex, FrameMetrics* outMetrics)
    {
        FrameMetrics metrics = {};

        if (timestampReady_[frameIndex] && timestampReadback_[frameIndex].mapped_data != nullptr)
        {
            const uint64_t* q = static_cast<const uint64_t*>(timestampReadback_[frameIndex].mapped_data);
            const double gpuFrequency = static_cast<double>(device_->GetTimestampFrequency()) / 1000.0;
            if (gpuFrequency > 0.0)
            {
                auto safeDelta = [&](uint32_t a, uint32_t b) -> double {
                    if (q[b] < q[a])
                    {
                        return 0.0;
                    }
                    const double ms = static_cast<double>(q[b] - q[a]) / gpuFrequency;
                    if (ms > 1'000'000.0)
                    {
                        return 0.0;
                    }
                    return ms;
                };

                metrics.gpuCullMs = safeDelta(kTimestampCullStart, kTimestampCullEnd);
                metrics.gpuDrawMs = safeDelta(kTimestampDrawStart, kTimestampDrawEnd);
                metrics.gpuFrameMs = safeDelta(kTimestampFrameStart, kTimestampFrameEnd);
            }
        }

        if (hashReady_[frameIndex] && hashReadback_[frameIndex].mapped_data != nullptr)
        {
            metrics.hashValue = *static_cast<const uint32_t*>(hashReadback_[frameIndex].mapped_data);
        }

        if (activePipeline_ == PipelineStyle::TVB)
        {
            metrics.visibleCommands = activeCommandCount_;
        }
        else
        {
            if (visibleCountReady_[frameIndex] && visibleCountReadback_[frameIndex].mapped_data != nullptr)
            {
                metrics.visibleCommands = *static_cast<const uint32_t*>(visibleCountReadback_[frameIndex].mapped_data);
            }
            else
            {
                metrics.visibleCommands = 0u;
            }
        }

        if (outMetrics != nullptr)
        {
            *outMetrics = metrics;
        }
    }

    void StepAutoRun(const FrameMetrics& metrics)
    {
        const uint32_t warmupFrames = 40u;
        const uint32_t measureFrames = 150u;

        if (autoRunComboIndex_ >= combos_.size())
        {
            autoRun_ = false;
            ResetCameraToScene();
            SDL_Log("[WickedVisibilityPipelineBenchmark] auto-run finished");
            return;
        }

        ++autoRunFrame_;
        if (autoRunFrame_ <= warmupFrames)
        {
            return;
        }

        autoRunStats_.cpuMs.push_back(metrics.cpuMs);
        autoRunStats_.gpuCullMs.push_back(metrics.gpuCullMs);
        autoRunStats_.gpuDrawMs.push_back(metrics.gpuDrawMs);
        autoRunStats_.gpuFrameMs.push_back(metrics.gpuFrameMs);
        autoRunStats_.visibleCommands.push_back(metrics.visibleCommands);
        autoRunStats_.hashValues.push_back(metrics.hashValue);

        if ((autoRunFrame_ - warmupFrames) < measureFrames)
        {
            return;
        }

        WriteCSVRow(combos_[autoRunComboIndex_], autoRunStats_);

        autoRunComboIndex_++;
        autoRunFrame_ = 0;
        ResetAggregate(&autoRunStats_);

        if (autoRunComboIndex_ < combos_.size())
        {
            ApplyCombo(combos_[autoRunComboIndex_]);
        }
        else
        {
            autoRun_ = false;
            ResetCameraToScene();
            SDL_Log("[WickedVisibilityPipelineBenchmark] auto-run completed all %zu combos", combos_.size());
        }
    }

    void EnsureCSVHeader()
    {
        if (csvHeaderWritten_)
        {
            return;
        }

        const std::filesystem::path path = std::filesystem::current_path() / "wicked_visibility_pipeline_benchmark.csv";
        const bool exists = std::filesystem::exists(path);

        std::ofstream out(path, std::ios::app);
        if (!out.is_open())
        {
            std::fprintf(stderr, "Failed to open CSV for writing: %s\n", path.string().c_str());
            return;
        }

        if (!exists)
        {
            out << "timestamp,backend,suite,pipeline,scenario,tier,active_instances,active_commands,"
                << "binding,"
                << "avg_cpu_ms,avg_gpu_cull_ms,avg_gpu_draw_ms,avg_gpu_frame_ms,p95_gpu_frame_ms,median_visible_commands,hash_median\n";
        }

        csvPath_ = path;
        csvHeaderWritten_ = true;
    }

    void WriteCSVRow(const ComboConfig& combo, const AggregateStats& stats)
    {
        EnsureCSVHeader();
        if (!csvHeaderWritten_)
        {
            return;
        }

        std::ofstream out(csvPath_, std::ios::app);
        if (!out.is_open())
        {
            std::fprintf(stderr, "Failed to append CSV row: %s\n", csvPath_.string().c_str());
            return;
        }

        const double avgCPU = Average(stats.cpuMs);
        const double avgCull = Average(stats.gpuCullMs);
        const double avgDraw = Average(stats.gpuDrawMs);
        const double avgFrame = Average(stats.gpuFrameMs);
        const double p95Frame = Percentile95(stats.gpuFrameMs);
        const uint32_t medianVisible = MedianU32(stats.visibleCommands);
        const uint32_t medianHash = MedianU32(stats.hashValues);

        out << SDL_GetTicks() << ","
            << BackendName() << ","
            << SuiteName(combo.suite) << ","
            << PipelineName(combo.pipeline) << ","
            << ScenarioName(combo.scenario) << ","
            << kTierPresets[combo.tierIndex].name << ","
            << activeInstanceCount_ << ","
            << activeCommandCount_ << ","
            << BindingModeName(activeBindingMode_) << ","
            << avgCPU << ","
            << avgCull << ","
            << avgDraw << ","
            << avgFrame << ","
            << p95Frame << ","
            << medianVisible << ","
            << medianHash
            << "\n";

        SDL_Log(
            "[WickedVisibilityPipelineBenchmark] csv row | suite=%s pipeline=%s scenario=%s tier=%s avg_gpu=%.3fms p95=%.3fms",
            SuiteName(combo.suite),
            PipelineName(combo.pipeline),
            ScenarioName(combo.scenario),
            kTierPresets[combo.tierIndex].name,
            avgFrame,
            p95Frame);
    }

    void UpdateWindowTitle(const FrameMetrics& metrics)
    {
        const double fps = metrics.cpuMs > 0.0 ? (1000.0 / metrics.cpuMs) : 0.0;
        const double tierMillions = static_cast<double>(kTierPresets[activeTier_].targetTriangles) / 1'000'000.0;

        char title[512];
        std::snprintf(
            title,
            sizeof(title),
            "Visibility Benchmark [%s] | %s | %s | %s | %s | tier %s (%.1fM tris) | cmd %u vis %u inst %u | FPS %.1f | CPU %.2fms | GPU cull %.2fms draw %.2fms frame %.2fms | async=%s tokens=%s hiz=%s flecs=%s parity=%s auto=%s",
            BackendName(),
            SuiteName(activeSuite_),
            PipelineName(activePipeline_),
            ScenarioName(activeScenario_),
            BindingModeName(activeBindingMode_),
            kTierPresets[activeTier_].name,
            tierMillions,
            activeCommandCount_,
            metrics.visibleCommands,
            activeInstanceCount_,
            fps,
            metrics.cpuMs,
            metrics.gpuCullMs,
            metrics.gpuDrawMs,
            metrics.gpuFrameMs,
            IsAsyncCullActiveForCurrentMode() ? "on" : "off",
            tokenSubmissionEnabled_ ? "on" : "off",
            hiZOcclusionEnabled_ ? "on" : "off",
            ecsMotionEnabled_ ? "on" : "off",
            benchmarkParityMode_ ? "on" : "off",
            autoRun_ ? "on" : "off");
        SDL_SetWindowTitle(window_, title);
    }

private:
    SDL_Window* window_ = nullptr;
#if defined(__APPLE__) && WICKED_SUBSET_USE_METAL
    SDL_MetalView metalView_ = nullptr;
#endif
    void* nativeWindow_ = nullptr;

    std::unique_ptr<wi::GraphicsDevice> device_;
    SwapChain swapchain_ = {};

    Shader vsIndexed_ = {};
    Shader vsIndexedBindless_ = {};
    Shader msCluster_ = {};
    Shader msClusterBindless_ = {};
    Shader psIndexed_ = {};
    Shader psIndexedBindless_ = {};
    Shader psMesh_ = {};
    Shader psMeshBindless_ = {};
    Shader vsPresent_ = {};
    Shader psPresent_ = {};

    Shader csInstanceFilter_ = {};
    Shader csInstanceFilterBindless_ = {};
    Shader csClusterFilter_ = {};
    Shader csClusterFilterBindless_ = {};
    Shader csCompactArgs_ = {};
    Shader csTVBFilter_ = {};
    Shader csTVBFilterBindless_ = {};
    Shader csHiZInit_ = {};
    Shader csHiZDownsample_ = {};
    Shader csHash_ = {};
    Shader csHashBindless_ = {};
    Shader csMeshArgs_ = {};
    Shader csMeshArgsBindless_ = {};

    PipelineState pipelineIndexed_ = {};
    PipelineState pipelineIndexedBindless_ = {};
    PipelineState pipelineMesh_ = {};
    PipelineState pipelineMeshBindless_ = {};
    PipelineState pipelinePresent_ = {};
    InputLayout indexedInputLayout_ = {};
    RasterizerState rasterState_ = {};
    DepthStencilState depthStencilState_ = {};
    BlendState blendState_ = {};

    Texture primitiveIDTexture_ = {};
    Texture depthTexture_ = {};
    Texture hiZTexture_ = {};

    GPUBuffer vertexBuffer_ = {};
    GPUBuffer instanceBuffer_ = {};
    GPUBuffer commandBuffer_ = {};
    GPUBuffer clusterTemplateBuffer_ = {};
    GPUBuffer templateVerticesBuffer_ = {};
    GPUBuffer templateTrianglesBuffer_ = {};
    GPUBuffer clusterIndexBuffer_ = {};
    GPUBuffer drawCommandIndexBuffer_ = {};

    GPUBuffer baseArgsBuffer_ = {};
    std::array<GPUBuffer, kCullOutputSlotCount> visibleArgsBuffers_ = {};
    std::array<GPUBuffer, kCullOutputSlotCount> tvbArgsBuffers_ = {};

    GPUBuffer baseCountBuffer_ = {};
    std::array<GPUBuffer, kCullOutputSlotCount> visibleCountBuffers_ = {};
    std::array<GPUBuffer, kCullOutputSlotCount> meshDispatchArgsBuffers_ = {};

    std::array<GPUBuffer, kCullOutputSlotCount> visibleCommandIndicesBuffers_ = {};
    GPUBuffer instanceVisibleBuffer_ = {};

    std::array<GPUBuffer, kCullOutputSlotCount> tvbFilteredIndexBuffers_ = {};
    std::array<GPUBuffer, kCullOutputSlotCount> tvbFilteredPrimitiveIDBuffers_ = {};

    GPUBuffer hashBuffer_ = {};
    std::array<GPUBuffer, wi::GraphicsDevice::GetBufferCount()> hashReadback_ = {};
    std::array<GPUBuffer, wi::GraphicsDevice::GetBufferCount()> visibleCountReadback_ = {};

    ResourceState vertexBufferState_ = ResourceState::SHADER_RESOURCE;
    ResourceState instanceBufferState_ = ResourceState::SHADER_RESOURCE;
    ResourceState drawCommandIndexBufferState_ = ResourceState::VERTEX_BUFFER;
    ResourceState clusterIndexBufferState_ = ResourceState::INDEX_BUFFER;
    ResourceState baseCountBufferState_ = ResourceState::INDIRECT_ARGUMENT;
    std::array<ResourceState, kCullOutputSlotCount> visibleCountBufferStates_ = {};
    std::array<ResourceState, kCullOutputSlotCount> meshDispatchArgsBufferStates_ = {};
    ResourceState hashBufferState_ = ResourceState::UNORDERED_ACCESS;
    ResourceState instanceVisibleBufferState_ = ResourceState::UNORDERED_ACCESS;
    std::array<ResourceState, kCullOutputSlotCount> visibleCommandIndicesBufferStates_ = {};
    std::array<ResourceState, kCullOutputSlotCount> visibleArgsBufferStates_ = {};
    std::array<ResourceState, kCullOutputSlotCount> tvbFilteredIndexBufferStates_ = {};
    std::array<ResourceState, kCullOutputSlotCount> tvbArgsBufferStates_ = {};
    std::array<ResourceState, kCullOutputSlotCount> tvbFilteredPrimitiveIDBufferStates_ = {};

    GPUQueryHeap timestampQueryHeap_ = {};
    std::array<GPUBuffer, wi::GraphicsDevice::GetBufferCount()> timestampReadback_ = {};
    std::array<bool, wi::GraphicsDevice::GetBufferCount()> timestampReady_ = {};
    std::array<bool, wi::GraphicsDevice::GetBufferCount()> hashReady_ = {};
    std::array<bool, wi::GraphicsDevice::GetBufferCount()> visibleCountReady_ = {};

    std::vector<Vec3> vertices_;
    std::vector<GPUInstanceData> instances_;
    std::vector<GPUInstanceData> ecsSnapshotScratch_;
    std::vector<GPUClusterCommand> commands_;
    std::vector<GPUClusterTemplate> clusterTemplates_;
    std::vector<uint32_t> templateVertices_;
    std::vector<uint32_t> templatePackedTriangles_;
    std::vector<uint32_t> clusterDrawIndices_;
    std::vector<uint32_t> drawCommandIndices_;
    std::vector<DrawIndexedIndirectCountArgs> baseArgs_;
    std::vector<ShapeTemplate> shapeTemplates_;

    uint32_t totalInstanceCount_ = 0;
    uint32_t totalCommandCount_ = 0;
    uint32_t totalPrimitiveCount_ = 0;

    std::array<uint32_t, kTierPresets.size()> tierActiveCommandCount_ = {};
    std::array<uint32_t, kTierPresets.size()> tierActiveInstanceCount_ = {};

    bool supportsMeshShaders_ = false;
    bool bindlessShadersAvailable_ = false;

    SuiteMode activeSuite_ = SuiteMode::Portable;
    PipelineStyle activePipeline_ = PipelineStyle::Wicked;
    ScenarioMode activeScenario_ = ScenarioMode::AllVisible;
    BindingMode activeBindingMode_ = BindingMode::Bindful;
    uint32_t activeTier_ = 3;

    uint32_t activeCommandCount_ = 0;
    uint32_t activeInstanceCount_ = 0;

    bool countsDirty_ = true;
    bool requestQuit_ = false;
    bool validationEnabled_ = true;
    bool hiZOcclusionEnabled_ = true;
    bool hiZOcclusionValid_ = false;
    uint32_t hiZMipCount_ = 1u;
    float cullPaddingPixels_ = 6.0f;
    float hiZOcclusionBias_ = 0.0025f;
    bool asyncComputeEnabled_ = true;
    bool tokenSubmissionEnabled_ = false;
    bool bindlessDescriptorFailureLogged_ = false;
    bool ecsMotionEnabled_ = true;
    bool benchmarkParityMode_ = false;
    bool cullHistoryValid_ = false;
    uint32_t cullReadSlot_ = 0u;
    uint32_t cullWriteSlot_ = 1u;
    std::array<SubmissionToken, kCullOutputSlotCount> cullCompletionTokens_ = {};
    SubmissionToken lastGraphicsCompletionToken_ = {};

    float sceneTime_ = 0.0f;
    float sceneExtent_ = 25.0f;
    std::unique_ptr<flecs::world> ecsWorld_;

    Vec3 cameraPosition_ = { 0.0f, 4.0f, -16.0f };
    float cameraYaw_ = 0.0f;
    float cameraPitch_ = -0.10f;
    bool mouseLookActive_ = false;
    float mouseLookSensitivity_ = 0.0025f;
    float cameraMoveSpeed_ = 22.0f;

    bool autoRun_ = false;
    bool timedBenchmarkMode_ = false;
    uint64_t timedBenchmarkStartTick_ = 0;
    std::vector<ComboConfig> combos_;
    size_t autoRunComboIndex_ = 0;
    uint32_t autoRunFrame_ = 0;
    AggregateStats autoRunStats_ = {};

    FrameMetrics latestMetrics_ = {};
    uint64_t runPerfFrequency_ = 0;
    uint64_t runStartTick_ = 0;
    uint64_t runEndTick_ = 0;
    uint64_t runFrameCount_ = 0;
    double runCpuMsAccum_ = 0.0;

    bool csvHeaderWritten_ = false;
    std::filesystem::path csvPath_;
};

} // namespace

int main(int argc, char** argv)
{
    (void)argc;

    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);

    if (!InstallSDLMemoryOverrides())
    {
        return 1;
    }

    SetWorkingDirectoryToExecutableDir(argv != nullptr ? argv[0] : nullptr);

    WickedVisibilityPipelineBenchmark app;
    if (!app.Initialize())
    {
        app.Shutdown();
        return 1;
    }

    app.Run();
    app.Shutdown();
    return 0;
}
