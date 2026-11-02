// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware facts of the Switch's GPU (Maxwell, GM20B) that NVN state and
// descriptors are expressed in, and their Vulkan equivalents. Layouts follow the
// public nouveau/envytools documentation.
#pragma once

#include <vulkan/vulkan.h>

#include <array>

#include "../common.h"

namespace nxrt::render {

// Texture image control entry (TIC): the 32-byte texture descriptor.
struct Tic {
    u32 w[8];
    u32 Format() const { return w[0] & 0x7F; }
    u32 ComponentType(int c) const { return (w[0] >> (7 + 3 * c)) & 7; } // r, g, b, a
    u32 Swizzle(int c) const { return (w[0] >> (19 + 3 * c)) & 7; }     // x, y, z, w sources
    u64 Address() const { return w[1] | (static_cast<u64>(w[2] & 0xFFFF) << 32); }
    u32 HeaderVersion() const { return (w[2] >> 21) & 7; }
    bool IsBlockLinear() const { return HeaderVersion() == 3 || HeaderVersion() == 4; }
    bool IsPitch() const { return HeaderVersion() == 1 || HeaderVersion() == 2; }
    u32 BlockHeight() const { return (w[3] >> 3) & 7; }
    u32 BlockDepth() const { return (w[3] >> 6) & 7; }
    u32 TileWidthSpacing() const { return (w[3] >> 10) & 7; }
    u32 Pitch() const { return (w[3] & 0xFFFF) << 5; }
    u32 MaxMipLevel() const { return (w[3] >> 28) & 0xF; }
    u32 Width() const { return (w[4] & 0xFFFF) + 1; }
    bool Srgb() const { return (w[4] >> 22) & 1; }
    u32 Type() const { return (w[4] >> 23) & 0xF; }
    u32 Height() const { return (w[5] & 0xFFFF) + 1; }
    u32 Depth() const { return ((w[5] >> 16) & 0x3FFF) + 1; }
    bool NormalizedCoords() const { return (w[5] >> 31) & 1; }
};
static_assert(sizeof(Tic) == 32);

enum TicType : u32 {
    kTex1D = 0, kTex2D = 1, kTex3D = 2, kTexCube = 3, kTex1DArray = 4, kTex2DArray = 5,
    kTex1DBuffer = 6, kTex2DNoMip = 7, kTexCubeArray = 8,
};

// Texture sampler control entry (TSC).
struct Tsc {
    u32 w[8];
    u32 WrapU() const { return w[0] & 7; }
    u32 WrapV() const { return (w[0] >> 3) & 7; }
    u32 WrapP() const { return (w[0] >> 6) & 7; }
    bool DepthCompare() const { return (w[0] >> 9) & 1; }
    u32 DepthFunc() const { return (w[0] >> 10) & 7; }
    u32 MaxAnisotropyLog2() const { return (w[0] >> 20) & 7; }
    u32 MagFilter() const { return w[1] & 3; }
    u32 MinFilter() const { return (w[1] >> 4) & 3; }
    u32 MipFilter() const { return (w[1] >> 6) & 3; }
    float LodBias() const {
        const u32 raw = (w[1] >> 12) & 0x1FFF;
        return static_cast<float>(static_cast<s32>(raw << 19) >> 19) / 256.0f;
    }
    float MinLod() const { return static_cast<float>(w[2] & 0xFFF) / 256.0f; }
    float MaxLod() const { return static_cast<float>((w[2] >> 12) & 0xFFF) / 256.0f; }
};
static_assert(sizeof(Tsc) == 32);

struct TextureFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 bytes_per_block = 4;
    u32 block_width = 1, block_height = 1;
    bool depth = false, stencil = false, integer = false;
};

// Format from a TIC's format and component types; VK_FORMAT_UNDEFINED if unknown.
TextureFormat TicFormat(const Tic& tic);
VkComponentSwizzle Swizzle(u32 source);

// Vertex attribute size code and type -> Vulkan format.
VkFormat VertexFormat(u32 size, u32 type);
u32 VertexAttributeComponentCount(u32 size);

VkBlendFactor BlendFactor(u32 nvn_factor);
VkBlendOp BlendOp(u32 equation);
VkCompareOp CompareOp(u32 func);         // 1-based D3D style (NVN)
VkCompareOp SamplerCompareOp(u32 func);  // 0-based (TSC)
VkStencilOp StencilOp(u32 op);
VkPrimitiveTopology Topology(u32 mode);
VkSamplerAddressMode WrapMode(u32 wrap);
VkFilter Filter(u32 filter);
VkSamplerMipmapMode MipmapMode(u32 filter);

// Converts a block-linear (GOB-tiled) 2D surface to linear rows.
void UnswizzleBlockLinear(const u8* src, size_t src_size, u8* dst, u32 bytes_per_pixel, u32 width_blocks,
                          u32 height_blocks, u32 block_height_log2, u32 tile_width_spacing);
// Bytes a block-linear 2D level occupies.
size_t BlockLinearSize(u32 bytes_per_pixel, u32 width_blocks, u32 height_blocks, u32 block_height_log2,
                       u32 tile_width_spacing);

// 3D variants: `depth` slices, blocks 2^block_depth_log2 slices deep. Output slices are consecutive.
void UnswizzleBlockLinear3D(const u8* src, size_t src_size, u8* dst, u32 bytes_per_pixel, u32 width_blocks,
                            u32 height_blocks, u32 depth, u32 block_height_log2, u32 block_depth_log2,
                            u32 tile_width_spacing);
size_t BlockLinearSize3D(u32 bytes_per_pixel, u32 width_blocks, u32 height_blocks, u32 depth,
                         u32 block_height_log2, u32 block_depth_log2, u32 tile_width_spacing);

} // namespace nxrt::render
