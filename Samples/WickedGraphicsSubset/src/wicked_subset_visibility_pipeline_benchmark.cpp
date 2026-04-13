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
using wi::RasterizerState;
using wi::RenderPassImage;
using wi::ResourceMiscFlag;
using wi::ResourceState;
using wi::Shader;
using wi::ShaderModel;
using wi::ShaderStage;
using wi::SwapChain;
using wi::SwapChainDesc;
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
    uint32_t padding[3] = {};
};

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

static constexpr std::array<TierPreset, 6> kTierPresets = {
    TierPreset{ "500K", 500'000ull },
    TierPreset{ "1M", 1'000'000ull },
    TierPreset{ "2M", 2'000'000ull },
    TierPreset{ "5M", 5'000'000ull },
    TierPreset{ "10M", 10'000'000ull },
    TierPreset{ "20M", 20'000'000ull },
};

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
            std::make_unique<wi::GraphicsDevice_Metal>(ValidationMode::Disabled, wi::GPUPreference::Discrete);
#else
#error "WICKED_SUBSET_BACKEND=Metal requires an Apple build."
#endif
#elif WICKED_SUBSET_USE_DX12
#if defined(_WIN32)
            std::make_unique<wi::GraphicsDevice_DX12>(ValidationMode::Disabled, wi::GPUPreference::Discrete);
#else
#error "WICKED_SUBSET_BACKEND=DX12 requires a Windows build."
#endif
#else
            std::make_unique<wi::GraphicsDevice_Vulkan>((wi::platform::window_type)nativeWindow_, ValidationMode::Disabled, wi::GPUPreference::Discrete);
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
            "[WickedVisibilityPipelineBenchmark] initialized | backend=%s | mesh=%s | async_compute=%s | instances=%u | commands=%u | auto-run=%s | camera=fly",
            BackendName(),
            supportsMeshShaders_ ? "yes" : "no",
            supportsAsyncCompute_ ? "yes" : "no",
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

            if (autoRun_)
            {
                StepAutoRun(metrics);
            }

            UpdateWindowTitle(metrics);
        }
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

        DestroyResources();

        pipelineIndexed_ = {};
        pipelineMesh_ = {};
        pipelinePresent_ = {};
        csInstanceFilter_ = {};
        csClusterFilter_ = {};
        csCompactArgs_ = {};
        csTVBFilter_ = {};
        csHiZInit_ = {};
        csHiZDownsample_ = {};
        csHash_ = {};
        csMeshArgs_ = {};

        vsIndexed_ = {};
        msCluster_ = {};
        psIndexed_ = {};
        psMesh_ = {};
        vsPresent_ = {};
        psPresent_ = {};

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

    bool CompileShader(ShaderStage stage, const char* entrypoint, Shader* shader)
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

        wi::shadercompiler::CompilerOutput output;
        wi::shadercompiler::Compile(input, output);
        if (!output.IsValid())
        {
            std::fprintf(stderr, "Shader compile failed (%s): %s\n", entrypoint, output.error_message.c_str());
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
        const float spacing = 5.2f;
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

            const float jitterX = (Hash01(instanceIndex * 7u + 13u) - 0.5f) * 0.35f;
            const float jitterY = (Hash01(instanceIndex * 13u + 5u) - 0.5f) * 0.35f;
            const float jitterZ = (Hash01(instanceIndex * 17u + 9u) - 0.5f) * 0.35f;

            const Vec3 translation = {
                (static_cast<float>(x) - half) * spacing + jitterX,
                (static_cast<float>(y) - half) * spacing * 0.55f + jitterY,
                (static_cast<float>(z) - half) * spacing + jitterZ,
            };

            const float scale = 0.75f + Hash01(instanceIndex * 31u + 7u) * 0.65f;

            GPUInstanceData instance = {};
            instance.world = MatScaleTranslate(scale, translation);
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

            for (uint32_t localCluster = 0; localCluster < shape.clusterCount; ++localCluster)
            {
                const uint32_t clusterIndex = shape.clusterStart + localCluster;
                const GPUClusterTemplate& cluster = clusterTemplates_[clusterIndex];

                GPUClusterCommand command = {};
                command.clusterTemplateIndex = clusterIndex;
                command.instanceIndex = instanceIndex;
                command.primitiveBase = primitiveBase + cluster.primitiveOffset;
                commands_.push_back(command);

                IndirectDrawArgsIndexedInstanced args = {};
                args.IndexCountPerInstance = cluster.indexCount;
                args.InstanceCount = 1;
                args.StartIndexLocation = cluster.indexOffset;
                args.BaseVertexLocation = static_cast<int32_t>(cluster.baseVertex);
                args.StartInstanceLocation = static_cast<uint32_t>(commands_.size() - 1u);
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
        desc.size = static_cast<uint64_t>(baseArgs_.size() * sizeof(IndirectDrawArgsIndexedInstanced));
        if (!device_->CreateBuffer(&desc, baseArgs_.data(), &baseArgsBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(baseArgsBuffer) failed\n");
            return false;
        }

        desc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        if (!device_->CreateBuffer(&desc, nullptr, &visibleArgsBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(visibleArgsBuffer) failed\n");
            return false;
        }
        if (!device_->CreateBuffer(&desc, nullptr, &tvbArgsBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(tvbArgsBuffer) failed\n");
            return false;
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
        if (!device_->CreateBuffer(&countDesc, &zero, &visibleCountBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(visibleCountBuffer) failed\n");
            return false;
        }

        GPUBufferDesc dispatchDesc = {};
        dispatchDesc.usage = Usage::DEFAULT;
        dispatchDesc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        dispatchDesc.misc_flags = ResourceMiscFlag::INDIRECT_ARGS | ResourceMiscFlag::BUFFER_RAW;
        dispatchDesc.size = sizeof(IndirectDispatchArgs);
        const IndirectDispatchArgs initialDispatch = { 0u, 1u, 1u };
        if (!device_->CreateBuffer(&dispatchDesc, &initialDispatch, &meshDispatchArgsBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(meshDispatchArgsBuffer) failed\n");
            return false;
        }

        GPUBufferDesc visibilityDesc = {};
        visibilityDesc.usage = Usage::DEFAULT;
        visibilityDesc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        visibilityDesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
        visibilityDesc.stride = sizeof(uint32_t);
        visibilityDesc.size = static_cast<uint64_t>(totalCommandCount_) * sizeof(uint32_t);
        if (!device_->CreateBuffer(&visibilityDesc, nullptr, &visibleCommandIndicesBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(visibleCommandIndicesBuffer) failed\n");
            return false;
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
        if (!device_->CreateBuffer(&tvbIndexDesc, nullptr, &tvbFilteredIndexBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(tvbFilteredIndexBuffer) failed\n");
            return false;
        }

        GPUBufferDesc tvbPrimDesc = {};
        tvbPrimDesc.usage = Usage::DEFAULT;
        tvbPrimDesc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::BIND_UNORDERED_ACCESS;
        tvbPrimDesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
        tvbPrimDesc.stride = sizeof(uint32_t);
        tvbPrimDesc.size = static_cast<uint64_t>(totalCommandCount_) * static_cast<uint64_t>(kMaxClusterTriangles) * sizeof(uint32_t);
        if (!device_->CreateBuffer(&tvbPrimDesc, nullptr, &tvbFilteredPrimitiveIDBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(tvbFilteredPrimitiveIDBuffer) failed\n");
            return false;
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
        device_->SetName(&visibleArgsBuffer_, "subset_visibility_benchmark_visible_args");
        device_->SetName(&tvbArgsBuffer_, "subset_visibility_benchmark_tvb_args");
        device_->SetName(&baseCountBuffer_, "subset_visibility_benchmark_base_count");
        device_->SetName(&visibleCountBuffer_, "subset_visibility_benchmark_visible_count");
        device_->SetName(&meshDispatchArgsBuffer_, "subset_visibility_benchmark_mesh_dispatch_args");
        device_->SetName(&visibleCommandIndicesBuffer_, "subset_visibility_benchmark_visible_commands");
        device_->SetName(&instanceVisibleBuffer_, "subset_visibility_benchmark_instance_visible");
        device_->SetName(&tvbFilteredIndexBuffer_, "subset_visibility_benchmark_tvb_indices");
        device_->SetName(&tvbFilteredPrimitiveIDBuffer_, "subset_visibility_benchmark_tvb_primitive_ids");
        device_->SetName(&hashBuffer_, "subset_visibility_benchmark_hash");

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
        visibleArgsBuffer_ = {};
        tvbArgsBuffer_ = {};
        baseCountBuffer_ = {};
        visibleCountBuffer_ = {};
        meshDispatchArgsBuffer_ = {};
        visibleCommandIndicesBuffer_ = {};
        instanceVisibleBuffer_ = {};
        tvbFilteredIndexBuffer_ = {};
        tvbFilteredPrimitiveIDBuffer_ = {};
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

        SDL_Log(
            "[WickedVisibilityPipelineBenchmark] combo -> suite=%s | pipeline=%s | scenario=%s | tier=%s | commands=%u | instances=%u",
            SuiteName(activeSuite_),
            PipelineName(activePipeline_),
            ScenarioName(activeScenario_),
            kTierPresets[activeTier_].name,
            activeCommandCount_,
            activeInstanceCount_);
    }

    bool IsAsyncCullActiveForCurrentMode() const
    {
#if WICKED_SUBSET_USE_DX12
        // Portable path currently reuses shared indirect/index buffers without per-frame buffering.
        // Keep cull on graphics queue on DX12 to avoid cross-queue overlap corruption.
        if (activeSuite_ == SuiteMode::Portable)
        {
            return false;
        }
#endif
        return asyncComputeEnabled_ && supportsAsyncCompute_;
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
            autoRunComboIndex_ = 0;
            autoRunFrame_ = 0;
            ResetAggregate(&autoRunStats_);
            ApplyCombo(combos_[autoRunComboIndex_]);
            mouseLookActive_ = false;
            SDL_SetWindowRelativeMouseMode(window_, false);
            ResetCameraToScene();
            SDL_Log("[WickedVisibilityPipelineBenchmark] benchmark sweep started");
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
            if (supportsAsyncCompute_)
            {
                asyncComputeEnabled_ = !asyncComputeEnabled_;
                SDL_Log(
                    "[WickedVisibilityPipelineBenchmark] async compute cull queue -> requested=%s effective=%s",
                    asyncComputeEnabled_ ? "enabled" : "disabled",
                    IsAsyncCullActiveForCurrentMode() ? "enabled" : "disabled");
            }
            else
            {
                SDL_Log("[WickedVisibilityPipelineBenchmark] async compute queue is not supported on this backend");
            }
            return;
        }
        else if (key == SDLK_O)
        {
            hiZOcclusionEnabled_ = !hiZOcclusionEnabled_;
            SDL_Log("[WickedVisibilityPipelineBenchmark] Hi-Z occlusion -> %s", hiZOcclusionEnabled_ ? "enabled" : "disabled");
            return;
        }
        else if (key == SDLK_P)
        {
            SDL_Log(
                "[WickedVisibilityPipelineBenchmark] snapshot | suite=%s pipeline=%s scenario=%s tier=%s cmd=%u inst=%u async=%s hiz=%s cpu=%.3fms cull=%.3fms draw=%.3fms frame=%.3fms visible=%u hash=%u",
                SuiteName(activeSuite_),
                PipelineName(activePipeline_),
                ScenarioName(activeScenario_),
                kTierPresets[activeTier_].name,
                activeCommandCount_,
                activeInstanceCount_,
                IsAsyncCullActiveForCurrentMode() ? "on" : "off",
                hiZOcclusionEnabled_ ? "on" : "off",
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

        if (autoRun_)
        {
            autoRun_ = false;
            SDL_Log("[WickedVisibilityPipelineBenchmark] auto-run disabled by manual override");
        }

        activeCommandCount_ = tierActiveCommandCount_[activeTier_];
        activeInstanceCount_ = tierActiveInstanceCount_[activeTier_];
        countsDirty_ = true;

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
        SDL_Log("  [R/B] start benchmark auto-run sweep | [SPACE] toggle auto-run");
        SDL_Log("  [C] reset camera near center");
        SDL_Log("  Camera: hold RMB to look, WASD move, Q/E vertical, Shift boost");
        SDL_Log("  [1/2/3] pipeline Wicked/TVB/Esoterica | [M] suite Portable/Mesh");
        SDL_Log("  [Z/X] scenario AllVisible/HighCulling | [UP/DOWN] or [[/]] or [PgUp/PgDn] triangle tier");
        SDL_Log("  [J] toggle async compute cull queue | [O] toggle Hi-Z occlusion | [V] toggle validation/hash pass");
        SDL_Log("  [P] print current perf snapshot | [H] print controls and path summary");
    }

    void LogRenderPathSummary() const
    {
        SDL_Log("[WickedVisibilityPipelineBenchmark] Render path summary:");
        SDL_Log("  Wicked: instance+cluster (sphere+Hi-Z) cull -> compact visible indirect args -> indexed cluster draw");
        SDL_Log("  TVB: triangle visibility filtering for all active commands -> indexed draw from TVB filtered index stream");
        SDL_Log("  Esoterica: instance+cluster (sphere+Hi-Z) cull -> compact visible list -> TVB triangle filtering on visible clusters");
        SDL_Log("  Shared backbone: GPU instance cull, cluster cull, visible list compaction, and Hi-Z depth pyramid build");
        SDL_Log("  Queueing: cull can run on async compute queue and graphics waits before draw");
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

    bool RenderFrame(float dt, FrameMetrics* outMetrics)
    {
        if (requestQuit_)
        {
            return false;
        }

        const uint64_t perfFreq = SDL_GetPerformanceFrequency();
        const uint64_t cpuBegin = SDL_GetPerformanceCounter();

        sceneTime_ += dt;

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
        sceneCB.hiZEnabled = (hiZOcclusionEnabled_ && activePipeline_ != PipelineStyle::TVB) ? 1u : 0u;
        sceneCB.hiZValid = hiZOcclusionValid_ ? 1u : 0u;

        FrameMetrics metrics = {};
        const uint32_t frameIndex = device_->GetBufferIndex();
        ReadTimingAndCounters(frameIndex, &metrics);

        const bool useAsyncComputeForCull = IsAsyncCullActiveForCurrentMode();
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

        if (countsDirty_)
        {
            device_->UpdateBuffer(&baseCountBuffer_, &activeCommandCount_, cmdGraphics, sizeof(activeCommandCount_), 0);
            countsDirty_ = false;
        }

        const uint32_t zero = 0;
        device_->UpdateBuffer(&visibleCountBuffer_, &zero, cmdCull, sizeof(zero), 0);
        device_->UpdateBuffer(&hashBuffer_, &zero, cmdGraphics, sizeof(zero), 0);

        device_->QueryReset(&timestampQueryHeap_, 0, kTimestampCount, cmdGraphics);
        device_->QueryEnd(&timestampQueryHeap_, kTimestampFrameStart, cmdGraphics);

        if (!useAsyncComputeForCull)
        {
            device_->QueryEnd(&timestampQueryHeap_, kTimestampCullStart, cmdGraphics);
            ExecuteCullStage(sceneCB, cmdCull);
            device_->QueryEnd(&timestampQueryHeap_, kTimestampCullEnd, cmdGraphics);
        }
        else
        {
            ExecuteCullStage(sceneCB, cmdCull);
            device_->WaitCommandList(cmdGraphics, cmdCull);

            // Cull timestamps are graphics-queue-local in this benchmark path.
            // Keep them valid and deterministic when cull executes on async compute.
            device_->QueryEnd(&timestampQueryHeap_, kTimestampCullStart, cmdGraphics);
            device_->QueryEnd(&timestampQueryHeap_, kTimestampCullEnd, cmdGraphics);
        }
        device_->CopyBuffer(&visibleCountReadback_[frameIndex], 0, &visibleCountBuffer_, 0, sizeof(uint32_t), cmdGraphics);

        device_->QueryEnd(&timestampQueryHeap_, kTimestampDrawStart, cmdGraphics);
        ExecuteDrawStage(sceneCB, cmdGraphics);
        device_->QueryEnd(&timestampQueryHeap_, kTimestampDrawEnd, cmdGraphics);

        ExecuteHiZBuildStage(sceneCB, cmdGraphics);

        device_->QueryEnd(&timestampQueryHeap_, kTimestampHashStart, cmdGraphics);
        ExecuteHashStage(sceneCB, cmdGraphics, frameIndex);
        device_->QueryEnd(&timestampQueryHeap_, kTimestampHashEnd, cmdGraphics);

        ExecutePresentStage(cmdGraphics);

        device_->QueryEnd(&timestampQueryHeap_, kTimestampFrameEnd, cmdGraphics);

        device_->QueryResolve(&timestampQueryHeap_, 0, kTimestampCount, &timestampReadback_[frameIndex], 0, cmdGraphics);

        device_->SubmitCommandLists();

        timestampReady_[frameIndex] = true;
        hashReady_[frameIndex] = true;
        visibleCountReady_[frameIndex] = true;

        const uint64_t cpuEnd = SDL_GetPerformanceCounter();
        metrics.cpuMs = perfFreq > 0 ? (1000.0 * static_cast<double>(cpuEnd - cpuBegin) / static_cast<double>(perfFreq)) : 0.0;
        latestMetrics_ = metrics;

        if (outMetrics != nullptr)
        {
            *outMetrics = metrics;
        }

        return true;
    }

    void ExecuteCullStage(const SceneCB& sceneCB, CommandList cmd)
    {
        device_->BindDynamicConstantBuffer(sceneCB, 0, cmd);
        BindCommonResources(cmd);
        device_->BindResource(&hiZTexture_, 13, cmd);

        if (activePipeline_ == PipelineStyle::TVB)
        {
            device_->BindComputeShader(&csTVBFilter_, cmd);
            device_->Dispatch(std::max(1u, activeCommandCount_), 1, 1, cmd);
            device_->Barrier(cmd);
            return;
        }

        device_->BindComputeShader(&csInstanceFilter_, cmd);
        const uint32_t instanceGroups = (activeInstanceCount_ + 63u) / 64u;
        device_->Dispatch(std::max(1u, instanceGroups), 1, 1, cmd);
        device_->Barrier(cmd);

        device_->BindComputeShader(&csClusterFilter_, cmd);
        const uint32_t commandGroups = (activeCommandCount_ + 63u) / 64u;
        device_->Dispatch(std::max(1u, commandGroups), 1, 1, cmd);
        device_->Barrier(cmd);

        device_->BindComputeShader(&csCompactArgs_, cmd);
        device_->Dispatch(std::max(1u, commandGroups), 1, 1, cmd);
        device_->Barrier(cmd);

        if (activePipeline_ == PipelineStyle::Esoterica && activeSuite_ == SuiteMode::Portable)
        {
            device_->BindComputeShader(&csTVBFilter_, cmd);
            device_->Dispatch(std::max(1u, activeCommandCount_), 1, 1, cmd);
            device_->Barrier(cmd);
        }
    }

    void ExecuteDrawStage(const SceneCB& sceneCB, CommandList cmd)
    {
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
            device_->BindPipelineState(&pipelineMesh_, cmd);
            BindCommonResources(cmd);

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
            device_->BindPipelineState(&pipelineIndexed_, cmd);
            device_->BindDynamicConstantBuffer(sceneCB, 0, cmd);
            BindCommonResources(cmd);

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
                device_->BindIndexBuffer(&tvbFilteredIndexBuffer_, wi::IndexBufferFormat::UINT32, 0, cmd);
                device_->DrawIndexedInstancedIndirectCount(&tvbArgsBuffer_, 0, &baseCountBuffer_, 0, activeCommandCount_, cmd);
            }
            else if (activePipeline_ == PipelineStyle::Esoterica)
            {
                device_->BindIndexBuffer(&tvbFilteredIndexBuffer_, wi::IndexBufferFormat::UINT32, 0, cmd);
                device_->DrawIndexedInstancedIndirectCount(&tvbArgsBuffer_, 0, &visibleCountBuffer_, 0, activeCommandCount_, cmd);
            }
            else
            {
                device_->BindIndexBuffer(&clusterIndexBuffer_, wi::IndexBufferFormat::UINT32, 0, cmd);
                device_->DrawIndexedInstancedIndirectCount(&visibleArgsBuffer_, 0, &visibleCountBuffer_, 0, activeCommandCount_, cmd);
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

    void ExecuteHashStage(const SceneCB& sceneCB, CommandList cmd, uint32_t frameIndex)
    {
        if (!validationEnabled_)
        {
            return;
        }

        device_->BindDynamicConstantBuffer(sceneCB, 0, cmd);
        BindCommonResources(cmd);

        device_->BindComputeShader(&csHash_, cmd);

        const uint32_t groupCountX = (swapchain_.desc.width + 7u) / 8u;
        const uint32_t groupCountY = (swapchain_.desc.height + 7u) / 8u;
        device_->Dispatch(groupCountX, groupCountY, 1, cmd);
        device_->Barrier(cmd);

        device_->CopyBuffer(&hashReadback_[frameIndex], 0, &hashBuffer_, 0, sizeof(uint32_t), cmd);
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

    void BindCommonResources(CommandList cmd)
    {
        device_->BindResource(&vertexBuffer_, 0, cmd);
        device_->BindResource(&instanceBuffer_, 1, cmd);
        device_->BindResource(&commandBuffer_, 2, cmd);
        device_->BindResource(&clusterTemplateBuffer_, 3, cmd);
        device_->BindResource(&templateVerticesBuffer_, 4, cmd);
        device_->BindResource(&templateTrianglesBuffer_, 5, cmd);
        device_->BindResource(&instanceVisibleBuffer_, 6, cmd);
        device_->BindResource(&visibleCommandIndicesBuffer_, 7, cmd);
        device_->BindResource(&baseArgsBuffer_, 8, cmd);
        device_->BindResource(&tvbFilteredPrimitiveIDBuffer_, 9, cmd);
        device_->BindResource(&visibleCountBuffer_, 10, cmd);
        device_->BindResource(&primitiveIDTexture_, 11, cmd);

        device_->BindUAV(&instanceVisibleBuffer_, 0, cmd);
        device_->BindUAV(&visibleCommandIndicesBuffer_, 1, cmd);
        device_->BindUAV(&visibleCountBuffer_, 2, cmd);
        device_->BindUAV(&visibleArgsBuffer_, 3, cmd);
        device_->BindUAV(&tvbFilteredIndexBuffer_, 4, cmd);
        device_->BindUAV(&tvbArgsBuffer_, 5, cmd);
        device_->BindUAV(&tvbFilteredPrimitiveIDBuffer_, 6, cmd);
        device_->BindUAV(&hashBuffer_, 7, cmd);
        device_->BindUAV(&meshDispatchArgsBuffer_, 8, cmd);
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
            "Visibility Benchmark [%s] | %s | %s | %s | tier %s (%.1fM tris) | cmd %u vis %u inst %u | FPS %.1f | CPU %.2fms | GPU cull %.2fms draw %.2fms frame %.2fms | async=%s hiz=%s auto=%s",
            BackendName(),
            SuiteName(activeSuite_),
            PipelineName(activePipeline_),
            ScenarioName(activeScenario_),
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
            hiZOcclusionEnabled_ ? "on" : "off",
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
    Shader msCluster_ = {};
    Shader psIndexed_ = {};
    Shader psMesh_ = {};
    Shader vsPresent_ = {};
    Shader psPresent_ = {};

    Shader csInstanceFilter_ = {};
    Shader csClusterFilter_ = {};
    Shader csCompactArgs_ = {};
    Shader csTVBFilter_ = {};
    Shader csHiZInit_ = {};
    Shader csHiZDownsample_ = {};
    Shader csHash_ = {};
    Shader csMeshArgs_ = {};

    PipelineState pipelineIndexed_ = {};
    PipelineState pipelineMesh_ = {};
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
    GPUBuffer visibleArgsBuffer_ = {};
    GPUBuffer tvbArgsBuffer_ = {};

    GPUBuffer baseCountBuffer_ = {};
    GPUBuffer visibleCountBuffer_ = {};
    GPUBuffer meshDispatchArgsBuffer_ = {};

    GPUBuffer visibleCommandIndicesBuffer_ = {};
    GPUBuffer instanceVisibleBuffer_ = {};

    GPUBuffer tvbFilteredIndexBuffer_ = {};
    GPUBuffer tvbFilteredPrimitiveIDBuffer_ = {};

    GPUBuffer hashBuffer_ = {};
    std::array<GPUBuffer, wi::GraphicsDevice::GetBufferCount()> hashReadback_ = {};
    std::array<GPUBuffer, wi::GraphicsDevice::GetBufferCount()> visibleCountReadback_ = {};

    GPUQueryHeap timestampQueryHeap_ = {};
    std::array<GPUBuffer, wi::GraphicsDevice::GetBufferCount()> timestampReadback_ = {};
    std::array<bool, wi::GraphicsDevice::GetBufferCount()> timestampReady_ = {};
    std::array<bool, wi::GraphicsDevice::GetBufferCount()> hashReady_ = {};
    std::array<bool, wi::GraphicsDevice::GetBufferCount()> visibleCountReady_ = {};

    std::vector<Vec3> vertices_;
    std::vector<GPUInstanceData> instances_;
    std::vector<GPUClusterCommand> commands_;
    std::vector<GPUClusterTemplate> clusterTemplates_;
    std::vector<uint32_t> templateVertices_;
    std::vector<uint32_t> templatePackedTriangles_;
    std::vector<uint32_t> clusterDrawIndices_;
    std::vector<uint32_t> drawCommandIndices_;
    std::vector<IndirectDrawArgsIndexedInstanced> baseArgs_;
    std::vector<ShapeTemplate> shapeTemplates_;

    uint32_t totalInstanceCount_ = 0;
    uint32_t totalCommandCount_ = 0;
    uint32_t totalPrimitiveCount_ = 0;

    std::array<uint32_t, kTierPresets.size()> tierActiveCommandCount_ = {};
    std::array<uint32_t, kTierPresets.size()> tierActiveInstanceCount_ = {};

    bool supportsMeshShaders_ = false;

    SuiteMode activeSuite_ = SuiteMode::Portable;
    PipelineStyle activePipeline_ = PipelineStyle::Wicked;
    ScenarioMode activeScenario_ = ScenarioMode::AllVisible;
    uint32_t activeTier_ = 3;

    uint32_t activeCommandCount_ = 0;
    uint32_t activeInstanceCount_ = 0;

    bool countsDirty_ = true;
    bool requestQuit_ = false;
    bool validationEnabled_ = true;
    bool hiZOcclusionEnabled_ = true;
    bool hiZOcclusionValid_ = false;
    uint32_t hiZMipCount_ = 1u;
    bool supportsAsyncCompute_ = true;
    bool asyncComputeEnabled_ = true;

    float sceneTime_ = 0.0f;
    float sceneExtent_ = 25.0f;

    Vec3 cameraPosition_ = { 0.0f, 4.0f, -16.0f };
    float cameraYaw_ = 0.0f;
    float cameraPitch_ = -0.10f;
    bool mouseLookActive_ = false;
    float mouseLookSensitivity_ = 0.0025f;
    float cameraMoveSpeed_ = 22.0f;

    bool autoRun_ = false;
    std::vector<ComboConfig> combos_;
    size_t autoRunComboIndex_ = 0;
    uint32_t autoRunFrame_ = 0;
    AggregateStats autoRunStats_ = {};

    FrameMetrics latestMetrics_ = {};

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
