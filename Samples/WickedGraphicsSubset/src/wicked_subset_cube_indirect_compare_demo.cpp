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
#include <cstdlib>
#include <filesystem>
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

#ifndef WICKED_SUBSET_CUBE_INDIRECT_COMPARE_SHADER_PATH
#define WICKED_SUBSET_CUBE_INDIRECT_COMPARE_SHADER_PATH ""
#endif

#ifndef WICKED_SUBSET_ENGINE_SHADER_DIR
#define WICKED_SUBSET_ENGINE_SHADER_DIR ""
#endif

#if WICKED_SUBSET_BACKEND < WICKED_SUBSET_BACKEND_VULKAN || \
    WICKED_SUBSET_BACKEND > WICKED_SUBSET_BACKEND_METAL
#error "WICKED_SUBSET_BACKEND must be 0 (Vulkan), 1 (DX12), or 2 (Metal)."
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
using wi::CommandList;
using wi::Format;
using wi::GPUBuffer;
using wi::GPUBufferDesc;
using wi::InputClassification;
using wi::InputLayout;
using wi::PipelineState;
using wi::PipelineStateDesc;
using wi::PrimitiveTopology;
using wi::QUEUE_GRAPHICS;
using wi::ResourceMiscFlag;
using wi::Shader;
using wi::ShaderModel;
using wi::ShaderStage;
using wi::SwapChain;
using wi::SwapChainDesc;
using wi::Usage;
using wi::ValidationMode;

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
    if (!WickedMMGRInitialize("wicked_subset_cube_indirect_compare_demo"))
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

struct Mat4
{
    float m[16] = {};
};

struct SceneCB
{
    float viewProj[16] = {};
    float timeSeconds = 0.0f;
    float padding[3] = {};
};

struct CubeVertex
{
    float position[3] = {};
    float color[3] = {};
};
static_assert(sizeof(CubeVertex) == 24, "CubeVertex ABI mismatch");

static constexpr std::array<Vec3, 36> kUnitCubeVertices = {
    Vec3{-1.0f, -1.0f,  1.0f}, Vec3{ 1.0f, -1.0f,  1.0f}, Vec3{ 1.0f,  1.0f,  1.0f},
    Vec3{-1.0f, -1.0f,  1.0f}, Vec3{ 1.0f,  1.0f,  1.0f}, Vec3{-1.0f,  1.0f,  1.0f},

    Vec3{ 1.0f, -1.0f, -1.0f}, Vec3{-1.0f, -1.0f, -1.0f}, Vec3{-1.0f,  1.0f, -1.0f},
    Vec3{ 1.0f, -1.0f, -1.0f}, Vec3{-1.0f,  1.0f, -1.0f}, Vec3{ 1.0f,  1.0f, -1.0f},

    Vec3{ 1.0f, -1.0f,  1.0f}, Vec3{ 1.0f, -1.0f, -1.0f}, Vec3{ 1.0f,  1.0f, -1.0f},
    Vec3{ 1.0f, -1.0f,  1.0f}, Vec3{ 1.0f,  1.0f, -1.0f}, Vec3{ 1.0f,  1.0f,  1.0f},

    Vec3{-1.0f, -1.0f, -1.0f}, Vec3{-1.0f, -1.0f,  1.0f}, Vec3{-1.0f,  1.0f,  1.0f},
    Vec3{-1.0f, -1.0f, -1.0f}, Vec3{-1.0f,  1.0f,  1.0f}, Vec3{-1.0f,  1.0f, -1.0f},

    Vec3{-1.0f,  1.0f,  1.0f}, Vec3{ 1.0f,  1.0f,  1.0f}, Vec3{ 1.0f,  1.0f, -1.0f},
    Vec3{-1.0f,  1.0f,  1.0f}, Vec3{ 1.0f,  1.0f, -1.0f}, Vec3{-1.0f,  1.0f, -1.0f},

    Vec3{-1.0f, -1.0f, -1.0f}, Vec3{ 1.0f, -1.0f, -1.0f}, Vec3{ 1.0f, -1.0f,  1.0f},
    Vec3{-1.0f, -1.0f, -1.0f}, Vec3{ 1.0f, -1.0f,  1.0f}, Vec3{-1.0f, -1.0f,  1.0f},
};

static constexpr std::array<Vec3, 36> kUnitCubeNormals = {
    Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, 1.0f},
    Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, 1.0f},

    Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, -1.0f},
    Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, -1.0f},

    Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
    Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},

    Vec3{-1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f},
    Vec3{-1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f},

    Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
    Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},

    Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f},
    Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f},
};

// Must match WickedEngine/shaders/ShaderInterop.h::IndirectDrawArgsInstanced layout.
struct IndirectDrawArgsInstanced
{
    uint32_t VertexCountPerInstance = 0;
    uint32_t InstanceCount = 0;
    uint32_t StartVertexLocation = 0;
    uint32_t StartInstanceLocation = 0;
};
static_assert(sizeof(IndirectDrawArgsInstanced) == 16, "IndirectDrawArgsInstanced ABI mismatch");

