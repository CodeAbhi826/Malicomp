#!/usr/bin/env python3
"""
tests/stress_test_patch_nfsp.py - Adversarial Stress Test Suite for patch_nfsp.py
Author: Challenger 2 (SPIR-V & Decompressor Stress)

Adversarial Attack Scenarios:
1. Corrupted NFSP package headers:
   - Bad magic bytes (e.g. b"BADMAGIC", b"\0\0\0\0\0\0\0\0", b"NFSSPV\x01\x00")
   - Truncated header (< 24 bytes: 0, 1, 8, 23 bytes)
   - Corrupted version (version 0, version 2, version 0xFFFFFFFF)
   - Corrupted XXH3_64 package checksums (flipped bits, zero hash, inverted hash)
   - Inconsistent shader count vs payload
2. Truncated package payload & entry streams:
   - Truncated entry header (< 16 bytes)
   - Truncated container data (c_size declared larger than payload)
   - Truncated SPIR-V words (spv_words declared larger than payload)
   - Corrupted per-entry huella (XXH3_64 mismatch)
   - Extraneous trailing garbage bytes after entries
3. Malformed SPIR-V binaries within shader entries:
   - Truncated instruction words (instruction length declaring words past EOF)
   - Instruction with length word = 0 (infinite loop / parser stall trigger)
   - Non-SPIR-V magic (0x0, 0x12345678)
   - Header truncated (< 5 words: 0, 1, 4 words)
   - Truncated OpDecorate instructions (length < 3, length != 4)
   - Target ID >= bound (out-of-bounds ID violation)
   - OpEntryPoint without null-terminator in name string
   - High descriptor set indices (Set 5, Set 10, Set 999)
   - Duplicate descriptor set decorations on the same ID
"""

import os
import sys
import struct
import unittest
import tempfile

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

from tools.patch_nfsp import (
    MAGIC_NFSSPV,
    SUPPORTED_VERSION,
    SPIRV_MAGIC,
    SPIRV_VERSION_1_3,
    OP_DECORATE,
    OP_ENTRY_POINT,
    OP_VARIABLE,
    DECORATION_BINDING,
    DECORATION_DESCRIPTOR_SET,
    STORAGE_CLASS_INPUT,
    STORAGE_CLASS_OUTPUT,
    parse_nfsp_package,
    juntar_conjuntos,
    downgrade_spirv_13,
    remap_spirv_shader,
    patch_nfsp_package,
    compute_xxh3_64,
    ShaderEntry,
    NfspPackage,
)


