#!/usr/bin/env python3
"""Patch the CI XNU kernel for guacamole touch bring-up.

In touch_thread (inlined touch_se_init) the SE gate reads GENI_IF_DISABLE_RO
(0x64) and refuses to probe, because ABL's I2C firmware left the SE's FIFO
interface disabled.  Mainline Linux clears FIFO_IF_DISABLE (bit 0 of
SE_FIFO_IF_DISABLE, 0x2008) before using the FIFO.

The gate's instructions are:

    b5dc: ldr  x8, [x19, #0x6f8]   ; x8 = st.se (SE base)
    b5e0: ldr  w8, [x8, #0x64]     ; read the read-only mirror
    b5e4: dmb  oshld
    b5e8: adrp x0, <fail string>   ; prepared for the failure print
    b5ec: add  x0, x0, #0xc40
    b5f0: tbnz w8, #0, <fail>

so the read can become the write we need, and the branch becomes a nop.  w8 is
overwritten by "and w8, w21, #0x3f000000" just after, so nothing else changes
and none of the driver's own register writes are lost.
"""
import shutil, struct, sys

SRC = "xnu/out-53/kernel.development.sweet"
DST = "xnu/out-53/kernel.development.sweet-touchfix"

TEXT_EXEC_VA = 0xFFFFFE00072F4000
TEXT_EXEC_OFF = 0x2F0000

def foff(va):
    return TEXT_EXEC_OFF + (va - TEXT_EXEC_VA)

PATCHES = [
    # str wzr, [x8, #0x2008]  -- clear FIFO_IF_DISABLE
    (0xFFFFFE00074FB5E0, 0xB9406508, 0xB920091F),
    # nop                      -- drop the "FIFO disabled" branch
    (0xFFFFFE00074FB5F0, 0x37000C68, 0xD503201F),
]

def main():
    shutil.copyfile(SRC, DST)
    data = bytearray(open(DST, "rb").read())
    for va, old, new in PATCHES:
        off = foff(va)
        cur = struct.unpack_from("<I", data, off)[0]
        if cur != old:
            sys.exit(f"0x{va:016x}: expected 0x{old:08x}, found 0x{cur:08x}")
        struct.pack_into("<I", data, off, new)
        print(f"0x{va:016x} (file 0x{off:06x}): 0x{old:08x} -> 0x{new:08x}")
    open(DST, "wb").write(data)
    print("wrote", DST)

if __name__ == "__main__":
    main()
