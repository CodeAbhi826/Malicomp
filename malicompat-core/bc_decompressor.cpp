#include "bc_decompressor.h"
#include <cstring>
#include <algorithm>

namespace malicompat {

namespace {

inline uint32_t Rgb565ToRgba8(uint16_t c, uint8_t a = 255) {
    uint8_t r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
    uint8_t g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
    uint8_t b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8)  |
           static_cast<uint32_t>(r);
}

} // namespace

void BcTextureDecompressor::DecodeBc1Block(const uint8_t* block, uint32_t* outPixels, uint32_t stride) {
    uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
    uint32_t code = static_cast<uint32_t>(block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24));

    uint32_t palette[4];
    palette[0] = Rgb565ToRgba8(c0, 255);
    palette[1] = Rgb565ToRgba8(c1, 255);

    uint8_t r0 = static_cast<uint8_t>(palette[0] & 0xFF);
    uint8_t g0 = static_cast<uint8_t>((palette[0] >> 8) & 0xFF);
    uint8_t b0 = static_cast<uint8_t>((palette[0] >> 16) & 0xFF);

    uint8_t r1 = static_cast<uint8_t>(palette[1] & 0xFF);
    uint8_t g1 = static_cast<uint8_t>((palette[1] >> 8) & 0xFF);
    uint8_t b1 = static_cast<uint8_t>((palette[1] >> 16) & 0xFF);

    if (c0 > c1) {
        palette[2] = (255u << 24) | (((2 * b0 + b1) / 3) << 16) | (((2 * g0 + g1) / 3) << 8) | ((2 * r0 + r1) / 3);
        palette[3] = (255u << 24) | (((b0 + 2 * b1) / 3) << 16) | (((g0 + 2 * g1) / 3) << 8) | ((r0 + 2 * r1) / 3);
    } else {
        palette[2] = (255u << 24) | (((b0 + b1) / 2) << 16) | (((g0 + g1) / 2) << 8) | ((r0 + r1) / 2);
        palette[3] = 0; // Transparent black
    }

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t idx = static_cast<uint8_t>(code & 0x03);
            outPixels[y * stride + x] = palette[idx];
            code >>= 2;
        }
    }
}

void BcTextureDecompressor::DecodeBc3Block(const uint8_t* block, uint32_t* outPixels, uint32_t stride) {
    uint8_t a0 = block[0];
    uint8_t a1 = block[1];
    uint64_t aCode = 0;
    for (int i = 0; i < 6; ++i) {
        aCode |= (static_cast<uint64_t>(block[2 + i]) << (i * 8));
    }

    uint8_t alphaPalette[8];
    alphaPalette[0] = a0;
    alphaPalette[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) {
            alphaPalette[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
        }
    } else {
        for (int i = 1; i <= 4; ++i) {
            alphaPalette[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
        }
        alphaPalette[6] = 0;
        alphaPalette[7] = 255;
    }

    DecodeBc1Block(block + 8, outPixels, stride);

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t aIdx = static_cast<uint8_t>(aCode & 0x07);
            uint8_t alpha = alphaPalette[aIdx];
            aCode >>= 3;
            outPixels[y * stride + x] = (outPixels[y * stride + x] & 0x00FFFFFF) | (static_cast<uint32_t>(alpha) << 24);
        }
    }
}

void BcTextureDecompressor::DecodeBc4Block(const uint8_t* block, uint8_t* outPixels, uint32_t stride) {
    uint8_t r0 = block[0];
    uint8_t r1 = block[1];
    uint64_t code = 0;
    for (int i = 0; i < 6; ++i) {
        code |= (static_cast<uint64_t>(block[2 + i]) << (i * 8));
    }

    uint8_t palette[8];
    palette[0] = r0;
    palette[1] = r1;
    if (r0 > r1) {
        for (int i = 1; i <= 6; ++i) {
            palette[i + 1] = static_cast<uint8_t>(((7 - i) * r0 + i * r1) / 7);
        }
    } else {
        for (int i = 1; i <= 4; ++i) {
            palette[i + 1] = static_cast<uint8_t>(((5 - i) * r0 + i * r1) / 5);
        }
        palette[6] = 0;
        palette[7] = 255;
    }

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t idx = static_cast<uint8_t>(code & 0x07);
            outPixels[y * stride + x] = palette[idx];
            code >>= 3;
        }
    }
}

void BcTextureDecompressor::DecodeBc5Block(const uint8_t* block, uint8_t* outPixels, uint32_t stride) {
    // BC5 consists of two consecutive BC4 blocks: red then green
    uint8_t r[16], g[16];
    DecodeBc4Block(block, r, 4);
    DecodeBc4Block(block + 8, g, 4);

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            outPixels[(y * stride + x) * 2 + 0] = r[y * 4 + x];
            outPixels[(y * stride + x) * 2 + 1] = g[y * 4 + x];
        }
    }
}

bool BcTextureDecompressor::DecompressImage(
    int format,
    uint32_t width,
    uint32_t height,
    const uint8_t* src,
    uint8_t* dst) {

    if (!src || !dst || !width || !height) return false;

    uint32_t blocksX = (width + 3) / 4;
    uint32_t blocksY = (height + 3) / 4;

    uint32_t blockSize = (format == 1 || format == 4) ? 8 : 16;
    size_t srcOffset = 0;

    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            const uint8_t* block = src + srcOffset;
            srcOffset += blockSize;

            if (format == 1) {
                uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst) + (by * 4 * width + bx * 4);
                DecodeBc1Block(block, dst32, width);
            } else if (format == 3) {
                uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst) + (by * 4 * width + bx * 4);
                DecodeBc3Block(block, dst32, width);
            } else if (format == 4) {
                uint8_t* dst8 = dst + (by * 4 * width + bx * 4);
                DecodeBc4Block(block, dst8, width);
            } else if (format == 5) {
                uint8_t* dst16 = dst + (by * 4 * width + bx * 4) * 2;
                DecodeBc5Block(block, dst16, width);
            } else {
                return false;
            }
        }
    }

    return true;
}

} // namespace malicompat
