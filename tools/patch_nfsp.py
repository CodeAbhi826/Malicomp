#!/usr/bin/env python3
"""
tools/patch_nfsp.py - NFSP Shader Package Remapper & Patcher for ARM Mali-G71 (Bifrost)

Author / Attribution:
  Part of Malicomp - Need for Speed: Most Wanted (2005) Android Recompilation Mali Fixes.
  Incorporates the JuntarConjuntos 4-set remapping algorithm based on victorgbd/NFSMW-Recompiled-Mobile
  and SPIR-V 1.3 Vulkan 1.1 compatibility downgrade from codepdbh/nfsmw-android.

Features (F10 & F11):
  - Inspects and parses 'nfsmw_shaders.nfsp' packages (24-byte header: NFSSPV\\0\\0, ver 1, count, XXH3_64).
  - Unpacks individual Xbox 360 shader containers and SPIR-V binaries into structured directories.
  - Implements the JuntarConjuntos algorithm: remaps 5 native descriptor sets (0..4) to 4 sets (0..3)
    to strictly satisfy ARM Mali's hardware limit maxBoundDescriptorSets == 4:
      * Set 2 (Cube map textures) mapped into Set 1, Binding 1.
      * Set 3 (Samplers) decremented to Set 2, Binding 0.
      * Set 4 (UBO constants) decremented to Set 3.
  - Downgrades SPIR-V header to SPIR-V 1.3 (0x00010300) and cleans OpEntryPoint interfaces (only Input/Output)
    for strict Vulkan 1.1 compliance on Mali Bifrost r26p0.
  - Recalculates exact per-entry huella (XXH3_64) and whole-package payload XXH3_64 checksums.
  - Repacks modified shaders into fully valid .nfsp binary archives.
"""

import os
import sys
import struct
import json
import argparse
from typing import List, Tuple, Dict, Optional, Any

try:
    import xxhash
    def compute_xxh3_64(data: bytes) -> int:
        return xxhash.xxh3_64_intdigest(data)
except ImportError:
    raise ImportError(
        "The 'xxhash' library is required to calculate exact XXH3_64 checksums for .nfsp packages. "
        "Please install it via: pip install xxhash"
    )

MAGIC_NFSSPV = b"NFSSPV\x00\x00"
SUPPORTED_VERSION = 1
SPIRV_MAGIC = 0x07230203
SPIRV_VERSION_1_3 = 0x00010300
OP_ENTRY_POINT = 15
OP_VARIABLE = 59
OP_DECORATE = 71
DECORATION_BINDING = 33
DECORATION_DESCRIPTOR_SET = 34
STORAGE_CLASS_INPUT = 1
STORAGE_CLASS_OUTPUT = 3


class ShaderEntry:
    def __init__(
        self,
        index: int,
        container_bytes: bytes,
        spirv_words: List[int],
        huella: Optional[int] = None
    ):
        self.index = index
        self.container_bytes = container_bytes
        self.spirv_words = list(spirv_words)
        self.huella = huella if huella is not None else compute_xxh3_64(container_bytes)

    @property
    def container_size(self) -> int:
        return len(self.container_bytes)

    @property
    def spirv_words_count(self) -> int:
        return len(self.spirv_words)

    @property
    def spirv_bytes(self) -> bytes:
        return struct.pack(f"<{len(self.spirv_words)}I", *self.spirv_words)

    @property
    def spirv_version(self) -> Optional[int]:
        if len(self.spirv_words) >= 2 and self.spirv_words[0] == SPIRV_MAGIC:
            return self.spirv_words[1]
        return None

    def get_descriptor_sets(self) -> List[int]:
        """Returns sorted list of unique descriptor sets used in this shader."""
        sets = set()
        p = self.spirv_words
        n_words = len(p)
        if n_words < 5 or p[0] != SPIRV_MAGIC:
            return []
        i = 5
        while i < n_words:
            w = p[i]
            n = w >> 16
            op = w & 0xFFFF
            if n == 0 or i + n > n_words:
                break
            if op == OP_DECORATE and n >= 4:
                if p[i + 2] == DECORATION_DESCRIPTOR_SET:
                    sets.add(p[i + 3])
            i += n
        return sorted(sets)

    def pack_entry(self) -> bytes:
        """Packs this shader entry into binary format."""
        header = struct.pack("<IIQ", self.container_size, self.spirv_words_count, self.huella)
        return header + self.container_bytes + self.spirv_bytes


