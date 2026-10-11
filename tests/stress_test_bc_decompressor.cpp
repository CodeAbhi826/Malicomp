#include "bc_decompressor.h"
#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cassert>

// Test harness for BcTextureDecompressor stress testing
void test_zero_dimensions() {
    std::cout << "[BC-Stress] Test 1: Zero dimensions & null pointers...\n";
    uint8_t dummy[64]{};
    // Null pointers
    assert(!malicompat::BcTextureDecompressor::DecompressImage(1, 4, 4, nullptr, dummy));
    assert(!malicompat::BcTextureDecompressor::DecompressImage(1, 4, 4, dummy, nullptr));
    // Zero dimensions
    assert(!malicompat::BcTextureDecompressor::DecompressImage(1, 0, 4, dummy, dummy));
    assert(!malicompat::BcTextureDecompressor::DecompressImage(1, 4, 0, dummy, dummy));
    assert(!malicompat::BcTextureDecompressor::DecompressImage(1, 0, 0, dummy, dummy));
    // Invalid format
    assert(!malicompat::BcTextureDecompressor::DecompressImage(99, 4, 4, dummy, dummy));
    std::cout << "[BC-Stress] Test 1 PASSED: Zero dimensions and null checks return false.\n";
}

void test_odd_dimensions_bc1() {
    std::cout << "[BC-Stress] Test 2: Odd dimensions (5x5) for BC1...\n";
    // 5x5 image: blocksX = (5+3)/4 = 2, blocksY = (5+3)/4 = 2 -> 4 blocks = 32 bytes compressed
    std::vector<uint8_t> compressed(4 * 8, 0); // 4 BC1 blocks
    // Normal allocation for a 5x5 RGBA8 image is 5 * 5 * 4 = 100 bytes (25 uint32_t pixels)
    std::vector<uint32_t> dstPixels(5 * 5, 0x12345678);

    std::cout << "[BC-Stress] Invoking DecompressImage(format=1, width=5, height=5)...\n";
    bool ok = malicompat::BcTextureDecompressor::DecompressImage(
        1, 5, 5, compressed.data(), reinterpret_cast<uint8_t*>(dstPixels.data()));

    std::cout << "[BC-Stress] Completed with result: " << ok << "\n";
}

void test_1x1_dimension_bc1() {
    std::cout << "[BC-Stress] Test 3: Minimal dimension (1x1) for BC1...\n";
    // 1x1 image: blocksX = 1, blocksY = 1 -> 1 block = 8 bytes
    std::vector<uint8_t> compressed(8, 0);
    // Exact buffer for 1x1 RGBA8 is 1 pixel = 4 bytes
    std::vector<uint32_t> dstPixels(1, 0);

    std::cout << "[BC-Stress] Invoking DecompressImage(format=1, width=1, height=1)...\n";
    bool ok = malicompat::BcTextureDecompressor::DecompressImage(
        1, 1, 1, compressed.data(), reinterpret_cast<uint8_t*>(dstPixels.data()));

    std::cout << "[BC-Stress] Completed with result: " << ok << "\n";
}

void test_truncated_stream_bc1() {
    std::cout << "[BC-Stress] Test 4: Truncated source stream (8x8 image needs 4 blocks = 32 bytes, provided only 1 block = 8 bytes)...\n";
    // 8x8 needs 4 blocks (32 bytes). Provide only 8 bytes.
    std::vector<uint8_t> truncatedSrc(8, 0);
    std::vector<uint32_t> dstPixels(8 * 8, 0);

    std::cout << "[BC-Stress] Invoking DecompressImage with truncated source buffer...\n";
    bool ok = malicompat::BcTextureDecompressor::DecompressImage(
        1, 8, 8, truncatedSrc.data(), reinterpret_cast<uint8_t*>(dstPixels.data()));

    std::cout << "[BC-Stress] Completed with result: " << ok << "\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        test_zero_dimensions();
        test_odd_dimensions_bc1();
        test_1x1_dimension_bc1();
        test_truncated_stream_bc1();
        return 0;
    }

    std::string testName = argv[1];
    if (testName == "zero") {
        test_zero_dimensions();
    } else if (testName == "odd_5x5") {
        test_odd_dimensions_bc1();
    } else if (testName == "odd_1x1") {
        test_1x1_dimension_bc1();
    } else if (testName == "truncated") {
        test_truncated_stream_bc1();
    }
    return 0;
}