// Must match WickedEngine/shaders/ShaderInterop.h::IndirectDispatchArgs layout.
struct IndirectDispatchArgs
{
    uint32_t ThreadGroupCountX = 0;
    uint32_t ThreadGroupCountY = 0;
    uint32_t ThreadGroupCountZ = 0;
};
static_assert(sizeof(IndirectDispatchArgs) == 12, "IndirectDispatchArgs ABI mismatch");

#if WICKED_SUBSET_USE_DX12
// DX12 DispatchMeshIndirectCount command signature prefixes a 32-bit root constant.
struct MeshIndirectCountArgs
{
    uint32_t DrawID = 0;
    IndirectDispatchArgs Dispatch = {};
};
static_assert(sizeof(MeshIndirectCountArgs) == 16, "MeshIndirectCountArgs ABI mismatch");
#else
using MeshIndirectCountArgs = IndirectDispatchArgs;
#endif

Mat4 MatIdentity()
{
    Mat4 out = {};
    out.m[0] = 1.0f;
    out.m[5] = 1.0f;
    out.m[10] = 1.0f;
    out.m[15] = 1.0f;
    return out;
}

Mat4 MatMul(const Mat4& a, const Mat4& b)
{
    Mat4 result = {};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                sum += a.m[row * 4 + k] * b.m[k * 4 + col];
            }
            result.m[row * 4 + col] = sum;
        }
    }
    return result;
}

