# astcenc vendored by nxrt

Source: Arm astc-encoder 5.7.0, commit `baff485b0ff36d2f95d28961605106502c653966`.
Upstream: https://github.com/ARM-software/astc-encoder
License: Apache-2.0 (see `LICENSE.txt`).

nxrt builds the portable scalar decompressor only (`ASTCENC_DECOMPRESSOR`,
`ASTCENC_ISA_NONE`) for CPU fallback when the Vulkan device does not support
ASTC sampled images. No ASTC command-line tool or encoder is built.
