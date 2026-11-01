#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace nxrt::render {

// Vulkan descriptor writes hold pointers to these values until vkUpdateDescriptorSets.
// std::deque keeps element addresses stable while later descriptor infos are appended.
template <typename T>
class DescriptorInfoStorage {
public:
    void Reserve(std::size_t capacity) { values_.reserve(capacity); }
    void Clear() { values_.clear(); }

    const T* Push(T value) {
        values_.push_back(std::move(value));
        return &values_.back();
    }

    std::size_t size() const { return values_.size(); }
    const T& operator[](std::size_t index) const { return values_[index]; }

private:
    std::vector<T> values_;
};

} // namespace nxrt::render