class NfspPackage:
    def __init__(self, version: int = SUPPORTED_VERSION, entries: Optional[List[ShaderEntry]] = None):
        self.version = version
        self.entries = entries or []

    @property
    def shader_count(self) -> int:
        return len(self.entries)

    def pack(self) -> bytes:
        """Packs the entire package into a 24-byte header followed by entries."""
        payload = bytearray()
        for entry in self.entries:
            payload.extend(entry.pack_entry())
        payload_bytes = bytes(payload)
        checksum = compute_xxh3_64(payload_bytes)
        header = struct.pack("<8sIIQ", MAGIC_NFSSPV, self.version, self.shader_count, checksum)
        return header + payload_bytes


def parse_nfsp_package(data: bytes) -> NfspPackage:
    """Parses a complete .nfsp package binary and validates its header and checksum."""
    if len(data) < 24:
        raise ValueError(f"Package truncated: size {len(data)} bytes is less than 24-byte header.")
    magic = data[:8]
    if magic != MAGIC_NFSSPV:
        raise ValueError(f"Invalid magic: {magic!r} != {MAGIC_NFSSPV!r}")
    version, count, expected_checksum = struct.unpack("<IIQ", data[8:24])
    if version != SUPPORTED_VERSION:
        raise ValueError(f"Unsupported package version: {version} (expected {SUPPORTED_VERSION})")

    payload = data[24:]
    actual_checksum = compute_xxh3_64(payload)
    if actual_checksum != expected_checksum:
        raise ValueError(
            f"Package checksum mismatch: computed {hex(actual_checksum)} != expected {hex(expected_checksum)}"
        )

    entries = []
    off = 0
    payload_len = len(payload)
    for idx in range(count):
        if off + 16 > payload_len:
            raise ValueError(f"Entry {idx} header truncated at payload offset {off}")
        c_size, spv_words, huella = struct.unpack("<IIQ", payload[off:off + 16])
        off += 16
        if off + c_size > payload_len:
            raise ValueError(f"Entry {idx} container data truncated")
        container = payload[off:off + c_size]
        off += c_size
        spv_bytes_len = spv_words * 4
        if off + spv_bytes_len > payload_len:
            raise ValueError(f"Entry {idx} SPIR-V data truncated")
        spv_bytes = payload[off:off + spv_bytes_len]
        off += spv_bytes_len

        # Verify entry huella
        calc_huella = compute_xxh3_64(container)
        if calc_huella != huella:
            raise ValueError(
                f"Entry {idx} huella mismatch: computed {hex(calc_huella)} != expected {hex(huella)}"
            )

        spv_words_list = list(struct.unpack(f"<{spv_words}I", spv_bytes))
        entries.append(ShaderEntry(idx, container, spv_words_list, huella))

    if off != payload_len:
        raise ValueError(f"Extraneous bytes in payload: parsed {off} bytes, total payload {payload_len} bytes")

    return NfspPackage(version=version, entries=entries)


def juntar_conjuntos(p: List[int]) -> Tuple[bool, List[int]]:
    """
    Implements JuntarConjuntos 5-set to 4-set remapping algorithm.
    Maps Sets {0, 1, 2, 3, 4} to Sets {0, 1, 1, 2, 3}:
      - Set 2 (Cube maps) -> Set 1, Binding incremented (+1 -> Binding 1)
      - Set 3 (Samplers) -> Set 2
      - Set 4 (UBOs) -> Set 3
    Returns (success, remapped_words).
    """
    if len(p) < 5 or p[0] != SPIRV_MAGIC:
        return False, p

    conjuntos: Dict[int, int] = {}
    n_words = len(p)
    i = 5
    # Pass 1: find and validate OpDecorate instructions for DescriptorSet & Binding
    while i < n_words:
        w = p[i]
        n = w >> 16
        op = w & 0xFFFF
        if n == 0 or i + n > n_words:
            return False, p
        if op == OP_DECORATE:
            if n < 3:
                return False, p
            dec = p[i + 2]
            if dec == DECORATION_BINDING or dec == DECORATION_DESCRIPTOR_SET:
                if n != 4 or p[i + 1] >= p[3]: # target ID must be < bound
                    return False, p
                if dec == DECORATION_DESCRIPTOR_SET:
                    target = p[i + 1]
                    s = p[i + 3]
                    if s > 4 or target in conjuntos:
                        return False, p
                    conjuntos[target] = s
        i += n

    # Pass 2: mutate DescriptorSet and Binding values
    out = list(p)
    i = 5
    while i < n_words:
        w = out[i]
        n = w >> 16
        op = w & 0xFFFF
        if op == OP_DECORATE and n == 4:
            target = out[i + 1]
            if target in conjuntos:
                orig_set = conjuntos[target]
                dec = out[i + 2]
                if dec == DECORATION_DESCRIPTOR_SET and orig_set >= 2:
                    out[i + 3] = orig_set - 1
                elif dec == DECORATION_BINDING and orig_set == 2:
                    out[i + 3] += 1
        i += n

    return True, out


