#include "render/descriptor_info_storage.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdio>
#include <vector>

using nxrt::render::DescriptorInfoStorage;

template <typename Storage>
int VerifyReservedPointersRemainStable() {
    if constexpr (!requires(Storage& storage, std::size_t capacity) { storage.Reserve(capacity); }) {
        std::fprintf(stderr, "descriptor info storage has no preallocation API\n");
        return 1;
    } else {
        Storage image_infos;
        image_infos.Reserve(510);
        std::vector<const VkDescriptorImageInfo*> descriptor_image_pointers;

        for (unsigned i = 0; i < 510; ++i) {
            descriptor_image_pointers.push_back(image_infos.Push({}));
        }

        if (image_infos.size() != descriptor_image_pointers.size()) {
            std::fprintf(stderr, "descriptor info count mismatch\n");
            return 2;
        }
        for (std::size_t i = 0; i < image_infos.size(); ++i) {
            if (descriptor_image_pointers[i] != &image_infos[i]) {
                std::fprintf(stderr, "descriptor info pointer %zu became stale\n", i);
                return 3;
            }
        }
        return 0;
    }
}

int main() {
    return VerifyReservedPointersRemainStable<DescriptorInfoStorage<VkDescriptorImageInfo>>();
}
