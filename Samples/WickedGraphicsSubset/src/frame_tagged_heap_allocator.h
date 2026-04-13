#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace wi::framealloc
{
enum class FrameTagKind : uint8_t
{
    Game = 1,
    Render = 2,
    Submit = 3,
};

constexpr uint64_t kFrameTagKindMask = 0xffull;
constexpr uint32_t kFrameTagKindBits = 8;
constexpr uint32_t kFrameTagKindCount = 3;

inline uint64_t MakeFrameTag(uint64_t frameId, FrameTagKind kind)
{
    return (frameId << kFrameTagKindBits) | static_cast<uint64_t>(kind);
}

inline uint32_t FrameTagKindIndex(FrameTagKind kind)
{
    switch (kind)
    {
    case FrameTagKind::Game:
        return 0;
    case FrameTagKind::Render:
        return 1;
    case FrameTagKind::Submit:
        return 2;
    default:
        return 0;
    }
}

inline FrameTagKind FrameTagKindFromTag(uint64_t tag)
{
    return static_cast<FrameTagKind>(tag & kFrameTagKindMask);
}

class FrameTaggedHeapAllocator
{
public:
    static constexpr uint32_t kMaxThreads = 8;
    static constexpr size_t kDefaultBlockSizeBytes = 2ull * 1024ull * 1024ull;

    template<typename T>
    T* Allocate(uint64_t tag, size_t count = 1, uint32_t threadIndex = 0)
    {
        if (count == 0)
            return nullptr;
        return reinterpret_cast<T*>(AllocateBytes(tag, sizeof(T) * count, alignof(T), threadIndex));
    }

    void FreeTag(uint64_t tag)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tagBlocks_.find(tag);
        if (it == tagBlocks_.end())
            return;

        for (size_t blockIndex : it->second)
        {
            if (blockIndex >= blocks_.size())
                continue;
            Block* block = blocks_[blockIndex].get();
            if (block == nullptr)
                continue;
            block->ownerTag.store(0ull, std::memory_order_release);
            block->offset = 0;
            freeBlocks_.push_back(blockIndex);
        }
        tagBlocks_.erase(it);

        for (auto& perThread : activeByThread_)
        {
            for (std::atomic<Block*>& active : perThread)
            {
                Block* block = active.load(std::memory_order_acquire);
                if (block != nullptr && block->ownerTag.load(std::memory_order_acquire) == 0ull)
                {
                    active.store(nullptr, std::memory_order_release);
                }
            }
        }
    }

private:
    struct Block
    {
        std::unique_ptr<uint8_t[]> memory;
        size_t capacity = 0;
        size_t offset = 0;
        std::atomic<uint64_t> ownerTag = 0ull;
    };

    static size_t AlignUp(size_t value, size_t alignment)
    {
        const size_t mask = alignment - 1;
        return (value + mask) & ~mask;
    }

    void* AllocateBytes(uint64_t tag, size_t sizeBytes, size_t alignment, uint32_t threadIndex)
    {
        if (sizeBytes == 0)
            return nullptr;
        if (alignment == 0)
            alignment = 1;
        if (threadIndex >= kMaxThreads)
            threadIndex = 0;

        const uint32_t kind = FrameTagKindIndex(FrameTagKindFromTag(tag));
        Block* active = activeByThread_[threadIndex][kind].load(std::memory_order_acquire);
        if (active != nullptr && active->ownerTag.load(std::memory_order_acquire) == tag)
        {
            const size_t alignedOffset = AlignUp(active->offset, alignment);
            if (alignedOffset + sizeBytes <= active->capacity)
            {
                uint8_t* ptr = active->memory.get() + alignedOffset;
                active->offset = alignedOffset + sizeBytes;
                return ptr;
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        active = activeByThread_[threadIndex][kind].load(std::memory_order_acquire);
        if (active != nullptr && active->ownerTag.load(std::memory_order_acquire) == tag)
        {
            const size_t alignedOffset = AlignUp(active->offset, alignment);
            if (alignedOffset + sizeBytes <= active->capacity)
            {
                uint8_t* ptr = active->memory.get() + alignedOffset;
                active->offset = alignedOffset + sizeBytes;
                return ptr;
            }
        }

        Block* block = FindOrAcquireBlock(tag, sizeBytes, alignment);
        if (block == nullptr)
            return nullptr;

        const size_t alignedOffset = AlignUp(block->offset, alignment);
        uint8_t* ptr = block->memory.get() + alignedOffset;
        block->offset = alignedOffset + sizeBytes;
        activeByThread_[threadIndex][kind].store(block, std::memory_order_release);
        return ptr;
    }

    Block* FindOrAcquireBlock(uint64_t tag, size_t sizeBytes, size_t alignment)
    {
        auto it = tagBlocks_.find(tag);
        if (it != tagBlocks_.end())
        {
            for (size_t blockIndex : it->second)
            {
                if (blockIndex >= blocks_.size())
                    continue;
                Block* block = blocks_[blockIndex].get();
                if (block == nullptr)
                    continue;
                const size_t alignedOffset = AlignUp(block->offset, alignment);
                if (alignedOffset + sizeBytes <= block->capacity)
                {
                    return block;
                }
            }
        }

        const size_t requiredCapacity = AlignUp(sizeBytes, alignment);
        size_t blockIndex = SIZE_MAX;
        for (size_t i = 0; i < freeBlocks_.size(); ++i)
        {
            const size_t candidate = freeBlocks_[i];
            if (candidate >= blocks_.size())
                continue;
            Block* block = blocks_[candidate].get();
            if (block == nullptr)
                continue;
            if (block->capacity >= requiredCapacity)
            {
                blockIndex = candidate;
                freeBlocks_.erase(freeBlocks_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }

        if (blockIndex == SIZE_MAX)
        {
            std::unique_ptr<Block> block = std::make_unique<Block>();
            block->capacity = requiredCapacity > kDefaultBlockSizeBytes ? requiredCapacity : kDefaultBlockSizeBytes;
            block->memory = std::make_unique<uint8_t[]>(block->capacity);
            block->offset = 0;
            blocks_.push_back(std::move(block));
            blockIndex = blocks_.size() - 1;
        }

        Block* selected = blocks_[blockIndex].get();
        if (selected == nullptr)
            return nullptr;
        selected->offset = 0;
        selected->ownerTag.store(tag, std::memory_order_release);
        tagBlocks_[tag].push_back(blockIndex);
        return selected;
    }

    std::array<std::array<std::atomic<Block*>, kFrameTagKindCount>, kMaxThreads> activeByThread_ = {};
    std::vector<std::unique_ptr<Block>> blocks_;
    std::vector<size_t> freeBlocks_;
    std::unordered_map<uint64_t, std::vector<size_t>> tagBlocks_;
    std::mutex mutex_;
};
} // namespace wi::framealloc
