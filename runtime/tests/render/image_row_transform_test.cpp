#include "render/image_row_transform.h"

#include <array>
#include <algorithm>

int main() {
    std::array<unsigned char, 24> pixels{
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16,
        17, 18, 19, 20, 21, 22, 23, 24,
    };

    nxrt::render::MirrorImageRows(pixels.data(), 2, 3, 4);

    constexpr std::array<unsigned char, 24> expected{
        17, 18, 19, 20, 21, 22, 23, 24,
        9, 10, 11, 12, 13, 14, 15, 16,
        1, 2, 3, 4, 5, 6, 7, 8,
    };
    return std::equal(pixels.begin(), pixels.end(), expected.begin()) ? 0 : 1;
}
