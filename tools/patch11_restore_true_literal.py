#!/usr/bin/env python3
"""
patch11 - restore the "true" string literal that patch4 destroyed.

Background
----------
nfsmw-android's guest enables the native-shader feature path with:

    rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");

Disassembled in libmain.so (identity-mapped, file offset == vaddr):

    0x235AC28  adrp x0, #0x8f1000
    0x235AC2C  add  x0, x0, #0x7d4     ; x0 = 0x8F17D4 "vulkan_native_shader_features"
    0x235AC30  adrp x2, #0x8ee000
    0x235AC34  add  x2, x2, #0x2d3     ; x2 = 0x8EE2D3   <-- the literal "true"
    0x235AC38  mov  w1, #0x1d          ; 29 == strlen("vulkan_native_shader_features")
    0x235AC3C  mov  w3, #4             ; 4  == strlen("true")
    0x235AC40  bl   0x24b49f0          ; rex::cvar::SetFlagByName  (return value DISCARDED)
    0x235AC48  mov  w0, wzr            ; with_gpu_emulation = 0
    0x235AC4C  mov  w1, #1             ; with_presentation  = 1
    0x235AC50  bl   0x24b53b0          ; rex::ui::vulkan::VulkanProvider::Create

patch4 wanted to change the `game_root` cvar default from the placeholder
"game_root" to "/sdcard/nsfmw-androidevolved", and wrote 29 bytes starting at
0x8EE2C9. That region is:

    stock : "game_root\0true\0Registrar las llamadas al Direct3D..."
    patched: "/sdcard/nsfmw-androidevolved\0llamadas al Direct3D..."

so the write clobbered the "true" literal sitting immediately after the NUL.
Since then SetFlagByName has been receiving "fmw-" instead of "true", failing
silently.

Consequence in librexruntime.so VulkanDevice::CreateIfSupported:

    0x433160  ldrb w8, [sp+0x11C4]    ; with_gpu_emulation
    0x433164  cbz   w8, #0x433320      ; false -> 0x433320
    0x433168  ldrb w8, [x20, #0xAB0]   ; vulkan_native_shader_features
    0x43316C  cbz   w8, #0x4335E0      ; false -> block C
    0x433320  ldrb w8, [x20, #0xAB0]   ; (re-checked)
    0x433324  cbz   w8, #0x4335E0
             ; block B writes ALL SIX capabilities the guest requires:
             ;   +0x1DA @0x4333A8  +0x1DC @0x433180  +0x1DD @0x4331EC
             ;   +0x1DE @0x433258  +0x1DF @0x4332C4  +0x1CA @0x4334AC

With the cvar stuck false, block B never runs, VulkanDevice::properties_ keeps
its `= false` defaults, and nfsmw_nativo_dibujos.cpp DibujosVulkan::Inicializar
bails on the first entry:

    [nativo] C6: el dispositivo Vulkan no tiene independentBlend: no se dibuja

This is the intended design, not a bug upstream: `vulkan_native_shader_features`
is exactly the switch that populates the shader feature block without turning on
Xenos GPU emulation. Block B never touches pEnabledFeatures, so no
vkCreateDevice sanitiser is required either.

Fix
---
Restore bytes 0x8EE2C9..0x8EE2E5 (29 B) to their stock contents, which brings
back both the "game_root" placeholder and the "true" literal. The game root is
supplied explicitly on the command line (--game_data_root=content://...) so the
placeholder default is never used.

Every other patch site from patch4..patch10 is left untouched.
"""

import hashlib
import sys
from pathlib import Path

VADDR = 0x8EE2C9
LENGTH = 29


def main() -> int:
    stock_path = Path("libmain_usa.so")
    prev_path = Path("libmain_usa_patched10.so")
    out_path = Path("libmain_usa_patched11.so")

    for p in (stock_path, prev_path):
        if not p.exists():
            print(f"error: {p} not found (run from the Malicomp repo root)", file=sys.stderr)
            return 1

    stock = bytearray(stock_path.read_bytes())
    prev = bytearray(prev_path.read_bytes())
    out = bytearray(prev)

    if len(stock) != len(prev):
        print("error: stock and patched10 differ in size", file=sys.stderr)
        return 1

    # Sanity: the byte we are about to restore must really be the literal.
    literal = bytes(stock[VADDR + 10 : VADDR + 14])
    if literal != b"true":
        print(f"error: expected 'true' at 0x{VADDR + 10:X}, found {literal!r}", file=sys.stderr)
        return 1
    if bytes(prev[VADDR + 10 : VADDR + 14]) == b"true":
        print("error: patched10 already has the literal; nothing to do", file=sys.stderr)
        return 1

    out[VADDR : VADDR + LENGTH] = stock[VADDR : VADDR + LENGTH]

    # Verify the patch site itself was untouched.
    for va in (0x235AC28, 0x235AC4C, 0x235AC50):
        if out[va : va + 4] != prev[va : va + 4]:
            print(f"error: call site 0x{va:X} moved unexpectedly", file=sys.stderr)
            return 1

    # Verify every changed byte is inside the intended window and that every
    # byte of the window that stock and patched10 disagreed on is now fixed.
    changed = [i for i in range(len(out)) if out[i] != prev[i]]
    stray = [i for i in changed if not (VADDR <= i < VADDR + LENGTH)]
    if stray:
        print(f"error: changed bytes outside the window: {['0x%X' % i for i in stray[:8]]}",
              file=sys.stderr)
        return 1
    stale = [i for i in range(VADDR, VADDR + LENGTH) if out[i] != stock[i]]
    if stale:
        print(f"error: window not fully restored: {['0x%X' % i for i in stale[:8]]}",
              file=sys.stderr)
        return 1

    out_path.write_bytes(out)

    print("patch11: restored 29 bytes at 0x%X" % VADDR)
    print("  stock    0x%X: %r" % (VADDR, bytes(stock[VADDR : VADDR + LENGTH])))
    print("  patched10 0x%X: %r" % (VADDR, bytes(prev[VADDR : VADDR + LENGTH])))
    print("  patched11 0x%X: %r" % (VADDR, bytes(out[VADDR : VADDR + LENGTH])))
    print("  'true' literal at 0x%X: %s" % (VADDR + 10, bytes(out[VADDR + 10 : VADDR + 14])))
    print("  md5 %s" % hashlib.md5(out).hexdigest())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())