Mat4 MatPerspectiveFovLH(float fovYRadians, float aspect, float zNear, float zFar)
{
    Mat4 result = {};
    const float yScale = 1.0f / std::tan(fovYRadians * 0.5f);
    const float xScale = yScale / std::max(0.001f, aspect);
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
    const float len2 = VecDot(v, v);
    if (len2 <= 1e-12f)
    {
        return Vec3{ 0.0f, 0.0f, 0.0f };
    }
    const float invLen = 1.0f / std::sqrt(len2);
    return Vec3{ v.x * invLen, v.y * invLen, v.z * invLen };
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

void* GetNativeWindowHandle(SDL_Window* window)
{
#if !defined(__APPLE__) && !defined(_WIN32)
    return window;
#else
    if (window == nullptr)
    {
        return nullptr;
    }

    auto queryHandle = [window]() -> void*
    {
        if (!SDL_SyncWindow(window))
        {
            SDL_ClearError();
        }

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
    };

    void* nativeHandle = queryHandle();

#if defined(_WIN32)
    if (nativeHandle == nullptr || IsWindow(static_cast<HWND>(nativeHandle)) == FALSE)
    {
        SDL_PumpEvents();
        nativeHandle = queryHandle();
    }
    if (nativeHandle == nullptr || IsWindow(static_cast<HWND>(nativeHandle)) == FALSE)
    {
        return nullptr;
    }
#endif

    return nativeHandle;
#endif
}

void SetWorkingDirectoryToExecutableDir(const char* argv0)
{
    std::filesystem::path targetPath;

    const char* basePath = SDL_GetBasePath();
    if (basePath != nullptr && basePath[0] != '\0')
    {
        targetPath = std::filesystem::path(basePath);
    }
    else if (argv0 != nullptr && argv0[0] != '\0')
    {
        std::error_code ecAbs;
        targetPath = std::filesystem::absolute(std::filesystem::path(argv0), ecAbs).parent_path();
    }

    if (targetPath.empty())
    {
        return;
    }

    std::error_code ec;
    std::filesystem::current_path(targetPath, ec);
    if (ec)
    {
        std::fprintf(stderr, "Warning: failed to set cwd to executable dir: %s\n", ec.message().c_str());
    }
}

class WickedBackendCubeIndirectCompareDemo
{
public:
    bool Initialize()
    {
        SDL_Log("[WickedBackendCubeIndirectCompareDemo] Initialize begin");

        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }

        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__APPLE__) && WICKED_SUBSET_USE_METAL
        flags = static_cast<SDL_WindowFlags>(flags | SDL_WINDOW_METAL);
#endif
        window_ = SDL_CreateWindow("Wicked Cube Indirect Compare", 1280, 720, flags);
        if (window_ == nullptr)
        {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return false;
        }

        if (!SDL_SyncWindow(window_))
        {
            SDL_ClearError();
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
            std::fprintf(stderr, "Failed to create Wicked graphics device for this platform\n");
            return false;
        }

        if (!CreateSwapchain())
        {
            return false;
        }
        if (!CreatePipeline())
        {
            return false;
        }
        if (!CreateSceneBuffers())
        {
            return false;
        }

        SDL_Log(
            "[WickedBackendCubeIndirectCompareDemo] initialized, cubes=%u, mesh=%s, mesh_count=%s",
            cubeCount_,
            supportsMeshShaders_ ? "yes" : "no",
            supportsMeshIndirectCount_ ? "yes" : "no");
        return true;
    }

    void Run()
    {
        bool running = true;
        uint64_t prevTick = SDL_GetPerformanceCounter();
        const uint64_t perfFreq = SDL_GetPerformanceFrequency();

        double fpsAccum = 0.0;
        uint32_t fpsFrames = 0;

        while (running)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT)
                {
                    running = false;
                }
                else if (event.type == SDL_EVENT_KEY_DOWN && event.key.down && !event.key.repeat)
                {
                    if (event.key.key == SDLK_ESCAPE)
                    {
                        running = false;
                    }
                    else if (event.key.key == SDLK_1)
                    {
                        autoCycleModes_ = false;
                        SetDrawMode(DrawMode::Draw);
                    }
                    else if (event.key.key == SDLK_2)
                    {
                        autoCycleModes_ = false;
                        SetDrawMode(DrawMode::DrawIndirect);
                    }
                    else if (event.key.key == SDLK_3)
                    {
                        autoCycleModes_ = false;
                        SetDrawMode(DrawMode::DrawIndirectCount);
                    }
                    else if (event.key.key == SDLK_4)
                    {
                        autoCycleModes_ = false;
                        SetDrawMode(DrawMode::DispatchMesh);
                    }
                    else if (event.key.key == SDLK_5)
                    {
                        autoCycleModes_ = false;
                        SetDrawMode(DrawMode::DispatchMeshIndirect);
                    }
                    else if (event.key.key == SDLK_6)
                    {
                        autoCycleModes_ = false;
                        SetDrawMode(DrawMode::DispatchMeshIndirectCount);
                    }
                    else if (event.key.key == SDLK_SPACE)
                    {
                        autoCycleModes_ = !autoCycleModes_;
                        modeTimerSeconds_ = 0.0;
                        SDL_Log("[WickedBackendCubeIndirectCompareDemo] auto-cycle %s", autoCycleModes_ ? "enabled" : "disabled");
                    }
                    else if (event.key.key == SDLK_UP)
                    {
                        SetVisibleCubeCount(visibleCubeCount_ + cubeCountStep_);
                    }
                    else if (event.key.key == SDLK_DOWN)
                    {
                        const uint32_t next = visibleCubeCount_ > cubeCountStep_ ? (visibleCubeCount_ - cubeCountStep_) : 1u;
                        SetVisibleCubeCount(next);
                    }
                }
                else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED)
                {
                    if (!RecreateSwapchain())
                    {
                        running = false;
                    }
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

            if (autoCycleModes_)
            {
                modeTimerSeconds_ += rawDt;
                if (modeTimerSeconds_ >= modeAutoSwitchSeconds_)
                {
                    const DrawMode nextMode = NextMode(drawMode_);
                    SetDrawMode(nextMode);
                }
            }

            double cpuFrameMs = 0.0;
            if (!RenderFrame(dt, &cpuFrameMs))
            {
                running = false;
                break;
            }
            AccumulateModeStats(rawDt, cpuFrameMs);

            fpsAccum += rawDt;
            ++fpsFrames;
            if (fpsAccum >= 0.5)
            {
                const double fps = static_cast<double>(fpsFrames) / fpsAccum;
                const ModeStats& stats = modeStats_[ModeToIndex(drawMode_)];
                const double avgCpuMs = stats.frames > 0 ? (stats.cpuMsAccum / static_cast<double>(stats.frames)) : 0.0;

                char title[320];
                std::snprintf(
                    title,
                    sizeof(title),
                    "Wicked Cube Compare (%s) | %s | cubes %u/%u | FPS %.1f | CPU %.2f ms | 1/2/3/4/5/6 mode, Up/Down cubes, Space auto",
                    BackendName(),
                    DrawModeName(drawMode_),
                    visibleCubeCount_,
                    cubeCount_,
                    fps,
                    avgCpuMs);
                SDL_SetWindowTitle(window_, title);

                fpsAccum = 0.0;
                fpsFrames = 0;
            }
        }

        LogModeStats(drawMode_);
    }

    void Shutdown()
    {
        SDL_Log("[WickedBackendCubeIndirectCompareDemo] Shutdown begin");

        if (device_ != nullptr)
        {
            device_->WaitForGPU();
        }

        pipeline_ = {};
        pipelineMesh_ = {};
        vs_ = {};
        ms_ = {};
        ps_ = {};
        wi::DestroyInputLayout(inputLayout_);
        vertexBuffer_ = {};
        indirectArgsBuffer_ = {};
        indirectCountBuffer_ = {};
        meshIndirectArgsBuffer_ = {};
        meshIndirectCountArgsBuffer_ = {};
        meshIndirectCommandCountBuffer_ = {};
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
        SDL_Log("[WickedBackendCubeIndirectCompareDemo] SDL_Quit skipped (MMGR+config)");
#else
        SDL_Quit();
#endif
        SDL_Log("[WickedBackendCubeIndirectCompareDemo] Shutdown end");
    }

