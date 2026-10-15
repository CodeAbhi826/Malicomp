#!/usr/bin/env python3
"""
patch12 - flip VulkanProvider::Create's with_gpu_emulation from false to true.

EXPERIMENT ONLY. This is not proposed as a fix.

Where it sits
-------------
nfsmw's SistemaGraficoNativo::SetupPresentation (app/src/nfsmw_nativo_sistema.cpp)
does:

    #if REX_PLATFORM_ANDROID
        provider_ = rex::ui::vulkan::VulkanProvider::Create(false, true);
    #else
        provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
    #endif

with the comment "Native shaders read their own buffers. They do not use Xenos
memory export / EDRAM emulation, which requires vertex stores and atomics."

In libmain.so that lands on a single call site:

    0x235AC28  adrp x0, #0x8f1000
    0x235AC2C  add  x0, x0, #0x7d4   ; "vulkan_native_shader_features"
    0x235AC30  adrp x2, #0x8ee000
    0x235AC34  add  x2, x2, #0x2d3   ; the literal "true"
    0x235AC38  mov  w1, #0x1d
    0x235AC3C  mov  w3, #4
    0x235AC40  bl  0x24b49f0        ; rex::cvar::SetFlagByName(name, "true")
    0x235AC48  mov  w0, wzr          ; with_gpu_emulation = 0   <-- THIS BYTE
    0x235AC4C  mov  w1, #1           ; with_presentation  = 1
    0x235AC50  bl  0x24b53b0        ; VulkanProvider::Create

The first LOAD segment is Offset 0 / VirtAddr 0, so file offset == vaddr.

    0x235AC48  0x2A1F03E0  ORR w0, wzr, wzr   -> 0x52800020  MOVZ w0, #1

The MOVZ encoding is self-validated by the adjacent instruction: 0x235AC4C holds
0x52800021, which is exactly MOVZ w1, #1, i.e. 0x52800000 | (1 << 5) | Rd.

Prerequisites this needs to even reach the vendor
--------------------------------------------------
With with_gpu_emulation = true, XE_UI_VULKAN_FEATURE copies supported_features
wholesale into enabled_features, so every bit the layer spoofs in
vkGetPhysicalDeviceFeatures comes back as a hard vkCreateDevice request.

vulkan_device.cpp:114 and :121 then become unconditional:

    if (!supported_features.fragmentStoresAndAtomics)   return nullptr;
    if (!supported_features.vertexPipelineStoresAndAtomics) return nullptr;

Measured on this device (M18 log): fragmentStoresAndAtomics = 1,
vertexPipelineStoresAndAtomics = 0. So the run needs BOTH:

  - vertexPipelineStoresAndAtomics spoofed VK_TRUE in the layer, and
  - the M24 pEnabledFeatures sanitiser, which strips that bit back out before the
    real driver sees it.

vulkan_require_geometry_shader and vulkan_require_fill_mode_non_solid are already
passed false on our command line, so those two gates are skipped either way.

What this will NOT fix
----------------------
Cause 53 is the guest's bindless texture heaps, built in
DibujosVulkan::CrearDescriptores:

    enlaces[0].descriptorCount = kCapacidadMonton[i];   // compile-time

observed as SAMPLED_IMAGE x 4096 and SAMPLER x 512 against driver limits of 256
and 128. Those constants are independent of with_gpu_emulation, so the heaps stay
illegal and the geometry pipelines should keep returning
VK_ERROR_INITIALIZATION_FAILED. The experiment is worth running anyway to confirm
that, and to see whether anything else changes.
"""

import hashlib
import struct
import sys
from pathlib import Path

VADDR = 0x235AC48
OLD = 0x2A1F03E0  # ORR w0, wzr, wzr
NEW = 0x52800020  # MOVZ w0, #1


def main() -> int:
    src = Path("libmain_usa.so")
    out = Path("libmain_usa_gpueemu.so")
    if not src.exists():
        print(f"error: {src} not found (run from the Malicomp repo root)", file=sys.stderr)
        return 1

    data = bytearray(src.read_bytes())
    found = struct.unpack_from("<I", data, VADDR)[0]
    if found != OLD:
        print(f"error: expected 0x{OLD:08X} at 0x{VADDR:X}, found 0x{found:08X}", file=sys.stderr)
        return 1

    # Sanity: the next two instructions must be the ones we believe they are.
    if struct.unpack_from("<I", data, VADDR + 4)[0] != 0x52800021:
        print("error: 0x235AC4C is not MOVZ w1, #1", file=sys.stderr)
        return 1
    if struct.unpack_from("<I", data, VADDR + 8)[0] & 0xFC000000 != 0x94000000:
        print("error: 0x235AC50 is not a BL", file=sys.stderr)
        return 1

    struct.pack_into("<I", data, VADDR, NEW)
    out.write_bytes(data)

    changed = [i for i in range(len(data)) if data[i] != src.read_bytes()[i]]
    if changed != list(range(VADDR, VADDR + 4)):
        print(f"error: unexpected extra changes: {['0x%X' % i for i in changed[:8]]}",
              file=sys.stderr)
        return 1

    print("patch12: with_gpu_emulation false -> true")
    print("  0x%X  0x%08X (ORR w0,wzr,wzr) -> 0x%08X (MOVZ w0, #1)"
          % (VADDR, OLD, NEW))
    print("  total diff vs stock: %d bytes" % len(changed))
    print("  md5 %s" % hashlib.md5(data).hexdigest())
    print("  stock md5 %s" % hashlib.md5(src.read_bytes()).hexdigest())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())