def downgrade_spirv_13(p: List[int]) -> Tuple[bool, List[int]]:
    """
    Downgrades SPIR-V 1.4/1.5 module to SPIR-V 1.3:
      - Sets version word (word 1) to 0x00010300 (SPIR-V 1.3).
      - Collects storage classes of all OpVariable instructions.
      - Filters OpEntryPoint interface variable IDs down to only Input (1) and Output (3)
        storage classes, satisfying SPIR-V 1.3 Section 3.32.5 constraints.
    Returns (success, downgraded_words).
    """
    if len(p) < 5 or p[0] != SPIRV_MAGIC:
        return False, p

    n = len(p)
    # Collect storage class for every OpVariable
    clase: Dict[int, int] = {}
    i = 5
    while i < n:
        cuenta = p[i] >> 16
        op = p[i] & 0xFFFF
        if cuenta == 0 or i + cuenta > n:
            return False, p
        if op == OP_VARIABLE and cuenta >= 4:
            # OpVariable: result_type=p[i+1], result_id=p[i+2], storage_class=p[i+3]
            clase[p[i + 2]] = p[i + 3]
        i += cuenta

    salida = list(p[:5])
    salida[1] = SPIRV_VERSION_1_3

    i = 5
    while i < n:
        cuenta = p[i] >> 16
        op = p[i] & 0xFFFF
        if op != OP_ENTRY_POINT or cuenta < 4:
            salida.extend(p[i:i + cuenta])
            i += cuenta
            continue

        # OpEntryPoint: execution_model, function_id, nul-terminated name, followed by interface ids
        j = i + 3
        while j < i + cuenta:
            w = p[j]
            # Check for nul-byte terminator in any byte of 32-bit word
            if ((w & 0xFF) == 0 or ((w >> 8) & 0xFF) == 0 or
                ((w >> 16) & 0xFF) == 0 or (w >> 24) == 0):
                j += 1
                break
            j += 1

        inicio = len(salida)
        salida.extend(p[i:j])
        for k in range(j, i + cuenta):
            var_id = p[k]
            st_cl = clase.get(var_id)
            if st_cl == STORAGE_CLASS_INPUT or st_cl == STORAGE_CLASS_OUTPUT:
                salida.append(var_id)

        # Update instruction length in word 0
        new_instruction_len = len(salida) - inicio
        salida[inicio] = (new_instruction_len << 16) | OP_ENTRY_POINT
        i += cuenta

    return True, salida


def remap_spirv_shader(spv_words: List[int]) -> Tuple[bool, List[int]]:
    """Applies SPIR-V 1.3 downgrade followed by JuntarConjuntos 4-set remapping."""
    ok_down, downgraded = downgrade_spirv_13(spv_words)
    if not ok_down:
        return False, spv_words
    ok_remap, remapped = juntar_conjuntos(downgraded)
    if not ok_remap:
        return False, downgraded
    return True, remapped


def patch_nfsp_package(input_data: bytes) -> bytes:
    """Reads input NFSP package binary, applies 4-set remapping to all shaders, and returns patched binary."""
    package = parse_nfsp_package(input_data)
    for entry in package.entries:
        ok, new_spv = remap_spirv_shader(entry.spirv_words)
        if not ok:
            raise RuntimeError(f"Failed to remap shader entry {entry.index}")
        entry.spirv_words = new_spv
    return package.pack()


def unpack_nfsp_to_directory(data: bytes, output_dir: str) -> None:
    """Unpacks NFSP package into individual container .bin and .spv files plus manifest.json."""
    os.makedirs(output_dir, exist_ok=True)
    package = parse_nfsp_package(data)
    manifest = {
        "version": package.version,
        "shader_count": package.shader_count,
        "entries": []
    }

    for entry in package.entries:
        bin_name = f"shader_{entry.index:03d}_{hex(entry.huella)[2:]}.bin"
        spv_name = f"shader_{entry.index:03d}_{hex(entry.huella)[2:]}.spv"
        bin_path = os.path.join(output_dir, bin_name)
        spv_path = os.path.join(output_dir, spv_name)

        with open(bin_path, "wb") as f:
            f.write(entry.container_bytes)
        with open(spv_path, "wb") as f:
            f.write(entry.spirv_bytes)

        manifest["entries"].append({
            "index": entry.index,
            "huella": hex(entry.huella),
            "container_file": bin_name,
            "container_size": entry.container_size,
            "spirv_file": spv_name,
            "spirv_words": entry.spirv_words_count,
            "spirv_version": hex(entry.spirv_version) if entry.spirv_version else None,
            "descriptor_sets": entry.get_descriptor_sets()
        })

    manifest_path = os.path.join(output_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)


