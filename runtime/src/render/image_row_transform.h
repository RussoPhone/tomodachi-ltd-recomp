#pragma once

#include <cstddef>
#include <cstring>
#include <vector>

namespace nxrt::render {

inline bool MirrorImageRows(void* pixels, std::size_t width, std::size_t height, std::size_t pixel_size) {
    if (!pixels || !width || !height || !pixel_size || width > static_cast<std::size_t>(-1) / pixel_size) {
        return false;
    }
    const std::size_t row_size = width * pixel_size;
    auto* bytes = static_cast<unsigned char*>(pixels);
    std::vector<unsigned char> row(row_size);
    for (std::size_t y = 0; y < height / 2; ++y) {
        unsigned char* top = bytes + y * row_size;
        unsigned char* bottom = bytes + (height - 1 - y) * row_size;
        std::memcpy(row.data(), top, row_size);
        std::memcpy(top, bottom, row_size);
        std::memcpy(bottom, row.data(), row_size);
    }
    return true;
}

} // namespace nxrt::render
