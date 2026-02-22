#pragma once

#include <vector>
#include <cstdint>
#include <stdexcept>

namespace OrderMatcher {

template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t size) : pool(size), freeList(size) {
        for (size_t i = 0; i < size; ++i) {
            freeList[i] = &pool[i];
        }
        nextFree = size;
    }

    T* allocate() {
        if (nextFree == 0) {
            throw std::runtime_error("ObjectPool exhausted");
        }
        return freeList[--nextFree];
    }

    void deallocate(T* obj) {
        if (nextFree >= freeList.size()) {
            // This should not happen if usage is correct
            return; 
        }
        freeList[nextFree++] = obj;
    }

    // Reset pool for testing/benchmarking
    void reset() {
        nextFree = pool.size();
        for (size_t i = 0; i < pool.size(); ++i) {
            freeList[i] = &pool[i];
        }
    }

private:
    std::vector<T> pool;
    std::vector<T*> freeList;
    size_t nextFree;
};

} // namespace OrderMatcher
