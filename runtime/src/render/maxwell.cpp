// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/maxwell.h"

#include <cstring>

namespace nxrt::render {

namespace {

enum Component : u32 { SNORM = 1, UNORM = 2, SINT = 3, UINT = 4, FLOAT = 7 };

TextureFormat F(VkFormat f, u32 bpb, u32 bw = 1, u32 bh = 1) {
    TextureFormat t;
    t.format = f;
    t.bytes_per_block = bpb;
    t.block_width = bw;
    t.block_height = bh;
    return t;
}

TextureFormat Depth(VkFormat f, u32 bpb, bool stencil) {
    TextureFormat t = F(f, bpb);
    t.depth = true;
    t.stencil = stencil;
    return t;
}

// Picks among UNORM/SNORM/UINT/SINT/FLOAT variants by the red component type.
TextureFormat ByType(u32 type, VkFormat unorm, VkFormat snorm, VkFormat uint_, VkFormat sint, VkFormat float_,
                     u32 bpb) {
    VkFormat f = VK_FORMAT_UNDEFINED;
    bool integer = false;
    switch (type) {
    case UNORM: f = unorm; break;
    case SNORM: f = snorm; break;
    case UINT: f = uint_; integer = true; break;
    case SINT: f = sint; integer = true; break;
    case FLOAT: f = float_; break;
    default: f = unorm; break;
    }
    TextureFormat t = F(f, bpb);
    t.integer = integer;
    return t;
}

} // namespace

TextureFormat TicFormat(const Tic& tic) {
    const u32 r = tic.ComponentType(0);
    const bool srgb = tic.Srgb();
    switch (tic.Format()) {
    case 0x01: return ByType(r, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_UINT,
                             VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    case 0x02: return ByType(r, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_UINT,
                             VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_SFLOAT, 12);
    case 0x03: return ByType(r, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                             VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT,
                             VK_FORMAT_R16G16B16A16_SFLOAT, 8);
    case 0x04: return ByType(r, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_UINT,
                             VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SFLOAT, 8);
    case 0x07: // X8B8G8R8
    case 0x08: // A8B8G8R8
        if (r == UNORM && srgb) return F(VK_FORMAT_R8G8B8A8_SRGB, 4);
        return ByType(r, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT,
                      VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_UNORM, 4);
    case 0x09: return ByType(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                             VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32,
                             VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4);
    case 0x0c: return ByType(r, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT,
                             VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_SFLOAT, 4);
    case 0x0f: return ByType(r, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT,
                             VK_FORMAT_R32_SFLOAT, 4);
    case 0x10: return F(VK_FORMAT_BC6H_SFLOAT_BLOCK, 16, 4, 4);
    case 0x11: return F(VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, 4, 4);
    case 0x12: return F(VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2);
    case 0x13: return F(VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2);
    case 0x14: return F(VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2);
    case 0x15: return F(VK_FORMAT_R5G6B5_UNORM_PACK16, 2);
    case 0x17: return F(srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK, 16, 4, 4);
    case 0x18: return ByType(r, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
                             VK_FORMAT_R8G8_UNORM, 2);
    case 0x1b: return ByType(r, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
                             VK_FORMAT_R16_SFLOAT, 2);
    case 0x1d: return ByType(r, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
                             VK_FORMAT_R8_UNORM, 1);
    case 0x20: return F(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4);
    case 0x21: return F(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4);
    case 0x24: return F(srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, 4);
    case 0x25: return F(srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, 4);
    case 0x26: return F(srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, 4);
    case 0x27: return F(r == SNORM ? VK_FORMAT_BC4_SNORM_BLOCK : VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, 4);
    case 0x28: return F(r == SNORM ? VK_FORMAT_BC5_SNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, 4);
    case 0x29: case 0x2b: return Depth(VK_FORMAT_D24_UNORM_S8_UINT, 4, true);   // Z24S8 / S8Z24
    case 0x2a: return Depth(VK_FORMAT_D24_UNORM_S8_UINT, 4, false);             // X8Z24
    case 0x2f: return Depth(VK_FORMAT_D32_SFLOAT, 4, false);                    // Z32
    case 0x30: return Depth(VK_FORMAT_D32_SFLOAT_S8_UINT, 8, true);             // Z32_X24S8
    case 0x3a: return Depth(VK_FORMAT_D16_UNORM, 2, false);                     // Z16
    case 0x40: return F(srgb ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 16, 4, 4);
    case 0x41: return F(srgb ? VK_FORMAT_ASTC_5x5_SRGB_BLOCK : VK_FORMAT_ASTC_5x5_UNORM_BLOCK, 16, 5, 5);
    case 0x42: return F(srgb ? VK_FORMAT_ASTC_6x6_SRGB_BLOCK : VK_FORMAT_ASTC_6x6_UNORM_BLOCK, 16, 6, 6);
    case 0x44: return F(srgb ? VK_FORMAT_ASTC_8x8_SRGB_BLOCK : VK_FORMAT_ASTC_8x8_UNORM_BLOCK, 16, 8, 8);
    case 0x45: return F(srgb ? VK_FORMAT_ASTC_10x10_SRGB_BLOCK : VK_FORMAT_ASTC_10x10_UNORM_BLOCK, 16, 10, 10);
    case 0x46: return F(srgb ? VK_FORMAT_ASTC_12x12_SRGB_BLOCK : VK_FORMAT_ASTC_12x12_UNORM_BLOCK, 16, 12, 12);
    case 0x50: return F(srgb ? VK_FORMAT_ASTC_5x4_SRGB_BLOCK : VK_FORMAT_ASTC_5x4_UNORM_BLOCK, 16, 5, 4);
    case 0x51: return F(srgb ? VK_FORMAT_ASTC_6x5_SRGB_BLOCK : VK_FORMAT_ASTC_6x5_UNORM_BLOCK, 16, 6, 5);
    case 0x52: return F(srgb ? VK_FORMAT_ASTC_8x6_SRGB_BLOCK : VK_FORMAT_ASTC_8x6_UNORM_BLOCK, 16, 8, 6);
    case 0x53: return F(srgb ? VK_FORMAT_ASTC_10x8_SRGB_BLOCK : VK_FORMAT_ASTC_10x8_UNORM_BLOCK, 16, 10, 8);
    case 0x54: return F(srgb ? VK_FORMAT_ASTC_12x10_SRGB_BLOCK : VK_FORMAT_ASTC_12x10_UNORM_BLOCK, 16, 12, 10);
    case 0x55: return F(srgb ? VK_FORMAT_ASTC_8x5_SRGB_BLOCK : VK_FORMAT_ASTC_8x5_UNORM_BLOCK, 16, 8, 5);
    case 0x56: return F(srgb ? VK_FORMAT_ASTC_10x5_SRGB_BLOCK : VK_FORMAT_ASTC_10x5_UNORM_BLOCK, 16, 10, 5);
    case 0x57: return F(srgb ? VK_FORMAT_ASTC_10x6_SRGB_BLOCK : VK_FORMAT_ASTC_10x6_UNORM_BLOCK, 16, 10, 6);
    default: return {};
    }
}

VkComponentSwizzle Swizzle(u32 source) {
    switch (source) {
    case 0: return VK_COMPONENT_SWIZZLE_ZERO;
    case 2: return VK_COMPONENT_SWIZZLE_R;
    case 3: return VK_COMPONENT_SWIZZLE_G;
    case 4: return VK_COMPONENT_SWIZZLE_B;
    case 5: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_ONE;
    }
}

u32 VertexAttributeComponentCount(u32 size) {
    switch (size) {
    case 0x01: case 0x03: case 0x0A: case 0x30: case 0x33: return 4;
    case 0x02: case 0x05: case 0x13: case 0x31: return 3;
    case 0x04: case 0x0F: case 0x18: case 0x32: return 2;
    default: return 1;
    }
}

VkFormat VertexFormat(u32 size, u32 type) {
    enum { SNorm = 1, UNorm = 2, SInt = 3, UInt = 4, UScaled = 5, SScaled = 6, Float = 7 };
    auto pick = [type](VkFormat unorm, VkFormat snorm, VkFormat uint_, VkFormat sint, VkFormat uscaled,
                       VkFormat sscaled, VkFormat float_) {
        switch (type) {
        case UNorm: return unorm;
        case SNorm: return snorm;
        case UInt: return uint_;
        case SInt: return sint;
        case UScaled: return uscaled;
        case SScaled: return sscaled;
        default: return float_;
        }
    };
    switch (size) {
    case 0x01: return pick(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_UINT,
                           VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
                           VK_FORMAT_R32G32B32A32_SFLOAT);
    case 0x02: return pick(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_UINT,
                           VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT,
                           VK_FORMAT_R32G32B32_SFLOAT);
    case 0x03: return pick(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
                           VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_USCALED,
                           VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_SFLOAT);
    case 0x04: return pick(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
                           VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SFLOAT);
    case 0x05: return pick(VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT,
                           VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED,
                           VK_FORMAT_R16G16B16_SFLOAT);
    case 0x0A: case 0x33:
        return pick(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT,
                    VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                    VK_FORMAT_R8G8B8A8_UNORM);
    case 0x0F: return pick(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT,
                           VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED,
                           VK_FORMAT_R16G16_SFLOAT);
    case 0x12: return pick(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT,
                           VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32_SFLOAT);
    case 0x13: return pick(VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT,
                           VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED,
                           VK_FORMAT_R8G8B8_UNORM);
    case 0x18: case 0x32:
        return pick(VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
                    VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8_UNORM);
    case 0x1B: return pick(VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
                           VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16_SFLOAT);
    case 0x1D: case 0x34:
        return pick(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
                    VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, VK_FORMAT_R8_UNORM);
    case 0x30: return pick(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                           VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32,
                           VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                           VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    case 0x31: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkBlendFactor BlendFactor(u32 v) {
    switch (v & 0x1F) {
    case 0x01: return VK_BLEND_FACTOR_ZERO;
    case 0x02: return VK_BLEND_FACTOR_ONE;
    case 0x03: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x04: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x05: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x06: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x07: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x08: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x09: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0A: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0B: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x0E: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x0F: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(u32 e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp CompareOp(u32 f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkCompareOp SamplerCompareOp(u32 f) {
    return CompareOp(f + 1);
}

VkStencilOp StencilOp(u32 op) {
    switch (op) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

VkPrimitiveTopology Topology(u32 mode) {
    switch (mode) {
    case 0x0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 0x1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 0x3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 0x5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 0x6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 0xA: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 0xB: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 0xC: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 0xD: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case 0xE: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkSamplerAddressMode WrapMode(u32 wrap) {
    switch (wrap) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: case 7: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

VkFilter Filter(u32 f) {
    return f == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkSamplerMipmapMode MipmapMode(u32 f) {
    return f == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

// ---------------------------------------------------------------- block linear
// A GOB is 64 bytes x 8 rows (512 bytes); a block stacks 2^block_height GOBs
// vertically; blocks run left to right, then down.

size_t BlockLinearSize(u32 bpp, u32 wb, u32 hb, u32 bh_log2, u32 tws) {
    const u32 gob_h = 1u << bh_log2;
    u32 gobs_x = (wb * bpp + 63) / 64;
    const u32 align = 1u << tws;
    gobs_x = (gobs_x + align - 1) / align * align;
    const u32 rows_of_blocks = (hb + 8 * gob_h - 1) / (8 * gob_h);
    return static_cast<size_t>(rows_of_blocks) * gobs_x * gob_h * 512;
}

void UnswizzleBlockLinear(const u8* src, size_t src_size, u8* dst, u32 bpp, u32 wb, u32 hb, u32 bh_log2,
                          u32 tws) {
    const u32 gob_h = 1u << bh_log2;
    u32 gobs_x = (wb * bpp + 63) / 64;
    const u32 align = 1u << tws;
    gobs_x = (gobs_x + align - 1) / align * align;
    const size_t block_size = static_cast<size_t>(gob_h) * 512;
    const size_t row_of_blocks = block_size * gobs_x;
    const u32 row_bytes = wb * bpp;
    for (u32 y = 0; y < hb; ++y) {
        const size_t base_y = (y / (8 * gob_h)) * row_of_blocks + ((y / 8) % gob_h) * 512;
        const u32 yy = y % 8;
        const u32 y_part = (yy / 2) * 64 + (yy % 2) * 16;
        u8* out = dst + static_cast<size_t>(y) * row_bytes;
        for (u32 x = 0; x < row_bytes; x += 16) {
            const size_t offset = base_y + (x / 64) * block_size + ((x % 64) / 32) * 256 + y_part +
                                  ((x % 32) / 16) * 32;
            const u32 n = std::min<u32>(16, row_bytes - x);
            if (offset + n <= src_size) std::memcpy(out + x, src + offset, n);
        }
    }
}

// 3D surfaces: a block is 2^block_height GOBs tall and 2^block_depth slices deep (z-major inside the
// block); rows of blocks follow each other within a slice group, then the next group of slices.
size_t BlockLinearSize3D(u32 bpp, u32 wb, u32 hb, u32 depth, u32 bh_log2, u32 bd_log2, u32 tws) {
    const u32 gob_d = 1u << bd_log2;
    const size_t slice_groups = (depth + gob_d - 1) / gob_d;
    return slice_groups * gob_d * BlockLinearSize(bpp, wb, hb, bh_log2, tws);
}

void UnswizzleBlockLinear3D(const u8* src, size_t src_size, u8* dst, u32 bpp, u32 wb, u32 hb, u32 depth,
                            u32 bh_log2, u32 bd_log2, u32 tws) {
    const u32 gob_h = 1u << bh_log2;
    const u32 gob_d = 1u << bd_log2;
    u32 gobs_x = (wb * bpp + 63) / 64;
    const u32 align = 1u << tws;
    gobs_x = (gobs_x + align - 1) / align * align;
    const size_t block_size = static_cast<size_t>(gob_h) * gob_d * 512;
    const size_t row_of_blocks = block_size * gobs_x;
    const size_t rows_of_blocks = (hb + 8 * gob_h - 1) / (8 * gob_h);
    const size_t slice_group = row_of_blocks * rows_of_blocks;
    const u32 row_bytes = wb * bpp;
    for (u32 z = 0; z < depth; ++z) {
        const size_t base_z = (z / gob_d) * slice_group + static_cast<size_t>(z % gob_d) * gob_h * 512;
        for (u32 y = 0; y < hb; ++y) {
            const size_t base_y = base_z + (y / (8 * gob_h)) * row_of_blocks + ((y / 8) % gob_h) * 512;
            const u32 yy = y % 8;
            const u32 y_part = (yy / 2) * 64 + (yy % 2) * 16;
            u8* out = dst + (static_cast<size_t>(z) * hb + y) * row_bytes;
            for (u32 x = 0; x < row_bytes; x += 16) {
                const size_t offset = base_y + (x / 64) * block_size + ((x % 64) / 32) * 256 + y_part +
                                      ((x % 32) / 16) * 32;
                const u32 n = std::min<u32>(16, row_bytes - x);
                if (offset + n <= src_size) std::memcpy(out + x, src + offset, n);
            }
        }
    }
}

} // namespace nxrt::render
