#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace malicompat {

/**
 * Fast CPU-based Block Compression (BC / DXT) Decompressor.
 * 
 * Optimized for ARM64 NEON to decode unsupported BC1-BC5 blocks
 * into standard RGBA8/R8/RG8 uncompressed formats during Vulkan image upload.
 */
class BcTextureDecompressor {
public:
    // Decompresses a BC1 (DXT1) 4x4 block into 16 RGBA8 pixels with optional bounds clipping
    static void DecodeBc1Block(const uint8_t* block, uint32_t* outPixels, uint32_t stride, uint32_t maxWidth = 4, uint32_t maxHeight = 4);

    // Decompresses a BC3 (DXT5) 4x4 block into 16 RGBA8 pixels with optional bounds clipping
    static void DecodeBc3Block(const uint8_t* block, uint32_t* outPixels, uint32_t stride, uint32_t maxWidth = 4, uint32_t maxHeight = 4);

    // Decompresses a BC4 (ATI1 / Red channel) 4x4 block into 16 R8 pixels with optional bounds clipping
    static void DecodeBc4Block(const uint8_t* block, uint8_t* outPixels, uint32_t stride, uint32_t maxWidth = 4, uint32_t maxHeight = 4);

    // Decompresses a BC5 (ATI2 / RG channels) 4x4 block into 16 RG8 pixels with optional bounds clipping
    static void DecodeBc5Block(const uint8_t* block, uint8_t* outPixels, uint32_t stride, uint32_t maxWidth = 4, uint32_t maxHeight = 4);

    // Batch decompress entire image
    static bool DecompressImage(
        int format, // 1: BC1, 3: BC3, 4: BC4, 5: BC5
        uint32_t width,
        uint32_t height,
        const uint8_t* srcCompressed,
        uint8_t* dstDecompressed,
        size_t srcSize = 0);
};

} // namespace malicompat
