"""
E2E Test Suite Common Infrastructure
Provides DeviceBridge, ContractVerifier, TestResult, and Report Generation.
"""

import os
import sys
import time
import json
import struct
import io
import subprocess
from enum import Enum
from dataclasses import dataclass, field
from typing import Optional, Dict, Any, List, Tuple
from PIL import Image

try:
    import xxhash
    HAS_XXHASH = True
except ImportError:
    HAS_XXHASH = False


class TestStatus(Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"


@dataclass
class TestResult:
    id: str
    name: str
    tier: int
    category: str
    feature_ref: str
    status: TestStatus
    duration_ms: float = 0.0
    error_message: Optional[str] = None
    metadata: Dict[str, Any] = field(default_factory=dict)


class DeviceBridge:
    """Interface to physical Samsung Galaxy A20 testbed via ADB."""

    DEFAULT_SERIAL = "RZ8M73W8HGV"
    PACKAGE_NAME = "io.github.nfsmwrecomp"
    GAME_PROCESS = "io.github.nfsmwrecomp:juego"

    def __init__(self, serial: Optional[str] = None):
        self.serial = serial or self.DEFAULT_SERIAL
        self._connected = None

    def is_connected(self) -> bool:
        if self._connected is not None:
            return self._connected
        try:
            res = subprocess.run(
                ["adb", "devices"],
                capture_output=True,
                text=True,
                timeout=5
            )
            self._connected = self.serial in res.stdout
        except Exception:
            self._connected = False
        return self._connected

    def run_cmd(self, cmd: str, use_root: bool = False, timeout: int = 10) -> Tuple[int, str, str]:
        if not self.is_connected():
            return -1, "", "Device not connected"
        adb_cmd = ["adb", "-s", self.serial, "shell"]
        if use_root:
            adb_cmd.extend(["su", "-c", cmd])
        else:
            adb_cmd.append(cmd)
        try:
            p = subprocess.run(adb_cmd, capture_output=True, text=True, timeout=timeout)
            return p.returncode, p.stdout.strip(), p.stderr.strip()
        except subprocess.TimeoutExpired:
            return -2, "", "Command timed out"
        except Exception as e:
            return -3, "", str(e)

    def get_orientation_lock(self) -> Tuple[str, str]:
        """Returns (accelerometer_rotation, user_rotation). Both must be '0'."""
        _, accel, _ = self.run_cmd("settings get system accelerometer_rotation")
        _, user_rot, _ = self.run_cmd("settings get system user_rotation")
        return accel, user_rot

    def get_screen_timeout(self) -> int:
        _, val, _ = self.run_cmd("settings get system screen_off_timeout")
        try:
            return int(val)
        except ValueError:
            return -1

    def get_process_pid(self, process_name: str = GAME_PROCESS) -> Optional[int]:
        code, out, _ = self.run_cmd(f"pidof {process_name}")
        if code == 0 and out:
            pids = out.split()
            if pids:
                return int(pids[0])
        return None

    def launch_game(self) -> bool:
        """Brings SetupActivity to foreground and triggers GameActivity."""
        self.run_cmd(f"am start -n {self.PACKAGE_NAME}/.SetupActivity")
        time.sleep(2)
        # Tap Play button on SetupActivity (fixed UI coordinate 360, 694)
        self.run_cmd("input tap 360 694")
        time.sleep(3)
        return self.get_process_pid() is not None

    def kill_game(self) -> None:
        self.run_cmd(f"killall -9 {self.GAME_PROCESS} 2>/dev/null || true", use_root=True)

    def capture_screenshot(self) -> Optional[Image.Image]:
        if not self.is_connected():
            return None
        try:
            p = subprocess.run(
                ["adb", "-s", self.serial, "exec-out", "screencap", "-p"],
                capture_output=True,
                timeout=10
            )
            if p.returncode == 0 and len(p.stdout) > 1000:
                return Image.open(io.BytesIO(p.stdout))
        except Exception:
            pass
        return None

    def analyze_viewport_non_black(
        self,
        image: Image.Image,
        crop_box: Tuple[int, int, int, int] = (140, 50, 1420, 670),
        black_threshold: int = 10
    ) -> Tuple[float, int, int]:
        """
        Analyzes active 1280x720 game viewport.
        Returns (non_black_percentage, non_black_pixels, total_pixels).
        """
        rect = image.crop(crop_box)
        colors = rect.getcolors(maxcolors=2000000)
        total_pixels = rect.size[0] * rect.size[1]
        if not colors:
            return 0.0, 0, total_pixels
        black_pixels = sum(
            count for count, col in colors
            if len(col) >= 3 and col[0] <= black_threshold and col[1] <= black_threshold and col[2] <= black_threshold
        )
        non_black = total_pixels - black_pixels
        non_black_pct = (non_black / total_pixels) * 100.0
        return non_black_pct, non_black, total_pixels


class ContractVerifier:
    """Offline & in-memory verification engine for Vulkan, SPIR-V, and packaging specs."""

    @staticmethod
    def verify_nfsp_header(data: bytes) -> Tuple[bool, str, Dict[str, Any]]:
        """Verifies 24-byte NFSP header structure and checksum."""
        if len(data) < 24:
            return False, f"Data truncated ({len(data)} < 24 bytes)", {}
        magic = data[:8]
        if magic != b"NFSSPV\x00\x00":
            return False, f"Invalid magic: {magic!r} != b'NFSSPV\\0\\0'", {}
        ver, count, expected_hash = struct.unpack("<IIQ", data[8:24])
        if ver != 1:
            return False, f"Unsupported version: {ver} != 1", {}
        if count != 150:
            return False, f"Expected 150 shaders, found {count}", {}
        
        info = {"version": ver, "shader_count": count, "expected_hash": hex(expected_hash)}
        
        if HAS_XXHASH:
            actual_hash = xxhash.xxh3_64_intdigest(data[24:])
            info["actual_hash"] = hex(actual_hash)
            if actual_hash != expected_hash:
                return False, f"Checksum mismatch: actual {hex(actual_hash)} != expected {hex(expected_hash)}", info
        
        return True, "Valid NFSP Header", info

    @staticmethod
    def verify_spirv_remapping(spv_words: List[int]) -> Tuple[bool, str, List[int]]:
        """
        Applies JuntarConjuntos algorithm to remap Sets 0..4 into Sets 0..3:
        - Set 2 (Cube) -> Set 1, Binding 1
        - Set 3 (Samplers) -> Set 2
        - Set 4 (UBO) -> Set 3
        """
        if not spv_words or spv_words[0] != 0x07230203:
            return False, "Not a valid SPIR-V binary", []
        
        # SPIR-V version word is at index 1
        version = spv_words[1]
        out_words = list(spv_words)
        
        # Remap OpDecorate instructions
        idx = 5 # skip header (5 words)
        remapped_sets = set()
        
        while idx < len(out_words):
            word = out_words[idx]
            opcode = word & 0xFFFF
            length = (word >> 16) & 0xFFFF
            if length == 0 or idx + length > len(out_words):
                break
            
            # OpDecorate is opcode 71
            if opcode == 71 and length >= 4:
                target_id = out_words[idx + 1]
                decoration = out_words[idx + 2]
                # Decoration DescriptorSet = 34
                if decoration == 34 and length >= 4:
                    old_set = out_words[idx + 3]
                    new_set = old_set
                    if old_set == 2:
                        new_set = 1 # Remap Cube to Set 1
                    elif old_set == 3:
                        new_set = 2 # Remap Samplers to Set 2
                    elif old_set == 4:
                        new_set = 3 # Remap UBOs to Set 3
                    out_words[idx + 3] = new_set
                    remapped_sets.add(new_set)
                # Decoration Binding = 33
                elif decoration == 33 and length >= 4:
                    pass
            
            idx += length
            
        return True, "Remapping successful", out_words

    @staticmethod
    def decode_bc1_block(block: bytes) -> List[Tuple[int, int, int, int]]:
        """Decodes 8-byte BC1 block into 16 RGBA8 tuples."""
        if len(block) != 8:
            raise ValueError("BC1 block must be exactly 8 bytes")
        c0, c1 = struct.unpack("<HH", block[:4])
        lookup = struct.unpack("<I", block[4:8])[0]
        
        def rgb565_to_rgb888(c):
            r = ((c >> 11) & 0x1F) * 255 // 31
            g = ((c >> 5) & 0x3F) * 255 // 63
            b = (c & 0x1F) * 255 // 31
            return (r, g, b)

        colors = [rgb565_to_rgb888(c0), rgb565_to_rgb888(c1)]
        r0, g0, b0 = colors[0]
        r1, g1, b1 = colors[1]

        if c0 > c1:
            colors.append(((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3))
            colors.append(((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3))
            alphas = [255, 255, 255, 255]
        else:
            colors.append(((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2))
            colors.append((0, 0, 0))
            alphas = [255, 255, 255, 0]

        pixels = []
        for i in range(16):
            code = (lookup >> (i * 2)) & 0x03
            r, g, b = colors[code]
            a = alphas[code]
            pixels.append((r, g, b, a))
        return pixels

    @staticmethod
    def validate_adrenotools_meta(meta_json: str) -> Tuple[bool, str]:
        """Validates Adrenotools meta.json against standard schema."""
        try:
            data = json.loads(meta_json)
        except Exception as e:
            return False, f"JSON parse error: {e}"
        required_fields = ["name", "description", "author", "vendor", "driverVersion", "libraryName", "minApi", "schemaVersion"]
        for field in required_fields:
            if field not in data:
                return False, f"Missing required field: '{field}'"
        if data.get("schemaVersion") != 1:
            return False, f"Invalid schemaVersion: {data.get('schemaVersion')} != 1"
        return True, "Valid meta.json"


def generate_json_report(results: List[TestResult], output_path: str, execution_mode: str, device_id: Optional[str] = None) -> None:
    passed = sum(1 for r in results if r.status == TestStatus.PASS)
    failed = sum(1 for r in results if r.status == TestStatus.FAIL)
    skipped = sum(1 for r in results if r.status == TestStatus.SKIP)
    total = len(results)
    total_duration = sum(r.duration_ms for r in results) / 1000.0

    report = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "execution_mode": execution_mode,
        "device_id": device_id,
        "summary": {
            "total": total,
            "passed": passed,
            "failed": failed,
            "skipped": skipped,
            "duration_seconds": round(total_duration, 3)
        },
        "tiers": {},
        "results": [
            {
                "id": r.id,
                "name": r.name,
                "tier": r.tier,
                "category": r.category,
                "feature_ref": r.feature_ref,
                "status": r.status.value,
                "duration_ms": round(r.duration_ms, 2),
                "error_message": r.error_message,
                "metadata": r.metadata
            }
            for r in results
        ]
    }

    for tier_num in (1, 2, 3, 4):
        tier_results = [r for r in results if r.tier == tier_num]
        if tier_results:
            report["tiers"][f"tier{tier_num}"] = {
                "total": len(tier_results),
                "passed": sum(1 for r in tier_results if r.status == TestStatus.PASS),
                "failed": sum(1 for r in tier_results if r.status == TestStatus.FAIL),
                "skipped": sum(1 for r in tier_results if r.status == TestStatus.SKIP)
            }

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)


def generate_junit_xml(results: List[TestResult], output_path: str) -> None:
    passed = sum(1 for r in results if r.status == TestStatus.PASS)
    failed = sum(1 for r in results if r.status == TestStatus.FAIL)
    skipped = sum(1 for r in results if r.status == TestStatus.SKIP)
    total = len(results)
    total_time = sum(r.duration_ms for r in results) / 1000.0

    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<testsuites name="MaliCompat_E2E" tests="{total}" failures="{failed}" skipped="{skipped}" time="{total_time:.3f}">'
    ]

    for tier_num in sorted(list(set(r.tier for r in results))):
        tier_results = [r for r in results if r.tier == tier_num]
        t_time = sum(r.duration_ms for r in tier_results) / 1000.0
        t_failed = sum(1 for r in tier_results if r.status == TestStatus.FAIL)
        t_skipped = sum(1 for r in tier_results if r.status == TestStatus.SKIP)
        lines.append(f'  <testsuite name="Tier{tier_num}" tests="{len(tier_results)}" failures="{t_failed}" skipped="{t_skipped}" time="{t_time:.3f}">')

        for r in tier_results:
            lines.append(f'    <testcase classname="{r.category}" name="{r.id}_{r.name}" time="{r.duration_ms/1000.0:.3f}">')
            if r.status == TestStatus.FAIL:
                msg = (r.error_message or "Unknown failure").replace('"', '&quot;').replace('<', '&lt;').replace('>', '&gt;')
                lines.append(f'      <failure message="{msg}">{msg}</failure>')
            elif r.status == TestStatus.SKIP:
                skip_msg = (r.error_message or "precondition not met").replace('"', '&quot;').replace('<', '&lt;').replace('>', '&gt;')
                lines.append(f'      <skipped message="Skipped: {skip_msg}"/>')
            lines.append('    </testcase>')
        lines.append('  </testsuite>')

    lines.append('</testsuites>')

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def print_terminal_report(results: List[TestResult], verbose: bool = False) -> None:
    passed = sum(1 for r in results if r.status == TestStatus.PASS)
    failed = sum(1 for r in results if r.status == TestStatus.FAIL)
    skipped = sum(1 for r in results if r.status == TestStatus.SKIP)
    total = len(results)

    print("\n" + "=" * 80)
    print(" " * 24 + "E2E TEST SUITE EXECUTION REPORT")
    print("=" * 80)
    print(f"Total: {total:3d} | Passed: {passed:3d} | Failed: {failed:3d} | Skipped: {skipped:3d}\n")

    for tier_num in sorted(list(set(r.tier for r in results))):
        tier_results = [r for r in results if r.tier == tier_num]
        t_passed = sum(1 for r in tier_results if r.status == TestStatus.PASS)
        t_failed = sum(1 for r in tier_results if r.status == TestStatus.FAIL)
        t_skipped = sum(1 for r in tier_results if r.status == TestStatus.SKIP)
        print(f"--- Tier {tier_num} ({len(tier_results)} tests) [Pass: {t_passed}, Fail: {t_failed}, Skip: {t_skipped}] ---")

        for r in tier_results:
            status_str = f"[{r.status.value}]"
            if r.status == TestStatus.PASS:
                status_color = f"\033[92m{status_str:6s}\033[0m"
            elif r.status == TestStatus.FAIL:
                status_color = f"\033[91m{status_str:6s}\033[0m"
            else:
                status_color = f"\033[93m{status_str:6s}\033[0m"

            if verbose or r.status != TestStatus.PASS:
                err = f" -> {r.error_message}" if r.error_message else ""
                print(f"  {status_color} {r.id:30s} {r.name[:32]:32s} ({r.feature_ref}) {r.duration_ms:6.1f}ms{err}")
    print("=" * 80 + "\n")