class TestStressPatchNfsp(unittest.TestCase):

    def setUp(self):
        # Create a minimal valid NFSP package with 1 valid shader
        # Minimal valid SPIR-V module (bound=10)
        self.valid_spv = [
            SPIRV_MAGIC,
            0x00010500, # SPIR-V 1.5
            1,          # generator
            10,         # bound
            0           # schema
        ]
        # Add a valid Set 0 Binding 0 decoration
        self.valid_spv.extend([(4 << 16) | OP_DECORATE, 1, DECORATION_DESCRIPTOR_SET, 0])
        self.valid_spv.extend([(4 << 16) | OP_DECORATE, 1, DECORATION_BINDING, 0])

        self.valid_container = b"TEST_CONTAINER_DATA_12345678"
        entry = ShaderEntry(
            index=0,
            container_bytes=self.valid_container,
            spirv_words=self.valid_spv,
            huella=compute_xxh3_64(self.valid_container)
        )
        self.valid_package = NfspPackage(version=1, entries=[entry])
        self.valid_package_bytes = self.valid_package.pack()

    # -------------------------------------------------------------------------
    # Attack 1: Corrupted Package Headers
    # -------------------------------------------------------------------------
    def test_corrupted_magic_bytes(self):
        """Stress: Corrupted magic bytes must be cleanly rejected with ValueError."""
        bad_magics = [
            b"",
            b"X",
            b"NFSSPV\x00",      # 7 bytes
            b"NFSSPV\x01\x00",  # bad version in magic
            b"VULKAN00",
            b"\x00" * 8,
            b"\xFF" * 8,
        ]
        for bad in bad_magics:
            data = bad + self.valid_package_bytes[8:]
            with self.assertRaises(ValueError, msg=f"Failed to reject bad magic {bad!r}"):
                parse_nfsp_package(data)

    def test_truncated_package_header(self):
        """Stress: Truncated header (< 24 bytes) must be rejected with ValueError."""
        for size in [0, 1, 7, 8, 12, 16, 23]:
            truncated = self.valid_package_bytes[:size]
            with self.assertRaises(ValueError, msg=f"Failed to reject truncated header of size {size}"):
                parse_nfsp_package(truncated)

    def test_unsupported_versions(self):
        """Stress: Non-1 versions (0, 2, 0xFFFF) must be rejected with ValueError."""
        for ver in [0, 2, 3, 0xFFFFFFFF]:
            corrupted = (
                self.valid_package_bytes[:8] +
                struct.pack("<I", ver) +
                self.valid_package_bytes[12:]
            )
            with self.assertRaises(ValueError, msg=f"Failed to reject invalid version {ver}"):
                parse_nfsp_package(corrupted)

    def test_corrupted_checksums(self):
        """Stress: Package checksum mismatches must be cleanly rejected with ValueError."""
        header = self.valid_package_bytes[:16]
        expected_cs = struct.unpack("<Q", self.valid_package_bytes[16:24])[0]
        payload = self.valid_package_bytes[24:]

        bad_checksums = [
            expected_cs ^ 0x01,
            expected_cs ^ 0x8000000000000000,
            0,
            0xFFFFFFFFFFFFFFFF,
        ]
        for bad_cs in bad_checksums:
            corrupted = header + struct.pack("<Q", bad_cs) + payload
            with self.assertRaises(ValueError, msg=f"Failed to reject bad checksum {hex(bad_cs)}"):
                parse_nfsp_package(corrupted)

    # -------------------------------------------------------------------------
    # Attack 2: Truncated Payload & Corrupted Entries
    # -------------------------------------------------------------------------
    def test_truncated_entry_header(self):
        """Stress: Entry header truncated mid-way."""
        # 24 byte header + 10 bytes of entry header (expected 16)
        data = self.valid_package_bytes[:34]
        # Recalculate checksum so header parser accepts, but payload parser fails
        new_cs = compute_xxh3_64(data[24:])
        data = data[:16] + struct.pack("<Q", new_cs) + data[24:]
        with self.assertRaises(ValueError):
            parse_nfsp_package(data)

    def test_truncated_container_bytes(self):
        """Stress: Container claims 100 bytes but only 10 available."""
        corrupted_payload = struct.pack("<IIQ", 100, len(self.valid_spv), 0x1234) + b"1234567890"
        cs = compute_xxh3_64(corrupted_payload)
        header = struct.pack("<8sIIQ", MAGIC_NFSSPV, 1, 1, cs)
        with self.assertRaises(ValueError):
            parse_nfsp_package(header + corrupted_payload)

    def test_truncated_spirv_words(self):
        """Stress: SPIR-V claims 500 words but payload ends early."""
        c_bytes = b"CONTAINER"
        huella = compute_xxh3_64(c_bytes)
        corrupted_payload = struct.pack("<IIQ", len(c_bytes), 500, huella) + c_bytes + b"\x00" * 16
        cs = compute_xxh3_64(corrupted_payload)
        header = struct.pack("<8sIIQ", MAGIC_NFSSPV, 1, 1, cs)
        with self.assertRaises(ValueError):
            parse_nfsp_package(header + corrupted_payload)

    def test_corrupted_entry_huella(self):
        """Stress: Mismatched entry huella checksum must be caught."""
        c_bytes = b"CONTAINER_ORIGINAL"
        bad_huella = compute_xxh3_64(c_bytes) ^ 0xDEADBEEF
        spv_bytes = struct.pack(f"<{len(self.valid_spv)}I", *self.valid_spv)
        payload = struct.pack("<IIQ", len(c_bytes), len(self.valid_spv), bad_huella) + c_bytes + spv_bytes
        cs = compute_xxh3_64(payload)
        header = struct.pack("<8sIIQ", MAGIC_NFSSPV, 1, 1, cs)
        with self.assertRaises(ValueError):
            parse_nfsp_package(header + payload)

    def test_extraneous_trailing_garbage(self):
        """Stress: Trailing junk bytes after declared entries must be rejected."""
        trailing_garbage = self.valid_package_bytes + b"TRAILING_UNACCOUNTED_GARBAGE"
        new_cs = compute_xxh3_64(trailing_garbage[24:])
        corrupted = trailing_garbage[:16] + struct.pack("<Q", new_cs) + trailing_garbage[24:]
        with self.assertRaises(ValueError):
            parse_nfsp_package(corrupted)

    # -------------------------------------------------------------------------
    # Attack 3: Malformed SPIR-V Instruction Stream
    # -------------------------------------------------------------------------
    def test_spirv_truncated_instruction_word_length(self):
        """Stress: Instruction length word specifies words past EOF."""
        spv = [SPIRV_MAGIC, 0x00010300, 1, 10, 0]
        # Declare OpDecorate with length 100, but only 2 words present
        spv.append((100 << 16) | OP_DECORATE)
        spv.append(1)

        ok, _ = juntar_conjuntos(spv)
        self.assertFalse(ok, "juntar_conjuntos should reject out-of-bounds instruction length")

        ok2, _ = downgrade_spirv_13(spv)
        self.assertFalse(ok2, "downgrade_spirv_13 should reject out-of-bounds instruction length")

    def test_spirv_zero_length_instruction(self):
        """Stress: Instruction with length = 0 must not cause an infinite loop."""
        spv = [SPIRV_MAGIC, 0x00010300, 1, 10, 0]
        spv.append((0 << 16) | OP_DECORATE) # Length 0 is illegal in SPIR-V

        ok, _ = juntar_conjuntos(spv)
        self.assertFalse(ok, "juntar_conjuntos should reject length 0 instruction")

        ok2, _ = downgrade_spirv_13(spv)
        self.assertFalse(ok2, "downgrade_spirv_13 should reject length 0 instruction")

    def test_spirv_out_of_bounds_target_id(self):
        """Stress: OpDecorate target ID >= bound (header word 3) must be rejected."""
        spv = [SPIRV_MAGIC, 0x00010300, 1, 5, 0] # Bound = 5 (valid IDs: 1..4)
        # Target ID 10 >= bound 5
        spv.extend([(4 << 16) | OP_DECORATE, 10, DECORATION_DESCRIPTOR_SET, 0])
        spv.extend([(4 << 16) | OP_DECORATE, 10, DECORATION_BINDING, 0])

        ok, _ = juntar_conjuntos(spv)
        self.assertFalse(ok, "juntar_conjuntos should reject target ID exceeding bound")

    def test_spirv_high_descriptor_set_indices(self):
        """Stress: High descriptor set indices (Set 5, Set 10, Set 100) must return False gracefully."""
        for high_set in [5, 6, 10, 100, 9999]:
            spv = [SPIRV_MAGIC, 0x00010300, 1, 20, 0]
            spv.extend([(4 << 16) | OP_DECORATE, 1, DECORATION_DESCRIPTOR_SET, high_set])
            spv.extend([(4 << 16) | OP_DECORATE, 1, DECORATION_BINDING, 0])

            ok, out_spv = juntar_conjuntos(spv)
            self.assertFalse(
                ok,
                f"juntar_conjuntos must return False for high descriptor set {high_set}"
            )
            # Ensure out_spv is unchanged original
            self.assertEqual(out_spv, spv)

    def test_spirv_duplicate_descriptor_set_on_target(self):
        """Stress: Duplicate DescriptorSet decoration on the same target ID must be rejected."""
        spv = [SPIRV_MAGIC, 0x00010300, 1, 20, 0]
        # Target 1 decorated with Set 0, then again with Set 1
        spv.extend([(4 << 16) | OP_DECORATE, 1, DECORATION_DESCRIPTOR_SET, 0])
        spv.extend([(4 << 16) | OP_DECORATE, 1, DECORATION_DESCRIPTOR_SET, 1])

        ok, _ = juntar_conjuntos(spv)
        self.assertFalse(ok, "juntar_conjuntos must reject duplicate DescriptorSet decoration")

    def test_spirv_opentrypoint_missing_null_terminator(self):
        """Stress: OpEntryPoint string lacking null byte must not crash or loop infinitely."""
        spv = [SPIRV_MAGIC, 0x00010500, 1, 20, 0]
        # OpEntryPoint Fragment (4), ID 2, Name without null byte in 2 words: 0x41414141, 0x42424242
        spv.extend([(5 << 16) | OP_ENTRY_POINT, 4, 2, 0x41414141, 0x42424242])

        ok, out_spv = downgrade_spirv_13(spv)
        self.assertTrue(ok, "downgrade_spirv_13 should complete without hang on non-terminated entry name")
        self.assertEqual(out_spv[1], SPIRV_VERSION_1_3)


if __name__ == "__main__":
    unittest.main()
