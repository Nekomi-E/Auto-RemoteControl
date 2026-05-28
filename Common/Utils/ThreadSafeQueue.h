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

    void push(const T& item) {//T& 传入常量引用，确保item指向的对象不会被修改（左值引用）
        std::unique_lock lock(m_mutex);
        m_notFull.wait(lock, [this] { return m_queue.size() < m_maxSize || m_closed; });//释放锁并阻塞当前线程，直到满足lambda表达式返回true
        if (m_closed) return;
        m_queue.push(item);//由于这里item是一个常量引用，实际上是将调用者传入的对象复制到队列中
        lock.unlock();
        m_notEmpty.notify_one();
    }

    void push(T&& item) {//T&&传入右值引用，允许移动语义
        std::unique_lock lock(m_mutex);
        m_notFull.wait(lock, [this] { return m_queue.size() < m_maxSize || m_closed; });
        if (m_closed) return;
        m_queue.push(std::move(item));//item本来就是右值引用，std::move将其转换为右值，触发移动语义
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
        std::unique_lock lock(m_mutex);//wait_for要求调用前线程持有锁
        if (!m_notEmpty.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return !m_queue.empty() || m_closed; })) {//wait_for先释放锁并阻塞当前线程，线程被唤醒后重新获取锁并检查lambda表达式
            return std::nullopt;
        }
        if (m_closed && m_queue.empty()) return std::nullopt;
        T item = std::move(m_queue.front());//std::move触发移动语义，之后m_queue.front()的状态未定义，可能被视为空对象，不能再被访问
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
    mutable std::mutex m_mutex;//mutable允许在const成员函数中修改m_mutex，保证size()和empty()等查询函数也能正确同步访问队列状态
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    size_t m_maxSize;
    bool m_closed = false;
};
