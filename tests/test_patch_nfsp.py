#!/usr/bin/env python3
"""
tests/test_patch_nfsp.py - Test Suite for patch_nfsp.py and SPIR-V 4-Set Remapper

Tests:
  1. NFSP Header Parsing and Corruption Detection (BVA / Corner cases)
  2. SPIR-V 1.3 Downgrade & OpEntryPoint Interface Cleaning
  3. JuntarConjuntos 5-Set to 4-Set Remapping Algorithm (Cube -> Set 1 Binding 1)
  4. Real NFSP Package Patching (all 150 shaders from nfsmw-usa.apk / extracted_apk)
  5. Unpack / Repack Round-Trip Byte-for-Byte Fidelity
  6. ContractVerifier Interoperability
"""

import os
import sys
import struct
import tempfile
import unittest
import zipfile

# Add project root to sys.path
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
    unpack_nfsp_to_directory,
    pack_directory_to_nfsp,
    inspect_nfsp_package,
    compute_xxh3_64,
    ShaderEntry,
    NfspPackage,
)

from tests.e2e.common import ContractVerifier


class TestNfspPatcher(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        # Locate original nfsmw_shaders.nfsp
        cls.orig_nfsp_path = os.path.join(PROJECT_ROOT, "extracted_apk", "assets", "nativo", "nfsmw_shaders.nfsp")
        if not os.path.exists(cls.orig_nfsp_path):
            apk_path = os.path.join(PROJECT_ROOT, "nfsmw-usa.apk")
            if os.path.exists(apk_path):
                with zipfile.ZipFile(apk_path, "r") as z:
                    cls.orig_data = z.read("assets/nativo/nfsmw_shaders.nfsp")
            else:
                raise FileNotFoundError("Could not find nfsmw_shaders.nfsp or nfsmw-usa.apk")
        else:
            with open(cls.orig_nfsp_path, "rb") as f:
                cls.orig_data = f.read()

    def test_01_original_header_specification(self):
        """Verifies original 24-byte header specification and checksum match."""
        self.assertGreaterEqual(len(self.orig_data), 24)
        magic = self.orig_data[:8]
        self.assertEqual(magic, MAGIC_NFSSPV)
        ver, count, checksum = struct.unpack("<IIQ", self.orig_data[8:24])
        self.assertEqual(ver, 1)
        self.assertEqual(count, 150)
        calc_hash = compute_xxh3_64(self.orig_data[24:])
        self.assertEqual(calc_hash, checksum)

    def test_02_corrupted_header_handling(self):
        """Verifies rejection of corrupted header magic, version, and checksums."""
        # Truncated
        with self.assertRaises(ValueError):
            parse_nfsp_package(b"SHORT")

        # Bad magic
        bad_magic = b"BADMAGIC" + self.orig_data[8:]
        with self.assertRaises(ValueError):
            parse_nfsp_package(bad_magic)

        # Bad version
        bad_ver = self.orig_data[:8] + struct.pack("<IIQ", 99, 150, 0x1234) + self.orig_data[24:]
        with self.assertRaises(ValueError):
            parse_nfsp_package(bad_ver)

        # Bad checksum
        ver, count, checksum = struct.unpack("<IIQ", self.orig_data[8:24])
        bad_checksum = self.orig_data[:8] + struct.pack("<IIQ", ver, count, checksum ^ 0xFFFFFFFF) + self.orig_data[24:]
        with self.assertRaises(ValueError):
            parse_nfsp_package(bad_checksum)

    def test_03_juntar_conjuntos_synthetic_5_to_4_sets(self):
        """
        Synthesizes a SPIR-V module with all 5 descriptor sets:
          - Set 0, Binding 0 (2D texture)
          - Set 1, Binding 0 (3D texture)
          - Set 2, Binding 0 (Cube map texture)
          - Set 3, Binding 0 (Sampler)
          - Set 4, Binding 0, 1, 2 (UBO constants)
        Verifies:
          - Set 0 remains Set 0, Binding 0
          - Set 1 remains Set 1, Binding 0
          - Set 2 becomes Set 1, Binding 1 (Cube map packed into Set 1)
          - Set 3 becomes Set 2, Binding 0 (Samplers decremented)
          - Set 4 becomes Set 3, Bindings 0, 1, 2 (UBOs decremented)
        """
        # Header: magic, version, gen, bound=20, schema=0
        synthetic_spv = [SPIRV_MAGIC, 0x00010500, 1, 20, 0]

        # Target IDs:
        # ID 1: Set 0, Binding 0
        # ID 2: Set 1, Binding 0
        # ID 3: Set 2, Binding 0 (Cube)
        # ID 4: Set 3, Binding 0 (Sampler)
        # ID 5: Set 4, Binding 0 (UBO 0)
        # ID 6: Set 4, Binding 1 (UBO 1)
        # ID 7: Set 4, Binding 2 (UBO 2)

        def add_decorations(spv, target, set_idx, binding_idx):
            # OpDecorate target DescriptorSet set_idx
            spv.extend([(4 << 16) | OP_DECORATE, target, DECORATION_DESCRIPTOR_SET, set_idx])
            # OpDecorate target Binding binding_idx
            spv.extend([(4 << 16) | OP_DECORATE, target, DECORATION_BINDING, binding_idx])

        add_decorations(synthetic_spv, 1, 0, 0)
        add_decorations(synthetic_spv, 2, 1, 0)
        add_decorations(synthetic_spv, 3, 2, 0)
        add_decorations(synthetic_spv, 4, 3, 0)
        add_decorations(synthetic_spv, 5, 4, 0)
        add_decorations(synthetic_spv, 6, 4, 1)
        add_decorations(synthetic_spv, 7, 4, 2)

        ok, remapped = juntar_conjuntos(synthetic_spv)
        self.assertTrue(ok)
        self.assertEqual(len(remapped), len(synthetic_spv))

        # Inspect remapped decorations
        remapped_decorations = {}
        idx = 5
        while idx < len(remapped):
            w = remapped[idx]
            op = w & 0xFFFF
            length = w >> 16
            if op == OP_DECORATE and length == 4:
                target = remapped[idx + 1]
                dec = remapped[idx + 2]
                val = remapped[idx + 3]
                if target not in remapped_decorations:
                    remapped_decorations[target] = {}
                remapped_decorations[target][dec] = val
            idx += length

        # Verify mappings
        # ID 1: Set 0, Binding 0
        self.assertEqual(remapped_decorations[1][DECORATION_DESCRIPTOR_SET], 0)
        self.assertEqual(remapped_decorations[1][DECORATION_BINDING], 0)

        # ID 2: Set 1, Binding 0
        self.assertEqual(remapped_decorations[2][DECORATION_DESCRIPTOR_SET], 1)
        self.assertEqual(remapped_decorations[2][DECORATION_BINDING], 0)

        # ID 3: Set 2 -> Set 1, Binding 0 -> Binding 1 (Cube Map packed into Set 1 Binding 1!)
        self.assertEqual(remapped_decorations[3][DECORATION_DESCRIPTOR_SET], 1)
        self.assertEqual(remapped_decorations[3][DECORATION_BINDING], 1)

        # ID 4: Set 3 -> Set 2, Binding 0 (Samplers decremented to Set 2!)
        self.assertEqual(remapped_decorations[4][DECORATION_DESCRIPTOR_SET], 2)
        self.assertEqual(remapped_decorations[4][DECORATION_BINDING], 0)

        # ID 5: Set 4 -> Set 3, Binding 0
        self.assertEqual(remapped_decorations[5][DECORATION_DESCRIPTOR_SET], 3)
        self.assertEqual(remapped_decorations[5][DECORATION_BINDING], 0)

        # ID 6: Set 4 -> Set 3, Binding 1
        self.assertEqual(remapped_decorations[6][DECORATION_DESCRIPTOR_SET], 3)
        self.assertEqual(remapped_decorations[6][DECORATION_BINDING], 1)

        # ID 7: Set 4 -> Set 3, Binding 2
        self.assertEqual(remapped_decorations[7][DECORATION_DESCRIPTOR_SET], 3)
        self.assertEqual(remapped_decorations[7][DECORATION_BINDING], 2)

        # Max set index is 3
        max_set = max(d[DECORATION_DESCRIPTOR_SET] for d in remapped_decorations.values())
        self.assertEqual(max_set, 3)

    def test_04_spirv_13_downgrade_and_opentrypoint_pruning(self):
        """Verifies version downgrade to 0x00010300 and pruning non-Input/Output from OpEntryPoint."""
        # Header: magic, version=1.5, gen, bound=20, schema=0
        spv = [SPIRV_MAGIC, 0x00010500, 1, 20, 0]

        # Declare OpVariables:
        # ID 10: Input (1)
        # ID 11: Output (3)
        # ID 12: Uniform (2)
        # ID 13: UniformConstant (0)
        spv.extend([(4 << 16) | OP_VARIABLE, 1, 10, STORAGE_CLASS_INPUT])
        spv.extend([(4 << 16) | OP_VARIABLE, 1, 11, STORAGE_CLASS_OUTPUT])
        spv.extend([(4 << 16) | OP_VARIABLE, 1, 12, 2]) # Uniform
        spv.extend([(4 << 16) | OP_VARIABLE, 1, 13, 0]) # UniformConstant

        # OpEntryPoint Fragment (4), entry_id=2, name="main\0" (2 words: 0x6e69616d, 0x00000000), interface=[10, 11, 12, 13]
        # Total words: 1 (op+len) + 1 (exec) + 1 (id) + 2 (name) + 4 (interfaces) = 9 words
        spv.extend([(9 << 16) | OP_ENTRY_POINT, 4, 2, 0x6e69616d, 0x00000000, 10, 11, 12, 13])

        ok, downgraded = downgrade_spirv_13(spv)
        self.assertTrue(ok)
        self.assertEqual(downgraded[1], SPIRV_VERSION_1_3)

        # Verify OpEntryPoint now only has 10 and 11
        # Find OpEntryPoint
        idx = 5
        entry_point_found = False
        while idx < len(downgraded):
            w = downgraded[idx]
            op = w & 0xFFFF
            length = w >> 16
            if op == OP_ENTRY_POINT:
                entry_point_found = True
                # Format: op/len, exec_model(4), id(2), name_w1, name_w2, then interface IDs
                interfaces = downgraded[idx + 5 : idx + length]
                self.assertEqual(interfaces, [10, 11])
                self.assertEqual(length, 7) # 1+1+1+2+2 = 7 words
            idx += length

        self.assertTrue(entry_point_found)

    def test_05_patch_real_package_end_to_end(self):
        """Patches the actual 150-shader NFSP package and validates all hardware constraints."""
        orig_info = inspect_nfsp_package(self.orig_data)
        self.assertEqual(orig_info["shader_count"], 150)
        self.assertEqual(orig_info["max_descriptor_set"], 4)
        self.assertIn("0x10500", orig_info["spirv_versions"])

        # Run patch
        patched_data = patch_nfsp_package(self.orig_data)
        self.assertGreater(len(patched_data), 24)

        # Verify with ContractVerifier
        valid, msg, info = ContractVerifier.verify_nfsp_header(patched_data)
        self.assertTrue(valid, msg)
        self.assertEqual(info["version"], 1)
        self.assertEqual(info["shader_count"], 150)

        # Inspect patched package
        patched_info = inspect_nfsp_package(patched_data)
        self.assertEqual(patched_info["shader_count"], 150)
        self.assertEqual(patched_info["max_descriptor_set"], 3) # Strictly <= 3!
        self.assertEqual(patched_info["spirv_versions"], ["0x10300"]) # Strictly SPIR-V 1.3!

        # Verify that all combinations use only Sets {0, 1, 2, 3}
        for combo_str in patched_info["set_combinations"].keys():
            combo = eval(combo_str)
            for s in combo:
                self.assertIn(s, [0, 1, 2, 3])

    def test_06_unpack_and_pack_roundtrip(self):
        """Verifies that unpacking to files and repacking yields a valid package."""
        patched_data = patch_nfsp_package(self.orig_data)

        with tempfile.TemporaryDirectory() as tmpdir:
            unpack_nfsp_to_directory(patched_data, tmpdir)
            repacked_data = pack_directory_to_nfsp(tmpdir)

            # Check that repacked is byte-for-byte identical
            self.assertEqual(len(repacked_data), len(patched_data))
            self.assertEqual(repacked_data, patched_data)

            # Check ContractVerifier
            valid, msg, _ = ContractVerifier.verify_nfsp_header(repacked_data)
            self.assertTrue(valid, msg)


if __name__ == "__main__":
    unittest.main()
