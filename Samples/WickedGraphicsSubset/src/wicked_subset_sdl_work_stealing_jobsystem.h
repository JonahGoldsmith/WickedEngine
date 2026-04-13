#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace wicked_subset
{

class SDLWorkStealingJobSystem
{
public:
    SDLWorkStealingJobSystem() = default;
    SDLWorkStealingJobSystem(const SDLWorkStealingJobSystem&) = delete;
    SDLWorkStealingJobSystem& operator=(const SDLWorkStealingJobSystem&) = delete;

    ~SDLWorkStealingJobSystem()
    {
        Shutdown();
    }

    bool Initialize(uint32_t worker_count)
    {
        Shutdown();

        if (worker_count == 0)
        {
            return true;
        }

        workers_.clear();
        for (uint32_t i = 0; i < worker_count; ++i)
        {
            workers_.emplace_back();
        }
        threads_.resize(worker_count, nullptr);
        startInfo_.resize(worker_count);

        running_.store(true, std::memory_order_release);
        wakeGeneration_.store(0, std::memory_order_release);
        enqueueCursor_.store(0, std::memory_order_release);

        for (uint32_t i = 0; i < worker_count; ++i)
        {
            startInfo_[i].self = this;
            startInfo_[i].workerIndex = i;
            threads_[i] = SDL_CreateThread(&SDLWorkStealingJobSystem::WorkerEntry, "subset_job_worker", &startInfo_[i]);
            if (threads_[i] == nullptr)
            {
                Shutdown();
                return false;
            }
        }

        return true;
    }

    void Shutdown()
    {
        const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
        if (!was_running)
        {
            workers_.clear();
            threads_.clear();
            startInfo_.clear();
            return;
        }

        {
            std::lock_guard<std::mutex> lock(wakeMutex_);
            wakeGeneration_.fetch_add(1, std::memory_order_release);
        }
        wakeCv_.notify_all();

        for (SDL_Thread* thread : threads_)
        {
            if (thread != nullptr)
            {
                SDL_WaitThread(thread, nullptr);
            }
        }

        workers_.clear();
        threads_.clear();
        startInfo_.clear();
    }

    bool IsInitialized() const
    {
        return running_.load(std::memory_order_acquire) && !workers_.empty();
    }

    template<typename Fn>
    void ParallelFor(uint32_t item_count, uint32_t min_grain, Fn&& fn)
    {
        if (item_count == 0)
        {
            return;
        }

        const uint32_t grain = std::max(1u, min_grain);
        const uint32_t task_count = (item_count + grain - 1u) / grain;
        if (!IsInitialized() || task_count <= 1u)
        {
            fn(0, item_count);
            return;
        }

        JobGroup group;
        group.remaining.store(task_count, std::memory_order_release);

        const std::function<void(uint32_t, uint32_t)> task_fn = std::forward<Fn>(fn);
        for (uint32_t task_index = 0; task_index < task_count; ++task_index)
        {
            const uint32_t begin = task_index * grain;
            const uint32_t end = std::min(item_count, begin + grain);

            Job job;
            job.begin = begin;
            job.end = end;
            job.fn = task_fn;
            job.group = &group;
            Enqueue(std::move(job));
        }

        while (group.remaining.load(std::memory_order_acquire) != 0)
        {
            if (!RunOneJobOnCallingThread())
            {
                SDL_Delay(0);
            }
        }
    }

private:
    struct JobGroup
    {
        std::atomic<uint32_t> remaining{ 0 };
    };

    struct Job
    {
        uint32_t begin = 0;
        uint32_t end = 0;
        std::function<void(uint32_t, uint32_t)> fn;
        JobGroup* group = nullptr;
    };

    struct WorkerQueue
    {
        std::deque<Job> jobs;
        std::mutex mutex;
    };

    struct WorkerStartInfo
    {
        SDLWorkStealingJobSystem* self = nullptr;
        uint32_t workerIndex = 0;
    };

    static int SDLCALL WorkerEntry(void* userdata)
    {
        WorkerStartInfo* start_info = static_cast<WorkerStartInfo*>(userdata);
        if (start_info == nullptr || start_info->self == nullptr)
        {
            return 0;
        }
        return start_info->self->WorkerMain(start_info->workerIndex);
    }

    int WorkerMain(uint32_t worker_index)
    {
        while (running_.load(std::memory_order_acquire))
        {
            Job job;
            if (PopOwn(worker_index, &job) || Steal(worker_index, &job))
            {
                ExecuteJob(job);
                continue;
            }

            const uint32_t wake_snapshot = wakeGeneration_.load(std::memory_order_acquire);
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wakeCv_.wait_for(lock, std::chrono::milliseconds(1), [this, wake_snapshot]() {
                return !running_.load(std::memory_order_acquire) ||
                    wakeGeneration_.load(std::memory_order_acquire) != wake_snapshot;
            });
        }

        return 0;
    }

    void ExecuteJob(Job& job)
    {
        if (job.fn)
        {
            job.fn(job.begin, job.end);
        }

        if (job.group != nullptr)
        {
            job.group->remaining.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    void Enqueue(Job&& job)
    {
        if (workers_.empty())
        {
            ExecuteJob(job);
            return;
        }

        const uint32_t worker_count = static_cast<uint32_t>(workers_.size());
        const uint32_t worker_index = enqueueCursor_.fetch_add(1, std::memory_order_relaxed) % worker_count;

        {
            std::lock_guard<std::mutex> lock(workers_[worker_index].mutex);
            workers_[worker_index].jobs.emplace_back(std::move(job));
        }

        {
            std::lock_guard<std::mutex> lock(wakeMutex_);
            wakeGeneration_.fetch_add(1, std::memory_order_release);
        }
        wakeCv_.notify_one();
    }

    bool PopOwn(uint32_t worker_index, Job* out)
    {
        if (out == nullptr || worker_index >= workers_.size())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(workers_[worker_index].mutex);
        if (workers_[worker_index].jobs.empty())
        {
            return false;
        }

        *out = std::move(workers_[worker_index].jobs.back());
        workers_[worker_index].jobs.pop_back();
        return true;
    }

    bool Steal(uint32_t worker_index, Job* out)
    {
        if (out == nullptr)
        {
            return false;
        }

        const uint32_t worker_count = static_cast<uint32_t>(workers_.size());
        for (uint32_t i = 0; i < worker_count; ++i)
        {
            if (i == worker_index)
            {
                continue;
            }

            std::lock_guard<std::mutex> lock(workers_[i].mutex);
            if (workers_[i].jobs.empty())
            {
                continue;
            }

            *out = std::move(workers_[i].jobs.front());
            workers_[i].jobs.pop_front();
            return true;
        }

        return false;
    }

    bool RunOneJobOnCallingThread()
    {
        if (!running_.load(std::memory_order_acquire))
        {
            return false;
        }

        Job job;
        for (uint32_t i = 0; i < workers_.size(); ++i)
        {
            if (Steal(i, &job))
            {
                ExecuteJob(job);
                return true;
            }
        }
        return false;
    }

private:
    std::deque<WorkerQueue> workers_;
    std::vector<SDL_Thread*> threads_;
    std::vector<WorkerStartInfo> startInfo_;

    std::mutex wakeMutex_;
    std::condition_variable wakeCv_;

    std::atomic<uint32_t> wakeGeneration_{ 0 };
    std::atomic<uint32_t> enqueueCursor_{ 0 };
    std::atomic<bool> running_{ false };
};

} // namespace wicked_subset
