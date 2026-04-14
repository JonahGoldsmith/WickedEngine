#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wiGraphicsDevice.h"

namespace wicked_subset
{

enum class FrameAllocTag : uint8_t
{
    UpdateScratch = 1,
    ExtractedPersistent = 2,
    PreparedPersistent = 3,
    GPUUntilFence = 4,
};

inline uint64_t MakeFrameTag(uint64_t frameId, FrameAllocTag kind)
{
    return (frameId << 8u) | static_cast<uint64_t>(kind);
}

// Lightweight tagged allocator for frame-pipeline payloads.
// Allocations are grouped by tag and released in bulk via ReleaseTag().
class FrameTaggedHeapAllocator
{
public:
    FrameTaggedHeapAllocator() = default;
    FrameTaggedHeapAllocator(const FrameTaggedHeapAllocator&) = delete;
    FrameTaggedHeapAllocator& operator=(const FrameTaggedHeapAllocator&) = delete;

    ~FrameTaggedHeapAllocator()
    {
        Reset();
    }

    template<typename T>
    T* Allocate(uint64_t tag, size_t count, uint32_t /*allocIndex*/ = 0)
    {
        if (tag == 0 || count == 0)
            return nullptr;
        const size_t bytes = sizeof(T) * count;
        void* memory = std::malloc(bytes);
        if (memory == nullptr)
            return nullptr;
        std::memset(memory, 0, bytes);

        std::scoped_lock lock(mutex_);
        allocations_[tag].emplace_back(Allocation{ memory, bytes });
        return static_cast<T*>(memory);
    }

    void ReleaseTag(uint64_t tag)
    {
        if (tag == 0)
            return;

        std::vector<Allocation> retired = {};
        {
            std::scoped_lock lock(mutex_);
            auto it = allocations_.find(tag);
            if (it == allocations_.end())
                return;
            retired = std::move(it->second);
            allocations_.erase(it);
        }

        for (const Allocation& allocation : retired)
        {
            std::free(allocation.ptr);
        }
    }

    size_t BytesUsed(uint64_t tag) const
    {
        std::scoped_lock lock(mutex_);
        auto it = allocations_.find(tag);
        if (it == allocations_.end())
            return 0;
        size_t result = 0;
        for (const Allocation& allocation : it->second)
        {
            result += allocation.bytes;
        }
        return result;
    }

    void Reset()
    {
        std::unordered_map<uint64_t, std::vector<Allocation>> retired = {};
        {
            std::scoped_lock lock(mutex_);
            retired = std::move(allocations_);
            allocations_.clear();
        }
        for (auto& pair : retired)
        {
            for (const Allocation& allocation : pair.second)
            {
                std::free(allocation.ptr);
            }
        }
    }

private:
    struct Allocation
    {
        void* ptr = nullptr;
        size_t bytes = 0;
    };

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::vector<Allocation>> allocations_;
};

} // namespace wicked_subset
