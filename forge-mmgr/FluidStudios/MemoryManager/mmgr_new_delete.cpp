#include "mmgr.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <thread>

extern "C" bool initMemAlloc(const char* appName);
extern "C" void exitMemAlloc(void);

namespace
{
    enum MMGRState : int
    {
        MMGR_STATE_UNINITIALIZED = 0,
        MMGR_STATE_INITIALIZING = 1,
        MMGR_STATE_ACTIVE = 2,
        MMGR_STATE_REPORT_GENERATED = 3,
    };

    std::atomic<int>& MMGRStateFlag() noexcept
    {
        static std::atomic<int> state{ MMGR_STATE_UNINITIALIZED };
        return state;
    }

    std::atomic<bool>& MMGRAtexitRegisteredFlag() noexcept
    {
        static std::atomic<bool> registered{ false };
        return registered;
    }

    void ShutdownMMGROnce() noexcept
    {
        auto& state = MMGRStateFlag();
        for (;;)
        {
            const int current = state.load(std::memory_order_acquire);
            if (current == MMGR_STATE_UNINITIALIZED || current == MMGR_STATE_REPORT_GENERATED)
            {
                return;
            }
            if (current == MMGR_STATE_INITIALIZING)
            {
                std::this_thread::yield();
                continue;
            }

            int expected = MMGR_STATE_ACTIVE;
            if (state.compare_exchange_weak(expected, MMGR_STATE_REPORT_GENERATED, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                exitMemAlloc();
                return;
            }
        }
    }

    void MMGRAtexitHandler()
    {
        ShutdownMMGROnce();
    }

    bool EnsureMMGRInitialized(const char* appName = nullptr) noexcept
    {
        auto& state = MMGRStateFlag();
        for (;;)
        {
            const int current = state.load(std::memory_order_acquire);
            if (current == MMGR_STATE_ACTIVE || current == MMGR_STATE_REPORT_GENERATED)
            {
                return true;
            }
            if (current == MMGR_STATE_INITIALIZING)
            {
                std::this_thread::yield();
                continue;
            }

            int expected = MMGR_STATE_UNINITIALIZED;
            if (!state.compare_exchange_weak(expected, MMGR_STATE_INITIALIZING, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                continue;
            }

            const char* resolvedName = (appName != nullptr && appName[0] != '\0') ? appName : "wickedengine";
            if (!initMemAlloc(resolvedName))
            {
                state.store(MMGR_STATE_UNINITIALIZED, std::memory_order_release);
                return false;
            }

            auto& atexitRegistered = MMGRAtexitRegisteredFlag();
            bool wasRegistered = false;
            if (atexitRegistered.compare_exchange_strong(wasRegistered, true, std::memory_order_acq_rel))
            {
                std::atexit(MMGRAtexitHandler);
            }

            state.store(MMGR_STATE_ACTIVE, std::memory_order_release);
            return true;
        }
    }

    void* MmgrAllocate(size_t size, unsigned int allocationType, size_t alignment)
    {
        if (size == 0)
        {
            size = 1;
        }

        if (!EnsureMMGRInitialized())
        {
            void* fallback = std::malloc(size);
            if (fallback == nullptr)
            {
                throw std::bad_alloc();
            }
            return fallback;
        }
        if (alignment < sizeof(void*))
        {
            alignment = sizeof(void*);
        }

        void* memory = mmgrAllocator("operator new", 0, "operator new", allocationType, alignment, size);
        if (memory == nullptr)
        {
            memory = std::malloc(size);
            if (memory == nullptr)
            {
                throw std::bad_alloc();
            }
        }
        return memory;
    }

    void MmgrFree(void* reportedAddress, unsigned int deallocationType) noexcept
    {
        if (reportedAddress == nullptr)
        {
            return;
        }

        if (!EnsureMMGRInitialized())
        {
            std::free(reportedAddress);
            return;
        }

        if (!mmgrValidateAddress(reportedAddress))
        {
            std::free(reportedAddress);
            return;
        }

        mmgrDeallocator("operator delete", 0, "operator delete", deallocationType, reportedAddress);
    }
}

extern "C" bool WickedMMGRInitialize(const char* appName)
{
    return EnsureMMGRInitialized(appName);
}

extern "C" void WickedMMGRShutdown(void)
{
    ShutdownMMGROnce();
}

void* operator new(size_t size)
{
    return MmgrAllocate(size, m_alloc_new, alignof(std::max_align_t));
}

void* operator new[](size_t size)
{
    return MmgrAllocate(size, m_alloc_new_array, alignof(std::max_align_t));
}

void* operator new(size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return ::operator new(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return ::operator new[](size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void* reportedAddress) noexcept
{
    MmgrFree(reportedAddress, m_alloc_delete);
}

void operator delete[](void* reportedAddress) noexcept
{
    MmgrFree(reportedAddress, m_alloc_delete_array);
}

void operator delete(void* reportedAddress, size_t) noexcept
{
    MmgrFree(reportedAddress, m_alloc_delete);
}

void operator delete[](void* reportedAddress, size_t) noexcept
{
    MmgrFree(reportedAddress, m_alloc_delete_array);
}

#if defined(__cpp_aligned_new)
void* operator new(size_t size, std::align_val_t alignment)
{
    return MmgrAllocate(size, m_alloc_new, static_cast<size_t>(alignment));
}

void* operator new[](size_t size, std::align_val_t alignment)
{
    return MmgrAllocate(size, m_alloc_new_array, static_cast<size_t>(alignment));
}

void operator delete(void* reportedAddress, std::align_val_t) noexcept
{
    MmgrFree(reportedAddress, m_alloc_delete);
}

void operator delete[](void* reportedAddress, std::align_val_t) noexcept
{
    MmgrFree(reportedAddress, m_alloc_delete_array);
}

void operator delete(void* reportedAddress, size_t, std::align_val_t) noexcept
{
    MmgrFree(reportedAddress, m_alloc_delete);
}

void operator delete[](void* reportedAddress, size_t, std::align_val_t) noexcept
{
    MmgrFree(reportedAddress, m_alloc_delete_array);
}
#endif
