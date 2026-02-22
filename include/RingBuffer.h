#pragma once

#include <atomic>
#include <vector>
#include <cassert>

namespace OrderMatcher {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t size) : buffer(size), head(0), tail(0) {
        // Size must be power of 2 for bitwise masking optimization
        // For simplicity here, we enforce size, but production code would round up
        if ((size & (size - 1)) != 0) {
            // Ideally throw or adjust size
             assert((size & (size - 1)) == 0); 
        }
        mask = size - 1;
    }

    bool push(const T& item) {
        const auto current_tail = tail.load(std::memory_order_relaxed);
        const auto next_tail = (current_tail + 1) & mask;
        
        if (next_tail != head.load(std::memory_order_acquire)) {
            buffer[current_tail] = item;
            tail.store(next_tail, std::memory_order_release);
            return true;
        }
        return false; // Buffer full
    }

    bool pop(T& item) {
        const auto current_head = head.load(std::memory_order_relaxed);
        
        if (current_head == tail.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }

        item = buffer[current_head];
        head.store((current_head + 1) & mask, std::memory_order_release);
        return true;
    }

private:
    std::vector<T> buffer;
    std::atomic<size_t> head;
    std::atomic<size_t> tail;
    size_t mask;
};

} // namespace OrderMatcher