private:
    enum class DrawMode : uint32_t
    {
        Draw = 0,
        DrawIndirect = 1,
        DrawIndirectCount = 2,
        DispatchMesh = 3,
        DispatchMeshIndirect = 4,
        DispatchMeshIndirectCount = 5,
    };

    static constexpr uint32_t kDrawModeCount = 6;

    struct ModeStats
    {
        double frameSecondsAccum = 0.0;
        double cpuMsAccum = 0.0;
        uint64_t frames = 0;
    };

    static constexpr uint32_t ModeToIndex(DrawMode mode)
    {
        return static_cast<uint32_t>(mode);
    }

    static const char* DrawModeName(DrawMode mode)
    {
        switch (mode)
        {
            case DrawMode::Draw:
                return "Draw";
            case DrawMode::DrawIndirect:
                return "DrawIndirect";
            case DrawMode::DrawIndirectCount:
                return "DrawIndirectCount";
            case DrawMode::DispatchMesh:
                return "DispatchMesh";
            case DrawMode::DispatchMeshIndirect:
                return "DispatchMeshIndirect";
            case DrawMode::DispatchMeshIndirectCount:
                return "DispatchMeshIndirectCount";
            default:
                return "Unknown";
        }
    }

    static const char* BackendName()
    {
#if WICKED_SUBSET_USE_DX12
        return "DX12";
#elif WICKED_SUBSET_USE_METAL
        return "Metal";
#else
        return "Vulkan";
#endif
    }

    bool IsDrawModeSupported(DrawMode mode) const
    {
        switch (mode)
        {
            case DrawMode::DispatchMesh:
            case DrawMode::DispatchMeshIndirect:
                return supportsMeshShaders_;
            case DrawMode::DispatchMeshIndirectCount:
                return supportsMeshIndirectCount_;
            default:
                return true;
        }
    }

    DrawMode NextMode(DrawMode mode) const
    {
        const uint32_t currentIndex = ModeToIndex(mode);
        for (uint32_t i = 1; i <= kDrawModeCount; ++i)
        {
            const DrawMode candidate = static_cast<DrawMode>((currentIndex + i) % kDrawModeCount);
            if (IsDrawModeSupported(candidate))
            {
                return candidate;
            }
        }
        return mode;
    }

    void SetDrawMode(DrawMode mode)
    {
        if (!IsDrawModeSupported(mode))
        {
            SDL_Log(
                "[WickedBackendCubeIndirectCompareDemo] mode %s is not supported on backend %s",
                DrawModeName(mode),
                BackendName());
            return;
        }

        if (drawMode_ == mode)
        {
            return;
        }

        LogModeStats(drawMode_);
        drawMode_ = mode;
        modeTimerSeconds_ = 0.0;
        SDL_Log("[WickedBackendCubeIndirectCompareDemo] mode -> %s", DrawModeName(drawMode_));
    }

    void SetVisibleCubeCount(uint32_t count)
    {
        const uint32_t clamped = std::max(1u, std::min(cubeCount_, count));
        if (clamped == visibleCubeCount_)
        {
            return;
        }
        visibleCubeCount_ = clamped;
        indirectCountDirty_ = true;
        SDL_Log("[WickedBackendCubeIndirectCompareDemo] visible cubes -> %u", visibleCubeCount_);
    }

    void AccumulateModeStats(double frameSeconds, double cpuMs)
    {
        ModeStats& stats = modeStats_[ModeToIndex(drawMode_)];
        stats.frameSecondsAccum += frameSeconds;
        stats.cpuMsAccum += cpuMs;
        ++stats.frames;
    }

    void LogModeStats(DrawMode mode)
    {
        ModeStats& stats = modeStats_[ModeToIndex(mode)];
        if (stats.frames == 0)
        {
            return;
        }

        const double fps = static_cast<double>(stats.frames) / std::max(1e-9, stats.frameSecondsAccum);
        const double avgCpuMs = stats.cpuMsAccum / static_cast<double>(stats.frames);
        SDL_Log(
            "[WickedBackendCubeIndirectCompareDemo] mode summary | %s | frames=%llu | FPS=%.2f | CPU(ms)=%.3f | cubes=%u",
            DrawModeName(mode),
            static_cast<unsigned long long>(stats.frames),
            fps,
            avgCpuMs,
            visibleCubeCount_);

        stats = {};
    }

    bool RenderFrame(float dt, double* cpuFrameMs)
    {
        if (device_ == nullptr)
        {
            return false;
        }

        sceneTimeSeconds_ += dt;
        sceneOrbitAngle_ += dt * orbitSpeed_;

        const float orbitRadius = std::max(sceneExtent_ * 2.2f, 20.0f);
        const Vec3 eye = {
            std::sin(sceneOrbitAngle_) * orbitRadius,
            std::max(4.0f, sceneExtent_ * 0.55f),
            std::cos(sceneOrbitAngle_) * orbitRadius,
        };
        const Vec3 target = { 0.0f, 0.0f, 0.0f };
        const float aspect = static_cast<float>(std::max(1u, swapchain_.desc.width)) /
                             static_cast<float>(std::max(1u, swapchain_.desc.height));

        const Mat4 view = MatLookAtLH(eye, target, Vec3{ 0.0f, 1.0f, 0.0f });
        const Mat4 proj = MatPerspectiveFovLH(0.92f, aspect, 0.05f, std::max(100.0f, sceneExtent_ * 8.0f));
        const Mat4 viewProj = MatMul(view, proj);

        SceneCB sceneCB = {};
        std::memcpy(sceneCB.viewProj, viewProj.m, sizeof(sceneCB.viewProj));
        sceneCB.timeSeconds = sceneTimeSeconds_;

        const uint64_t perfFreq = SDL_GetPerformanceFrequency();
        const uint64_t cpuBegin = SDL_GetPerformanceCounter();

        CommandList cmd = device_->BeginCommandList(QUEUE_GRAPHICS);

        if (indirectCountDirty_)
        {
            const uint32_t indirectCommandCount = visibleCubeCount_;
            device_->UpdateBuffer(&indirectCountBuffer_, &indirectCommandCount, cmd, sizeof(indirectCommandCount), 0);
            if (supportsMeshShaders_)
            {
                const IndirectDispatchArgs meshArgs = {
                    visibleCubeCount_,
                    1u,
                    1u,
                };
                device_->UpdateBuffer(&meshIndirectArgsBuffer_, &meshArgs, cmd, sizeof(meshArgs), 0);
                MeshIndirectCountArgs meshCountArgs = {};
#if WICKED_SUBSET_USE_DX12
                meshCountArgs.DrawID = 0u;
                meshCountArgs.Dispatch = meshArgs;
#else
                meshCountArgs = meshArgs;
#endif
                device_->UpdateBuffer(&meshIndirectCountArgsBuffer_, &meshCountArgs, cmd, sizeof(meshCountArgs), 0);
                const uint32_t meshIndirectCommandCount = 1u;
                device_->UpdateBuffer(&meshIndirectCommandCountBuffer_, &meshIndirectCommandCount, cmd, sizeof(meshIndirectCommandCount), 0);
            }
            indirectCountDirty_ = false;
        }

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
        const bool meshMode =
            drawMode_ == DrawMode::DispatchMesh ||
            drawMode_ == DrawMode::DispatchMeshIndirect ||
            drawMode_ == DrawMode::DispatchMeshIndirectCount;

        if (meshMode)
        {
            device_->BindPipelineState(&pipelineMesh_, cmd);
            device_->BindDynamicConstantBuffer(sceneCB, 0, cmd);
            device_->BindResource(&vertexBuffer_, 0, cmd);
        }
        else
        {
            device_->BindPipelineState(&pipeline_, cmd);
            device_->BindDynamicConstantBuffer(sceneCB, 0, cmd);

            const GPUBuffer* vbs[] = {
                &vertexBuffer_,
            };
            const uint32_t strides[] = {
                sizeof(CubeVertex),
            };
            const uint64_t offsets[] = {
                0,
            };
            device_->BindVertexBuffers(vbs, 0, 1, strides, offsets, cmd);
        }

        switch (drawMode_)
        {
            case DrawMode::Draw:
            {
                for (uint32_t i = 0; i < visibleCubeCount_; ++i)
                {
                    device_->Draw(36, i * 36u, cmd);
                }
            }
            break;
            case DrawMode::DrawIndirect:
            {
                const uint64_t argStride = sizeof(IndirectDrawArgsInstanced);
                for (uint32_t i = 0; i < visibleCubeCount_; ++i)
                {
                    device_->DrawInstancedIndirect(&indirectArgsBuffer_, argStride * static_cast<uint64_t>(i), cmd);
                }
            }
            break;
            case DrawMode::DrawIndirectCount:
            {
                device_->DrawInstancedIndirectCount(&indirectArgsBuffer_, 0, &indirectCountBuffer_, 0, visibleCubeCount_, cmd);
            }
            break;
            case DrawMode::DispatchMesh:
            {
                device_->DispatchMesh(visibleCubeCount_, 1, 1, cmd);
            }
            break;
            case DrawMode::DispatchMeshIndirect:
            {
                device_->DispatchMeshIndirect(&meshIndirectArgsBuffer_, 0, cmd);
            }
            break;
            case DrawMode::DispatchMeshIndirectCount:
            {
                device_->DispatchMeshIndirectCount(&meshIndirectCountArgsBuffer_, 0, &meshIndirectCommandCountBuffer_, 0, 1, cmd);
            }
            break;
        }

        device_->RenderPassEnd(cmd);

        device_->SubmitCommandLists();

        const uint64_t cpuEnd = SDL_GetPerformanceCounter();
        if (cpuFrameMs != nullptr)
        {
            *cpuFrameMs = perfFreq > 0 ? (1000.0 * static_cast<double>(cpuEnd - cpuBegin) / static_cast<double>(perfFreq)) : 0.0;
        }

        return true;
    }

    bool RecreateSwapchain()
    {
        if (device_ == nullptr)
        {
            return false;
        }

        device_->WaitForGPU();
        swapchain_ = {};
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
        desc.format = wi::Format::B8G8R8A8_UNORM;
        desc.vsync = false;
        desc.clear_color[0] = 0.06f;
        desc.clear_color[1] = 0.08f;
        desc.clear_color[2] = 0.12f;
        desc.clear_color[3] = 1.0f;

#if WICKED_SUBSET_USE_VULKAN && !defined(_WIN32)
        nativeWindow_ = window_;
#else
        nativeWindow_ = GetNativeWindowHandle(window_);
        if (nativeWindow_ == nullptr)
        {
            std::fprintf(stderr, "CreateSwapChain aborted: invalid native window handle\n");
            return false;
        }
#endif

        if (!device_->CreateSwapChain(&desc, (wi::platform::window_type)nativeWindow_, &swapchain_))
        {
            std::fprintf(
                stderr,
                "CreateSwapChain failed (backend=%s, nativeWindow=%p, size=%ux%u)\n",
                device_ != nullptr ? device_->GetTag() : "unknown",
                nativeWindow_,
                desc.width,
                desc.height
            );
            return false;
        }
        return true;
    }

    bool CompileShader(ShaderStage stage, const char* entrypoint, Shader* shader)
    {
        wi::shadercompiler::CompilerInput input;
        input.flags = wi::shadercompiler::Flags::STRIP_REFLECTION;
        input.format = device_->GetShaderFormat();
        input.stage = stage;
        input.minshadermodel = ShaderModel::SM_6_6;
        input.entrypoint = entrypoint;
        input.shadersourcefilename = WICKED_SUBSET_CUBE_INDIRECT_COMPARE_SHADER_PATH;
        input.include_directories.push_back(std::filesystem::path(WICKED_SUBSET_CUBE_INDIRECT_COMPARE_SHADER_PATH).parent_path().string());
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

    bool CreatePipeline()
    {
        supportsMeshShaders_ = false;
        supportsMeshIndirectCount_ = false;

        if (!CompileShader(ShaderStage::VS, "vs_main", &vs_))
        {
            return false;
        }
        if (!CompileShader(ShaderStage::PS, "ps_main", &ps_))
        {
            return false;
        }

        wi::DestroyInputLayout(inputLayout_);
        arrsetlen(inputLayout_.elements, 2);
        for (size_t i = 0; i < 2; ++i)
        {
            wi::InitInputLayoutElement(inputLayout_.elements[i]);
        }
        inputLayout_.elements[0].semantic_name = wi::CloneCString("POSITION");
        inputLayout_.elements[0].semantic_index = 0;
        inputLayout_.elements[0].format = Format::R32G32B32_FLOAT;
        inputLayout_.elements[0].input_slot = 0;
        inputLayout_.elements[0].aligned_byte_offset = (uint32_t)offsetof(CubeVertex, position);
        inputLayout_.elements[0].input_slot_class = InputClassification::PER_VERTEX_DATA;
        inputLayout_.elements[1].semantic_name = wi::CloneCString("COLOR");
        inputLayout_.elements[1].semantic_index = 0;
        inputLayout_.elements[1].format = Format::R32G32B32_FLOAT;
        inputLayout_.elements[1].input_slot = 0;
        inputLayout_.elements[1].aligned_byte_offset = (uint32_t)offsetof(CubeVertex, color);
        inputLayout_.elements[1].input_slot_class = InputClassification::PER_VERTEX_DATA;

        PipelineStateDesc pso = {};
        pso.vs = &vs_;
        pso.ps = &ps_;
        pso.il = &inputLayout_;
        pso.pt = PrimitiveTopology::TRIANGLELIST;

        if (!device_->CreatePipelineState(&pso, &pipeline_))
        {
            std::fprintf(stderr, "CreatePipelineState failed\n");
            return false;
        }

        if (device_->CheckCapability(wi::MESH_SHADER))
        {
            if (!CompileShader(ShaderStage::MS, "ms_main", &ms_))
            {
                return false;
            }

            PipelineStateDesc psoMesh = {};
            psoMesh.ms = &ms_;
            psoMesh.ps = &ps_;
            psoMesh.pt = PrimitiveTopology::TRIANGLELIST;
            if (!device_->CreatePipelineState(&psoMesh, &pipelineMesh_))
            {
                std::fprintf(stderr, "CreatePipelineState(mesh) failed\n");
                return false;
            }

            supportsMeshShaders_ = true;
            supportsMeshIndirectCount_ = true;
        }

        return true;
    }

    bool CreateSceneBuffers()
    {
        std::vector<CubeVertex> vertices(static_cast<size_t>(cubeCount_) * 36u);
        std::vector<IndirectDrawArgsInstanced> drawArgs(cubeCount_);

        const uint32_t side = static_cast<uint32_t>(std::ceil(std::cbrt(static_cast<double>(cubeCount_))));
        const float spacing = 2.35f;
        const float half = static_cast<float>(side - 1u) * 0.5f;
        const float cubeScale = 0.46f;
        const Vec3 lightDir = Vec3{ 0.35f, 0.85f, 0.25f };
        const float lightLen = std::sqrt(lightDir.x * lightDir.x + lightDir.y * lightDir.y + lightDir.z * lightDir.z);
        const Vec3 lightDirN = lightLen > 0.0f
            ? Vec3{ lightDir.x / lightLen, lightDir.y / lightLen, lightDir.z / lightLen }
            : Vec3{ 0.0f, 1.0f, 0.0f };

        uint32_t index = 0;
        for (uint32_t z = 0; z < side && index < cubeCount_; ++z)
        {
            for (uint32_t y = 0; y < side && index < cubeCount_; ++y)
            {
                for (uint32_t x = 0; x < side && index < cubeCount_; ++x)
                {
                    const float fx = (static_cast<float>(x) - half) * spacing;
                    const float fy = (static_cast<float>(y) - half) * spacing * 0.70f;
                    const float fz = (static_cast<float>(z) - half) * spacing;

                    const float hue = static_cast<float>(index) / static_cast<float>(std::max(1u, cubeCount_ - 1u));
                    const float baseColor[3] = {
                        0.35f + 0.65f * (0.5f + 0.5f * std::sin(6.283185f * (hue + 0.00f))),
                        0.35f + 0.65f * (0.5f + 0.5f * std::sin(6.283185f * (hue + 0.33f))),
                        0.35f + 0.65f * (0.5f + 0.5f * std::sin(6.283185f * (hue + 0.66f))),
                    };

                    const size_t vertexBase = static_cast<size_t>(index) * 36u;
                    for (size_t vertex = 0; vertex < 36u; ++vertex)
                    {
                        const Vec3 local = kUnitCubeVertices[vertex];
                        const Vec3 normal = kUnitCubeNormals[vertex];
                        const float ndotl = std::max(0.0f, normal.x * lightDirN.x + normal.y * lightDirN.y + normal.z * lightDirN.z);
                        const float lighting = 0.28f + 0.72f * ndotl;

                        CubeVertex outVertex = {};
                        outVertex.position[0] = local.x * cubeScale + fx;
                        outVertex.position[1] = local.y * cubeScale + fy;
                        outVertex.position[2] = local.z * cubeScale + fz;
                        outVertex.color[0] = baseColor[0] * lighting;
                        outVertex.color[1] = baseColor[1] * lighting;
                        outVertex.color[2] = baseColor[2] * lighting;
                        vertices[vertexBase + vertex] = outVertex;
                    }

                    IndirectDrawArgsInstanced arg = {};
                    arg.VertexCountPerInstance = 36;
                    arg.InstanceCount = 1;
                    arg.StartVertexLocation = index * 36u;
                    arg.StartInstanceLocation = 0;
                    drawArgs[index] = arg;

                    ++index;
                }
            }
        }

        sceneExtent_ = std::max(8.0f, half * spacing);

        GPUBufferDesc vertexDesc = {};
        vertexDesc.usage = Usage::DEFAULT;
        vertexDesc.bind_flags = BindFlag::BIND_VERTEX_BUFFER | BindFlag::BIND_SHADER_RESOURCE;
        vertexDesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
        vertexDesc.stride = sizeof(CubeVertex);
        vertexDesc.size = static_cast<uint64_t>(vertices.size() * sizeof(CubeVertex));
        if (!device_->CreateBuffer(&vertexDesc, vertices.data(), &vertexBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(vertexBuffer) failed\n");
            return false;
        }

        GPUBufferDesc argsDesc = {};
        argsDesc.usage = Usage::DEFAULT;
        argsDesc.misc_flags = ResourceMiscFlag::INDIRECT_ARGS;
        argsDesc.size = static_cast<uint64_t>(drawArgs.size() * sizeof(IndirectDrawArgsInstanced));
        if (!device_->CreateBuffer(&argsDesc, drawArgs.data(), &indirectArgsBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(indirectArgsBuffer) failed\n");
            return false;
        }

        GPUBufferDesc countDesc = {};
        countDesc.usage = Usage::DEFAULT;
        countDesc.misc_flags = ResourceMiscFlag::INDIRECT_ARGS;
        countDesc.size = sizeof(uint32_t);
        const uint32_t indirectCommandCount = visibleCubeCount_;
        if (!device_->CreateBuffer(&countDesc, &indirectCommandCount, &indirectCountBuffer_))
        {
            std::fprintf(stderr, "CreateBuffer(indirectCountBuffer) failed\n");
            return false;
        }

        if (supportsMeshShaders_)
        {
            GPUBufferDesc meshArgsDesc = {};
            meshArgsDesc.usage = Usage::DEFAULT;
            meshArgsDesc.misc_flags = ResourceMiscFlag::INDIRECT_ARGS;
            meshArgsDesc.size = sizeof(IndirectDispatchArgs);
            const IndirectDispatchArgs meshArgs = {
                visibleCubeCount_,
                1u,
                1u,
            };
            if (!device_->CreateBuffer(&meshArgsDesc, &meshArgs, &meshIndirectArgsBuffer_))
            {
                std::fprintf(stderr, "CreateBuffer(meshIndirectArgsBuffer) failed\n");
                return false;
            }

            GPUBufferDesc meshCountArgsDesc = {};
            meshCountArgsDesc.usage = Usage::DEFAULT;
            meshCountArgsDesc.misc_flags = ResourceMiscFlag::INDIRECT_ARGS;
            meshCountArgsDesc.size = sizeof(MeshIndirectCountArgs);
            MeshIndirectCountArgs meshCountArgs = {};
#if WICKED_SUBSET_USE_DX12
            meshCountArgs.DrawID = 0u;
            meshCountArgs.Dispatch = meshArgs;
#else
            meshCountArgs = meshArgs;
#endif
            if (!device_->CreateBuffer(&meshCountArgsDesc, &meshCountArgs, &meshIndirectCountArgsBuffer_))
            {
                std::fprintf(stderr, "CreateBuffer(meshIndirectCountArgsBuffer) failed\n");
                return false;
            }

            const uint32_t meshIndirectCommandCount = 1u;
            if (!device_->CreateBuffer(&countDesc, &meshIndirectCommandCount, &meshIndirectCommandCountBuffer_))
            {
                std::fprintf(stderr, "CreateBuffer(meshIndirectCommandCountBuffer) failed\n");
                return false;
            }
        }

        device_->SetName(&vertexBuffer_, "subset_indirect_compare_vertices");
        device_->SetName(&indirectArgsBuffer_, "subset_indirect_compare_draw_args");
        device_->SetName(&indirectCountBuffer_, "subset_indirect_compare_draw_count");
        if (supportsMeshShaders_)
        {
            device_->SetName(&meshIndirectArgsBuffer_, "subset_indirect_compare_mesh_args");
            device_->SetName(&meshIndirectCountArgsBuffer_, "subset_indirect_compare_mesh_count_args");
            device_->SetName(&meshIndirectCommandCountBuffer_, "subset_indirect_compare_mesh_count");
        }

        return true;
    }

    SDL_Window* window_ = nullptr;
#if defined(__APPLE__) && WICKED_SUBSET_USE_METAL
    SDL_MetalView metalView_ = nullptr;
#endif
    void* nativeWindow_ = nullptr;

    std::unique_ptr<wi::GraphicsDevice> device_;
    SwapChain swapchain_ = {};
    Shader vs_ = {};
    Shader ms_ = {};
    Shader ps_ = {};
    PipelineState pipeline_ = {};
    PipelineState pipelineMesh_ = {};
    InputLayout inputLayout_ = {};

    GPUBuffer vertexBuffer_ = {};
    GPUBuffer indirectArgsBuffer_ = {};
    GPUBuffer indirectCountBuffer_ = {};
    GPUBuffer meshIndirectArgsBuffer_ = {};
    GPUBuffer meshIndirectCountArgsBuffer_ = {};
    GPUBuffer meshIndirectCommandCountBuffer_ = {};

    uint32_t cubeCount_ = 8192;
    uint32_t visibleCubeCount_ = 8192;
    uint32_t cubeCountStep_ = 512;
    bool indirectCountDirty_ = false;
    bool supportsMeshShaders_ = false;
    bool supportsMeshIndirectCount_ = false;

    float sceneTimeSeconds_ = 0.0f;
    float sceneOrbitAngle_ = 0.0f;
    float orbitSpeed_ = 0.20f;
    float sceneExtent_ = 16.0f;

    DrawMode drawMode_ = DrawMode::Draw;
    std::array<ModeStats, kDrawModeCount> modeStats_ = {};
    bool autoCycleModes_ = true;
    double modeTimerSeconds_ = 0.0;
    double modeAutoSwitchSeconds_ = 6.0;
};

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);

    if (!InstallSDLMemoryOverrides())
    {
        return 1;
    }

    SetWorkingDirectoryToExecutableDir(argv != nullptr ? argv[0] : nullptr);

    WickedBackendCubeIndirectCompareDemo app;
    if (!app.Initialize())
    {
        app.Shutdown();
        return 1;
    }

    app.Run();
    app.Shutdown();

#if defined(WICKED_MMGR_ENABLED)
    WickedMMGRShutdown();
#endif

    return 0;
}