def pack_directory_to_nfsp(input_dir: str) -> bytes:
    """Reconstructs NFSP package from directory containing manifest.json and shader files."""
    manifest_path = os.path.join(input_dir, "manifest.json")
    if not os.path.exists(manifest_path):
        raise FileNotFoundError(f"Manifest file not found: {manifest_path}")

    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    entries = []
    for item in manifest.get("entries", []):
        idx = item["index"]
        bin_path = os.path.join(input_dir, item["container_file"])
        spv_path = os.path.join(input_dir, item["spirv_file"])

        with open(bin_path, "rb") as f:
            c_bytes = f.read()
        with open(spv_path, "rb") as f:
            spv_raw = f.read()

        words_count = len(spv_raw) // 4
        spv_words = list(struct.unpack(f"<{words_count}I", spv_raw))
        huella = int(item["huella"], 16) if "huella" in item else None
        entries.append(ShaderEntry(idx, c_bytes, spv_words, huella))

    package = NfspPackage(version=manifest.get("version", SUPPORTED_VERSION), entries=entries)
    return package.pack()


def inspect_nfsp_package(data: bytes, verbose: bool = False) -> Dict[str, Any]:
    """Inspects package and returns structured diagnostic summary."""
    package = parse_nfsp_package(data)
    set_combos: Dict[str, int] = {}
    max_set_found = 0
    versions_found = set()

    entries_summary = []
    for entry in package.entries:
        sets = entry.get_descriptor_sets()
        key = str(sets)
        set_combos[key] = set_combos.get(key, 0) + 1
        if sets:
            max_set_found = max(max_set_found, max(sets))
        if entry.spirv_version:
            versions_found.add(hex(entry.spirv_version))

        if verbose:
            entries_summary.append({
                "index": entry.index,
                "huella": hex(entry.huella),
                "c_size": entry.container_size,
                "spv_words": entry.spirv_words_count,
                "version": hex(entry.spirv_version) if entry.spirv_version else "none",
                "sets": sets
            })

    return {
        "valid": True,
        "version": package.version,
        "shader_count": package.shader_count,
        "max_descriptor_set": max_set_found,
        "set_combinations": set_combos,
        "spirv_versions": list(sorted(versions_found)),
        "entries": entries_summary if verbose else None
    }


