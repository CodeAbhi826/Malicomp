#include "bc_decompressor.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>

int main() {
    std::cout << "[MaliCompat-Test] Starting BC Texture Decompressor Unit Test...\n";

    // 1. Construct a solid red 4x4 BC1 compressed block
    // Red in RGB565 is 0xF800 (11111 000000 00000)
    // Low byte: 0x00, High byte: 0xF8
    uint8_t bc1Block[8] = {
        0x00, 0xF8, // c0 = Red
        0x00, 0x00, // c1 = Black
        0x00, 0x00, 0x00, 0x00 // All pixels select color 0 (Red)
    };

    uint32_t decodedPixels[16]{};
    malicompat::BcTextureDecompressor::DecodeBc1Block(bc1Block, decodedPixels, 4);

    std::cout << "[MaliCompat-Test] Checking decoded pixels (RGBA8):\n";
    bool pass = true;
    for (int i = 0; i < 16; ++i) {
        uint32_t p = decodedPixels[i];
        uint8_t r = p & 0xFF;
        uint8_t g = (p >> 8) & 0xFF;
        uint8_t b = (p >> 16) & 0xFF;
        uint8_t a = (p >> 24) & 0xFF;
        if (r < 240 || g > 10 || b > 10 || a != 255) {
            std::cerr << "Pixel " << i << " mismatch: R=" << int(r) << " G=" << int(g) << " B=" << int(b) << " A=" << int(a) << "\n";
            pass = false;
            break;
        }
    }

    if (pass) {
        std::cout << "[MaliCompat-Test] SUCCESS: BC1 Block correctly decompressed to 16 native RGBA8 pixels!\n";
    }

    // 2. Test full image batch decompression
    std::vector<uint8_t> compressedImage(8 * 4); // 4 blocks (8x8 texture)
    for (size_t b = 0; b < 4; ++b) {
        std::memcpy(compressedImage.data() + b * 8, bc1Block, 8);
    }

    std::vector<uint8_t> decompressedImage(8 * 8 * 4); // 8x8 RGBA8
    bool imgPass = malicompat::BcTextureDecompressor::DecompressImage(
        1, 8, 8, compressedImage.data(), decompressedImage.data());

    if (imgPass) {
        std::cout << "[MaliCompat-Test] SUCCESS: 8x8 BC1 image successfully decompressed into RGBA8 buffer!\n";
    }

    return (pass && imgPass) ? 0 : 1;
}
