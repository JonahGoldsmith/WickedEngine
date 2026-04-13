#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#endif
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

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
// Force helper macros to follow the selected subset backend for this TU,
// even if EngineConfig defaults differ from CMake target definitions.
#undef WICKED_SUBSET_USE_DX12
#undef WICKED_SUBSET_USE_METAL
#undef WICKED_SUBSET_USE_VULKAN
#define WICKED_SUBSET_USE_DX12 (WICKED_SUBSET_BACKEND == WICKED_SUBSET_BACKEND_DX12)
#define WICKED_SUBSET_USE_METAL (WICKED_SUBSET_BACKEND == WICKED_SUBSET_BACKEND_METAL)
#define WICKED_SUBSET_USE_VULKAN (WICKED_SUBSET_BACKEND == WICKED_SUBSET_BACKEND_VULKAN)

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
#include "frame_tagged_heap_allocator.h"

#ifndef WICKED_SUBSET_FRAME_BUFFERED
#define WICKED_SUBSET_FRAME_BUFFERED 0
#endif
#ifndef WICKED_SUBSET_FRAME_SLOT_COUNT
#define WICKED_SUBSET_FRAME_SLOT_COUNT 16
#endif

#ifndef WICKED_SUBSET_CUBE_SHADER_PATH
#define WICKED_SUBSET_CUBE_SHADER_PATH ""
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

using wi::CommandList;
using wi::PipelineState;
using wi::PipelineStateDesc;
using wi::PrimitiveTopology;
using wi::QUEUE_GRAPHICS;
using wi::Shader;
using wi::ShaderModel;
using wi::ShaderStage;
using wi::SubmissionToken;
using wi::SubmitDesc;
using wi::SwapChain;
using wi::SwapChainDesc;
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
    if (!WickedMMGRInitialize("wicked_subset_cube_demo"))
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

struct CameraCB
{
    float viewProj[16] = {};
};

struct CubePushConstants
{
    float color[4] = {};
    float rotationAxisAngle[4] = {};
};

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

Vec3 VecAdd(const Vec3& a, const Vec3& b)
{
    return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z };
}