def main():
    # Handle shorthand positional arguments directly before parser if subcommands not used
    known_commands = {"inspect", "patch", "unpack", "pack", "verify", "-h", "--help"}
    if len(sys.argv) == 3 and sys.argv[1] not in known_commands:
        input_file = sys.argv[1]
        output_file = sys.argv[2]
        print(f"[MaliCompat] Patching NFSP package: {input_file} -> {output_file}")
        with open(input_file, "rb") as f:
            data = f.read()
        patched = patch_nfsp_package(data)
        with open(output_file, "wb") as f:
            f.write(patched)
        print(f"[MaliCompat] Successfully patched {input_file} ({len(data)} bytes) -> {output_file} ({len(patched)} bytes).")
        return 0
    elif len(sys.argv) == 2 and sys.argv[1] not in known_commands:
        file_path = sys.argv[1]
        with open(file_path, "rb") as f:
            data = f.read()
        info = inspect_nfsp_package(data, verbose=False)
        print(f"NFSP Package Summary for: {file_path}")
        print(f"  Version: {info['version']}")
        print(f"  Shader Count: {info['shader_count']}")
        print(f"  Max Descriptor Set: {info['max_descriptor_set']}")
        print(f"  SPIR-V Versions: {info['spirv_versions']}")
        print(f"  Descriptor Set Combinations:")
        for combo, count in info["set_combinations"].items():
            print(f"    {combo}: {count} shaders")
        return 0

    parser = argparse.ArgumentParser(
        description="NFSP Shader Package Remapper & Patcher for ARM Mali-G71 (Bifrost)",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )

    subparsers = parser.add_subparsers(dest="command", help="Subcommand to execute")

    # Command: inspect
    inspect_cmd = subparsers.add_parser("inspect", help="Inspect .nfsp package structure and descriptor sets")
    inspect_cmd.add_argument("file", help="Path to .nfsp package file")
    inspect_cmd.add_argument("-v", "--verbose", action="store_true", help="Show all 150 shader entry details")

    # Command: patch
    patch_cmd = subparsers.add_parser("patch", help="Patch .nfsp package applying 4-set remapping and SPIR-V 1.3 downgrade")
    patch_cmd.add_argument("input", help="Source .nfsp package file")
    patch_cmd.add_argument("output", help="Target output .nfsp package file")

    # Command: unpack
    unpack_cmd = subparsers.add_parser("unpack", help="Unpack .nfsp package into directory")
    unpack_cmd.add_argument("input", help="Source .nfsp package file")
    unpack_cmd.add_argument("output_dir", help="Output directory to write files")

    # Command: pack
    pack_cmd = subparsers.add_parser("pack", help="Pack directory back into .nfsp binary")
    pack_cmd.add_argument("input_dir", help="Directory containing manifest.json and shader files")
    pack_cmd.add_argument("output", help="Output .nfsp package file")

    # Command: verify
    verify_cmd = subparsers.add_parser("verify", help="Verify .nfsp package conforms to Mali 4-set and SPIR-V 1.3 limits")
    verify_cmd.add_argument("file", help="Path to .nfsp package file")

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        return 1

    if args.command == "inspect":
        with open(args.file, "rb") as f:
            data = f.read()
        info = inspect_nfsp_package(data, verbose=args.verbose)
        print(f"NFSP Package Summary: {args.file}")
        print(f"  Version: {info['version']}")
        print(f"  Shader Count: {info['shader_count']}")
        print(f"  Max Descriptor Set: {info['max_descriptor_set']}")
        print(f"  SPIR-V Versions: {info['spirv_versions']}")
        print(f"  Descriptor Set Combinations:")
        for combo, count in info["set_combinations"].items():
            print(f"    {combo}: {count} shaders")
        if args.verbose and info["entries"]:
            print("\nDetailed Entries:")
            for e in info["entries"]:
                print(f"  [{e['index']:03d}] Huella: {e['huella']}, C_Size: {e['c_size']}, SPV_Words: {e['spv_words']}, Ver: {e['version']}, Sets: {e['sets']}")
        return 0

    elif args.command == "patch":
        with open(args.input, "rb") as f:
            data = f.read()
        print(f"[MaliCompat] Reading input package {args.input} ({len(data)} bytes)...")
        patched = patch_nfsp_package(data)
        with open(args.output, "wb") as f:
            f.write(patched)
        print(f"[MaliCompat] Wrote patched package {args.output} ({len(patched)} bytes).")
        info = inspect_nfsp_package(patched, verbose=False)
        print(f"[MaliCompat] Verified patched package: Max Set={info['max_descriptor_set']} (<= 3), Versions={info['spirv_versions']}")
        return 0

    elif args.command == "unpack":
        with open(args.input, "rb") as f:
            data = f.read()
        print(f"[MaliCompat] Unpacking {args.input} into {args.output_dir}...")
        unpack_nfsp_to_directory(data, args.output_dir)
        print(f"[MaliCompat] Successfully unpacked into {args.output_dir}.")
        return 0

    elif args.command == "pack":
        print(f"[MaliCompat] Packing directory {args.input_dir} into {args.output}...")
        data = pack_directory_to_nfsp(args.input_dir)
        with open(args.output, "wb") as f:
            f.write(data)
        print(f"[MaliCompat] Successfully created {args.output} ({len(data)} bytes).")
        return 0

    elif args.command == "verify":
        with open(args.file, "rb") as f:
            data = f.read()
        info = inspect_nfsp_package(data, verbose=False)
        errors = []
        if info["max_descriptor_set"] > 3:
            errors.append(f"Shader uses descriptor set {info['max_descriptor_set']} > 3 (exceeds Mali maxBoundDescriptorSets == 4)")
        for v in info["spirv_versions"]:
            if int(v, 16) > SPIRV_VERSION_1_3:
                errors.append(f"Shader uses SPIR-V version {v} > 0x10300 (SPIR-V 1.3)")
        if errors:
            print("[FAIL] Verification failed:")
            for err in errors:
                print(f"  - {err}")
            return 1
        else:
            print("[PASS] Verification passed: all shaders conform to Mali-G71 limits (Sets <= 3, SPIR-V <= 1.3).")
            return 0


if __name__ == "__main__":
    sys.exit(main())
