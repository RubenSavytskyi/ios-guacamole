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

# Only the gate branch is removed; the read of 0x64 is left in place.  Mainline
# does not clear FIFO_IF_DISABLE in geni_se_select_fifo_mode() either (it only
# clears GENI_DMA_MODE_EN, which this driver already does at 0x258), so this
# variant checks whether the SE works with the driver's own initialisation.
PATCHES_NOP_ONLY = [
    # nop                      -- drop the "FIFO disabled" branch
    (0xFFFFFE00074FB5F0, 0x37000C68, 0xD503201F),
]

PATCHES = [
    # str wzr, [x8, #0x2008]  -- clear FIFO_IF_DISABLE
    (0xFFFFFE00074FB5E0, 0xB9406508, 0xB920091F),
    # nop                      -- drop the "FIFO disabled" branch
    (0xFFFFFE00074FB5F0, 0x37000C68, 0xD503201F),
]

# Gate removed, and the FIFO_IF_DISABLE clear moved to the very end of
# touch_se_init(): by then the driver has cleared GENI_DMA_MODE_EN (0x258),
# GSI event/IRQ enables (0xe18/0xe1c/0x644) and programmed the SE serial
# clock (SE_CLK_SEL 0x7c, MCLK 0x48, SCL 0x278), so page 0x2000 is accessed in
# the same order mainline Linux uses.  The store replaces the `dsb osh` that
# follows the last SE write of the init sequence (0x614); x8 holds the SE base
# there and the `dmb oshst` before 0x614 already orders the stores.
PATCHES_LATE = [
    # nop                      -- drop the "FIFO disabled" branch
    (0xFFFFFE00074FB5F0, 0x37000C68, 0xD503201F),
    # str wzr, [x8, #0x2008]  -- clear FIFO_IF_DISABLE after SE setup
    (0xFFFFFE00074FB988, 0xD503339F, 0xB920091F),
]

# Diagnostic only: the "ios=" field of the SE print reads 0x908; point it at
# SE_FIFO_IF_DISABLE (0x2008) instead and leave the gate active, so the driver
# does nothing else.  If the phone survives and the log shows "ios=0x...", page
# 0x2000 is reachable and the earlier black screens were not a bus fault.  If
# the phone dies on this read too, that page has no register behind it on
# SM8150 and FIFO mode cannot be switched on from the AP at all.
PATCHES_READ_PAGE = [
    # ldr w10, [x10, #0x2008]  -- read FIFO_IF_DISABLE instead of 0x908
    (0xFFFFFE00074FB5B4, 0xB949094A, 0xB960094A),
]

def main():
    nop_only = "--nop-only" in sys.argv[1:]
    late = "--late" in sys.argv[1:]
    read_page = "--read-page" in sys.argv[1:]
    if nop_only:
        dst = DST.replace(".sweet-touchfix", ".sweet-touchopen")
        patches = PATCHES_NOP_ONLY
    elif late:
        dst = DST.replace(".sweet-touchfix", ".sweet-touchlate")
        patches = PATCHES_LATE
    elif read_page:
        dst = DST.replace(".sweet-touchfix", ".sweet-touchread")
        patches = PATCHES_READ_PAGE
    else:
        dst = DST
        patches = PATCHES
    shutil.copyfile(SRC, dst)
    data = bytearray(open(dst, "rb").read())
    for va, old, new in patches:
        off = foff(va)
        cur = struct.unpack_from("<I", data, off)[0]
        if cur != old:
            sys.exit(f"0x{va:016x}: expected 0x{old:08x}, found 0x{cur:08x}")
        struct.pack_into("<I", data, off, new)
        print(f"0x{va:016x} (file 0x{off:06x}): 0x{old:08x} -> 0x{new:08x}")
    open(dst, "wb").write(data)
    print("wrote", dst)

if __name__ == "__main__":
    main()