Vec3 VecSub(const Vec3& a, const Vec3& b)
{
    return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
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
    const float len2 = VecDot(v, v);
    if (len2 <= 1e-12f)
    {
        return Vec3{ 0.0f, 0.0f, 0.0f };
    }
    const float invLen = 1.0f / std::sqrt(len2);
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

class WickedBackendCubeDemo
{
public:
    bool Initialize()
    {
        SDL_Log("[WickedBackendCubeDemo] Initialize begin");
        SDL_Log("[WickedBackendCubeDemo] Initialize: SDL_Init(SDL_INIT_VIDEO) begin");
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[WickedBackendCubeDemo] Initialize: SDL_Init(SDL_INIT_VIDEO) failed");
            return false;
        }
        SDL_Log("[WickedBackendCubeDemo] Initialize: SDL_Init(SDL_INIT_VIDEO) end");

        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__APPLE__) && WICKED_SUBSET_USE_METAL
        flags = static_cast<SDL_WindowFlags>(flags | SDL_WINDOW_METAL);
#endif
        SDL_Log("[WickedBackendCubeDemo] Initialize: SDL_CreateWindow begin");
        window_ = SDL_CreateWindow("Wicked Backend Cube Demo", 1280, 720, flags);
        if (window_ == nullptr)
        {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[WickedBackendCubeDemo] Initialize: SDL_CreateWindow failed");
            return false;
        }
        SDL_Log("[WickedBackendCubeDemo] Initialize: SDL_CreateWindow end");

#if WICKED_SUBSET_USE_VULKAN && !defined(_WIN32)
        // Vulkan on non-Windows uses SDL surface creation directly from SDL_Window.
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
        // Ask Metal runtime for maximal diagnostics when available:
        setenv("MTL_DEBUG_LAYER", "1", 1);
        setenv("MTL_SHADER_VALIDATION", "1", 1);
        setenv("METAL_DEVICE_WRAPPER_TYPE", "1", 1);

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
            std::make_unique<wi::GraphicsDevice_Metal>(
                ValidationMode::Verbose,
                wi::GPUPreference::Discrete);
#else
#error "WICKED_SUBSET_BACKEND=Metal requires an Apple build."
#endif
#elif WICKED_SUBSET_USE_DX12
#if defined(_WIN32)
            std::make_unique<wi::GraphicsDevice_DX12>(
                ValidationMode::Verbose,
                wi::GPUPreference::Discrete);
#else
#error "WICKED_SUBSET_BACKEND=DX12 requires a Windows build."
#endif
#else
            std::make_unique<wi::GraphicsDevice_Vulkan>(
                (wi::platform::window_type)nativeWindow_,
                ValidationMode::Verbose,
                wi::GPUPreference::Discrete);
#endif

        if (device_ == nullptr)
        {
            std::fprintf(stderr, "Failed to create Wicked graphics device for this platform\n");
            return false;
        }
        std::fprintf(stderr, "[WickedBackendCubeDemo] device created, validation=%s\n", device_->IsDebugDevice() ? "on" : "off");

        if (!CreateSwapchain())
        {
            return false;
        }
        if (!CreatePipeline())
        {
            return false;
        }

        std::fprintf(
            stderr,
            "[WickedBackendCubeDemo] initialized.\n");
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
            // Keep delta-time straightforward and stable: only clamp large spikes.
            const float dt = std::min(static_cast<float>(rawDt), 1.0f / 15.0f);

            UpdateCamera(dt);
            if (!RenderFrame(dt))
            {
                running = false;
                break;
            }

            fpsAccum += rawDt;
            ++fpsFrames;
            if (fpsAccum >= 0.5)
            {
                const double fps = static_cast<double>(fpsFrames) / fpsAccum;
                char title[256];
                std::snprintf(
                    title,
                    sizeof(title),
#if WICKED_SUBSET_USE_DX12
                    "Wicked Backend Cube (DX12) | FPS %.1f | %.2f ms | RMB look, WASDQE move",
#elif WICKED_SUBSET_USE_METAL
                    "Wicked Backend Cube (Metal) | FPS %.1f | %.2f ms | RMB look, WASDQE move",
#else
                    "Wicked Backend Cube (Vulkan) | FPS %.1f | %.2f ms | RMB look, WASDQE move",
#endif
                    fps,
                    fps > 0.0 ? (1000.0 / fps) : 0.0);
                SDL_SetWindowTitle(window_, title);
                fpsAccum = 0.0;
                fpsFrames = 0;
            }
        }
    }

    void Shutdown()
    {
        SDL_Log("[WickedBackendCubeDemo] Shutdown begin");
        if (window_ != nullptr)
        {
            SDL_SetWindowRelativeMouseMode(window_, false);
        }

        if (device_ != nullptr)
        {
            SDL_Log("[WickedBackendCubeDemo] Shutdown: WaitForGPU begin");
            device_->WaitForGPU();
            SDL_Log("[WickedBackendCubeDemo] Shutdown: WaitForGPU end");
        }

        pipeline_ = {};
        vs_ = {};
        ps_ = {};
        swapchain_ = {};
        SDL_Log("[WickedBackendCubeDemo] Shutdown: reset pipeline/shader/swapchain done");
        device_.reset();
        SDL_Log("[WickedBackendCubeDemo] Shutdown: device reset done");

#if defined(__APPLE__) && WICKED_SUBSET_USE_METAL
        if (metalView_ != nullptr)
        {
            SDL_Log("[WickedBackendCubeDemo] Shutdown: SDL_Metal_DestroyView begin");
            SDL_Metal_DestroyView(metalView_);
            metalView_ = nullptr;
            SDL_Log("[WickedBackendCubeDemo] Shutdown: SDL_Metal_DestroyView end");
        }
#endif

        if (window_ != nullptr)
        {
            SDL_Log("[WickedBackendCubeDemo] Shutdown: SDL_DestroyWindow begin");
            SDL_DestroyWindow(window_);
            window_ = nullptr;
            SDL_Log("[WickedBackendCubeDemo] Shutdown: SDL_DestroyWindow end");
        }
#if defined(WICKED_MMGR_ENABLED) && WI_ENGINECONFIG_SUBSET_SKIP_SDL_QUIT
        SDL_Log("[WickedBackendCubeDemo] Shutdown: SDL_Quit skipped (MMGR+config)");
#else
        SDL_Log("[WickedBackendCubeDemo] Shutdown: SDL_Quit begin");
        SDL_Quit();
        SDL_Log("[WickedBackendCubeDemo] Shutdown: SDL_Quit end");
#endif
        SDL_Log("[WickedBackendCubeDemo] Shutdown end");
    }

