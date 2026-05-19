#pragma once
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <atomic>

// Simple reusable thread pool for CPU-bound parallel work.
// Replaces std::async per-frame thread creation, which on MSVC spawns
// and destroys threads on every call — at 60fps × 8 threads, that's
// 480 create/destroy pairs per second that bog down the OS scheduler.
class WorkerThreadPool {
public:
    explicit WorkerThreadPool(unsigned int numThreads = 0) {
        if (numThreads == 0) {
            numThreads = std::thread::hardware_concurrency();
            if (numThreads < 2) numThreads = 2;
            if (numThreads > 8) numThreads = 8;
        }
        m_workers.reserve(numThreads);
        for (unsigned int i = 0; i < numThreads; ++i) {
            m_workers.emplace_back(&WorkerThreadPool::WorkerLoop, this);
        }
    }

    ~WorkerThreadPool() {
        {
            std::lock_guard lock(m_mutex);
            m_shutdown = true;
        }
        m_cv.notify_all();
        for (auto& t : m_workers) {
            if (t.joinable()) t.join();
        }
    }

    // Number of worker threads
    size_t WorkerCount() const { return m_workers.size(); }

    // Submit a task. Thread-safe, may be called from any thread.
    void Enqueue(std::function<void()> task) {
        {
            std::lock_guard lock(m_mutex);
            m_tasks.push(std::move(task));
            ++m_pending;
        }
        m_cv.notify_one();
    }

    // Block until all submitted tasks have completed.
    void WaitAll() {
        std::unique_lock lock(m_mutex);
        m_allDone.wait(lock, [this] {
            return m_pending == 0 && m_tasks.empty();
        });
    }

    // Convenience: run N tasks in parallel and wait for completion.
    void ParallelFor(unsigned int n, std::function<void(unsigned int start, unsigned int end)> body) {
        unsigned int nt = static_cast<unsigned int>(m_workers.size());
        if (nt < 2 || n < nt * 2) {
            body(0, n);
            return;
        }
        unsigned int chunk = n / nt;
        for (unsigned int t = 0; t < nt; ++t) {
            unsigned int start = t * chunk;
            unsigned int end = (t + 1 == nt) ? n : start + chunk;
            Enqueue([start, end, &body] { body(start, end); });
        }
        WaitAll();
    }

private:
    void WorkerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] {
                    return m_shutdown || !m_tasks.empty();
                });
                if (m_shutdown && m_tasks.empty()) return;
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            task();
            {
                std::lock_guard lock(m_mutex);
                --m_pending;
            }
            m_allDone.notify_one();
        }
    }

    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_allDone;
    std::atomic<size_t> m_pending{0};
    bool m_shutdown = false;
};
