// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <vulkan/vulkan.h>

namespace nxrt::render {

// Tracks commands emitted into one recording of a graphics command buffer.
// Reset at every new recording and at any untracked graphics state mutation.
class GraphicsStateCache {
public:
    void Reset() { pipeline_.reset(); ResetDynamic(); }

    bool Pipeline(VkPipeline value) {
        if (pipeline_ && *pipeline_ == value) return false;
        pipeline_ = value;
        ResetDynamic();
        return true;
    }

    bool Viewport(const VkViewport& value) {
        const std::array<std::uint32_t, 6> bits{Bits(value.x), Bits(value.y), Bits(value.width), Bits(value.height),
                                                Bits(value.minDepth), Bits(value.maxDepth)};
        if (viewport_ && *viewport_ == bits) return false;
        viewport_ = bits;
        return true;
    }

    bool Scissor(const VkRect2D& value) {
        if (scissor_ && scissor_->offset.x == value.offset.x && scissor_->offset.y == value.offset.y &&
            scissor_->extent.width == value.extent.width && scissor_->extent.height == value.extent.height)
            return false;
        scissor_ = value;
        return true;
    }

    bool Blend(const std::array<float, 4>& value) {
        const std::array<std::uint32_t, 4> bits{Bits(value[0]), Bits(value[1]), Bits(value[2]), Bits(value[3])};
        if (blend_ && *blend_ == bits) return false;
        blend_ = bits;
        return true;
    }

private:
    static std::uint32_t Bits(float value) { return std::bit_cast<std::uint32_t>(value); }
    void ResetDynamic() { viewport_.reset(); scissor_.reset(); blend_.reset(); }

    std::optional<VkPipeline> pipeline_;
    std::optional<std::array<std::uint32_t, 6>> viewport_;
    std::optional<VkRect2D> scissor_;
    std::optional<std::array<std::uint32_t, 4>> blend_;
};

} // namespace nxrt::render
