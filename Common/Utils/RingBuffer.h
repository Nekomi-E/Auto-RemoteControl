#pragma once
#include <atomic>
#include <array>
#include <optional>

// Lock-free Single-Producer Single-Consumer (SPSC) ring buffer.
// Used for the hot capture->encode and decode->render paths.
template<typename T, size_t Capacity>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t Mask = Capacity - 1;

public:
    RingBuffer() : m_head(0), m_tail(0) {
        for (size_t i = 0; i < Capacity; ++i) {
            m_ready[i].store(false, std::memory_order_relaxed);
        }
    }

    // Producer: push one item. Returns false if buffer is full.
    bool push(const T& item) {
        size_t head = m_head.load(std::memory_order_relaxed);
        if (m_ready[head & Mask].load(std::memory_order_acquire)) {
            return false; // full
        }
        m_data[head & Mask] = item;
        m_ready[head & Mask].store(true, std::memory_order_release);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    bool push(T&& item) {
        size_t head = m_head.load(std::memory_order_relaxed);
        if (m_ready[head & Mask].load(std::memory_order_acquire)) {
            return false;
        }
        m_data[head & Mask] = std::move(item);
        m_ready[head & Mask].store(true, std::memory_order_release);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer: pop one item. Returns std::nullopt if buffer is empty.
    std::optional<T> pop() {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        if (!m_ready[tail & Mask].load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        T item = std::move(m_data[tail & Mask]);
        m_ready[tail & Mask].store(false, std::memory_order_release);
        m_tail.store(tail + 1, std::memory_order_release);
        return item;
    }

    bool empty() const {
        size_t tail = m_tail.load(std::memory_order_acquire);
        return !m_ready[tail & Mask].load(std::memory_order_acquire);
    }

    bool full() const {
        size_t head = m_head.load(std::memory_order_acquire);
        return m_ready[head & Mask].load(std::memory_order_acquire);
    }

    size_t size() const {
        // Return the number of items in the buffer. m_head and m_tail are
        // monotonic counters (may wrap) so simple unsigned subtraction yields
        // the correct difference modulo the counter width.
        size_t head = m_head.load(std::memory_order_acquire);
        size_t tail = m_tail.load(std::memory_order_acquire);
        return head - tail;
    }

private:
    std::array<T, Capacity> m_data;
    std::array<std::atomic<bool>, Capacity> m_ready;
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
};