private:
    bool RenderFrame(float dt)
    {
        if (device_ == nullptr)
        {
            return false;
        }

#if WICKED_SUBSET_FRAME_BUFFERED
        const uint32_t slotIndex = frameSlotCursor_ % static_cast<uint32_t>(frameSlots_.size());
        FrameSlot& slot = frameSlots_[slotIndex];
        if (slot.inUse && slot.submission.IsValid() && !device_->IsSubmissionComplete(slot.submission))
        {
            device_->WaitSubmission(slot.submission);
        }
        slot.inUse = false;
        ++frameSlotCursor_;

        const uint64_t frameTag = taggedHeapFrameId_++;
#endif

        colorPhase_ += colorCycleSpeed_ * dt;
        cubeRotationAngle_ += cubeRotationSpeed_ * dt;

        const Vec3 forward = BuildForwardFromYawPitch(cameraYaw_, cameraPitch_);
        const Vec3 target = VecAdd(cameraPosition_, forward);
        const float aspect = static_cast<float>(std::max(1u, swapchain_.desc.width)) /
                             static_cast<float>(std::max(1u, swapchain_.desc.height));

        const Mat4 view = MatLookAtLH(cameraPosition_, target, Vec3{ 0.0f, 1.0f, 0.0f });
        const Mat4 proj = MatPerspectiveFovLH(0.92f, aspect, 0.05f, 250.0f);
        const Mat4 viewProj = MatMul(view, proj);

        CameraCB cameraCB = {};
        std::memcpy(cameraCB.viewProj, viewProj.m, sizeof(cameraCB.viewProj));

        CubePushConstants push = {};
        push.color[0] = 0.25f + 0.75f * (0.5f + 0.5f * std::sin(colorPhase_ + 0.0f));
        push.color[1] = 0.25f + 0.75f * (0.5f + 0.5f * std::sin(colorPhase_ + 2.1f));
        push.color[2] = 0.25f + 0.75f * (0.5f + 0.5f * std::sin(colorPhase_ + 4.2f));
        push.color[3] = 1.0f;
        push.rotationAxisAngle[0] = 0.4f;
        push.rotationAxisAngle[1] = 1.0f;
        push.rotationAxisAngle[2] = 0.2f;
        push.rotationAxisAngle[3] = cubeRotationAngle_;

#if WICKED_SUBSET_FRAME_BUFFERED
        CameraCB* cameraCBTagged = taggedHeap_.Allocate<CameraCB>(wi::framealloc::MakeFrameTag(frameTag, wi::framealloc::FrameTagKind::Render), 1);
        CubePushConstants* pushTagged = taggedHeap_.Allocate<CubePushConstants>(wi::framealloc::MakeFrameTag(frameTag, wi::framealloc::FrameTagKind::Render), 1);
        if (cameraCBTagged != nullptr)
        {
            *cameraCBTagged = cameraCB;
        }
        if (pushTagged != nullptr)
        {
            *pushTagged = push;
        }
#endif

        CommandList cmd = device_->BeginCommandList(QUEUE_GRAPHICS);

        // Important: this path performs acquire/present scheduling for swapchain rendering.
#if WICKED_SUBSET_FRAME_BUFFERED
        device_->AcquireSwapChainBackBuffer(&swapchain_, cmd);
#endif
        device_->RenderPassBegin(&swapchain_, cmd);

        wi::Viewport vp;
        vp.top_left_x = 0.0f;
        vp.top_left_y = 0.0f;
        vp.width = static_cast<float>(swapchain_.desc.width);
        vp.height = static_cast<float>(swapchain_.desc.height);
        vp.min_depth = 0.0f;
        vp.max_depth = 1.0f;

        wi::Rect scissor;
        wi::wiGraphicsRectFromViewport(&scissor, &vp);

        device_->BindViewports(1, &vp, cmd);
        device_->BindScissorRects(1, &scissor, cmd);
        device_->BindPipelineState(&pipeline_, cmd);

        // Camera matrix is supplied through dynamic CB allocation each frame.
        device_->BindDynamicConstantBuffer(
#if WICKED_SUBSET_FRAME_BUFFERED
            cameraCBTagged != nullptr ? *cameraCBTagged : cameraCB,
#else
            cameraCB,
#endif
            0,
            cmd);
        // Per-draw color and rotation are push constants.
        const CubePushConstants& pushRef =
#if WICKED_SUBSET_FRAME_BUFFERED
            pushTagged != nullptr ? *pushTagged : push;
#else
            push;
#endif
        device_->PushConstants(&pushRef, static_cast<uint32_t>(sizeof(pushRef)), cmd);
        device_->DrawInstanced(36, 1, 0, 0, cmd);

        device_->RenderPassEnd(cmd);

#if WICKED_SUBSET_FRAME_BUFFERED
        SubmitDesc submit = {};
        submit.command_lists = &cmd;
        submit.command_list_count = 1;
        slot.submission = device_->SubmitCommandListsEx(submit);
        slot.inUse = slot.submission.IsValid();
        taggedHeap_.FreeTag(wi::framealloc::MakeFrameTag(frameTag, wi::framealloc::FrameTagKind::Render));
#else
        device_->SubmitCommandLists();
#endif
        if (framesRendered_ == 0)
        {
            std::fprintf(stderr, "[WickedBackendCubeDemo] first frame submitted\n");
        }
        ++framesRendered_;
        return true;
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
        const float speed = moveSpeed_ * speedMul;
        const Vec3 moveDir = VecNormalize(move);
        cameraPosition_ = VecAdd(cameraPosition_, VecScale(moveDir, speed * dt));
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
        desc.clear_color[0] = 0.08f;
        desc.clear_color[1] = 0.10f;
        desc.clear_color[2] = 0.13f;
        desc.clear_color[3] = 1.0f;

        if (!device_->CreateSwapChain(&desc, (wi::platform::window_type)nativeWindow_, &swapchain_))
        {
            std::fprintf(stderr, "CreateSwapChain failed\n");
            return false;
        }
        std::fprintf(stderr, "[WickedBackendCubeDemo] swapchain created: %ux%u\n", desc.width, desc.height);

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
        input.shadersourcefilename = WICKED_SUBSET_CUBE_SHADER_PATH;
        input.include_directories.push_back(std::filesystem::path(WICKED_SUBSET_CUBE_SHADER_PATH).parent_path().string());
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
        if (!output.error_message.empty())
        {
            std::fprintf(stderr, "Shader compile warning (%s): %s\n", entrypoint, output.error_message.c_str());
        }

        const bool created = device_->CreateShader(stage, output.shaderdata, output.shadersize, shader, entrypoint);
        if (!created)
        {
            std::fprintf(stderr, "CreateShader failed (%s), bytecode size=%zu\n", entrypoint, output.shadersize);
            return false;
        }
        std::fprintf(stderr, "Shader created (%s), bytecode size=%zu\n", entrypoint, output.shadersize);
        return true;
    }

    bool CreatePipeline()
    {
        if (!CompileShader(ShaderStage::VS, "vs_main", &vs_))
        {
            return false;
        }
        if (!CompileShader(ShaderStage::PS, "ps_main", &ps_))
        {
            return false;
        }

        PipelineStateDesc pso = {};
        pso.vs = &vs_;
        pso.ps = &ps_;
        pso.pt = PrimitiveTopology::TRIANGLELIST;

        if (!device_->CreatePipelineState(&pso, &pipeline_))
        {
            std::fprintf(stderr, "CreatePipelineState failed\n");
            return false;
        }
        std::fprintf(stderr, "Pipeline created\n");

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
    Shader ps_ = {};
    PipelineState pipeline_ = {};
    uint64_t framesRendered_ = 0;

    Vec3 cameraPosition_ = { 0.0f, 1.6f, -6.5f };
    float cameraYaw_ = 0.0f;
    float cameraPitch_ = -0.15f;
    bool mouseLookActive_ = false;
    float mouseLookSensitivity_ = 0.0025f;
    float moveSpeed_ = 5.0f;
    float colorPhase_ = 0.0f;
    float colorCycleSpeed_ = 0.9f;
    float cubeRotationAngle_ = 0.0f;
    float cubeRotationSpeed_ = 1.1f;

#if WICKED_SUBSET_FRAME_BUFFERED
    struct FrameSlot
    {
        SubmissionToken submission = {};
        bool inUse = false;
    };
    std::array<FrameSlot, WICKED_SUBSET_FRAME_SLOT_COUNT> frameSlots_ = {};
    uint32_t frameSlotCursor_ = 0;
    uint64_t taggedHeapFrameId_ = 1;
    wi::framealloc::FrameTaggedHeapAllocator taggedHeap_;
#endif
};

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
    SDL_Log("[WickedBackendCubeDemo] main begin");

    if (!InstallSDLMemoryOverrides())
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[WickedBackendCubeDemo] InstallSDLMemoryOverrides failed");
        return 1;
    }
    SDL_Log("[WickedBackendCubeDemo] InstallSDLMemoryOverrides done");

    SetWorkingDirectoryToExecutableDir(argv != nullptr ? argv[0] : nullptr);
    SDL_Log("[WickedBackendCubeDemo] Working directory configured");

    WickedBackendCubeDemo app;
    if (!app.Initialize())
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[WickedBackendCubeDemo] Initialize failed, entering Shutdown");
        app.Shutdown();
        return 1;
    }
    SDL_Log("[WickedBackendCubeDemo] Initialize succeeded, entering Run");
    app.Run();
    SDL_Log("[WickedBackendCubeDemo] Run exited, entering Shutdown");
    app.Shutdown();
#if defined(WICKED_MMGR_ENABLED)
    SDL_Log("[WickedBackendCubeDemo] Calling WickedMMGRShutdown");
    WickedMMGRShutdown();
    SDL_Log("[WickedBackendCubeDemo] WickedMMGRShutdown returned");
#endif
    SDL_Log("[WickedBackendCubeDemo] main end");
    return 0;
}
