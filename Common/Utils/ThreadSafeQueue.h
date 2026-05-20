#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <optional>

template<typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue(size_t maxSize = 256) : m_maxSize(maxSize) {}

    void push(const T& item) {
        std::unique_lock lock(m_mutex);
        m_notFull.wait(lock, [this] { return m_queue.size() < m_maxSize || m_closed; });
        if (m_closed) return;
        m_queue.push(item);
        lock.unlock();
        m_notEmpty.notify_one();
    }

    void push(T&& item) {
        std::unique_lock lock(m_mutex);
        m_notFull.wait(lock, [this] { return m_queue.size() < m_maxSize || m_closed; });
        if (m_closed) return;
        m_queue.push(std::move(item));
        lock.unlock();
        m_notEmpty.notify_one();
    }

    // Non-blocking push — returns false if full (drops on overflow).
    bool tryPush(const T& item) {
        std::lock_guard lock(m_mutex);
        if (m_queue.size() >= m_maxSize) return false;
        m_queue.push(item);
        m_notEmpty.notify_one();
        return true;
    }

    bool tryPush(T&& item) {
        std::lock_guard lock(m_mutex);
        if (m_queue.size() >= m_maxSize) return false;
        m_queue.push(std::move(item));
        m_notEmpty.notify_one();
        return true;
    }

    // Blocking pop with timeout.
    std::optional<T> tryPop(int timeoutMs = 100) {
        std::unique_lock lock(m_mutex);
        if (!m_notEmpty.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                 [this] { return !m_queue.empty() || m_closed; })) {
            return std::nullopt;
        }
        if (m_closed && m_queue.empty()) return std::nullopt;
        T item = std::move(m_queue.front());
        m_queue.pop();
        lock.unlock();
        m_notFull.notify_one();
        return item;
    }

    // Blocking pop — waits until an item is available.
    T pop() {
        std::unique_lock lock(m_mutex);
        m_notEmpty.wait(lock, [this] { return !m_queue.empty() || m_closed; });
        if (m_closed && m_queue.empty()) throw std::runtime_error("Queue closed");
        T item = std::move(m_queue.front());
        m_queue.pop();
        lock.unlock();
        m_notFull.notify_one();
        return item;
    }

    size_t size() const {
        std::lock_guard lock(m_mutex);
        return m_queue.size();
    }

    bool empty() const {
        std::lock_guard lock(m_mutex);
        return m_queue.empty();
    }

    void close() {
        std::lock_guard lock(m_mutex);
        m_closed = true;
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    void reopen() {
        std::lock_guard lock(m_mutex);
        m_closed = false;
        // Wake any threads waiting on the queue so they can proceed.
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

private:
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    size_t m_maxSize;
    bool m_closed = false;
};
