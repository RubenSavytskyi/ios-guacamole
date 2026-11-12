# Active handoff — iOS 16.7.16 boots to the lock screen and touch works (2026-11-12, opencode)

### >>> UPDATE 2026-11-12 (latest): TOUCH WORKS — patch 0257 (run 43203803753, image bb12) <<<

- **Result: touch is alive on the lock screen.** Image `shim/out/ios16-boot-bb12.img`
  (sha256 `ec2ad6f7…`) packs `xnu/out-0257/kernel.development.sweet` (sha256 `5a3c0f41…`) from CI run
  **`43203803753`** (`4cec248`, `macos-15-intel`, success in ~14 min).
- What changed vs 0157 (all in `xnu/patches/0257-guacamole-touch-sense-order.patch`): clock-stretch
  limit 2 ms → 100 ms, `TOUCH_FUNCTION`/`SENSE_ON` written **before** the `0x21` firmware-integrity
  read (Linux order), writes inside the 3-attempt retry loop, bus recovery after a failed `0x21`.
  Observation from 0157: `err=0/0/0/0/5` with `touch-function=5 sense-on=5` — the 2 ms limit aborted
  the `0x21` read mid-byte and poisoned the bus. Most likely the longer stretch limit is what fixed
  it, with the reordering as the second half; not yet isolated by a separate build.
- Kept for the record: earlier run `43198941064` (`61019f6`) proved the bit-bang path talked to the IC
  (`boot=0x20 devid=ac3761 panel 1440x3120`) but never enabled sensing.
- Next, if wanted: pack a clean-UI image (no console) from `xnu/out-0257` for daily use
  (`bash %TEMP%\opencode\build-bb.sh out-0257 <name>` currently hard-codes the visible console and
  `sweettouchdelay=10`), and confirm multi-touch plus a couple of lock/unlock cycles.

### Build log: 0157 bit-bang brought the IC back on the bus; 0257 finished it

- CI build `43198941064` (`61019f6`, `macos-15-intel`) succeeded in ~17 min →
  `xnu/out-0157/kernel.development.sweet` (sha256 `356982d1…`); boot image
  `shim/out/ios16-boot-bb10.img` (sha256 `1b229aa4…`, console visible, `sweettouchdelay=10`).
- Boot result: **no hang**, reached the lock screen, touch dead. Ring via `logdump.img`:
  `bit-bang 100 kHz timebase=19200000 half=96 ticks`, `line test sda pd/pu=1/1 scl pd/pu=1/1`,
  every read `bb xfer … err=0`, `first event: 09 00 10 00 00 00 00 00`, `boot=0x20`, `devid=ac3761`,
  panel `max x=1440 y=3120 tx=17`. So the IC does answer on GPIO55/56 and the QUP is fully bypassed.
  Failure: `s6sy761 attempt=1 err=0/0/0/0/5` — the `0x21` (firmware integrity) read aborted and the
  poisoned bus then made `touch-function=5` / `sense-on=5` fail, so sensing was never enabled.
- **Patch 0257** (`xnu/patches/0257-guacamole-touch-sense-order.patch`, generated the same way from the
  recon tree, so 0157 stays untouched): clock-stretch limit 2 ms → **100 ms** plus a
  `bb scl stuck (%u) sda=… cfg=…/…` diagnostic (a firmware-integrity read can hold SCL far longer than
  2 ms; aborting mid-byte is what poisoned the bus); Linux order restored — reads →
  `TOUCH_FUNCTION` word write → `SENSE_ON` → `FIRMWARE_INTEGRITY` read last (best effort); the two
  writes moved inside the 3-attempt retry loop and it only succeeds when `sense-on` returns 0; after a
  failed `0x21` read a 9-clock bus recovery runs before polling. Attempt line now prints
  `func=%d sense=%d`.

### CI through a fork + the 0157 bit-bang driver

- CI unlocked without a local macOS: fork **`RubenSavytskyi/ios-guacamole`** (public, Actions enabled via
  API), branch `guacamole-touch` force-pushed to the fork's `main` and set as its default branch.
  GitHub retired `macos-14` on 2026-11-02 (jobs are cancelled at queue time with
  `The configuration 'macos-14-arm64-us-default' is not supported`, 0 steps, conclusion `cancelled`),
  so the workflow is now `runs-on: macos-15-intel` (plain `macos-15` is arm64 and never acquired a
  runner: `The job was not acquired by Runner of type hosted`, run `43196064388`) and also copies
  `xnu/local/*.patch` into `dxb/patches/`
  (`0001-iorpc-kernelcontent.patch`: Xcode 16+ `iig` emits `IORPC.kernelContent`).
  Push with Windows git (Credential Manager, account `RubenSavytskyi`):
  `git -C "<repo>" push https://github.com/RubenSavytskyi/ios-guacamole.git guacamole-touch` and
  `… push --force … guacamole-touch:main` (a push to `main` is what starts the build). API helpers:
  `%TEMP%\opencode\gh-whoami.ps1`, `gh-fork.ps1`, `gh-dispatch.ps1`, `gh-runs.ps1`, `gh-jobdiag.ps1`.
  The workflow has `concurrency: xnu-${{ github.ref }}` with `cancel-in-progress: true`, so each new
  push to `main` cancels the build already running.
- Builds: baseline of the tip (`5f3cea7`, macos-15 fix) reached the **Build** step and was then
  cancelled by the next push (run `43194865341`); the bit-bang build is run **`43196064388`** on
  `6567cf1`.
- **New approach (patch 0157) — bit-bang I2C in the driver, QUP untouched.** `bsd/dev/sweet_touch.c`
  (`xnu/patches/0157-guacamole-touch-bitbang.patch`, generated from the fully patched file kept in
  `C:\Users\ruvim\AppData\Local\Temp\opencode\recon`). Default `sweetbb=1`; `sweetbb=0` keeps the old
  SE path. SDA/SCL = GPIO55/56, mux 0, pull-up, open-drain (drive low = output enable + latch 0,
  release = input; never driven high). Timing from `mach_absolute_time()` with the tick rate derived
  from `absolutetime_to_nanoseconds()` — no GCC clock votes and no SE access at all. `sweetbbkhz`
  (default 100) sets the bus clock. The read path keeps the Linux STOP_STRETCH shape: pointer write
  leaves SCL low, the read continues with a repeated START. Probes are retried 3x with a reset.
  On-screen diagnostics: pad cfg/io dump, `line test sda pd/pu=… scl pd/pu=…` (do the pads move, is
  the bus held low), a 9-clock bus clear, idle levels, one line per transfer (`bb xfer … err=`), and
  `s6sy761 attempt=… err=… boot=…`.
- Footnote: XNU's `delay(int)` takes **microseconds** (`delay_for_interval(usec, NSEC_PER_USEC)`), so
  the SE driver's `delay(1)` poll loops are 1 µs steps, not 1 ms.
- Pending: `shim/out/ios16-boot-touchread.img` (the `0x2008` read probe) was booted; its result has
  not been observed yet.

- State: the iOS 16.7.16 root (12 GiB HFS+, sparse `../guac-artifacts/ios16-root.simg`, sha256 `884f154b…`)
  is flashed to `userdata` (`/dev/block/sda19`) and boots to the lock screen on the phone (GM1911,
  serial 0ea51515, bootloader unlocked, slot b). Nothing but userdata was flashed; every test uses
  `fastboot boot`.
- Useful boot images (all `sweetusb=0`, the only value known to boot):
  - `shim/out/ios16-boot-clean.img` — clean UI (`sweetfb_noconsole=1`), touch driver off.
  - `shim/out/ios16-boot-touch10.img` — console visible, `sweettouchdelay=10`; boots fine, touch dead.
  - `shim/out/ios16-boot-touchkern.img` — clean UI + the first patched kernel (touch fix).
  - `shim/out/logdump.img` — shim only (no XNU payload): decodes the SWEETLOG ring at the end of
    `userdata` and prints the last `SWEET TOUCH`/`SWEET RPMH` lines of the previous boot on screen.
  - `shim/out/ios16-boot-touchopen10.img` — gate branch removed only (`--nop-only`); **boots**, touch
    dead. Log: the driver runs to the end of `touch_se_init()` and votes RPMh L17A, then every probe
    transfer to `0x48` times out (`err=60`, `tx=1 rx=0 status=0`, five attempts printed).
  - `shim/out/ios16-boot-touchlate10.img` / `…late45.img` — gate removed + `0x2008` write moved to the
    end of the SE init (`--late`); **hard hang**: screen goes black ~1-2 s in (late10) or after the
    lock screen (late45), no `panic(cpu …)` text, no panic line in the ring.
  - `shim/out/ios16-boot-touchread.img` (sha256 `87d5fe00…`) — diagnostic `--read-page`: only the
    `ios=` field of the SE print reads `0x2008` (`ldr w10,[x10,#0x2008]` at `0xfffffe00074fb5b4`, gate
    stays active, nothing else changes); built, not booted yet.
- Touch root cause (from the ring): `sweet_touch.c` stops in `touch_se_init()`:
  `SWEET TOUCH: se fw=0x303 proto=3 status=0 tx=0x20102964 rx=0x20104900 ios=0`, then
  `need ABL-loaded I2C firmware and 32-bit FIFOs; probes skipped`. Every gate term passes except
  `touch_rd(st.se, 0x64) & 1`: `0x64` is `GENI_IF_DISABLE_RO` and bit 0 is `FIFO_IF_DISABLE` — ABL left
  the SE configured for DMA/GSI (downstream `i2c-qcom-geni.c` reads the same bit and, when set, uses GSI
  mode only; it never switches back to FIFO).
- With the gate removed the driver's own init is not enough: `touch_se_init()` completes (packing
  `0x7f8fe/0xffcfe`), the RPMh `ldoa17` vote completes, then every probe transfer to `0x48` times out
  (`err=60`, `tx=1 rx=0 status=0`) — the SE does not execute FIFO commands.
- Why the `0x2008` write cannot be the answer on this SoC (observation + source reading): the
  msm-4.14/LineageOS SM8150 GENI sources (`include/linux/qcom-geni-se.h`,
  `drivers/tty/serial/msm_geni_serial.c`, `drivers/i2c/busses/i2c-qcom-geni.c`) define only
  `GENI_CLK_CTRL_RO 0x60` and `GENI_IF_FIFO_DISABLE_RO 0x64`; there is no `0x2000/0x2004/0x2008`
  register anywhere. Those three are the GENI **v2** map in mainline (`SE_GENI_FW_REVISION 0x1000`,
  `SE_GENI_CFG_RAMN 0x1010`, `SE_GENI_CLK_CTRL 0x2000`, `SE_DMA_IF_EN 0x2004`,
  `SE_FIFO_IF_DISABLE 0x2008`), used only by SoCs where Linux itself loads the SE firmware
  (`geni_load_se_firmware()` with a `firmware-name` DT property). SM8150 is GENI v1: the mode is fixed
  by the firmware the boot chain loads, and the AP has no writable mirror. A store to `0x2008` is a
  store into an address with no register behind it → the NoC never answers, the SoC hangs/resets
  (black screen, no Swift panic), which matches every observed run. `--read-page` is the pending
  confirmation (a read of the same address either returns a value or hangs the same way).
- Consequence: FIFO mode cannot be switched on from the AP on this phone. Realistic paths are
  (a) boot-chain firmware that leaves SE17 in FIFO mode (older OOS/qupfw — coreycusick's phone does;
  risky, touches firmware partitions), (b) driver work in source — GSI mode port, or the much simpler
  GPIO bit-bang I2C on SDA/SCL 55/56 — which needs a kernel build. The CI workflow
  (`.github/workflows/xnu.yml`) is self-contained (`workflow_dispatch` + `macos-14`), so a public fork
  of the repo could build and download a kernel without local macOS.
- Why it works for coreycusick: same branch, same kernel source (our checkout is the branch tip
  `b94a1e7` + local root commits). On his phone `0x64` bit 0 reads 0, i.e. his boot chain leaves SE17
  in FIFO mode; ours (OOS 12 firmware/ABL) leaves it in DMA/GSI mode. It is a bootloader-state
  difference, not a driver difference.
- Shim workaround: dead end, do not retry. Poking `0xc80000` from the shim resets the SoC (900E
  crash-dump, or straight back to fastboot). Voting `gcc 0x52014 |= 0x1f` and reading `0x68` work, but
  the first access to `0x64`/`0x2008` still kills the machine, including after enabling the RCG root
  (`0x1e148 |= 1`). `shim/shim.c` therefore has no SE access any more.
- Fix attempted in the kernel binary (`tools/patch-touch-kernel.py`, variants `--nop-only`, `--late`,
  `--read-page`, default): inside `_touch_thread` (where `touch_se_init` is inlined) the gate is
  `0xfffffe00074fb5e0 ldr w8,[x8,#0x64]` / `0xfffffe00074fb5f0 tbnz w8,#0,<fail>`; `w8` is overwritten
  by `and w8, w21, #0x3f000000` immediately after. `--late` writes `str wzr,[x8,#0x2008]` over the
  `dsb osh` at `0xfffffe00074fb988`, i.e. after `0x258` (DMA mode off), `0xe18`/`0xe1c`/`0x644` and
  `0x7c`/`0x48`/`0x278`. `--read-page` turns the `ios=` read at `0xfffffe00074fb5b4`
  (`ldr w10,[x10,#0x908]`) into `ldr w10,[x10,#0x2008]` and changes nothing else. Rebuild with
  `CPUS=8 TOUCH=1 KEYS=1 USB=0 tools/guac-mkboot.sh <patched kernel> <stock boot_b.img> shim/out/Image out.img "<args>"`.
  To revert, copy `kernel.development.sweet` back over the variant name.
- Device recovery after a reset: hold Power ~15-20 s, then Power+Volume Down for fastboot.
- Push/CI: WSL git has no credentials; Windows git has `RubenSavytskyi` in Credential Manager. That
  account has no write access to `coreycusick/ios-guacamole` (403), but the fork above is ours, its
  Actions build the kernel, and the `xnu-kernel` artifact is public — see the update block at the top.

# Active handoff — clean install of iOS 16 root on a second OnePlus 7 Pro (2026-11-11 22:30, opencode)

- Goal: reproduce the iOS 16.7.16 root on another guacamole (GM1911, serial 0ea51515, slot b, Android 16
  LineageOS 23.2 on OOS 12 vendor). Nothing flashed yet; `userdata` untouched (227 GB f2fs, LineageOS live).
- Done on this machine (Windows + WSL2 Ubuntu):
  - WSL2 kernel 6.6.87.2 rebuilt with `CONFIG_HFSPLUS_FS=y` (Microsoft's kernel has no HFS+ module):
    `~/wslk`, binary `C:\Users\ruvim\wsl-kernel\bzImage`, selected by `C:\Users\ruvim\.wslconfig` (delete that
    file to go back). Verified: `mount -t hfsplus -o loop,force` read-write keeps 0:0, setuid and symlinks.
  - `apfs-fuse` (sgan81) built from source into `~/apfs-fuse/build/apfs-fuse`, symlinked
    `/usr/local/bin/apfs-fuse` (Ubuntu has no package; libfsapfs 20201107 is only a fallback: it may lack
    LZBITMAP). `mkfs.hfsplus`, `fsapfsmount`, `dmg2img`, `img2simg`, `lzfse` installed.
  - tools/mkiosroot.sh: the APFS side now has two backends (apfs-fuse when present, else fsapfsmount +
    dmg2img for the UDIF OS image) and `vol_root()` finds the volume directory instead of hardcoding
    `os/root`. apfs-fuse path unchanged in behaviour, not yet run end to end.
  - IPSW: the user downloaded the complete `iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw`
    (6 171 035 648 B, 91 entries) into `../guac-artifacts/`; the partial file and `.aria2` were deleted.
    The 3 DMGs needed are OS `087-86698-044.dmg`, Cryptex1,SystemOS `087-86593-047.dmg`,
    Cryptex1,AppOS `087-86715-047.dmg`, extracted into `../guac-artifacts/ipsw-dmg/` (4.73 GB / 2.74 GB / 10 MB).
    The OS image is UDIF (`koly` trailer), the two cryptexes are raw APFS (`NXSB` at offset 32).
    apfs-fuse reads all three; OS volume reports 16.7.16 / 20H392, `usr/lib/dyld` 922 912 B (LZBITMAP OK).
    Logical sizes: os 7.72 GiB / 179 359 files, SystemOS 3.13 GiB / 1 457 files, AppOS 7.9 MB.
  - Stage-2 boot image `shim/out/ios16-boot.img` (26 132 480 B, sha256 5785aad2…):
    `CPUS=8 TOUCH=1 KEYS=1 USB=2` + full iOS boot-args
    (`rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1
    launchd_unsecure_cache=1 sweetfb_hz=30 sweetfb_noconsole=1`), ADT `guac-hi.adt` identical to
    `guac-final.adt` (105 904 B, GPU firmware + haptics). Live console over USB via `tools/sweet-usbnet.sh`.
- Network lesson (this machine, 2026-11-11): from WSL the Apple CDN crawls (10-300 KB/s, 25-40% loss to
  1.1.1.1); the same URL via Windows `curl.exe --http1.1 -r` with 4 parallel ranges does 5.5 MB/s from the
  same Wi-Fi. Download the IPSW on the Windows side, not in WSL.
- Root image ready (not flashed): `../guac-artifacts/ios16-root.simg`, 12 145 254 696 B, 16 GiB HFS+
  (case-sensitive, journaled), 12 GiB used, 181 772 files, `fsck_hfs` clean; android sparse header
  `0xed26ff3a`, 4 194 304 × 4096 B blocks, 27 811 chunks; sha256 `884f154b…`.
  Built from the 3 CRC-verified DMGs with `tools/mkiosroot.sh` on ext4 (`/root/ios16-build/`), then
  `sweet-root-tweaks.py --no-bbd` (see below), fsck, `img2simg`, copied to Windows.
  - launchd cache: 530 daemons, 7 × `_PanicOnCrasX`, `com.apple.CommCenter` + `com.apple.fairplayd.H2`
    removed, `com.sweet.autorun` added, backboardd → **`/usr/libexec/backboardd`**.
  - Deviation from the docstring: the tweak normally points backboardd at `/usr/libexec/sweet-bbd`,
    which is userland (installed later through SWEETBOX). A fresh IPSW root has no such binary, so the job
    would fail and the window server would never start. Added the `--no-bbd` flag for exactly this case;
    re-run the tweaks (or install the SWEETBOX box) once `userland/bbd` + `libsweet-bbd.dylib` exist.
  - Warning from the tweak: "no mobileactivationd system container; skipped data ark" — a fresh root has
    no system data container yet; activation still comes from the Hactivation branch (`/chosen`
    security-status, tools/mkadt.py). Untested here.
  - dyld shared cache: 44 files, all hard links; keybag and logd stores present; containers created.
- In progress / next: connect the phone, `fastboot flash userdata ../guac-artifacts/ios16-root.simg`,
  then `fastboot boot shim/out/ios16-boot.img`. **Ask the user first** — the flash wipes LineageOS and all
  user data (firmware dumps incl. modem/persist in `../guac-artifacts/backup/guac-backup/`, 15 files,
  880 MiB). Phone was not visible in `adb`/`fastboot` at 22:45.
- Stage-2 boot: `fastboot boot shim/out/ios16-boot.img`, then `tools/sweet-usbnet.sh logs/live-*.txt`
  on the PC (phone CDC-ECM at 192.168.7.2, console UDP to 192.168.7.1:5140).
- Phone state: last boot was `fastboot boot shim/out/out.img` (probe, read-only): UFS/GPT read OK, then the real
  kernel reached the XNU console (`FrameWork successfully initialized`, `ncache: 8 CPU(s){,}`), boot-args had
  no `rd=` and no `ufsrw`. A plain reboot returns to Android.

# Active handoff — iOS haptics reach the motor; AW8697 wedges -> REQUEST-J / 0258 (2026-11-11 02:35, Claude)

- Full iOS haptic chain works: MG DeviceSupportsHaptics=true (new) + DeviceSupportsClosedLoopHaptics=true. VirtualAudio
  creates VAD 'vzzz' VirtualAudioDevice_Actuator only if MGGetBoolAnswer(DeviceSupportsHaptics) (0x16dd14 in
  dtb/va/VirtualAudio.plugin); without it AVHapticServer init fails and every vibe returns '!itl'
  (research/haptics/ios-j.md). Now vibtest system sounds, Settings switches (UIKit level 2 / CoreHaptics) reach
  /dev/hakios-haptic and the motor.
- Open: after a few iOS plays the chip wedges (GO=1, GLB_STATE 7, standby off; silent for every path until
  `haptic init`). Dumps + ask: research/haptics/REQUEST-J.md (patch 0258 for Codex).
- 0252 confirmed (motor works at first command after boot). 0253 untested: CommCenter now aborts on an uncaught C++
  exception (no IPAPPENDER lines); CommCenter disabled again (launchctl disable system/com.apple.CommCenter).
- Rollback haptics: `mgtool unset DeviceSupportsHaptics` + restart mediaserverd.

# MILESTONE — video plays in Safari (2026-11-11 00:52, user)

- With 0256 (display-pipe dictionaries) + SweetMetal2 0257b (video frames re-converted at every bind): mp4 and the
  non-DRM YouTube videos play correctly (software decode, BT audio). Next for video: YCbCr->RGB on the GPU (sampling
  the planes in a shader) instead of a CPU copy per bind.

# Active handoff — SweetMetal2 0257b: re-convert video frames at every bind (2026-11-11 00:55, Claude)

- 0257 (no cache) did not help; the user sees the right frame mixed with the very first and middle frames, steady when
  paused. Cause: CoreAnimation makes ONE texture per IOSurface and keeps it, expecting a live view; our YCbCr texture
  is a converted copy, so each pooled decoder surface keeps the picture it had when CA first imported it.
- userland/metal2/NativeBridgeMetalFrontend.mm: sweetYCbCrFill() (the conversion into an existing texture);
  '420v'/'x420' textures remember their surface ("ycbcr-src") and are re-filled from it in -texture:index:stage:
  (every bind) via sweetYCbCrRefresh(); /var/local/mtl2-videocache: only when the seed moved. Rebuild + install the
  bundle; restart backboardd, MobileSafari, com.apple.WebKit.GPU/WebContent. Long-term: convert on the GPU.
- Codex rebuilt and ad-hoc signature verification passed. Installed on `192.168.0.61`, preserving the prior bundle as
  `SweetMetal2.bundle.pre-0257b-20261111`; pulled back and byte-compared `SweetMetal2`, Turnip, and CodeResources.
  SHA-256: `d0fc051e3f6c735a5b4bf27ff1037498ee6c96be20912e256a3f5bab7cdd5836`,
  `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`, and
  `9f7aea4c96dd1fb9c4b3c0769554ecede146d1fc025d1a81e6dd788f4bba79c4`. Resprung with `killall -9 backboardd`; no
  reboot. Archive: `outputs/guacamole-SweetMetal2-0257b-video-refresh.tar.gz`.

# Active handoff — Safari mp4 plays (0256 worked); flicker fix in SweetMetal2 (2026-11-11 00:45, Claude)

- After 0256 a plain mp4 plays in Safari (software decode) but flickers between two frames. YouTube: most show
  "error licensing" (FairPlay; not pursued), but some YouTube videos DO play (user, 00:39). mtl2.log: backboardd converts '420v' 1280x720 surfaces (pool of 12+,
  seed 1 each, different content), so decoding works; suspect the YCbCr->RGBA cache (by surface id + seed) serving a
  pooled surface's old picture when its seed does not move.
- userland/metal2/NativeBridgeMetalFrontend.mm sweetYCbCrTexture (labelled 0257): '420v'/'x420' surfaces are converted
  on every use (no cache); '420f' photos keep it; /var/local/mtl2-videocache restores the old behaviour. Needs the
  SweetMetal2 bundle rebuilt (userland/metal2/build.sh) and installed. CPU cost: one 720p conversion per composite.
- Codex rebuilt with `sh userland/metal2/build.sh`; ad-hoc signature verification passed. Installed over SSH to
  `/System/Library/Extensions/SweetMetal2.bundle` on `192.168.0.61`, preserving the previous bundle as
  `SweetMetal2.bundle.pre-0257-20261111`. Pulled back and byte-compared `SweetMetal2`, Turnip, and CodeResources.
  SHA-256: `d638fc2164578ba703afba4d25ff5e163f5e00270a3b29fc9d84877d5bca6116`,
  `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`, and
  `9f7aea4c96dd1fb9c4b3c0769554ecede146d1fc025d1a81e6dd788f4bba79c4`. Archive:
  `outputs/guacamole-SweetMetal2-0257-video-ycbcr-recache.tar.gz`. Resprung with `killall -9 backboardd`; no reboot.

# Active handoff — 0256: AppleCLCD display-pipe properties as dictionaries (Safari video crash) (2026-11-11 00:30, Claude)

- **Built by Codex (2026-11-11 00:26):** patches 0255 and 0256 applied cleanly in order and the local XNU kernel
  build succeeded. Kernel: `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet`
  (17,137,824 bytes, SHA-256 `cd1b0d107197c07493a6766ae5b015968e5413e1b49de1826201538302a7dd5d`). Fresh ADT:
  `work/guacamole-0256-audio-hakios.adt` (`--highmem --guacamole`, includes `/product/audio`). Boot image:
  `outputs/guacamole-0256-audio-hakios.img` (21,590,016 bytes, SHA-256
  `b7830e8ef9b87dfe06af1776598ebaf41950985ca3b8281326e30c98104948b9`); arguments, with `sweettime` refreshed and
  `sweetaudio=0` retained, are in `outputs/guacamole-0256-audio-hakios-boot-args.txt`. Payload verified against the
  boot image. Built only; not transferred to or booted on the phone.

- Codex: mediaserverd's video crash is VideoToolbox _FigGetAlignmentForIOSurfaceOutputToDisplayService reading AppleCLCD
  DisplayPipePlaneBaseAlignment as a dictionary (PlaneBaseAlignmentLinear) and DisplayPipeStrideRequirements
  (StrideLinearHorizontal); ours was the number 64 (since 0046).
- NEW xnu/patches/0256-guacamole-fb-display-pipe-dicts.patch (SweetFramebuffer.cpp, after 0168): both published as
  { key: 64 } (the framework's fallback values). The test image keeps sweetaudio=0 + HakiosAudio. Expect: video in
  Safari plays (software decode; AppleAVD absent).

# Plan — real speakers / mic: research/audio/speaker-mic-plan.md (2026-11-11, Claude)

- TFA9894 x2 (I2C 0x34/0x35 on i2c@890000) + WCD9340 Tavil (SLIMbus, ADSP-owned); LPASS belongs to the ADSP.
  Steps: ADSP PAS boot + GLINK/QRTR (reuse userland/wland's modem code, same SM8150 as Pixel 4) -> APR (AFE/ADM/ASM,
  port HackiOS/apollo sweet_adsp.c) -> speaker stream from /dev/hakios-audio -> TFA9894 driver -> later the mic.

# Active handoff — Safari video: mediaserverd crash (2026-11-11 00:20, Claude)

- Playing any video in Safari now gets past audio (routes work) and crashes mediaserverd: "-[__NSCFNumber
  objectForKey:]" right after MXSession setBestAvailableContentType; crash loop, no .ips. Also seen: CoreVideo
  "Failed to read HardwarePlatform, MG query returned NULL", AppleAVDCheckPlatform() FALSE (no hardware decoder;
  expected). Codex request: research/video/README.md (symbolicate the kernel bt / disassemble the caller).
- **Crash root cause found:** MediaToolbox's video render-pipeline setup calls VideoToolbox
  `_FigGetAlignmentForIOSurfaceOutputToDisplayService`, which expects `AppleCLCD.DisplayPipePlaneBaseAlignment`
  to be a dictionary containing `PlaneBaseAlignmentLinear`; Sweet currently publishes scalar `64`, so the CF
  dictionary lookup throws. The companion property/key is `DisplayPipeStrideRequirements` /
  `StrideLinearHorizontal`. Both fallback requirements in the framework are 64; stock D22 runtime numbers are not
  confirmed from the ADT/shared cache. Full symbolication and disassembly: `research/video/README.md`.

# MILESTONE — Bluetooth headphone audio works (2026-11-11 00:07, user)

- With HakiosAudio.driver + sweetaudio=0 (routes on the Speaker device), music/audio plays through Bluetooth
  headphones (A2DP). The phone's own speaker stays silent (no kernel audio sink yet).

# Active handoff — 0255: iOS haptics as waveforms through HakiosAudio's Actuator (2026-11-11 00:15, Claude)

- ios-apollo (Mi 10T: SAME AW8697) plays iOS haptics this way: HakiosAudio.driver's "Actuator" device gets iOS's
  haptic waveforms (VibeSynth "Unified Haptic/Vibe Engine", Core Haptics, UIKit level 2, system sounds) and writes them
  to /dev/apollo-haptic or /dev/hakios-haptic; a kernel service turns them into the chip's effects.
- NEW xnu/patches/0255-guacamole-haptic-ios-actuator.patch (after 0252..0254; only bsd/dev/sweet_haptic.c; dry-run OK;
  not compiled): /dev/hakios-haptic (0666) + hi_thread (their serve(), APSL, adapted): tap after a pause = waveform
  20/5/2 by peak (2/3, 1/3 of full scale), gain DATDBG 0x50..0x80 by peak; > 60 ms = waveform 10 looped until iOS
  stops (<= 1 s); <= 1 s of driving per 2 s window. Starts after the boot set-up; boot-arg sweethapticios=0 = off.
- iOS uses the Actuator only with DeviceSupportsClosedLoopHaptics true. Test: `mgtool setbool
  DeviceSupportsClosedLoopHaptics true`, restart mediaserverd + SpringBoard (expect "Using New Unified Haptic/Vibe
  Engine", no '!itl' errors now that an Actuator exists, Settings "Sounds & Haptics", switches/keyboard taps). If good:
  tools/mkadt.py could add /product/haptics (D22: closed-loop, supports-3rd-party-haptics) instead of the cache.
  The legacy path (SweetVibrator 0249/0250, converted D22 pattern plist) then becomes the fallback.

# Active handoff — MILESTONE: iOS audio routes work (HakiosAudio.driver + sweetaudio=0) (2026-11-10 23:59, Claude)

- sweetaudio=0 image + /System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver (Info.plist MUST be 0644, dir 0755:
  mediaserverd runs as mobile; scp kept 0600 and the plug-in was silently skipped). VirtualAudio ports: pspk Speaker,
  pspr Speaker Telemetry, pstp Speaker Tap, phki Hawking Input, pmbi Built-In Microphone, pact Actuator (+ pap, pnul,
  pnli, papa). AQMEIO "getting output port ID": 0. Speaker samples are dropped (no /dev/hakios-audio): silent.
- Next: YouTube / Music (silent playback), Bluetooth A2DP sound, Actuator (iOS haptics as waveforms -> AW8697 RTP?).

# Active handoff — audio: switch to ios-apollo's HakiosAudio.driver HAL plug-in (2026-11-10 23:55, Claude)

- github.com/aTechGeekDev/ios-apollo (Mi 10T, same ios-sweet base, iOS 16.7.16) has speakers + Bluetooth A2DP working
  with hakios-apollo's AudioServerPlugIn (MIT): devices UID "Speaker", "Hawking" (mandatory 'vhaw' companion: why
  routes stayed on Null_Device), "Codec" (input with data source 'imic': what our 0254 'who?' was), "Actuator"
  (Taptic waveforms). Copied unchanged to userland/audio/ (README, build.sh).
- Plan: boot with sweetaudio=0 (our IOAudio2 stand-in off; no sweetaudiotp/term/uid), install
  /System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver/{HakiosAudio,Info.plist}, restart mediaserverd. Without a
  kernel /dev/hakios-audio the speaker's samples are dropped: routes, app playback (silent) and Bluetooth A2DP first;
  real speaker = an ADSP/amp path later (hakios-apollo's kernel side is Mi 10T hardware).
- The mediaserverd autrace wrapper (sweet-msd, dtb/mediaserverd-autrace.plist) is only a runtime bootstrap: a reboot
  drops it; launchd.plist on the phone has a harmless DYLD env entry (stripped by launchd), backup dtb/launchd.plist.bak.

# Active handoff — MILESTONE: UID "Codec" + 'bltn' runs Device_Codec_Aspen (speaker + receiver ports) (2026-11-10 23:17, Claude)

- 0254 image, boot-args sweetaudiotp=0x626c746e sweetaudiouid=Codec sweetaudioname=Codec: VirtualAudio adopts it,
  Device_Codec_Aspen.cpp:1341 "creating a speaker port", :1345 "creating a speaker tap input port", :1356 "creating
  a receiver port", then DeviceManager.cpp:282 "Could not add PhysicalDevice ID: 33 UID: Codec. Reason:
  CAException (2003332927)" = 'who?' kAudioHardwareUnknownPropertyError: the codec setup asks for a property our
  device lacks right after the receiver. dtb/cd.log has no HAL line naming it; before it: 'pstp' speaker tap "port
  name is zero-length", "Device Codec does not support hardware volume range property". Codex request
  research/audio/README.md 23:20. Music shows no outputs on this boot (device not added).

# Active handoff — 0254 audio UID boot-args; D22 audio-data "Codec" (2026-11-10 23:15, Claude)

- research/ipsw-x/d22ap.adt: real D22 audio = IOAudio2 audio-data devices under arm-io/mca*: audio-codec
  (audio-data,cs42l75), audio-speaker-top (cs35l26), audio-codec-voice (mikeybus-secondary, data-sources name
  "Codec"), audio-codec-hawking ("Codec"), audio-loopback ("Loopback"), audio-halogen, audio-haptic ("Actuator"),
  audio-baseband-voice ("Baseband"); control nodes spi2/audio-codec (cs42l75: imic/lmic/smic/fmic, rx-receiver,
  receiver-pdm), i2c2/audio-speaker-top (primary-speaker 'SpoT'). VirtualAudio's configuration factory keys (Codec,
  Speaker, Voice, Reference, Baseband Voice, ...) look like these device names.
- Theory: built-in ('bltn') devices are not handled by DeviceFactory_Aspen (no case) but by the configuration path,
  keyed by device UID/name; ours was "SweetBuiltInAudio" (unknown key -> listed, never adopted, 0226/0227).
- NEW xnu/patches/0254-guacamole-ioaudio2-uid-bootargs.patch (after 0253; dry-run clean against the current 0253 tree):
  boot-args sweetaudiouid=STR, sweetaudioname=STR. Test image: sweetaudiotp=0x626c746e sweetaudiouid=Codec
  sweetaudioname=Codec (terminal boot-args back to defaults spkr/micr). Keep the mbus/hdph/hmic image as fallback.
  Check: oslog -l mediaserverd: Device_Codec_Aspen / "creating a speaker port" / pspk.
- BUILT 0254 locally after a clean apply check. Kernel:
  `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet` (17,137,456 bytes,
  SHA-256 `3d6e24ef26af3883924202d7b497c6ffeb62854ab255f44461ffc7cc6ed27e90`). Fresh ADT:
  `work/guacamole-0254-codec.adt` (`--highmem --guacamole`, includes `/product/audio`). Test image:
  `outputs/guacamole-0254-audio-codec-identity.img` (21,585,920 bytes, SHA-256
  `e8fb072ab71577ef61bdf76b942c5b0d17578145b9a91045d818b00be5df9b1c`); boot args are in
  `outputs/guacamole-0254-audio-codec-identity-boot-args.txt`. The 0253 headset image remains available as fallback.
  Built only; not copied to or booted on the phone.

# Active handoff — MILESTONE: VirtualAudio builds real ports on SweetBuiltInAudio (2026-11-10 22:05, Claude)

- Boot-args sweetaudiotp=0x6d627573 sweetaudioterm=0x68647068 sweetaudiointerm=0x686d6963 ('mbus'/'hdph'/'hmic')
  on the 0253 kernel + /product/audio ADT: Device_MikeyBus_Aspen "creating X90 input/output port subtype",
  published ports pmwi "Mikeybus Headset Microphone", phwt "Mikeybus Headphones Reference", phpw "Mikeybus
  Headphones" (+ papa). auprobe: device 33 transport 'mbus'. Keep these boot-args in every image from now on.
- Result: route stays pap -> pnul Null_Device: "no supported route in the current category" ('csav'/'imdf') for
  the MikeyBus ports; AQMEIO -1 continues; YouTube now reaches FairPlay ("error licensing this video"); Bluetooth
  route config EndedSuccess but no sound. Codex request research/audio/README.md "22:15" (route tables).

# Active handoff — audio: mbus wrapper engages; needs headset terminal types (2026-11-10 17:06, Claude)

- Built the headset-terminal test image: `outputs/guacamole-0253-audio-mbus-headset.img`, same 0253 kernel and
  `/product/audio` ADT, with `sweetaudiotp=0x6d627573 sweetaudioterm=0x68647068 sweetaudiointerm=0x686d6963`.
  Boot args are in `outputs/guacamole-0253-audio-mbus-headset-boot-args.txt`; payload verification confirms those
  three args and the required `rd=ufsroot0 ufsroot=ios` are present. SHA-256:
  `33b1e544964b202b12ffa3cb01b02e3d96891c00a99afb26bfbaf529042fc200`. Built only; not copied or booted.
  The boot-args field is limited to 608 bytes, so this test image omits `-enable_kprintf_spam`, `sweetcpufreq=2`,
  and `sweethaptic=2` to fit; root, Wi-Fi, Metal and current GPU-present args are retained.

- Booted outputs/guacamole-0253-audio-mbus.img (sweetaudiotp=0x6d627573). dtb/mb.log: VirtualAudio wraps
  SweetBuiltInAudio as `Device_MikeyBus_Aspen` ("Constructing MikeyBus device"), creates streams (i/35, i/78 = the
  tap/reference stream it enables, o/34), then `Ignoring headset input stream (0, micr)`, `(1, spkr)`, and
  `EXCEPTION "Unknown Stream Terminal Type :spkr"` -> "Could not add PhysicalDevice ID: 33". Null_Device stays the route.
- So MikeyBus wants headset terminal types. NEXT IMAGE (boot-args only, same 0253 kernel + ADT):
  `sweetaudiotp=0x6d627573 sweetaudioterm=0x68647068 sweetaudiointerm=0x686d6963` ('mbus' / 'hdph' / 'hmic', 0234).
  Expect a headphones output port (+ headset mic) for SweetBuiltInAudio; then AQMEIO's output port ID should exist
  (YouTube in Safari, Bluetooth routes). Check: oslog -l mediaserverd for Device_MikeyBus_Aspen / Port_* lines.

# Latest boot image — audio transport `mbus` test (2026-11-10 16:50)

- Built `outputs/guacamole-0253-audio-mbus.img` using the existing 0253 kernel and `/product/audio` ADT, with the existing boot arguments preserved, refreshed `sweettime`, and `sweetaudiotp=0x6d627573` appended. No kernel or ADT changes.
- Kernel SHA-256: `73aabaf4b788de2de43f0fedc47b5b7f0d3038f8a3d46316162888c4ff49c596`; ADT: `work/guacamole-0253-product-audio.adt`; args: `outputs/guacamole-0253-audio-mbus-boot-args.txt`.
- Image size: 21,585,920 bytes; SHA-256: `bfed4603d70dad51a7de9eac1314a291240affea05e9827a374adc7f7cf3257e`. Built only; not copied to or booted on the phone.

# Active handoff — audio: /product/audio works; investigate missing codec ports (2026-11-10 16:35, Codex)

- Booted with /product/audio: VirtualAudio now makes ports for SweetBuiltInAudio, but only Null ('pnul'/'pnli').
- NEW userland/jb/auprobe.c (build.sh): HAL client probe. `auprobe` lists the VADs; `auprobe 33` = our device:
  transport 'null', HasProperty(mute,outp,3/4) = 1, controls 36/37 mute ctrl outp el 3/4 instantiated (0235 OK).
- Correction from static analysis: `DeviceFactory_Aspen` does not instantiate a codec wrapper, but common per-device
  setup directly calls the `Device_Codec_Aspen.cpp` routine at `0x23f8d0` from `0x23d158`, without a transport/UID
  branch. No AppleAOPAudioController dependency was found on that path. With mute/outp elements 3/4 passing, why the
  discrete codec ports are absent is still a runtime discrepancy; add jack/outp/0 and volm/outp/2 to auprobe and
  capture the VirtualAudio connection log. Details: `research/audio/device-codec-construction-1635.md`.
- Null `pnul` routes are not a usable audio output fallback on this boot: AQMEIO's `dacp/outp/0` lookup returns -1.

# Active handoff — ADT /product/audio for VirtualAudio's speaker port (2026-11-10 16:30, Claude)

- Safari: GPU-process DOM rendering now ON (user, Settings > Safari > Advanced > Feature Flags); pages render, faster.
  (The old com.apple.mobilesafari key WebKit2GPUProcessForDOMRendering only forces it off; dtb/ has the plist copy.)
- YouTube in Safari fails on audio: mediaserverd `AQMEIO_HAL Error -1 getting output port ID` at play (no decoder
  error). With 0235, VirtualAudio still publishes only 'pap' + Null_Device 'pnul'/'pnli' ports, and logs
  `IORegistryEntryFromPath IODeviceTree:/product/audio returned MACH_PORT_NULL (i.e. failed)` during port setup.
- tools/mkadt.py --guacamole now adds /product/audio from the D22AP tree (wireless-splitter 1,
  supports-always-listening 1); NOT /product/haptics (would make MG DeviceSupportsClosedLoopHaptics true ->
  VibeSynth, no vibration). Verified with `mkadt.py OUT --guacamole --highmem --dump`; dump contains `/product/audio`
  and no `/product/haptics`. Rebuilt with the existing 0253 kernel and unchanged boot arguments (no kernel change):
  `outputs/guacamole-0253-product-audio.img` (21,585,920 bytes, SHA-256
  `e05d2bbb40c9fc95d803dd80fdaeabb4bf2c3ca75777cfc8a108b833df61209a`). ADT:
  `work/guacamole-0253-product-audio.adt`; args: `outputs/guacamole-0253-product-audio-boot-args.txt`. Image built, not
  booted. Check after boot: mediaserverd "creating a speaker port" / "Adding port [ type: pspk", no AQMEIO -1.
- tools/-h (stray mkadt output) moved to _to_delete/ (gitignored); delete it by hand.

# Active handoff — 0253 AppleIPAppender stand-in (CommCenter crash = slow Settings), coreduetd off (2026-11-10 16:05, Claude)

- Settings slowness: Settings > Sounds stalls ~1.2 s on CoreTelephony (voicemail check) while CommCenter crash-loops
  ("Could not find AppleIPAppender service" -> sig 11). NEW xnu/patches/0253-guacamole-appleipappender-stub.patch
  (iokit/Kernel/arm/SweetIPAppender.cpp + files.arm64 + KernelConfigTables, like IOWatchdog 0040). On 2026-11-10,
  `git apply --check` passed against the actual XNU tree (no fuzz/offset); local build through 0253 succeeded. Kernel:
  `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet` (17,047,048 bytes,
  SHA-256 `73aabaf4b788de2de43f0fedc47b5b7f0d3038f8a3d46316162888c4ff49c596`). Fresh ADT:
  `work/guacamole-0253.adt` (`--highmem --guacamole`). Boot image:
  `outputs/guacamole-0253-aw8697-sweethaptic2.img` (21,585,920 bytes, SHA-256
  `33f3f80c30244fc4257cb879fa22cbcac1d455250669630a9f4b45e6a6108b21`), using the 0252 arguments with refreshed
  `sweettime`; arguments are saved in `outputs/guacamole-0253-sweethaptic2-boot-args.txt`. Image built, not booted.
  See `research/commcenter/README.md`.
- coreduetd ~80% CPU: dasd runs `Duet: executeQuery sync` ~3/s (each ~0.3 s, ~110 Biome file opens). Not CommCenter
  (stopping it changed nothing). User ran `launchctl disable system/com.apple.coreduetd` + bootout (result pending).

# Active handoff — haptics through mediaserverd WORK with both haptic MG answers OUT of the cache (2026-11-10 15:39, Claude)

- `mgtool unset DeviceSupportsClosedLoopHaptics` + `mgtool unset DeviceSupportsHaptics`, then restart mediaserverd:
  `vibtest` (4095) -> FigVibrator IOKit route -> SweetVibrator request `+400@256 -1@0`, then stop; the motor vibrates.
- With DeviceSupportsClosedLoopHaptics in CacheExtra (true OR false!) mediaserverd logs "Using New Unified
  Haptic/Vibe Engine" (libVibeSynthEngine, research/haptics/ios-i) and every vibration goes to VibeSynth and fails
  ('!itl' 560559212); SpringBoard's UIKit picks CoreHaptics (level 2) and its haptic server times out.
  With DeviceSupportsHaptics=false cached: "Device will not vibe as vibrator is not initialized (or not available)"
  (-16490). Leave both answers to MobileGestalt (not cached); restarting MobileGestaltHelper changes nothing.
- So the Settings "Sounds & Haptics" title cannot come from MobileGestalt: edit Settings' own specifier instead (todo).
- vibtest's system-sound modes DO reach mediaserverd (Incoming Request in oslog); they failed only because of the above.
- Next: reinstall dtb/haptics/SystemSoundVibrationPatterns-legacy.plist (D22, chmod 644, restart mediaserverd) so
  Atoms-only sounds (1520 OrbPop_Haptic, switches) become taps.

# Active handoff — 0252 haptics: set the chip up again at the first play, one player at a time (2026-11-10, Claude)

- NEW xnu/patches/0252-guacamole-haptic-reinit-serialize.patch (after 0251; sweet_haptic.c + SweetVibrator.cpp).
  Patch applied cleanly after 0251 and built. Kernel: `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet`
  (17,039,120 bytes, SHA-256 `571dd6154e4f05714dc167ab8c12f9982a7346cccac3e5aa07804a3c46c09694`). Fresh ADT:
  `work/guacamole-0252.adt` (`--highmem --guacamole`). Boot image:
  `outputs/guacamole-0252-aw8697-sweethaptic2.img` (21,577,728 bytes, SHA-256
  `281dfb27581baed882b75a09373d3a5ee55966054558988b31ad888cc9938cb6`), with the usual 0250 haptics test args
  (`sweethaptic=2`, refreshed `sweettime`). Payload verified byte-for-byte after unpack. Built `userland/out/haptic`
  and installed atomically to `/var/local/bin/haptic` on 192.168.0.51 (cksum `554221565 50224`). Image not booted.
  - The boot-time set-up (50 s) leaves the motor silent until `haptic init`; so hp.fresh: the first play after boot
    (vibrator or sysctl) runs hap_configure again first (no click, ~0.7 s once). Boot-arg sweethapticidle=S also does
    it before any play after S idle seconds (default 0 = off), in case the chip goes quiet again later.
  - hap_configure now holds hp.lock for the whole reset + set-up (the reset pulse was outside it).
  - SweetVibrator's thread call is THREAD_CALL_OPTIONS_ONCE: a request during a playing pattern re-runs after it
    instead of a second play() racing on another worker (overlapping requests locked the chip).
- User wants every haptic through mediaserverd (no UIKit hook). Open: from the UI, mediaserverd currently sends the
  vibrator nothing (Settings switches / long-press / Ringtone>Vibration previews), with either pattern file. Earlier
  today previews (+1000/-1000) and one long-press (+400/-100 REPEAT) did arrive. vibtest's system-sound modes never
  reach it (root background client): only `vibtest io` is a valid test. Next: oslog of mediaserverd during a preview.

# Active handoff — 0251 haptics: `haptic init` + per-play chip status (2026-11-10, Claude)

- Status: motor SILENT on the current boots (0250): `haptic 1`, `haptic buzz`, the boot click and `vibtest io` all
  do nothing, though kern.sweet_haptic reads 0x97 (configured: RAM loaded) and SweetVibrator receives requests
  (`vibtest io 30` -> request #1 +30@256). The bootloader's vibration works (motor + wiring fine). It worked on the
  0248/0249 boots, which came straight after Android (register dumps): suspect Android's aw8697 driver leaves
  something set up that ours does not (cold boot = silent). The boot log is lost to `exec` spam before SSH is up (~60 s).
- NEW xnu/patches/0251-guacamole-haptic-init-report.patch (after 0250, only bsd/dev/sweet_haptic.c): setup moved
  into hap_configure(); sysctl kern.sweet_haptic = 200 (`haptic init`, userland/jb/haptic.c) redoes reset + config +
  RAM + click + register dump on demand; every play/buzz/click logs SYSST/SYSINT/GLB_STATE/SYSCTRL 5 and 25 ms after
  GO (`SWEET HAPTIC: play: +5ms sysst .. sysint .. glb ..`): shows boost error / UVL / OCD / OT or a play that never
  starts. `haptic reg R [V]` reads / writes any chip register live (value 0x100000|w<<16|R<<8|V; result in dmesg).
  `haptic dump` (201): chip registers + SE + GCC vote/branches + TLMM pins, nothing changed.
  FINDING 15:06 (built 0251): `haptic init` fixes the motor; direct `haptic N/stop/buzz` keep working; ANY
  `vibtest` run (even modes that never reach SweetVibrator: 4095 / 1520 via mediaserverd) locks the motor until
  `haptic init`. Suspect the audio side (mediaserverd/AudioToolbox waking our audio code) touching a shared GCC vote,
  QUP0 / pin (reset GPIO116) the AW8697 uses. Compare `haptic dump` working vs after vibtest.
  Phone 14:50: no play works at all now, with or without a stop first (one stop+play earlier did buzz once). Patch applied cleanly after 0250 and built locally: `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet` (17,039,336 bytes, SHA-256 `9988cced1472d860ea074d685b9027a96109dc396e987818d573f1e9f7aeb634`). Rebuilt `userland/out/haptic` from `userland/jb/haptic.c` and installed atomically to `/var/local/bin/haptic` on 192.168.0.51 (MD5 matches local build: `76952da9220f660510bd08fb49933e48`). Built fresh ADT `work/guacamole-0251.adt` (`--highmem --guacamole`) and boot image `outputs/guacamole-0251-aw8697-sweethaptic2.img` (21,581,824 bytes, SHA-256 `df28c49f70293e3bce3247481d9d2b53c98969bf3fcd53539870c9a79cb77989`), using the 0250 test arguments with refreshed `sweettime`; unpacked ramdisk byte-matches the payload. Image is built but not booted.
- Also new: userland/jb/vibtest.c (+ build.sh, with mgq): `vibtest [ID]`, `vibtest pat MS..`, `vibtest io [MS]`
  (straight IORegistryEntrySetCFProperties on "vibrator"). System sounds played from vibtest (root, background) reach
  nothing: not a valid test of mediaserverd.
- tools/haptic-patterns.py: adds legacy VibePattern to the Atoms-only entries of MediaExperience's
  D22/SystemSoundVibrationPatterns.plist (dtb/haptics/*-legacy.plist; currently NOT installed, original restored,
  backup /var/local/SystemSoundVibrationPatterns-D22.orig.plist on the phone). Untested while the motor is silent.

# Active handoff — Request H haptics disassembly (2026-11-10)

- Completed the iOS 16.7.16 / 20H392 analysis in `research/haptics/ios-h/RESULTS.md`. UIKit’s feedback tier reads `DeviceSupportsHaptics` only when the Core Haptics level-2 branch is unavailable/disabled; it does not read `DeviceSupportsClosedLoopHaptics` there. UIKit’s generic system-sound engine passes Flags and optional Volume, not Loop/LoopPeriod/VibrationPattern, and its stop path calls `AudioServicesStopSystemSound(soundID, true)`.
- FigVibrator’s legacy IOKit parser only understands `Intensity`, `VibePattern`, or legacy duration keys; it does not parse `Atoms`. Missing a recognized pattern triggers a one-second total-duration fallback. The exact request dictionary / Home Screen action ID and missing-stop point remain unproven without runtime capture. See RESULTS.md for proven details, inferred explanation, and capture points.

# Active handoff — 2026-11-10 (Claude): mgtool takes question names, not obfuscated keys

- `mgtool` hashes its NAME argument (base64(MD5("MGCopyAnswer"+name))[:22]). The haptics steps had passed the already
  obfuscated keys (`cBy4BcYs5YWtFHbBpt4C6A`, `gqDnklGQnpv5ilgh5uHckw`), which wrote junk keys
  (`EGAqnEa1S8GfhA8ch9Yh7g` for the latter) and left the real answers unset: why "Sounds & Haptics" never appeared.
- Fixed in NOTES / research docs: `mgtool setbool DeviceSupportsHaptics true`, `mgtool setbool DeviceSupportsClosedLoopHaptics true`;
  remove the junk with `mgtool unset cBy4BcYs5YWtFHbBpt4C6A` and `mgtool unset gqDnklGQnpv5ilgh5uHckw`; check with `mgq`.

# Active handoff — 0250 vibrator: every request logged, `haptic stop` ends patterns (2026-11-10, Claude)

- 0249 test: something makes the motor vibrate continuously for a long time; `haptic stop` stopped the motor
  only until SweetVibrator's pattern loop started the next segment (a REPEAT pattern), so it "stops after a while"
  (the 60 s cap). Request lines had rolled out of dmesg (only the first 20 were logged). Settings still says
  "Sounds" (Request G in research/haptics/README.md: the "can vibrate" MobileGestalt answer).
- NEW xnu/patches/0250-guacamole-vibrator-log-stop.patch (after 0249): sysctl stop (`haptic stop`) bumps a
  counter (sweet_haptic_stops) that ends the running pattern; repeat cap 30 s; EVERY request logged:
  `SWEET VIBRATOR: request #n: K segment(s) [REPEAT] [ms@level/256] +500@256 -500@0 ... (keys, hertz_millisecs class)`
  or `stop (OSNumber 0)`. patch --dry-run OK on 0249. Not compiled.
- BUILT (2026-11-10): patch 0250 applied cleanly after 0249; local XNU build succeeded. Kernel:
  `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet` (17,129,536 bytes,
  SHA-256 `7801bcad5e66054a0a40afd5153d7af50d9344fa4369e9a6730d43a288bc3073`). Fresh ADT:
  `work/guacamole-0250.adt` (`--highmem --guacamole`, 105,596 bytes). Payload:
  `outputs/payload-guac-0250-aw8697-sweethaptic2.bin`. Boot image:
  `outputs/guacamole-0250-aw8697-sweethaptic2.img` (21,581,824 bytes, SHA-256
  `c9c711909e6200785f8006a74627e0feb8a0079f417bab25c4c25c443d50a96f`). Args are saved in
  `outputs/guacamole-0250-sweethaptic2-boot-args.txt`; same as 0249's `sweethaptic=2` test args with a refreshed
  `sweettime`. The known 0249 image remains available as fallback. 0250 image has not been booted.
- Test: boot the 0250 image; when it buzzes, `dmesg | grep -a "SWEET VIB" | tail`; `haptic stop` must stop it
  for good and every request should be logged.

# Active handoff — 0249 "vibrator" IOService: iOS vibration -> AW8697 (2026-11-10, Claude)

- Request F (research/haptics/ios-e/REQUEST-F.md): UIKit feedback level = 2 only with Core Haptics first-party
  hardware (not here); else MobileGestalt bool cBy4BcYs5YWtFHbBpt4C6A (false -> 0 "Haptics: unsupported", true ->
  1). Level 1 plays feedback as system sounds with vibration -> mediaserverd SSServer -> MediaExperience
  FigVibrator; VibeSynth only if libVibeSynthEngine.dylib's VibeSynthEngineHasHardwareSupport() (expected false
  here) -> else IOServiceNameMatching("vibrator") + IORegistryEntrySetCFProperties. Keyboard "Haptics: disabled"
  = the keyboard haptic preference (Settings > Sounds & Haptics > Keyboard Feedback).
- NEW xnu/patches/0249-guacamole-vibrator-service.patch (after 0248): iokit/Kernel/arm/SweetVibrator.cpp
  (IOService named "vibrator", KernelConfigTables personality on IOResources, files.arm64), setProperties:
  {intensity SInt32 16.16, hertz_millisecs [on-state, ms, off-state, ms, ...], repeat} plays on a thread call
  (on <= 40 ms: tap waveform 1..5 by intensity, 20 if strong and >= 25 ms; longer: buzz; off: wait; repeat
  loops <= 60 s); a non-dictionary (FigVibrator's stop = CFNumber 0) stops. Logs `SWEET VIBRATOR: request #n`.
  sweet_haptic.c exports sweet_haptic_ready / _wave / _buzz. Patch applied cleanly after 0248 and compiled.
- Built through 0249 (2026-11-10): kernel at
  `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet` (17,039,232 bytes,
  SHA-256 `af53a6f439e748f7d8a215bd7c469406d4b0e94ceb4f18cb79a35683abd42e84`); fresh ADT
  `work/guacamole-0249.adt` (105,596 bytes, `--highmem --guacamole`); test boot image
  `outputs/guacamole-0249-aw8697-sweethaptic2.img` (21,577,728 bytes, SHA-256
  `8e944841d0a7a6f1dff15f047ea9ebb74a57397b8f850bc5a3fb5bc7a0989a12`). Payload is
  `outputs/payload-guac-0249-aw8697-sweethaptic2.bin`; exact boot args are in
  `outputs/guacamole-0249-sweethaptic2-boot-args.txt` (same 0248 test args, refreshed `sweettime`, includes
  `sweethaptic=2`). The packaged ramdisk was unpacked and byte-compared with the payload. Image has not been booted.
- Test: build through 0249, boot with sweethaptic=2. Phone: `mgtool setbool DeviceSupportsHaptics true`,
  killall -9 SpringBoard (or reboot); Settings > Sounds & Haptics: System Haptics on, Keyboard Feedback > Haptic.
  Then keyboard / switches / long-press; `dmesg | grep -a -e "SWEET VIBRATOR" -e "SWEET HAPTIC"`; and
  `grep -a "_UIKBFeedbackGenerator.*Updating mode"` in an oslog capture (want "Haptics: supported"? level 1 still
  logs unsupported: only 2 counts as supported there, see REQUEST-F).
- Caveat: keyboard haptics in _UIKBFeedbackGenerator want level 2; at level 1 the keyboard may stay audio-only
  while UIKit's impact/selection/notification generators and system vibration use this path.

# Active handoff — 0248 AW8697 haptics driver (stages 1 + 2) (2026-11-10, Claude)

- RESULT (user, 09:05): WORKS. `haptic 5` feels like a keyboard tap; `haptic buzz` like the charging buzz.
- Hardware (research/haptics/README.md): motor = Awinic AW8697 @ I2C 0x5a on QUP wrapper 0 SE7 (0x89c000),
  SDA/SCL GPIO98/99 (south, qup7 = mux 1), reset-gpio 116 (south; the DT *property* is what OnePlus's driver
  uses), irq-gpio 24 (east, unused), GCC vote 0x5200c bits 6/7/17, SE7 RCG 0x17998 (DFS), branches
  0x17004/0x17008/0x17994 (Request D). f0 168.9 Hz -> aw8697_haptic_168.bin.
- NEW xnu/patches/0248-guacamole-aw8697-haptics.patch: bsd/dev/sweet_haptic.c (+ bsd/conf/files, called from
  sweet_ufs.c after sweet_touch_init). Thread starts after sweethapticdelay (50 s). Boot-arg sweethaptic:
  0 off; 1 (default) probe only: clocks, SE7 init (stops with a log line if the SE has no I2C protocol firmware:
  `SE7 has no I2C protocol firmware loaded by the boot chain`), reset pulse, ID (want 0x97), register dump;
  2: soft reset, restore Android's idle configuration (table from dtb/haptics/aw8697-registers-android.txt,
  config registers only), RAM load from ADT /sweet-haptic "ram", one click, dump. sysctl kern.sweet_haptic:
  N plays waveform N, -ms buzzes (waveform 10 looped), 0 stops.
- tools/mkadt.py --guacamole adds /sweet-haptic { ram = dtb/haptics/aw8697_haptic_168.bin } (checksum checked;
  warning + no node if the file is missing). The file is OnePlus's, not in git.
- NEW userland/jb/haptic.c (build.sh: build haptic): `haptic`, `haptic N`, `haptic buzz MS`, `haptic stop`.
- Built (2026-11-10): kernel through 0248 at `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet` (17,035,336 bytes); `work/guacamole-0248.adt` includes `/sweet-haptic` with the 5,826-byte RAM image; `sh userland/build.sh` succeeded, including `userland/out/haptic` and the other userland tools. Default probe image: `outputs/guacamole-0248-aw8697-haptics.img` (21,577,728 bytes), SHA-256 `de85a1f0f390bb5e43b77b2adeef48c599dac0ff1fb6b5ee993fea2fadd13d84`, packaged with the fullmetal template/shim and the established 0239 boot arguments (refreshed `sweettime`); exact args in `outputs/guacamole-0248-boot-args.txt`. Test image with `sweethaptic=2`: `outputs/guacamole-0248-aw8697-sweethaptic2.img` (21,577,728 bytes), SHA-256 `475c7afa27c839847991eee16bc28312e2425d4c6042c70918388ed77de13f6c`; exact args in `outputs/guacamole-0248-sweethaptic2-boot-args.txt`. Neither image booted or installed.
- Test A (safe): build through 0248, new ADT, default boot-args (sweethaptic=1); after ~1 min
  `dmesg | grep -a "SWEET HAPTIC"`: want `id 0x97` + 8 dump lines matching the Android dump's values.
- Test B: add sweethaptic=2: a click ~50 s after boot; then `haptic 1` .. `haptic 20`, `haptic buzz 300`.

# Known limit — Weather / Stocks have no data: FairPlay (Mescal) signatures (2026-11-10, Claude)

- Weather shows dashes for every city, Stocks "Stocks unavailable". Stocks' log: `AMSFairPlayGetHardwareInfo:
  Failed to fetch hardware info. Error = -42184` -> `SDSAuthTokenService failed to generate mescal signature`
  (AMSErrorDomain 12 "Mescal Failed"); requests are never sent. Weather: no network activity at all, same layer.
- FairPlay (fairplayd.H2 job removed in v13; no FairPlay kernel extension) is Apple's DRM / device
  authentication tied to genuine hardware. Not emulated, by decision: services that need Mescal-signed
  requests (Stocks' data service, WeatherKit) stay empty. News WORKS (user, 08:14): not every Apple service needs
  Mescal. Third-party apps with their own back ends are unaffected.
- Not caused by CommCenter (stopping it changed nothing) or by the network path (no offline banner; Safari works).

# Active handoff — vibration / Taptic haptics: research first (2026-11-10, Claude)

- User wants the vibration motor, Taptic-style if possible. Guacamole DT identifies an AW8697 on I2C as the
  actual motor driver; the PM8150B haptics peripheral is not the motor path, so no SPMI haptics writes are
  needed. Request C's OnePlus AW8697 source is now copied to `dtb/haptics/src/`; provenance and the verified
  waveform-bin layout are in `research/haptics/README.md` and `dtb/haptics/src/SOURCE.md`. iOS 16 findings and
  the remaining uncertainty about the concrete IOKit hardware endpoint are in `research/haptics/README.md`.
  No kernel implementation yet.

# Active handoff — Location Services row missing = no CommCenter (2026-11-10, Claude)

- RESULT: with CommCenter back the row appears and toggles, but CommCenter crash-loops (NULL in
  ApplePDPHelperInterface::callStructMethod from the AQMController queue, ~5 s after start). Analysis + Codex request:
  research/commcenter/README.md (crash report there too).
- Settings > Privacy & Security shows everything except "Location Services". oslog Preferences:
  `-[PUIPrivacyController refreshLocationServicesAvailable]: Error retrieving subscription info: ...
  com.apple.commcenter.coretelephony.xpc was invalidated: failed at lookup with error 3 - No such process`.
  MobileGestalt location-services is already true; locationd is stable. CommCenter was removed from launchd.plist
  in v13 (crash loop from objc block trampolines, fixed by the 0044-era cryptex links; and Setup blocked on it,
  Setup is skipped since v14). vmd's 100% spin is likely the same missing CoreTelephony.
- NEW tools/sweet-root-tweaks.py `--add-job ORIGINAL_LAUNCHD_PLIST LABEL IN OUT`: copies one job from the IPSW's
  unmodified launchd.plist (Codex has it from mkiosroot's extraction) into the phone's.
- Test (Codex + user): pull /System/Library/xpc/launchd.plist from the phone (keep a backup there:
  /var/local/launchd.plist.pre-commcenter), run `--add-job ORIG com.apple.CommCenter phone.plist new.plist`, push,
  reboot. Check: `ps -A -o pid,%cpu,time,comm | grep -i commcenter` (stable pid, low CPU), Settings > Privacy row,
  vmd CPU. Rollback: copy the backup back, reboot. If it works: drop com.apple.CommCenter from REMOVED_JOBS.

# Active handoff — 0247 MSAA resolve targets (Maps black map, ROOT CAUSE) (2026-11-10, Claude)

- RESULT (user, 07:43): Maps' map is no longer black. 0247 confirmed.
- 0246 RESULT: every Maps drawable 64/64 black at present. Maps' passes: color 1125x2436 fmt 81 load DontCare,
  store 2 = MTLStoreActionMultisampleResolve: it renders 4x MSAA and resolves into the CAMetalLayer drawable.
  The bridge ABI (NmbPassAttachment) had no resolve texture at all, so the resolve target was never written.
- CHANGE (0247), needs the Rust bridge rebuilt (tools/build-m2v-bridge.sh) AND the bundle (userland/metal2/build.sh):
  - bridge/NativeMetalObjects.h + .rs: NmbPassAttachment gains resolve (NmbTexture*), resolve_level, resolve_slice,
    resolve_depth_plane (appended; header and Rust #[repr(C)] match); nmb_render_encoder_new sets
    resolve_texture/_level/_slice/_depth_plane on colour, depth and stencil attachments (metal2vulkan-runtime
    render_pass.rs already supports them).
  - NativeBridgeMetalFrontend.mm: attachment() passes a.resolveTexture (owned, surface-tracked, drawsToSurface),
    logs `pass resolves WxH (N samples) into WxH fmt F surf S #n (0247)` (first 3, every 300th).
  - Not compiled. Any app using MSAA with a resolve texture (games, Maps, MapKit views) was affected.
- 0243/0245 waits and the 0244/0246 logging stay for now (cheap); remove 0246 sampling once Maps is confirmed.

# Active handoff — 0243 Maps black map: presentsWithTransaction drawables (2026-11-10, Claude)

- 0245 RESULT: never fired (no in-flight GPU work on the drawable at present): timing is not the cause; still black.
- 0246 (same file): the present hook samples the drawable (prePresentBlackSamples, 8x8) for presents 1-10 and
  every 300th: `present #n frame WxH fmt F surf S: B/64 black (0246)`. Black -> Maps' rendering produces black
  (translation); not black -> backboardd/CA composition of presentsWithTransaction drawables.
- 0244 RESULT: Maps' layer 1125x2436 fmt 81 presentsWithTransaction=1, -[drawable present] after each
  nextDrawable; the 0243 wait fired only twice (after 5 presents): Maps presents without waitUntilScheduled.
- 0245 (same file): the swizzled -[drawable present] first waits (GPU only, nmb_command_buffer_wait) on every
  in-flight command buffer that uses the drawable's texture (nmb_command_buffer_uses_texture), logged
  "drawable present waited for its GPU work #n (0245)". Rebuild/install bundle, reboot, test Maps.
- RESULT: Maps still black (confirm the installed bundle had 0243). 0244 (same file): logs CAMetalLayer
  nextDrawable (size, fmt, presentsWithTransaction, framebufferOnly) and -[drawable present] (swizzled, not in
  backboardd; first 5 then every 300th), and the 0243 wait when it fires. Rebuild/install, reboot, log on, open Maps:
  `grep -a "Maps\]" /var/local/mtl2.log | grep -e nextDrawable -e "drawable present" -e 0243`. No nextDrawable at
  all = the map is not a CAMetalLayer (IOSurface layer contents: then the IOSurface seed after GPU writes).
- Maps: map area black. mtl2.log: no refusals/MISSING, 186 pipelines OK, map renders (work commit #60: 75 draws
  into a 1125x2436 fmt 81 IOSurface target), but NO "drawable queued" line ever: Maps never calls
  presentDrawable: on a command buffer. GeoServices cache has ResourceManifest/Resources (data side fine).
  Hypothesis: VectorKit's CAMetalLayer uses presentsWithTransaction (commit, waitUntilScheduled, [drawable present]);
  CA composites the IOSurface before Turnip has drawn it (our fence is invisible to CA) and the on-demand map
  never redraws -> black.
- CHANGE userland/metal2/NativeBridgeMetalFrontend.mm (0243): NBCommandBuffer.drawsToSurface set when a render
  pass attachment is an IOSurface; waitUntilScheduled then also waits for the GPU (nmb_command_buffer_wait, not the
  handler group: no deadlock with main-queue handlers). Skipped in backboardd. Not compiled.
- Test: rebuild/install bundle (with 0242), reboot, open Maps: map visible? If still black, next: log
  CAMetalDrawable -present calls in Maps (presentsWithTransaction vs plain present) with the trace flag.

# Active handoff — profile backboardd before touching the Rust bridge / blur (2026-11-10, Claude)

- First capture useless: tsample picked thread #23 at 0.5% (instantaneous cpu_usage), ~95% of samples in
  mach_msg. CHANGE userland/jb/tsample.c: picks the thread with the most CPU time over 1 s (user+system deltas),
  lists the top 5, optional 4th arg THREAD index. Rebuild + reinstall tsample. User then runs
  `sleep 4; tsample PID 1500 > /var/local/bb-scroll.txt` and scrolls during the sleep and the ~7 s capture.
- Home-screen scrolling now ~45-48 fps (was 19.5) after removing CPU hogs; GPU 60-71% busy, backboardd ~140% CPU.
- User wants cheaper blur or a faster Rust half. The bridge (metal-work/metal2vulkan, 220k lines in
  metal2vulkan-runtime) is already built with fat LTO, codegen-units 1, cortex-a76. Its per-draw path
  (render.rs draw -> draw_resident: sync_heaps, degrade_* checks, coverage/routing tracking per draw) is generic;
  no blind changes: profile first.
- NEEDED (Codex):
  1. Build tsample (not in userland/build.sh): `xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2
     -o out/tsample userland/jb/tsample.c && codesign -f -s - --entitlements userland/jb/tsample.plist out/tsample`,
     install /var/local/bin/tsample (root, 0755).
  2. User scrolls the home screen continuously; meanwhile on the phone:
     `tsample $(ps -A -o pid,comm | awk '/backboardd$/{print $1}') 1500 > /var/local/bb-scroll.txt`
     (awk may be missing: use the pid from ps). Copy it to research/metal/bb-scroll.txt.
  3. On the Mac: `tools/tsample-report.py research/metal/bb-scroll.txt --dsym userland/metal2/out/SweetMetal2.dSYM
     > research/metal/bb-scroll-report.txt` (NEW tool: atos-symbolicates SweetMetal2 frames, self/total %). The dSYM
     must be the one of the bundle installed on the phone (the 0242 build).
  4. Telemetry for pass counts (blur cost): `touch /var/local/mtl2-telemetry /var/local/mtl2.log; chmod 666
     /var/local/mtl2.log`, reboot, scroll 20 s, copy the two last "telemetry" blocks of mtl2.log (R passes, D draws,
     F full-screen passes, M biggest pass) to research/metal/telemetry-scroll.txt; then remove both files again.

# Active handoff — home screen lag = CPU hogs; btbridge busy loop fixed (2026-11-10, Claude)

- Home-screen scrolling 19.5 fps with GPU busy 10% of vsyncs: CPU-starved. `ps -A -o pid,%cpu,time,comm`:
  vmd 107% (visual voicemail, no CoreTelephony; the old launchctl disable did not survive), btbridge 99.6%,
  CalendarWidgetExtension 99%, coreduetd 82%, backboardd 29%, launchd 25%, biomed 20%, wland 17%.
- FIX userland/bt/btuart.c drain(): was uart_read(.., 3) = a 3 ms MMIO busy-poll on every btbridge main-loop
  pass (~100% of a core forever). Now empties the RX FIFO and returns at once (whole words when the buffer is
  nearly full; partial word left in the FIFO); btbridge's own usleep (0.5 / 5 ms) does the waiting. Also changes
  btuart's test modes (firmware download, scan) to non-waiting drains; their final uart_read waits are unchanged.
  Syntax-checked only. Rebuild userland (build.sh), install /var/local/bin/btbridge (.new + mv), `launchctl
  kickstart -k system/com.sweet.btbridge` (or reboot). Check: ps shows btbridge ~0-5%; AirPods still connect/play.
- User side: `launchctl disable system/com.apple.voicemail.vmd` and `user/501/...`, kill vmd and the calendar widget.

# Active handoff — 0242 frontend per-draw overhead (2026-11-10, Claude)

- @2x dropped: 828x1792 also showed the red rdar bar (user keeps @3x). Boot image A (sweetcpufreq=2, no
  sweetfb_scale, normal ADT); user restores the MobileGestalt cache from /var/local/MobileGestalt.plist.at3x.
  0240/0241 stay in the series (no effect without sweetfb_scale=2).
- CHANGE userland/metal2/NativeBridgeMetalFrontend.mm ("0242", frontend only, no kernel change):
  - Diagnostics that ran on EVERY bind/draw now only when /var/local/mtl2-trace exists (traceOn(), read once):
    0182 texture/sampler associated dictionaries + stringWithFormat per fragment texture/sampler bind, pipeline
    "pipe" association per setRenderPipelineState, traceBigTextureDraw per draw, encoder "target" string per
    render pass, 0188 watchedSurface mutex + NSNumber per texture bind.
  - Cull/winding/fill: plain ivars instead of associated NSNumbers; nmb_render_set_raster only when one changes.
  - resourceOwners / surfaces: skip addObject when the same buffer/texture is bound again in the encoder
    (_lastOwner[2], identity only; the earlier object is retained by the command buffer, so its address cannot be reused).
- Rebuilt with `METAL_WORK="$PWD/../metal-work" sh userland/metal2/build.sh`; ad-hoc signature verified. Installed at
  `/System/Library/Extensions/SweetMetal2.bundle` on `192.168.0.51` through a staged copy and rename. Prior bundle
  preserved as `SweetMetal2.bundle.pre-codex-20261110-064942`. Installed `SweetMetal2`, Turnip, and CodeResources
  checksums match the local build. SHA-256: `0ecfd83b432286baaaa136c88d3b4d2b110f8c98e06f72fe15567cfb16938cc8`
  (SweetMetal2), `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5` (Turnip), and
  `9f7aea4c96dd1fb9c4b3c0769554ecede146d1fc025d1a81e6dd788f4bba79c4` (CodeResources). No reboot performed;
  restart/reboot before testing so existing processes load the new bundle.
- Measure before/after: Subway Surfers ~20 s, `dmesg | grep -a "SWEET FB: 5s: .* swaps" | tail -4`, and
  `tsample` on SubwaySurf's busiest thread (top frames). If the bridge itself (libnative_metal_bridge, Rust,
  outside this repo in metal-work/m2v-bridge) dominates, that is the next target.

# Active handoff — 0241 @2x screen = 828x1792 (2026-11-10, Claude)

- Image B (750x1624 @2x): red rdar://45025538 bar at the top = UIKit has no status bar layout for 375x812 pt @2x.
- NEW xnu/patches/0241-guacamole-fb-scale2-xr-size.patch (after 0240): sweetfb_scale=2 -> 828x1792 (414x896 pt @2x,
  iPhone XR/11 screen, same 19.5:9 aspect; 1.85x fewer pixels than 1125x2436). mkadt.py --scale2 now writes canvas
  828x1792 (display-scale 2). Rebuild image B with kernel through 0241 + new --scale2 ADT; same boot-args.
- On the phone before booting it: move the MobileGestalt cache aside again (it now holds the 750x1624 answers);
  after boot re-apply `mgtool setstr WifiVendor USI` and `mgtool setint ArtworkTraits/ArtworkDeviceSubType 2556`.
- If UIKit still shows the red bar: the XR layout may also need ArtworkDeviceSubType 1792 (XR) instead of 2556,
  which would mean no Dynamic Island at @2x.
- Rebuilt kernel through 0241 successfully. Kernel SHA-256:
  `d133e619e639450317ed7621b0efef2d966f69560b4ae3d9e58160fc5cddc00a`.
- Rebuilt Image B with a freshly generated `mkadt.py --highmem --guacamole --scale2` ADT and the previous B boot
  arguments (including `sweetcpufreq=2 sweetfb_scale=2`; refreshed `sweettime`):
  `outputs/guacamole-0241-B-cpu-clocks-scale2-xr.img` (21,553,152 bytes), SHA-256
  `bd76e57430b7a411378d68eb984485e16d2fde5be1058b67a6f5bf9a02373cb8`. Boot args are in
  `outputs/guacamole-0241-B-boot-args.txt`. Not booted yet.

# Active handoff — performance: CPU clocks + @2x rendering (2026-11-10, Claude)

- User wants (1) CPU clocks up and (2) rendering at @2x.
- (1) No code change: sweet_cpufreq.c defaults to sweetcpufreq=1 (read/log only; cores stay at the boot's rate).
  Boot-arg `sweetcpufreq=2` asks both A76 domains (cpu 4-6 2419.2 MHz, cpu 7 2956.8 MHz) for their top row.
- (2) NEW xnu/patches/0240-guacamole-fb-scale2.patch (SweetFramebuffer.cpp, after 0239): boot-arg `sweetfb_scale=2`
  -> presented screen 750x1624 (375x812 pt @2x) instead of 1125x2436; panel blit upscales x1.92. Boot log line
  `SWEET FB: presented screen 750x1624 (@2x, 0240)`. tools/mkadt.py `--scale2`: /chosen display-scale 2 and
  disp0 canvas 750x1624 (checked: only those 5 bytes differ from a normal --highmem --guacamole ADT).
  BOTH are needed together (ADT and boot-arg must agree).
- Build TWO images so the effects can be told apart:
  A: kernel through 0240, normal ADT, current boot-args + `sweetcpufreq=2`.
  B: same kernel, ADT `mkadt.py ... --highmem --guacamole --scale2`, boot-args + `sweetcpufreq=2 sweetfb_scale=2`.
- Before booting B (user, over SSH): MobileGestalt caches the screen size/scale. Record the custom answers, back
  up the cache and move it aside (rebuilt at boot), then re-apply the answers after B boots:
  `mgtool get metal gamekit WifiVendor ArtworkTraits` (save output);
  `cp -p .../com.apple.MobileGestalt.plist /var/local/MobileGestalt.plist.at3x` and `mv` it to `.at3x` in place.
  Rollback: boot A / old image, put the .at3x copy back, reboot.
- Risk: the Dynamic Island comes from ArtworkTraits/ArtworkDeviceSubType 2556 (iPhone 14 Pro, an @3x device);
  at @2x SpringBoard may lay it out wrong or not at all.
- Built kernel through 0240 successfully. Kernel SHA-256:
  `995fe52a9057644a8f6f804fdb4a3dc47f51f8332c15e1a1c793e6d7e0a3dbb0`.
- Image A (CPU clocks at native 1125x2436): `outputs/guacamole-0240-A-cpu-clocks-native-resolution.img`, SHA-256
  `368a3a40f4ee78d64c0fd56aadbbd1ef766e57c5d3d97cf087d90e0a6546c1b9`.
- Image B (CPU clocks plus 750x1624 @2x canvas): `outputs/guacamole-0240-B-cpu-clocks-scale2.img`, SHA-256
  `73231b4e9c4578fde785d8e713b8ad02f8ceb06faf9878f6fcc2f34f20f65447`. Both use the same kernel and refreshed
  base boot arguments; only Image B uses the scale2 ADT and adds `sweetfb_scale=2`. Boot args are recorded in the
  corresponding `outputs/guacamole-0240-*-boot-args.txt` files. Neither image has been booted yet.

# Active handoff — 0239 island drawn as strips (2026-11-10, Claude)

- User on 0237: the "square" is still there. The island is a pill copied as its bounding box without blending
  (CP_BLIT, no blend), so its transparent corners show black.
- NEW xnu/patches/0239-guacamole-fb-island-strips.patch (SweetFramebuffer.cpp, after 0237): sweet_fb_top_band_box
  now returns up to 16 horizontal strips (struct sweet_fb_island), each as wide as the island's opaque black
  pixels in its rows; presentGPU copies each strip from the CA layer and the game layer above/below the island
  and left/right of each strip (every panel pixel still written once). blits[] = 4 + 3*16 on the stack (~2.9 KB),
  IB ~1.7k dwords of 4k. New log (first 4, then every 600th): `SWEET FB: island: N strips, logical box ...`.
- patch --dry-run OK on 0236+0237. Not compiled.
- Test: build through 0239 (0238 is sweet_gpu.c, independent); Subway Surfers with a timer: pill edges should be a
  fine staircase, no square. `dmesg | grep -a "SWEET FB: island"` gives the box size if it is still wrong.
- Built successfully through 0239 with `XNU_LOCAL="$PWD/work/xnu-local-latest" sh tools/xnu-local-build.sh`; kernel
  includes the 0238 recovery code and 0239 strip log. Kernel SHA-256:
  `edd0e6320cf17b1fe4110c83246736e32101687c1760e6b521bfd2dfdd01c564`.
- Boot image: `outputs/guacamole-0239-fb-island-strips.img` (21,553,152 bytes), SHA-256
  `a2c5fe98a6eccc7ebc111684af65aafef2e0933495c0ed81f6c123f86a859f47`; current ADT generation, fullmetal template,
  shim and 0238's established arguments with refreshed `sweettime`. Args are in
  `outputs/guacamole-0239-boot-args.txt`. Not booted yet.

# Active handoff — 0238 GPU hang recovery (2026-11-10, Claude)

- RESULT (user, 05:37): WORKS. Daily-wheel hang -> `RECOVER #1..#3 done` (GBIF acks 1 1 1, ME_INIT ran,
  SECURE_MODE(0) ran), user can go home. SCM zap resume returns 0xffcfffb7 (refused) yet SECURE_MODE(0) runs:
  the call is not needed after RBBM_SW_RESET (harmless; could be dropped). The game re-hangs every few seconds
  (stuck job = SubwaySurf, 3 IBs, IB1 rem 0x3c3 as in 0179): the looping shader itself is still to be found.
- User asked to fix the GPU hard locks (one stuck shader, e.g. Subway Surfers' daily wheel per the 0179 report,
  freezes the phone until reboot; killing the app does not help: no GPU reset existed).
- NEW xnu/patches/0238-guacamole-gpu-hang-recovery.patch (bsd/dev/sweet_gpu.c; independent of 0236/0237):
  - stage 7's register block moved into gpu_hw_regs(gpu, gmu) (no change for boot); scm_call -> scm_call_svc(svc, ...)
    with scm_call kept as the PIL (svc 2) wrapper.
  - sweet_gpu_hang_check(may_recover): a real waiter (sweet_gpu_wait_fence_ns with timeout != 0; never the display
    path's timeout-0 polls) that sees the done fence stuck >= sweetgpurecoverms (default 3000, min 500, 0 = off)
    with jobs queued takes gpu_be_lock and runs sweet_gpu_recover_locked(): CP_SQE_CNTL=3, GBIF halt sequence
    (msm a6xx_bus_clear_pending_transactions), RBBM_SW_RESET_CMD (0x43) 1 -> 200 us -> 0, gpu_hw_regs again, ring
    from 0 with CP_ME_INIT + WHERE_AM_I, SCM svc 1 cmd 0x0a (BOOT_SET_REMOTE_STATE(0,0) = zap resume),
    CP_SET_SECURE_MODE(0). be_wptr = new w; fence page = last queued fence (stuck + queued jobs dropped).
    On failure: be_ready = false (submits refused, display on CPU) instead of a frozen phone.
  - Logs: `SWEET GPU: RECOVER #n: ...` (stuck job, GBIF acks, ME_INIT, SCM status, SECURE_MODE, done/FAILED).
- Checked: patch --dry-run on the work/xnu-local-latest tree. Not compiled (no Darwin toolchain here): check the
  build for warnings-as-errors (unused results, the moved static const arrays in gpu_hw_regs).
- UNTESTED RISKS: RBBM_SW_RESET without the GMU power collapse msm does; whether the zap resume SCM is accepted;
  SweetFramebuffer's fbgpu state after the ring restarts. Fallback boot-arg: sweetgpurecoverms=0.
- Test: build through 0238, boot, reproduce a hang (Subway Surfers daily wheel), wait ~5 s, then
  `dmesg | grep -a -e "SWEET GPU: RECOVER" -e "SWEET GPU: HANG" | tail -20`.
- Built successfully through 0238 with `XNU_LOCAL="$PWD/work/xnu-local-latest" sh tools/xnu-local-build.sh`; kernel SHA-256:
  `96b1f330f1e3d07b9387bb88f7d7add572ced028073ce21243f101dfd1d74e98`. The linked kernel contains the recovery log.
- Boot image: `outputs/guacamole-0238-gpu-hang-recovery.img` (21,553,152 bytes), SHA-256
  `fa171d016db6ecc5388efafc914e84a7ec04f8ec75791749e347890907311ebf`, packaged with current ADT generation and
  the existing 0237 boot arguments (refreshed `sweettime`). Recovery remains at its default 3000 ms threshold;
  `sweetgpurecoverms=0` is not set. Args are in `outputs/guacamole-0238-boot-args.txt`. Not booted yet.

# Active handoff — 0237 island box: opaque black pixels only (2026-11-10, Claude)

- User on 0236: works ("island box" 375/377 per 5 s), but a black box/strip over the top of Subway Surfers.
  Cause: the box took every pixel with alpha != 0 in the CA layer's top band, so the status bar clock, icons and
  shading widened it to the full band; it is copied without blending, so the transparent parts show black.
- NEW xnu/patches/0237-guacamole-fb-island-box-black-only.patch (after 0236): the box only counts opaque
  (alpha >= 0xf0), black (RGB sum <= 48) pixels: the island pill. patch --dry-run OK on top of 0236.
- Test: build through 0237; Subway Surfers with a timer: only the pill (small square black corners at most).
- Built successfully with `XNU_LOCAL="$PWD/work/xnu-local-latest" sh tools/xnu-local-build.sh`; patches through 0237 are
  recorded in `work/xnu-local-latest/applied-patches.txt`. Kernel SHA-256:
  `3230730b523f10a78afa578e60716b4b2492cec7238ec366ad3b62bfaee1b582`.
- Boot image: `outputs/guacamole-0237-fb-island-black-only.img` (21,553,152 bytes), SHA-256
  `89d71119aca3bc7d3446267ede67db14a23b1616442016ef2f25fac1ee5359ff`. Packaged from current `mkadt.py
  --highmem --guacamole`, fullmetal template and shim, with the established 60 Hz GPU-present arguments and refreshed
  `sweettime`; 0237 is enabled by default. Boot args are in `outputs/guacamole-0237-boot-args.txt`. Not booted yet.

# Active handoff — 0236 Dynamic Island box on the GPU path (2026-11-10, Claude)

- User on 0225: Subway Surfers much slower, island still missing (one crash, no report found, phone did not reboot).
  Cause: 0225 made every game frame with a visible pixel in the CA layer's edge bands fail
  sweet_fb_layer_transparent, so presentGPU returned 0 and presentComposed CPU-blended the full screen; there its
  own 8x8 alpha grid (zero == 64) skipped the CA layer anyway, so no island.
- NEW xnu/patches/0236-guacamole-fb-island-box-on-gpu.patch (SweetFramebuffer.cpp, after 0225): removes 0225's band
  scan (transparent test is the 0185 grid again); when the top layer is grid-clear, sweet_fb_top_band_box() finds the
  box of visible pixels in its top 1/16 (every 2nd row, 4th px, padded 4 px). If found, the frame stays on the GPU:
  top layer blitted only inside that box, game layer around it (existing 4-cut code). Bottom band (home indicator)
  not drawn. Boot-arg sweetfbisland=0 disables. 5s log: "of those, drawn with the top layer's island box (0236): N".
- Checked: patch --dry-run applies on the work/xnu-local-latest tree (0001-0235). Not compiled yet.
- Test: build through 0236, boot, Subway Surfers with a timer running: island visible, speed as before 0225;
  `dmesg | grep -a "island box"`.
- Built through 0236 successfully with `XNU_LOCAL="$PWD/work/xnu-local-latest" sh tools/xnu-local-build.sh`; the applied
  patch ledger records 0001-0236. Kernel:
  `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet`, SHA-256
  `2cf3b513ef8534ef1cd2b8fcb24bf3dfed92c74d999d9e0a0703fee0c21e038b`.
- Boot image: `outputs/guacamole-0236-fb-island-box-gpu.img` (21,553,152 bytes), SHA-256
  `e46ac3b089190f1da724f59c326b05a078c14718488a85f908a5424321d4008a`. It was packaged with the current
  `mkadt.py --highmem --guacamole`, fullmetal template and shim, the established 0235/0233 GPU-present arguments,
  and refreshed `sweettime`; `sweetfbisland=0` is omitted so 0236 is enabled by default. Boot args are in
  `outputs/guacamole-0236-boot-args.txt`. Not yet booted on the phone.

## 2026-11-10 04:00: mute property fix

CoreAudio's HALS_IOA2Device resolves device-level custom properties against instantiated controls; it does not
consume a separate top-level `mute` key. See research/audio/ioa2-mute-property-0400.md. 0233 changed the mute control
base class from `'ctrl'` to `'togl'` at the same time it changed output channels, so the prior boot did not isolate
which change prevented `mute/outp/3,4`. Added 0235 to restore `'ctrl'` while retaining four output channels. Build
and boot 0235, then verify VirtualAudio creates the speaker / receiver ports (and query `mute/outp/4` directly).
The 0234 headset-personality boot-argument experiment remains available if needed. Patches 0226-0235 apply in order.

Build completed locally: `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet`
(SHA-256 `06e68e296b45b226c2631ecea8afa1fdde6dbff7b52c0ab45f3ce454cf2989f1`) includes 0234 and 0235. Boot image:
`outputs/guacamole-0235-ioaudio2-mute-ctrl.img` (SHA-256
`e41c8596fd72391c18a0d37e120edee54db230521df33fef5beb36cb4a0b9a6f`, 21,536,768 bytes). It uses the prior 0233
working-image arguments, retaining `rd=ufsroot0 ufsroot=ios`, 60 Hz GPU-present settings, and no forced
`sweetfb_refresh`/`sweetfb_vsync`; only `sweettime` was refreshed. The image has not been booted on the phone yet.

Rebuilt on request at 04:12 with the same kernel and regenerated ADT/payload. Refreshed boot arguments are recorded in
`outputs/guacamole-0235-rebuilt-boot-args.txt` and verified in the image header. Fresh image:
`outputs/guacamole-0235-ioaudio2-mute-ctrl-rebuilt-20261110.img` (21,536,768 bytes, SHA-256
`33efcc66d221dcb95bb22ad7c75b52e2d07b019f00b9f38d6f3f73b9d4633b14`). Kernel SHA-256 remains
`06e68e296b45b226c2631ecea8afa1fdde6dbff7b52c0ab45f3ce454cf2989f1`.

## 2026-11-10 03:50: 0233: still no speaker port; Codex request "03:50"; 0234 terminal boot-args

0233 changed nothing visible: no "creating a speaker port", AQMEIO "Error -1 getting output port ID" on every queue.
Likely Device_Null_Aspen ('null') never reaches the codec block. Codex request in research/audio/README.md "03:50".
NEW xnu/patches/0234-guacamole-ioaudio2-terminal-bootargs.patch (after 0233): sweetaudioterm / sweetaudiointerm
(FourCC numbers) set the output / input stream terminal types (defaults 'spkr' / 'micr'); boot log line "SWEET
AUDIO: transport ..., out terminal ..., in terminal ...". Candidate to try without a kernel build:
sweetaudiotp=0x6d627573 sweetaudioterm=0x68647068 sweetaudiointerm=0x686d6963 ('mbus' / 'hdph' / 'hmic').

## 2026-11-10 03:40: 0232: no more timeouts / 'what'; apps fail "Error -1 getting output port ID" -> 0233

With 0232 the route on SweetBuiltInAudio comes up cleanly (StartIOProcID, no nsrt/pft timeouts), but audio queues
fail AQMEIO_HAL "Error -1 (-1) getting output port ID": only a 'pnul' port; no "creating a speaker/receiver port"
(mute el 3/4 controls not surfaced). NEW xnu/patches/0233-guacamole-ioaudio2-four-out-channels.patch (after 0232):
output stream 4 channels (ring sized 2 * 4 bytes/frame), mute controls base class 'togl' instead of 'ctrl'.
Dry run OK. If still no speaker port: ask Codex how HALS_IOA2 maps an IOAudio2 control dict to a HAL mute property
(scope / element validation) and whether Device_Null_Aspen (transport 'null') runs the codec setup at all; or try
another transport via boot-arg sweetaudiotp.

Built with `XNU_LOCAL="$PWD/work/xnu-local-latest" sh tools/xnu-local-build.sh`; 0233 is recorded in
`work/xnu-local-latest/applied-patches.txt`. Kernel SHA-256:
`d241643d916c2b7050ca2d0d2288833211925b781ac7333ade0eb6c2625715d0`. Boot image:
`outputs/guacamole-0233-ioaudio2-four-out-channels.img` (21,536,768 bytes), SHA-256
`b024c100ab74902458f48b57701463dd656c294831f8281522252e16a094de47`; packaged with current
`mkadt.py --highmem --guacamole`, the existing fullmetal template and shim, and the 0232 boot args with `sweettime`
refreshed. Image has not been booted.

## 2026-11-10 03:30: 0231 works (format list OK, StartIOProcID on SweetBuiltInAudio); next: device rate 0 -> 0232

ms-0231: "Server_Internal_StartIOProcID: 93 SweetBuiltInAudio (VAD [vdef] AggDev 2)"; new failure: "timed out ...
[nsrt/glob/0] ... on id 33", "Error 'what' setting sample rate to 48000 Hz on HAL device 33", nominal rate reads 0 ->
falls back to 44100 (unsupported) -> virtual format 0 Hz. No sel 10 start seen yet. NEW
xnu/patches/0232-guacamole-ioaudio2-device-rate.patch (after 0231): device-level "sample rate" / "nominal sample rate"
/ "current sample rate" = 48000 << 32 (key unconfirmed: all three), sel 4 logged + answered with 'conf'. Dry run OK.
Codex could confirm the HAL's device nominal-rate key (HALS_IOA2Device) to drop the extra two.

Built with `XNU_LOCAL="$PWD/work/xnu-local-latest" sh tools/xnu-local-build.sh`; 0232 is recorded in
`work/xnu-local-latest/applied-patches.txt`. Kernel SHA-256:
`40427cc5a12a121446e9b38757d2653c4b71a8e1b685719c80f011b0a883117e`. Boot image:
`outputs/guacamole-0232-ioaudio2-device-rate.img` (21,536,768 bytes), SHA-256
`a930de7944b569b8a46cf8585892ac3c4ea36578ae15b7847699b8eb8584b203`; packaged with current
`mkadt.py --highmem --guacamole`, the existing fullmetal template and shim, and the 0231 boot args with `sweettime`
refreshed. Image has not been booted.

## 2026-11-10 03:20: 0230 conf/ack works; real cause of the 2 s loop: 0 Hz in the format list -> 0231

ms-0230.oslog: VirtualStream "Physical stream format list does not contain current physical stream format:
[16/48000/2]" -> "closest matching ... [16/0/2]" -> "Synchronously setting physical format to [16/0/2]" ->
HALPropertySynchronizer "timed out waiting 2000 milliseconds ... [pft /glob/0]" -> "Error 'what' ensuring valid
physical stream format" -> route fails. min/max sample rate were plain 48000 (read as 32.32 = 0 Hz).
xnu/patches/0231-guacamole-ioaudio2-rate-range-fixed.patch (after 0230): min/max sample rate 32.32 in the format
dictionaries and the stream dictionaries. Applies after 0226-0230 (dry run OK).

Built with `XNU_LOCAL="$PWD/work/xnu-local-latest" sh tools/xnu-local-build.sh`; 0231 is recorded in
`work/xnu-local-latest/applied-patches.txt`. Kernel SHA-256:
`7e1cf8422cc0b1c28bf483313b3332771d8309b177f24b33a53998b374a4a70c`. Boot image:
`outputs/guacamole-0231-ioaudio2-rate-range-fixed.img` (21,536,768 bytes), SHA-256
`581731928df4f5917562bc0dd6ba0905e28942f392384cec0ee66f3ee251403f`; packaged with current
`mkadt.py --highmem --guacamole`, the existing fullmetal template and shim, and the 0230 boot args with `sweettime`
refreshed. Image has not been booted.

## 2026-11-10 03:00: 0230 IOAudio2 notifications (conf / goin / halt) + more logging (built; not booted)

Codex: notification msg = 24-byte Mach header, refCon u32 @0x18, count u32 @0x1c, 32-byte events @0x20 {ID u32,
FourCC u32, 24-byte payload}; a 'conf' event goes back unchanged with sel 3; sel 5 itself does not wait; goin/halt are
async after sel 10/11; ASBD rate becomes 32.32 in the wrapper. The 2 s retry cause is still unidentified.
xnu/patches/0230-guacamole-ioaudio2-notifications.patch (after 0229): the UC keeps the notification port + refCon
(released on close / replacement) and sends 'conf' (arg0 = stream ID) after every sel 5, 'goin' after sel 10, 'halt'
after sel 11 (mach_msg_send_from_kernel_with_options, COPY_SEND, as IOUserClient.cpp); logs "sent 'conf' -> kr"
(first 40), every sel 3 ("config change ack") and sel 6 ("stream N active M"). Applies after 0226-0229 (dry run OK).

Built with `XNU_LOCAL="$PWD/work/xnu-local-latest" sh tools/xnu-local-build.sh`; 0230 is recorded in
`work/xnu-local-latest/applied-patches.txt`. Kernel SHA-256:
`5df01b03e45c982a144f3f6c0c6a6cc6d4ca45bfb247c896aa41d15b11e87396`. Boot image:
`outputs/guacamole-0230-ioaudio2-notifications.img` (21,536,768 bytes), SHA-256
`66dc3aed409c9deae46a34f8241bef3aebfab45623d1577fa421d293aa8d2e1c`; packaged with current
`mkadt.py --highmem --guacamole`, the existing fullmetal template and shim, and the 0229 boot args with `sweettime`
refreshed. Image has not been booted.

## 2026-11-10 02:50: 0229: 'vdef' on SweetBuiltInAudio; route loops (set format -> 2 s -> StopIOProc, 'what')

VirtualAudio: aggregate master PhysicalDevice 33 SweetBuiltInAudio (port still 'pnul': transport 'null' ->
Device_Null, no codec speaker port yet); CoreAudio writes "exclusive access owner" (hog) = pid / -1. Every ~2 s:
sel 5 set format stream 2 (our format; ASBD rate integer 48000), no sel 10 start, StopIOProcID, "CAException ...
ProcessRoute: 'what'". Hypothesis: HAL waits for a config-change notification we never send. Codex request in
research/audio/README.md "02:50" (message layout, events waited for). Also: dmesg loses lines (FB stats spam).

## 2026-11-10 02:35: 0228 adopted! then "Could not add PhysicalDevice" (registry write unsupported) -> 0229 (built, not booted)

With 0228 (transport 'null', mute el 3/4, terminal types): "Newly connected device located. UID: SweetBuiltInAudio",
"Creating Device ID: 33", then "Could not add PhysicalDevice ID: 33 ... CAException (-536870201)" = 0xE00002C7
kIOReturnUnsupported from "HALB_UCObject::SetProperty: got an error from the IORegistry". Kernel: mediaserverd mapped
type 0, 0x10000002, 0x10000001 OK, notification port type 0 accepted. NEW
xnu/patches/0229-guacamole-ioaudio2-set-properties.patch (after 0228): setProperties on IOAudio2Device and its user
client accept every key (kept as properties), first 24 writes logged "SWEET AUDIO: <proc> sets <device|user client>
property "<key>" = ...". Built successfully with the local XNU toolchain; 0229 reverse-apply check passes. Kernel SHA-256: `743bc5e66c5c185b4f39a27395cfe445e37e236be72eb6eda013d5b820bd3159`. Boot image: `outputs/guacamole-0229-audio-set-properties.img` (21,536,768 bytes), SHA-256 `d341ad27a1482254e5446b2e0d8a1eff134a81d877e1363a51b0112c7c38b3ac`; same 0228 boot arguments with `sweettime` refreshed and current `mkadt.py --highmem --guacamole`. Image not booted.

## 2026-11-10 02:25: 0228 codec ports: built kernel and boot image (not booted)

Codex (research/audio/devicefactory-codec-addendum.md, codec-controls-addendum.md): DeviceFactory_Aspen handles
null pci thun strk dprt hdmi mbus usb vlri pair blue - NOT 'bltn' (so 0226/0227's device was listed, never adopted);
the codec setup runs from the common per-device path and creates Built-In Speaker / Receiver only if the HAL device
has ('mute','outp', element 4 / 3). IOAudio2 control dict keys: control ID, class, base class, scope, element, value.
xnu/patches/0228-guacamole-ioaudio2-codec-ports.patch (after 0227): transport default 'null' (0x6e756c6c), boot-arg
sweetaudiotp=<FourCC as number> to try others without a kernel rebuild (e.g. 0x6d627573 'mbus'); controls: two mute
controls (IDs 1003/1004, class 'mute', base 'ctrl', scope 'outp', elements 3/4); stream "terminal type" 'spkr' out,
'micr' in. Mic data sources ('dsrc' "selectors" item format unverified) left out. Check after boot: mediaserverd log
for "Newly connected device located. UID: \"SweetBuiltInAudio\"", "creating a speaker port", "creating a receiver
port", then a route + the Beats.
Built successfully at 02:23 with the local XNU toolchain; 0228 reverse-apply check passes. Kernel SHA-256: `313299089300ee23eeb686b1c2279ccbf03f20f460feb7bee7649eefddfa82de`. Boot image: `outputs/guacamole-0228-audio-codec-ports.img` (21,536,768 bytes), SHA-256 `64771d8078fc55fd1b18558c9c519d31922b0c027bffa18274a29e55c95ca71e`; generated from current `mkadt.py --highmem --guacamole`, the 0227 boot args with `sweettime` refreshed, and the existing fullmetal template/shim. `sweetaudiotp` is omitted so the patch default `null` is used. Image has not been booted.

## 2026-11-10 01:50: 0227 booted: IOA2 stream errors gone; VirtualAudio still lists but does not adopt our device

Beats selectable with VirtualAudio back, but every route fails (no built-in speaker/receiver/mic ports: only
Null_Device's 'pnul'), bluetoothd only gets A2DP suspend/reset, never start; ringtone silent, YouTube in Safari crashes
(separate). With 0227 CoreAudio accepts the streams (no "no current format"), but DeviceListener_Aspen lists
SweetBuiltInAudio (id 33) and adopts only Null_Device; nothing logged why; our UC never opened. dmesg rolls over in
minutes (SWEET FB 5s stats), so boot-time SWEET AUDIO lines are lost. Codex request (README "01:50"): the adoption
predicate; also pending "01:45": terminal-type FourCCs + data-source registry keys. bluetooth boot job installed
(launchd.plist on the phone, backup /var/local/launchd.plist.bak): btbridge starts at boot.

## 2026-11-10: 0226 works: CoreAudio activates SweetBuiltInAudio; VirtualAudio back, 'vdef' built; 0227

With 0226 and VirtualAudio.plugin moved back to /Library/Audio/Plug-Ins/HAL/ (done on the phone 00:56), mediaserverd
is stable: "HALS_Device::Activate: activating device 33: SweetBuiltInAudio"; VirtualAudio starts, sees our UID but
adopts only Null_Device, and builds 'vdef' (VirtualAudioDevice_Default, id 70) on an aggregate with master
Null_Device. Our device was skipped because its streams were incomplete: "there is no current format", "there is no
starting channel number". Codex (research/audio/device-tree-virtualaudio-identities.md): no UID whitelist (UIDs with
"VirtualAudioDevice_" are filtered); "current format" = nested dict with 32.32 fixed-point UInt64 "sample rate" + UInt32
fields (no format ID read); "starting channel" UInt32. D22 /product/audio = {wireless-splitter 1,
supports-always-listening 1}; /product builtin-mics 4 (not added to our ADT yet; only a log line).
NEW xnu/patches/0227-guacamole-ioaudio2-current-format.patch (after 0226): "current format" + "starting channel" 1,
format dict "sample rate" as 32.32. Not built. Also: test the Beats now - 'vdef' exists even on Null_Device.

## 2026-11-10: 0223 + 0225 + 0226 kernel / audio stand-in boot image (built, not booted)

Applied the current 0225 and 0226 patches on top of the previously built 0223 tree and built successfully with
`XNU_LOCAL="$PWD/work/xnu-local-latest" sh tools/xnu-local-build.sh`. The 0225 patch had a malformed hunk count;
corrected its header from `-1270,15 +1270,38` to `-1270,13 +1270,35` (patch body unchanged). Reverse-apply checks
confirm 0223, 0225, and 0226 are all present. Kernel SHA-256:
`8c8c8fe347a3a95c3694377b83b2f52ffcc53b4c2fde834afdee6292d52822a0`.

Built `outputs/guacamole-0223-0225-0226-audio-standin.img` with current `mkadt.py --highmem --guacamole`,
the existing fullmetal template and shim, and the previous 0223 60 Hz GPU-present arguments refreshed with
`sweettime`. 0226 audio stand-in remains enabled (no `sweetaudio=0`); VirtualAudio remains a separate userland
plugin setting and was not changed. Image SHA-256: `5508773e53e42dcca644b6499297a3ab6284f1bf08270fc78f0a5c973bd07982`.
Not yet booted: `fastboot devices` is empty, and SSH to the last known phone IP `192.168.0.51` times out. Need the
phone in fastboot to boot this image, then re-establish SSH to inspect `SWEET AUDIO` / CoreAudio logs with
VirtualAudio still disabled.

## 2026-11-10: 0226 silent IOAudio2Device stand-in (kernel; built, not hardware-tested)

xnu/patches/0226-guacamole-ioaudio2-standin.patch: iokit/Kernel/arm/SweetAudio.cpp (+ files.arm64, KernelConfigTables
entry on IOResources, before IOWatchdog). Class IOAudio2Device (CoreAudio matches by class name), user client
IOAudio2DeviceUserClient (type 0). Registry: device UID "SweetBuiltInAudio", name "Built-In Audio", clock domain 1,
io buffer frame size 512, transport 'bltn', latencies 0, safety offsets 64; output stream ID 2 (2 ch) and input
stream ID 1 (1 ch), s16 48 kHz, ring 4096 frames, format dict as "format" + "supported formats"/"available formats".
Selectors: 2 echoes the value, 5 logs the ASBD, 10/11 start/stop the clock, rest accepted. Memory: type 0 = status
page (guess: 0x00 sample time, 0x08 host time of the last ring wrap, 0x10 == 0x18 seqlock counter), 0x10000001 /
0x10000002 = rings. A HIGH thread call advances the page once per ring pass (85.3 ms). Logs "SWEET AUDIO: ...";
boot-arg sweetaudio=0 disables. GUESSES to correct from logs: UID/name, format dictionary layout, status page.
Test plan: (1) boot with VirtualAudio still disabled: dmesg | grep 'SWEET AUDIO' (registered; does mediaserverd
open/map/start?). (2) then mv /Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin /Library/Audio/Plug-Ins/HAL/,
killall mediaserverd, watch its log / crash reports (undo: move it back).

## 2026-11-10: 0225 Dynamic Island vanishes in full-screen games: fb transparency test missed the edges

User: in Subway Surfers the island disappears (back on leaving the game; not shown even with a timer running).
0185's sweet_fb_layer_transparent samples an 8x8 grid whose outer rows are 1/16 of the height in (~150 px), so
CA's layer with only the island pill (top) and home indicator (bottom) visible counted as all-transparent and the
game layer was scanned out alone. NEW xnu/patches/0225-guacamole-fb-top-layer-edge-bands.patch: also scan the top
and bottom 1/16 bands (every 4th row, every 8th pixel). Cost to watch: such frames now go to the blended (CPU
compose) path every frame while the island shows - check fps and `dmesg | grep 'SWEET FB: 5s' | tail`; if it is
too slow, the next step is blending only the edge bands (or the whole top layer) on the GPU. Not built yet.

## 2026-11-10: 0224 photo colours (userland/metal2): YCbCr 4:2:0 converted to RGB on the CPU

User: photos show in the wrong colours. 0217's sweetYCbCrTexture stored raw Y/Cb/Cr in r/g/b assuming QuartzCore's
shaders convert; they do not (the hardware YCBCR8_420_2P formats sample as RGB). Now BT.601, '420v'/'x420' video
range else full range, format 501 -> RGBA8Unorm_sRGB; the 0218 stats log reads the source planes. Needs rebuild of
the metal2 bundle (as for 0217-0219).

## 2026-11-10: audio stand-in: plan = kernel "IOAudio2Device" specified from CoreAudio's client side

Codex IPSW findings in research/audio/README.md (D22: IOAudio2Family, AppleARMIISAudio, AppleEmbeddedAudio,
AppleCS42L75Audio codec, AppleCS35L26Amp speaker amp, AppleAOPAudio; iOS CoreAudio HALS_IOA2Manager matches
IOAudio2Device and calls UC selectors 2, 3, 5, 6, 10 (start), 11 (stop)). No stock D22 ioreg available, so next
Codex request = the client-side contract (matching, registry schema + required keys, selector shapes, notification
format, mapped memory + status page fields, VirtualAudio's searched UIDs + D22 VAD tuning file names). Then: kernel
patch with a silent IOAudio2Device stand-in, VirtualAudio restored, iterate from mediaserverd's log.

## 2026-11-10: MILESTONE: D22 iOS IOAudio2 stack researched

Inspected the local iOS 16.7.16 / 20H392 D22 kernelcache, DeviceTree and the matching
CoreAudio shared cache. Findings are in `research/audio/README.md`: D22 uses IOAudio2
through `HALS_IOA2Manager`/`HALS_IOA2UCDevice`; the tree identifies CS42L75 codec,
CS35L26 top-speaker amp, six MCA clusters and a distinct AOP audio endpoint; kernel
personalities and IOAudio2 registry key names are summarized there. The iOS HAL call
sites use selectors 2, 3, 5, 6, 10 and 11 (including flagged start/stop), so do not
substitute the older macOS 0/1 start/stop table.

Still unknown: exact primary D22 device UID/name, complete IOAudio2 stream/control
dictionaries and the kernel driver's selector dispatch table. The available IPSW dump
does not expose those runtime values; obtain a stock D22 IORegistry dump or extract and
disassemble the individual kext Mach-Os before implementing the stand-in personality.
All Apple binaries and raw disassembly were kept in `/private/tmp`, outside the repo.

## 2026-11-09: MILESTONE: AppleAOPAudioPlugin and VirtualAudio static findings

Documented the AppleAOPAudioController match, AOP audio/voice-trigger user clients,
observed IOConnectCallMethod selectors and the two mapped ring/status regions, plus
VirtualAudio's built-in speaker/receiver/microphone topology requirements in
`research/audio/README.md`. Exact D22 physical UIDs and numeric port IDs still need a
stock-device IORegistry capture or VirtualAudio topology disassembly; do not treat the
stand-in topology as fully specified yet.

## 2026-11-09: started: audio stand-in device (option 1) - research/audio/README.md (Codex task)

## 2026-11-09: MILESTONE: A2DP connects with 0223; route listed but not selectable: needs VirtualAudio

With kernel 0223 (Skywalk user-pipe priv) bluetoothd survives AVDTP OPEN and the Beats appear in the audio route
list. Picking them fails in mediaserverd: "FigRoutingManagerPickRouteDescriptorsForContext: Unable to find endpoint
for descriptor" ("Beats Solo 4"), routeConfigUpdateReasonEndedFailed; around it CoreAudio "no object with given ID 0",
"Couldn't get the VAD to set the audio object property listener", AQMEIO "error -66680 finding/initializing
Default-InputOutput". VAD = VirtualAudio.plugin, which image v12 moved to /Library/Audio/Plug-Ins/HAL.disabled/
(it crashed mediaserverd: ApplyBeamFormerPreference, no default virtual device 'vdef' because there is no audio
hardware). On iOS every route, Bluetooth included, is an endpoint of the VAD, so Bluetooth audio now waits on the
audio stack: VirtualAudio back + something that makes 'vdef' exist (a dummy built-in device or the real codec).
Bluetooth itself is done: 3 Mbaud, pairing, encryption, SDP, AVRCP, HFP AT, AAP, AVDTP to OPEN, route published.

## 2026-11-09: btbridge at boot: launchd job com.sweet.btbridge (not installed yet)

tools/sweet-root-tweaks.py NET_JOBS: com.sweet.btbridge = /var/local/bin/btbridge -q, RunAtLoad, KeepAlive,
ThrottleInterval 10 (fails and retries until wland has the rails up), log /var/local/btbridge.out. btbridge takes
/var/run/btbridge.lock (flock) so a hand-started second copy exits instead of resetting the controller; to debug by
hand: `launchctl bootout system/com.sweet.btbridge`. Install on the live phone: pull /System/Library/xpc/launchd.plist,
`python3 tools/sweet-root-tweaks.py --launchd-plist IN OUT --net`, push back (keep owner/mode), reboot.

## 2026-11-09: kernel 0223 Skywalk user-pipe privilege + boot image

Applied 0223 after 0222 and built locally with Xcode 27.2 / macOS 13.5 KDK
22G74 using `XNU_LOCAL=$(pwd)/work/xnu-local-latest sh tools/xnu-local-build.sh`.
The patch file's original unified-diff hunk counts were malformed; corrected
the header counts to match its existing context/additions, then `git apply
--check` and the build succeeded. Reverse-apply check confirms 0223 is present
in the built source. No other kernel patch was changed. Kernel:
`work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet`,
16,959,848 bytes, SHA-256
`994dafffe75cf811739d208e06a6bdfcc48eee18b43b5407e46d43f477f2a872`.

Built `outputs/guacamole-0223-skywalk-user-pipe-priv.img` from that kernel,
current `mkadt.py --highmem --guacamole`, the same shim and stock boot-image
template/DTB as 0222, and the 0222 boot args with `sweettime` refreshed.
Verified `rd=ufsroot0 ufsroot=ios`, `sweetfbgpu=1`, and no forced
`sweetfb_refresh=60 sweetfb_vsync=60`; embedded ADT matches the current
generator output. Image is 21,495,808 bytes, SHA-256
`59e96e9bdffabc899d6444c090a076f2e5b1fd65ecc5c02ba4610ca06968f1be`.
Built only; not booted on the phone.

## 2026-11-09: cause of the A2DP exit: no Skywalk user-pipe privilege for mobile; patch 0223

nxprobe: root USER_PIPE register + alloc OK; root KERNEL_PIPE register EINVAL (expected); mobile register EPERM;
nxprobe-bt (signed with bluetoothd's entitlements) as mobile: EPERM too - this kernel has no entitlements at all
(0025), and its MAC priv_grant (0064 sweet_sandbox_priv_grant) grants only PRIV_WORK_INTERVAL. bluetoothd runs as
mobile (501). 0223 grants only PRIV_SKYWALK_REGISTER_USER_PIPE. Built into
`outputs/guacamole-0223-skywalk-user-pipe-priv.img` (details above), not yet booted. After boot:
`su mobile -c /var/local/bin/nxprobe` -> expected register 0, alloc 0.

## 2026-11-09: nxprobe-bt rebuilt and installed; Skywalk permission delta

Extracted the iOS 16.7.x `bluetoothd` entitlements from
`/private/tmp/guac-rootfs-audit/usr/sbin/bluetoothd` to the ignored
`userland/bt/bluetoothd-ents.plist` with `codesign -d --entitlements :-`;
`plutil -lint` passed. `sh userland/build.sh` completed and built
`userland/out/nxprobe-bt`, ad-hoc signed with those entitlements. Installed it
to `/var/local/bin/nxprobe-bt` on `192.168.0.51` via `.new`, `chmod 755`, and
`mv -f`; pulled it back and verified byte-for-byte. SHA-256:
`53462a68e2bc5fd5f0d52fc16c7922521982eeefe010544af5b4c6b0fb2a7d6c`.

XNU source finding (`work/xnu-local-42140063871/dxb/xnu`): user-pipe provider
registration calls `skywalk_priv_check_cred(..., PRIV_SKYWALK_REGISTER_USER_PIPE)`,
whose entitlement name is `com.apple.private.skywalk.register-user-pipe`.
`os_channel_create` passes `cha_low_latency` into `CHMODE_LOW_LATENCY`; when set,
`ch_open()` separately requires `PRIV_SKYWALK_LOW_LATENCY_CHANNEL`, mapped to
`com.apple.private.skywalk.low-latency-channel`. Monitor channels separately
require `com.apple.private.skywalk.observe-all` and uid 0. The extracted
bluetoothd entitlements include register-user-pipe but do not include
low-latency-channel (they do include `com.apple.bluetooth.latencyCritical`, a
different entitlement). Thus a channel request marked low-latency can pass
registration and still fail channel open. `nxprobe-bt` currently repeats
`nxprobe.c`'s register/allocate-instance calls; it does not yet open a channel,
so it does not itself test that second permission check.

## 2026-11-09: nxprobe (errno of the A2DP user-pipe nexus alloc)

Built with `sh userland/build.sh` and installed to `/var/local/bin/nxprobe` on
`192.168.0.51` using `nxprobe.new` + `chmod 755` + `mv -f`. Pulled the installed
file back and confirmed byte-for-byte equality. SHA-256:
`7025b74958445cb0c7d6a173786bb17417632be332ec240bc3b48f7900382913` (51,184 bytes).
The build emitted existing warnings in `bt/btuart.c`, `wland/radio.c`, and other targets.

Codex: bluetoothd fails in os_nexus_controller_alloc_provider_instance for its NEXUS_TYPE_USER_PIPE provider
(BTAudioSkywalk, slot 2048, 1/1 rings, 8/8 slots); "Device supports Skywalk Path" = a virtual predicate (vtable +0x60),
source unknown; xnu nx_create candidates ENOENT (provider missing/closed) or a constructor error. No CRASH line in
dmesg for bluetoothd (exited, not faulted?). New userland/bt/nxprobe.c (build.sh): same register + alloc, prints
each errno; `nxprobe` (user pipe) and `nxprobe k` (kernel pipe), as root and as mobile.

## 2026-11-09: A2DP Skywalk disassembly findings

`research/bluetooth/a2dp-skywalk.md` records the confirmed provider setup and XNU
failure path. bluetoothd registers `BTAudioSkywalk` as `NEXUS_TYPE_USER_PIPE` with
2048-byte slots, one RX/TX ring, 8 RX/TX slots and MHINTS=2. The next call,
`os_nexus_controller_alloc_provider_instance`, is the one that fails. `nexus.c`'s
likely runtime return is `ENOENT` (provider UUID/controller lookup), but the log has
no errno; the UI capability log is a virtual predicate at vtable offset 0x60, and
its implementation/source condition remains unidentified. The failure helper logs
and returns through cleanup; it does not itself abort. No phone or kernel changes.

## 2026-11-09: bluetoothd dies at "Failed to alloc a nexus provider" (A2DP Skywalk path)

At AVDTP OPEN bluetoothd says "Device supports Skywalk Path", creates its own Skywalk nexus (slot 2048, 1/1 rings,
8/8 slots), "Created skywalk nexus provider", then "Failed to alloc a nexus provider" and the process is gone 0.1 s
later (no .ips, no "restarting"). Task for Codex: research/bluetooth/a2dp-skywalk.md (which nexus type/call fails,
why bluetoothd dies, what selects the Skywalk path, xnu error returns).

## 2026-11-09: after the cmd_sync fix: A2DP reaches AVDTP OPEN, then bluetoothd restarts

btbridge trace (run 2): the Beats connect to us, AVDTP: they Discover (6 local SEPs), Get_All_Caps SEID 1..6,
Set_Configuration SEID 5 = Apple vendor codec (ff 4c 00 00 / 00 01 ...), Delay_Report, Open: all accepted; the media
L2CAP channel (2nd PSM 0x19) opens and configures; AAP (PSM 0x1001) exchanges its certificate (639-byte ACL); HFP
AT commands flow. Within ~1 s of the media channel being configured, a new HCI_Reset: bluetoothd restarted (no
.ips). Repeats on every reconnect (5x). Reason not yet seen: need bt-a2dp2.oslog (the attachment was the old file).

## 2026-11-09: A2DP blocker found: btbridge's own command exchanges swallowed controller packets (fixed, untested)

bt-a2dp.oslog (bluetoothd is verbose: AVDTP, SDP, L2CAP state): Beats connect, SDP OK (AudioSink AVDTP 1.3, AVRCP,
HFP, AAP), AVDTP Discover -> SEID 1 SBC, SEID 2 AAC; Get_All_Capabilities SEID 3 sent, then "Didn't get command
response to transaction 4 within timeout period" -> "Disconnecting A2DP when no AVDTP stream has been established";
also "L2CAP Config Timeout" on the headset's SDP channel and an AAP (PSM 0x1001) timeout. In the btbridge -a trace
that request is followed at once by "LE scan parameters, done here": scan_parameters / sync_cmd /
ensure_random_address / the FC01->FC14 BD_ADDR path did uart_write + uart_read(300 ms) into a private buffer and
dropped whatever else arrived. Same cause for both earlier bluetoothd restarts (reason 621 ExpectedEventTimeout on
OI_DHCI_AUTHENT_REQ and OI_DHCI_ACCEPT_CNCT_REQ: Authentication Complete / Connection Complete eaten).
Fix in btbridge.c: ctl/nc are global; `cmd_sync()` reads into ctl, removes only its own Command Complete/Status and
leaves every other packet for the main loop. Also seen: Create Connection Command Status 0x3a (Controller Busy) a few
times while the headset paged us (retries succeed).

## 2026-11-09: ACL trace (btbridge -a): A2DP stops after AVDTP Get_All_Capabilities

Device is "Beats Solo 4" 4c:97:cc:06:4f:3e. L2CAP channels open fine: SDP (PSM 1), RFCOMM (3), Apple 0x1001, ATT (0x1f),
AVDTP (0x19), AVCTP (0x17). AVDTP: our Discover -> 3 SEPs (SEID 1/2/3, sink); our Get_All_Capabilities SEID 1 ->
accept (media transport, SBC ff ff bitpool 8-75, SCMS-T, delay reporting). Then bluetoothd sends NO Set_Configuration,
and does not answer the headset's own AVDTP Discover (label 3); later the headset closes a channel. Meanwhile
bluetoothd loops Broadcom vendor 0xFCE9 (sub 05/07/1e/35/32/01, ~750 times) and 0xFCE2 (sub 1e/36), all answered
blindly by btbridge (status 0 + zeros): suspect one of these answers is what bluetoothd waits on. Need bluetoothd oslog.

## 2026-11-09: 3 Mbaud works; AirPods connect but no audio; btbridge -a (ACL trace) added

Log: `uart: 3 Mbaud link up` (0xFC48 answer 04 ff 02 92 01 + 04 0e 04 01 00 00 00 dropped, BD_ADDR read OK at 3M).
AirPods 4c:97:cc:06:4f:3e: first Create Connection page timeout (04), then Connection Complete 0 handle 2, link key
reply, auth OK, encryption on, Exit Sniff, then only ~50 ACL each way (60 s summary: ACL 47 in / 54 out, 0 resyncs) and
no audio. Open: did AVDTP reach Start (needs ACL contents: new `btbridge -a`), and does iOS audio work at all on this
port (speaker / audiomxd)? Vendor commands answered blindly around then: fce9 (x70), fce2, fd4a, fd4b, fd4d.

## 2026-11-09: latest btbridge rebuilt and installed

Ran `sh userland/build.sh`; installed the resulting `out/btbridge` to `/var/local/bin/btbridge` on `192.168.0.51` via `/var/local/bin/btbridge.new` and `mv -f`. Pulled the installed binary back and verified it byte-for-byte with `cmp`. SHA-256: `4e5b2d8946680a742c9f973752fb6fd0b14e5af564c610db51d3e2e30c43e43f`. Build completed with existing unused-variable warnings (`scan`, `regs`) in `bt/btuart.c` and unrelated warnings from other targets. The running `btbridge` process was not restarted.

## 2026-11-09: btbridge rebuilt and installed (latest request)

Ran `sh userland/build.sh`; installed the resulting `userland/out/btbridge` to `/var/local/bin/btbridge` on `192.168.0.51` using the `.new` staging path, `chmod 755`, and `mv -f`. Pulled the installed binary back and verified byte-for-byte with `cmp`. SHA-256: `ca906da968329f0a497cba89d60722a9d61b57c41c95e07710e6dc83a4516a9f` (86,032 bytes). Build completed with existing warnings, including unused `scan`/`regs` in `bt/btuart.c` and warnings in other targets. The running process was not restarted.

## 2026-11-09: btbridge rebuilt and installed

Ran `sh userland/build.sh` successfully. Current `userland/out/btbridge` (85,936 bytes; SHA-256 `74012c1ec84f0a92f401f83fd8afd5f99b65d03e3156336b1ebda60e3828c2db`) is installed at `/var/local/bin/btbridge` on `192.168.0.51`. The earlier SSH attempt completed the staged rename before Wi-Fi dropped; after Wi-Fi returned, pulled the installed binary and verified it byte-for-byte with `cmp`. Build warnings: `scan`/`regs` unused in `bt/btuart.c`, plus existing warnings in other targets.

## 2026-11-09: AirPods connect -> btbridge stopped; fix: 3 Mbaud link + stream resync (NOT yet built/tested)

AirPods connected (encryption on, ACL flowing), then Bluetooth died: phone did NOT reboot; btbridge gone,
bluetoothd + BlueTool -R left, kernel `SWEET BT: ... 11532017 bytes to the bridge, 1613779 back, 1192 lost`.
Cause: controller link at 115200 (~11 KB/s) < A2DP; the host kernel control's 64 KB queue overflowed, the kernel
dropped bytes (sweet_bt_to_bridge), btbridge's H4 parser fell out of step on a bogus long ACL length, its 64 KB host
buffer filled, `read(m, .., 0)` returned 0 = "kernel control closed" -> exit. Changes:
- btuart.c: `rcg_set(m, n, hz)` (clk-rcg2 2d clamp), `src_hz` replaces SRC_HZ in set_baud; `uart_fast()`: RFR held
  not-ready, vendor 0xFC48 [0x0e = 3000000] (Linux hci_qca qca_set_speed), RCG 600 MHz x 4/25 = 96 MHz, divider 1,
  RFR back to hardware, answer dropped, HCI_Read_BD_ADDR must complete.
- btbridge.c: boot at 3 Mbaud by default (falls back to a full 115200 re-boot), `-s` stays at 115200; ACL length
  > 4096 or an event from the host = out of step -> skip byte, logged once, counted ("resyncs" in the summary); full
  host buffer -> dropped, never read(.., 0).
Kernel side still drops on a full queue (should stop consuming tx slots instead): next if loss remains.

## 2026-11-09: MILESTONE: Bluetooth works in iOS (Settings finds devices)

With 0x204a answered by btbridge: LE_Set_Scan_Parameters -> 0, no bluetoothd restart in 30 s, Settings' Bluetooth
switch on and a nearby device listed; Wi-Fi stays connected. Chain: ADT /arm-io/bluetooth (mkadt.py), WifiVendor=USI
(mgtool setstr), kernel 0220+0221+0222 (kernel controls com.apple.uart.bluetooth / .sk.bluetooth / com.sweet.bt.host,
Skywalk kernel pipe), userland/bt/btbridge (WCN3998 boot + H4 bridge + Broadcom/Apple compatibility answers).
Not done: btbridge as a launchd job at boot (after wland); pairing / ACL data / audio untested; host polls the UART
(CPU); Wi-Fi/BT coexistence not configured; real BD_ADDR source; the stand-in .hcd.

## 2026-11-09: latest btbridge installed with extended-scan workaround

Ran `sh userland/build.sh`; the current `out/btbridge` includes the `0x204a` reply that keeps the controller in legacy scan mode. Installed to `/var/local/bin/btbridge` on `192.168.0.51` using `.new` + `mv -f`, then pulled back and verified byte-for-byte. SHA-256: `ee4a92377f556e596ca818bf5c63e6234f856e1c182acd9d057c00d59e6386e5`. Existing warnings in `bt/btuart.c` (`scan`, `regs`) and unrelated build targets remain. Running bridge process was not restarted.

## 2026-11-09: LE legacy commands refused = extended mode (0x204a)

Even LE_Set_Scan_Enable(0) gets 0x0c: the chip has switched to extended-advertising mode, where legacy LE scan /
advertising commands are disallowed. Cause: bluetoothd's init sends LE_Read_Periodic_Advertiser_List_Size (0x204a,
"01 4a 20 00"), a periodic (extended) advertising command. btbridge now answers 0x204a itself (size 3) so the chip
stays legacy. The 0x200b handling (scan off, retry public) stays as a fallback.

## 2026-11-09: latest btbridge rebuilt and installed

Ran `sh userland/build.sh`; `out/btbridge` built successfully with the current scan-parameter fallback changes. Installed to `/var/local/bin/btbridge` on `192.168.0.51` via `.new` + `mv -f`, then pulled it back and confirmed a byte-for-byte match. SHA-256: `d08d582e882bc2e86cfa124eb0e93cc0f0a0755f484d7786793ebd90adc8f1e5`. Build emitted the existing unused-function warnings for `scan` and `regs` in `bt/btuart.c`, plus warnings in unrelated targets. Did not restart a running bridge process.

## 2026-11-09: LE scan still disallowed with a random address

LE_Set_Random_Address accepted (c7:34:a7:fb:2e:73), LE_Set_Scan_Parameters (now passive, 01e0/0030, own random) still
0x0c -> bluetoothd "restarting { reason=712 }". btbridge now does 0x200b itself: LE_Set_Scan_Enable(0) first, the
command as sent, and if still 0x0c with own address type public; logs every status; bluetoothd gets the last.
Also reported: Wi-Fi "unable to connect / channels in use" while BT stack runs (coexistence? not yet checked).

## 2026-11-09: btbridge rebuilt and installed

Ran `sh userland/build.sh`; `out/btbridge` built successfully. Existing warnings: unused `scan` and `regs` functions in `bt/btuart.c`, plus an unrelated wland parentheses warning and other pre-existing warnings. Installed to `/var/local/bin/btbridge` on `192.168.0.51` through `btbridge.new` + `mv -f`, then pulled it back and compared byte-for-byte. SHA-256: `cb54aa41c0944755756df7de858649677ffecfbc7d29d0fd7a2a830a3bb9ce1a`. Did not stop or restart any running bridge process.

## 2026-11-09: LE scan "Command Disallowed"

bluetoothd up to announcing the local device, then "Could not start LE scan (leScanParamsLPCoreCb): STATUS 712" ->
"Scan stopped improperly -- restart". lescan.md (ChatGPT): standard LE_Set_Scan_Parameters (0x200b). The chip answers
it 0x0c Command Disallowed every time (01 0b 20 07 01 40 00 30 00 01 00: own address type 1 = random). No scan enable
and no extended (0x2036-46) commands before it. Likely: no random address on the chip (Apple sets it through vendor
0xfce9 commands, answered by the bridge). btbridge now sets a static random address with LE_Set_Random_Address
(0x2005) before 0x200b / 0x2006 / 0x200d that use own address type random, once per HCI_Reset.

## 2026-11-09: bluetoothd LE scan path disassembly

`research/bluetooth/lescan.md` documents the iOS 16.7.16 `leScanParamsLPCoreCb` path. It uses standard HCI LE Set Scan Parameters (`0x200b`) or LE Set Extended Scan Parameters (`0x2041`), not vendor opcodes `0xfdd1`/`0xfd4e`; their Command Complete returns the normal one-byte HCI status. A separate asynchronous vendor scan-core event handler exists, but no mandatory event wait is in the parameter callback. Status 712 (`0x2c8`) is passed into the callback as an OI_STATUS and isn't synthesized there; exact lower-layer mapping remains undetermined. The LPCore decision reads runtime scan-core state and tracks a `scanLPCoreEnable` boolean; the branch contains no direct BCM chipset threshold. Whether that boolean comes from defaults or a negotiated feature bit remains undetermined. No device/config changes were made.

## 2026-11-09: bluetoothd stack up to STACK_READY; vendor replies with parameters

With the supported-commands fix: "Stack initialization complete", STACK_READY, "Done starting up stack", SDP/A2DP/HFP
registered, "Notifying stack listeners that the stack did start". Then ~1 s later: BD_VSC_LE_META_ENA_FEATURES
(0xfd48) -> "AssertFail bcm_vsc.c:642 : ByteStream_NumReadBytesAvail(*pBs) >= 1" (our vendor Command Complete had
only the status) -> exit, launchd restart every ~10 s (no crash report). btbridge: vendor commands answered here now
return status 0 + the command's first parameter byte (sub-opcode echo) + 15 zeros.

## 2026-11-09: supported-commands mask

0x0c5b never reaches the bridge: bluetoothd fails it locally (status 628) because the WCN3998's
Read_Local_Supported_Commands octet 18 = 0x83 (bit 3 Write_Default_Erroneous_Data_Reporting clear). btbridge now
sets octet 18 bits 2-3 in that reply; the chip's "unknown command" (status 1) is then turned into success.
Also seen (non-fatal so far): "Unable to turn on BT)_WAKE signal (status=65535)".

## 2026-11-09: MILESTONE: bluetoothd's stack talks to the WCN3998 through the 0222 Skywalk channel

dmesg: "channel open for pid 456", "channel connected"; btbridge: bluetoothd's own HCI init on the real chip
(Read_Local_Supported_Commands, BD_ADDR 02:53:57:45:45:02, features, LE masks, buffer sizes, LE white list / data
length / resolving list, host flow control...), all status 0. Then bluetoothd: "OI_HCICMD_WriteDefaultErroneous
DataReporting (status=628)", "HCI initialization failed", restart (reason 108) ~0.7 s after connect; Write_Default_
Erroneous_Data_Reporting (0x0c5b) does not show in the bridge log. btbridge now turns Command Complete status 0x01
(Unknown HCI Command) into success (logged). To check: does 0x0c5b reach the bridge (grep '5b 0c').

## 2026-11-09: 0222 Skywalk kernel-pipe channel for bluetoothd

With 0221 bluetoothd connects to com.apple.uart.sk.bluetooth, then "os_channel_create (on com.apple.uart.sk.bluetooth)
failed (No such file or directory)" -> fatal 1201. transport.md (ChatGPT): getsockopt 0x40487413 (72 bytes termios),
setsockopt 0x800c5414 (12 bytes), getsockopt 0x40105412 (16 bytes: nexus UUID), os_channel_create, setsockopt
0x80487414; slots carry the byte stream in chunks. (Its claim that H4BC skips this contradicts the phone's log.)
0222 (on 0221): kernel-pipe nexus as bsd/net/if_utun.c's utun_enable_channel: provider "com.sweet.nexus.bt.kpipe"
(2048-byte slots, 64 per ring), instance + pbufpool per open, bound to the asking pid (NEXUS_PORT_KERNEL_PIPE_CLIENT);
0x800c5414 opens it, 0x40105412 returns the instance UUID; sync_tx sends slot bytes to the bridge, bridge bytes are
queued (64 KB) and sync_rx fills rx slots after kern_channel_notify; closing the sk socket frees the instance.
Logs "SWEET BT: channel open for pid N" / "channel connected" / "channel closed". Untested; compile first.
(research/bluetooth/sweet_btuart.c.0222: full file after 0222, staging copy.)

## 2026-11-09: bluetoothd's stack opens com.apple.uart.sk.bluetooth (0221)

BlueTool's script now completes ("BlueTool finished running boot script", "Powering on chipset", "Starting up
StackManager"), then bluetoothd: "HCI Transport is set to H4BC" -> "ioctl(CTLIOCGINFO, com.apple.uart.sk.bluetooth)
failed" -> "Failed to initialize transport H4BC" -> "Bluetooth fatal error - crashing { reason=1201 }"; launchd
restarts it, Settings cycles unavailable / loading. xnu/patches/0221-guacamole-bluetooth-uart-sk-control.patch (on
top of 0220): also registers com.apple.uart.sk.bluetooth with the same handling; the latest iOS connection on
either control gets the bridge's bytes; dmesg says which control. (research/bluetooth/sweet_btuart.c.0221: the
full file after 0221, staging copy.)

## 2026-11-09: MILESTONE: BlueTool's whole D22 boot script runs against the WCN3998

With Read_Local_Version faked as BCM4357B1: "chip = BCM4357B1", fwPos 31; "wake failed: error 2" (no wake GPIO,
ignored); HCI_Reset (chip); FC18 baud; FC2E + ~25 FC4C Write_RAM records + FC4E Launch_RAM (BlueTool found an
.hcd with real Broadcom code: all answered by btbridge, nothing sent); FC18; FC01 Write_BD_ADDR 02:53:57:45:45:02;
HCI Write_Local_Name "iPhone" (chip, ok); FD98 (Broadcom TX power table) and FC27 (sleep mode, bcm -s) answered.
btbridge now forwards FC01 as QCA 0xfc14 (Linux qca_set_bdaddr) so the chip takes the real address.
Next: what bluetoothd does after the boot script (oslog bluetoothd), Settings' Bluetooth switch.

## 2026-11-09: btbridge Read_Local_Version opcode fix

The installed btbridge (ChatGPT's rework of the version fake + ensure_hcd with an H4-prefixed record 01 4e fc 04
ff ff ff ff) matched opcode 0x1002 and answered with 02 10: Read_Local_Version_Information is 0x1001 (wire 01 10).
BlueTool's 01 01 10 00 still went to the WCN3998 (0x001d / 0x02be) -> chip ALL. Fixed to 0x1001 in both places.

## 2026-11-09: BlueTool's script runs over 0220 + btbridge

Fixed 0220: BlueTool connects; btbridge log: HCI_Reset (chip, ok), FC18 baud (answered), Read_Local_Version (chip:
Qualcomm 0x001d / 0x02be), FC2E minidriver (answered). Then BlueTool's chip became "ALL": getChipset reads
Read_Local_Version and needs manufacturer 0x000f + LMP subversion 0x420e for BCM4357B1, caching the answer in the
process (killall BlueTool resets it). bcm -W reads /usr/share/firmware/bluetooth/BCM4357B1_19.1.282.5154_
Hercules_OS_USI_20210524.hcd and stops if missing (absent here). btbridge now answers Read_Local_Version as
BCM4357B1 (HCI/LMP 9, mfr 0x000f, subver 0x420e) and writes a one-record stand-in .hcd (4e fc 04 ff ff ff ff) if
missing. Open: whether the .hcd parser expects a 0x01 type byte per record (ChatGPT: "requires packet type 0x01").

## 2026-11-09: 0220 fix: stream kernel controls

First 0220 boot: both controls registered (CTLIOCGINFO found them), but btbridge's connect failed "Protocol wrong
type for socket" (EPROTOTYPE): a kernel control takes SOCK_STREAM clients only with CTL_FLAG_REG_SOCK_STREAM.
BlueTool (also SOCK_STREAM) then leaked a socket per retry: "socket failed (Too many open files)". 0220 now sets
CTL_FLAG_REG_SOCK_STREAM on both. After the new kernel BlueTool starts clean (fresh boot).

## 2026-11-09: 0220 Bluetooth kernel controls + btbridge over them

research/bluetooth/transport.md (ChatGPT): BlueTool's `device` sends transport-encoding 6 -> AppleBT, 7 -> PCIe,
anything else (3 here) -> PF_SYSTEM kernel control "com.apple.uart.bluetooth": socket(32,1,2), CTLIOCGINFO,
connect unit 0, termios through getsockopt(2, 0x40487413) / setsockopt(2, 0x80487414) (0x48 bytes), H4 byte
stream. bluetoothd only access()-checks /dev/cu.bluetooth or /dev/uart.bluetooth at start.
xnu/patches/0220-guacamole-bluetooth-kernel-control.patch: bsd/dev/sweet_btuart.c registers
com.apple.uart.bluetooth (unprivileged; termios options kept and answered) and com.sweet.bt.host (privileged, one
client) and passes bytes between them (latest iOS connection); logs "SWEET BT: ..."; boot-arg sweetbt=0 = off.
btbridge now connects to com.sweet.bt.host (no pty) and links /dev/cu.bluetooth + /dev/uart.bluetooth to /dev/null
for bluetoothd's existence check. (research/bluetooth/sweet_btuart.c.new: a staging copy, delete; not for git.)

## 2026-11-09: BlueTool transport paths disassembled

`research/bluetooth/transport.md` records the iOS 16.7.16 BlueTool and bluetoothd transport analysis. BlueTool reads DeviceTree `transport-encoding`: 6 selects AppleBT, 7 selects PCIe, and all other values (including Guacamole's 3) fall through to `PF_SYSTEM`/`SYSPROTO_CONTROL` named `com.apple.uart.bluetooth`. The generic path uses CTLIOCGINFO, connects a `sockaddr_ctl`, configures a termios-shaped control option, and carries H4-style bytes. bluetoothd reads the same DeviceTree property but has a separate, broader transport/config selector. Its `/dev/cu.bluetooth` and `/dev/uart.bluetooth` references are startup `_access(..., F_OK)` probes; this disassembly does not prove when either path is actually opened.

## 2026-11-09: BlueTool selects the D22 row; transport is a kernel control socket

With WifiVendor = USI: "vendor = USI", fwPos 31, boot function runs; first command `device -D`: "HCI transport is
other types from bluetool_command_device", then "ioctl(CTLIOCGINFO, com.apple.uart.bluetooth) failed (No such file
or directory)" -> result 3. With the ADT's transport-encoding 3 BlueTool talks to a KERNEL CONTROL socket
(PF_SYSTEM/SYSPROTO_CONTROL) named com.apple.uart.bluetooth, not to /dev/cu.bluetooth. Options: another
transport-encoding that takes the tty path (need bluetool_command_device's branches), or a kernel control
com.apple.uart.bluetooth (kernel patch) that hands bytes to btbridge. btbridge: leftover boot bytes now dropped,
EIO (no slave open) silent.

## 2026-11-09: btbridge (bluetoothd / BlueTool <-> WCN3998)

research/bluetooth/bluetool.md (ChatGPT): BlueTool picks its D22 row by ProductHash fd079b29... + chipset
BCM4357B1 + manufacturer from MGCopyAnswer("WifiVendor"): "Murata"/"V=m" -> MUR, "USI"/"V=u" -> USI (also Mitsumi,
Semco, TDK, Taiyo, TASS). D22 rows: MUR (Hercules 5153 .hcd) and USI (5154). Boot script (both): device -D; wake on;
power off; msleep 100; power on; msleep 100; hci reset; bcm -b 1500000; bcm -W; device -s 115200; bcm -B; bcm -A;
bcm -N; bcm -Q; bcm -x; bcm -s 0x01,...; quit.
Plan: `mgtool setstr WifiVendor USI`; userland/bt/btbridge.c: boots the controller (btuart code, included with
-DBTUART_NO_MAIN), pty with /dev/cu.bluetooth + /dev/uart.bluetooth -> slave (0666), H4 both ways; vendor commands
(OGF 0x3f, Broadcom's) answered locally with Command Complete status 0, the rest go to the WCN3998; QCA log ACL
(handle 0x2edc) dropped. Open questions: what `wake on` / `power off/on` and `bcm -W` (where is the .hcd?) need.
Built all of `userland/build.sh` and installed the rebuilt `out/btbridge` to `/var/local/bin/btbridge` and `out/mgtool`
to `/usr/local/bin/mgtool` on `192.168.0.51` via `.new` staging + rename. Pulled both back over SSH and byte-compared
them with the build. SHA-256: btbridge `f4f8b69643b31f925f407619df6e81d87b1caa1f992233cb7c712e9d6e3dab88`;
mgtool `69f43995fc7e9ba602106282cfa84e54e3c57a30616a6cc34fdfb1bc71b89525`. Neither tool was run on the phone.
The user tried `/var/local/bin/mgtool`; it was initially only installed in `/usr/local/bin`. Also installed the same
verified binary at `/var/local/bin/mgtool` (root:wheel, 0755) to match the path they used.

## 2026-11-09: bluetoothd matches /arm-io/bluetooth; BlueTool is next

With the ADT node at /arm-io/bluetooth: Settings' Bluetooth is no longer "unavailable"; bluetoothd says "Booting
chipset" and asks BlueTool (XPC com.apple.BlueTool, xpcRunBuiltinScript) to run the boot script, which times out.
BlueTool: "DeviceTree transport = 3", then getChipset -> getManufacturer -> MGCopyAnswer(kMGQWifiVendor), which waits
5 s for an AppleOLYHAL service ("failed to fetch wifi vendor (no AppleOLYHAL node)"): "Manufacturer is -1",
chip (null), "getFirmwarePosForPlatform fwPos = -1" (NO_FW_FOUND), "No WiFi - Iteration N", "BlueTool timed out!".
mgtool now has `setstr NAME STRING` (a cached WifiVendor answer would stop the wait). Needed from BlueTool's code:
the strings _getManufacturerFromString accepts, the firmwareList entry for D22, and its builtin boot script.

## 2026-11-09: BlueTool D22 firmware lookup extracted

`research/bluetooth/bluetool.md` records the analysis of the iOS 16.7.16 (20H392) `/usr/sbin/BlueTool` copied
from the phone. The phone's cache identifies ProductType `iPhone10,3` / `D22AP` and ProductHash
`fd079b298144a7deb4d7c87fef3eb474d8f0e7eb`; BlueTool's firmware table has BCM4357B1 Hercules rows for MUR
(`...5153...`) and USI (`...5154...`), with built-in boot callbacks. The failure observed in bluetoothd/BlueTool
is consistent with `WifiVendor` being unavailable: manufacturer resolves to -1 and neither vendor-specific row
matches. The report includes the WifiVendor input aliases/tags, chipset selector, table matching logic and
disassembled script commands. No device state was changed; the MobileGestalt cache was copied read-only for
ProductHash confirmation.

## 2026-11-09: Bluetooth ADT node v2

With /arm-io/uart1/bluetooth, bluetoothd still logs "marconi-bt / bluetooth: Unable to match bluetooth service in
device tree" (the first 20-s oslog just stopped early). SweetPlatformExpert::processTopLevel makes nubs for /arm-io's
direct children only, so uart1/bluetooth had no IOService. mkadt.py now puts the node at /arm-io/bluetooth (gets a
nub). /dev symlinks work (ln -s /dev/null /dev/cu.bluetooth): the serial bridge can be a pty in userland.
If this still does not match: get the matching dictionary from bluetoothd's code (function logging "Unable to
match bluetooth service in device tree").

## 2026-11-09: Bluetooth part 2, step 1: ADT node

research/bluetooth/ios-side.md (ChatGPT): bluetoothd matches the DT bluetooth service, then opens
/dev/cu.bluetooth or /dev/uart.bluetooth ("No valid serial port found (tried cu.bluetooth and uart.bluetooth)");
no /etc/bluetool scripts or .hcd on the rootfs; BlueTool runs as `BlueTool -R`.
tools/mkadt.py (--guacamole): /arm-io/uart1 (compatible guacamole,qup-uart, so pe_serial leaves it alone) with
child bluetooth as D22AP (bluetooth,n88, transport-speed 3000000, encoding 3, vendor 0x05ac, product 0x12a0,
coex 2, local-mac-address 02:53:57:45:45:02 = /chosen mac-address-bluetooth0).
Trial image built with the current generator: `outputs/guacamole-0215-bluetooth-adt.img`, SHA-256
`a2991cab20555e1e582e3dc42221cd925e2e2970e63921942998f1ebe8439f16`. It uses the 0215 kernel
(SHA-256 `70974e3bcb90e94d4ea304dcd16ca5d23ea64023965e06eec1a3d1b00db22ce6`), `mkadt.py --guacamole --highmem`,
and the working 0215 boot arguments with `sweettime` refreshed; root arguments are present and forced 60 Hz
refresh/vsync overrides are absent. Image is built, not yet booted.
Second image from the same generator/kernel and exact same arguments (including `sweettime=1794266329`):
`outputs/guacamole-0215-bluetooth-adt-rerun.img`, SHA-256
`21f7b01ebdc72fe76f3e7526a24f49aa02d1f8ed9bc576a47933cf20cb42998c`. Not yet booted.
Serial port plan: a userland bridge with a pty, /dev/cu.bluetooth -> its slave (if devfs takes the symlink),
translating Broadcom-specific setup; to be decided from what bluetoothd does next.

## 2026-11-09: MILESTONE: Bluetooth scan works from userland (btuart scan)

hello + load + scan 15: logging off, masks, extended inquiry + LE active scan all accepted; 25 devices (23 LE,
2 classic incl. "iPhone2" at -47 dBm). Part 1 (hardware) done.
Part 2 (iOS): the iPhone X ADT (research/ipsw-x/d22ap.adt) has /arm-io/uart1/bluetooth: compatible
"bluetooth,n88", device_type bluetooth, transport-speed 3000000, transport-encoding 3, vendor-id 0x05ac,
product-id 0x12a0, local-mac-address "macaddr/btaddr,syscfg/BMac/6,zeroes/6", function-power_enable /
function-bt_wake, coex 2. bluetoothd: "Unable to match bluetooth service in device tree" -> our ADT lacks it.
To find out: how bluetoothd/BlueTool open the transport (serial BSD node? IOKit user client?) and which
Broadcom-specific commands it sends (firmware .hcd download, baud change, ...) that the bridge must answer.

## 2026-11-09: MILESTONE: Bluetooth controller runs its firmware

btuart hello + load: rampatch + NVM (tag 17 patched: IBS off, 115200) accepted; HCI_Reset ok; Local Version
HCI 9 (Bluetooth 5.0), manufacturer 0x001d Qualcomm, LMP subversion 0x02be; BD_ADDR 39:90:21:74:07:00 (NVM
default, needs a real one later). Firmware logs stream as ACL on handle 0x2edc. (The installed btuart was the one
before the deep-sleep change: tag 27 stayed 01, and it worked.) Next: `btuart scan [SECS]` (logging off, event
masks, extended inquiry + LE active scan, one line per device).

## 2026-11-09: Bluetooth firmware download, first try

Rampatch crbtfw21.tlv (ROM build 0x201, download mode 3) accepted: one final event 04 ff 03 00 04 00 (result 0),
926 segments / 23.5 s at 115200. crnv21.bin is TLV type 4: a container with an inner type-2 NVM entry (45 tags)
and a type-3 entry. Its tag 17 = 82 01 11 (in-band sleep on, baud code 0x11 = 3.2 Mbaud) and tag 27 = 01 (deep
sleep): unpatched, the link went silent after HCI_Reset (reply "ff", then nothing). btuart now patches inside the
container: tag 17 byte 0 bit 7 off, byte 2 = 0 (115200); tag 27 bit 0 off. NVM default BD_ADDR 39:90:21:74:07:00.
Board ID request: 04 ff 05 00 00 10 0a 03 + status 01 (not used; crnv21.bin used).

## 2026-11-09: Bluetooth controller ANSWERS (btuart hello)

UART at 115200 on QUP wrapper 2 SE3 works; power pulses as hci_qca.c. QCA version event:
04 ff 0e 00 02 | product 0x0a, patch 0x6699, ROM 0x1001, SoC 0x40010224 = WCN3998 (Android CHEROKEE_VER_2_1 ->
crbtfw21.tlv + crnv21.*). HCI_Reset -> 04 0e 04 01 03 0c 00. Next: `btuart board` (board ID -> NVM variant: the
crnv21.bXX files are all identical, crnv21.bin differs) and `btuart load [NVM]` (rampatch + NVM at 115200 with CTS
flow control, NVM tag 17 baud byte -> 0, no IBS / deep-sleep changes; then reset, local version, BD_ADDR).

## 2026-11-09: Bluetooth step 2 (btuart)

btprobe clock: SE3 (0xc8c000) runs with vote bit 7; UART firmware loaded (FW rev 0x20b, protocol 2); QUP wrapper 2
HW 1.3 (-> 32x oversampling); TX FIFO 16 words, RX 32. GPIO43-46 in TLMM EAST, function 1 = qup13 (Linux
pinctrl-sm8150.c, checked by ChatGPT); RX (GPIO45) idles high = controller likely powered (wland's rails).
userland/bt/btuart.c: SE3 RCG -> 7.3728 MHz (GPLL0 600 x 384/31250, M/N), pins to qup13, GENI UART FIFO 8N1,
power-off 0xC0 @2400 / power-on 0xFC @115200 (hci_qca.c), QCA version (01 00 fc 01 19), HCI_Reset.
`btuart hello` does all of it. Assumptions to check if nothing answers: GPLL0 main = RCG source 1, M/N encoding,
flow control (off), the UART really being BT's.

## 2026-11-09: Bluetooth, step 1 (btprobe, read-only)

bluetoothd today: "marconi-bt / bluetooth: Unable to match bluetooth service in device tree", MAC all 00s (no
BT_DEVICE_ADDRESS, no boot-arg): it looks for an ADT bluetooth node / IOKit service first.
Hardware (research/bluetooth/): WCN3990 BT, no enable GPIO; rails L7A 1.8 / L2C 1.3 / L11C 3.0-3.3 V = the rails
wland already votes for Wi-Fi (ldoa7, ldoc2, ldoc11, rfclka2). UART candidate: qupv3_se13_4uart 0xc8c000 = QUP
wrapper 2 SE3 (GPIO43-46). Firmware: bluetooth_a partition (crbtfw21.tlv / crnv21.bin family), local ignored folder.
userland/bt/btprobe.c (build.sh): GCC wrapper-2 vote / SE3 branch / RCG + DFS levels, TLMM GPIO43-46 + 172-174 in
all four tiles, and the SE3 registers only if its clocks are on. Plan after that: clocks + pins + GENI UART FIFO
mode, WCN3990 power-on pulse (0xFC at 2400 baud), QCA version / rampatch / NVM download, HCI reset, scan
(milestone: a CLI that lists nearby devices); then the bluetoothd side (ADT node + transport).

## 2026-11-09: wallpaper work PAUSED (user: Bluetooth next)

State: photo shows in the picker; lock + home screen black. backboardd converts the photo's '420f' planes (0217) and
sees real data (0218: Y 0-226, Cb/Cr ~128). 0219 (draw trace for converted YCbCr textures) is written but not yet
built/tested: next step is `grep backboardd mtl2.log | grep 'ycbcr surf'` on the lock screen. Also open: '_RB4'
465x465 (format 542) refused, the surface row is 512 too (0216 did not help).

## 2026-11-09: 0217 (Metal frontend): YCbCr 4:2:0 wallpapers in backboardd

Lock and home screen both black with a photo wallpaper. backboardd composites PhotosPosterProvider's '420f' photo as
MTLPixelFormatYCBCR8_420_2P (500) over the surface and the runtime refuses it ("Unsupported ... NotImplemented",
3000+ times). 0217: newTextureWithDescriptor:iosurface:plane: with format 500/501 on a 2-plane surface (plane 0)
expands the planes on the CPU into an RGBA8 texture with Y/Cb/Cr in r/g/b (assumed: what the Apple format samples
as; QuartzCore's shader applies the matrix), cached per surface ID + size, rebuilt when the IOSurface seed changes.
If the photo shows with wrong colours (green/pink cast), the assumption is wrong: the hardware may return RGB.

- 2026-11-09 Codex rebuilt with `METAL_WORK=work/metal-work sh userland/metal2/build.sh`; local ad-hoc signature
  verification passed (4 existing incompatible-pointer warnings). Installed on `192.168.0.51` at
  `/System/Library/Extensions/SweetMetal2.bundle`; prior install preserved as
  `SweetMetal2.bundle.pre-0216-0217-20261109`. Read back over SSH and byte-compared `SweetMetal2`, Turnip, and
  `_CodeSignature/CodeResources`. SHA-256: `e5dd5218a481bd5299d8d0cee77ea335de4d7f948cbceb64a12bc6509f3e6f9f`,
  `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`, and
  `9f7aea4c96dd1fb9c4b3c0769554ecede146d1fc025d1a81e6dd788f4bba79c4`. Archive
  `outputs/guacamole-SweetMetal2-0216-0217.tar.gz`, SHA-256
  `feb483dc103362078bdbbbbe20f82840d485fea2086ef181cc77f27d45812945`. No reboot performed.

- 2026-11-09 Codex rebuilt the current Metal bundle with the 0218 Y/Cb/Cr sample diagnostics in
  `NativeBridgeMetalFrontend.mm`, verified its ad-hoc signature, and installed it on `192.168.0.51` at
  `/System/Library/Extensions/SweetMetal2.bundle`. Prior install preserved as
  `SweetMetal2.bundle.pre-0218-20261109`. Read back over SSH and byte-compared `SweetMetal2`, Turnip, and
  `_CodeSignature/CodeResources`. SHA-256: `be288d592761ef786c9ab45a20996eb9a2ca93b135000e80760907bc36dcd62e`,
  `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`, and
  `9f7aea4c96dd1fb9c4b3c0769554ecede146d1fc025d1a81e6dd788f4bba79c4`. Archive
  `outputs/guacamole-SweetMetal2-0218.tar.gz`, SHA-256
  `f859c922218434983639f75b90227a5e067a9471332698e12183133ded320340`. No reboot performed.

- 2026-11-09 Codex rebuilt the Metal bundle from current sources with
  `METAL_WORK=/Users/coreycusick/Documents/Codex/2026-11-06/ok/work/metal-work sh userland/metal2/build.sh`;
  local ad-hoc signature verification passed (4 existing incompatible-pointer warnings). Installed on `192.168.0.51`
  at `/System/Library/Extensions/SweetMetal2.bundle`, preserving the previous bundle as
  `SweetMetal2.bundle.pre-20261109-171609`. Read back over SSH and byte-compared `SweetMetal2`, Turnip, and
  `_CodeSignature/CodeResources`. SHA-256: `ac7bdeb46caab1cb3d4bb1405f1ae9ec89ac05a2ba44c73871e502a6901c452b`,
  `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`, and
  `9f7aea4c96dd1fb9c4b3c0769554ecede146d1fc025d1a81e6dd788f4bba79c4`. Archive
  `outputs/guacamole-SweetMetal2-20261109.tar.gz`, SHA-256
  `d4449b86a0263eccb21c484fce2248c0359bc163250a7ae5e7b1b36526461e58`. No reboot performed.

## 2026-11-09: 0216 (Metal frontend): '_RB4' after 0215

0215 works: PhotosPosterProvider's planes import cleanly; the photo shows in the wallpaper picker. The home screen is
still black. Side effect of 0215: backboardd's '_RB4' 465x465 surfaces (RGB8_444_2P, 542, narrowed to one BGRA8
plane) now report plane 0's own row (512) and were refused (RowPitch 512 < 1860). 0216 keeps the surface's row for
format 542, as before 0215. Real two-plane 444 support is still not done.

## 2026-11-09: 0215 plane bytes-per-row at the right offset (kernel)

0214 works (surfprobe: maxima 16384/16384/262144, bs_IOSurface gives surfaces). Next in PhotosPosterProvider: its
'420f' surfaces report IOSurfaceGetBytesPerRowOfPlane = 0 for both planes (0212's plane log: offsets 64 / 737344,
row 0). IOSurface.framework (research/iosurface/IOSurface, llvm-objdump) reads plane i at result+0x68+0x80*i:
+0 width, +4 height, +0xc offset, +0x10 bytesPerRow, +0x14 size, +0x18 bpe (u16), +0x1a/+0x1b element w/h, byte
+0x54 == 1 makes base address and row read 0. SweetSurface wrote the row at +0x08. 0215 also writes it at +0x10.
Note (applies with offsets): the patch context is from work/xnu-local-latest (after 0214).

## 2026-11-09: 0214 IOSurface maxima (kernel): black wallpapers

surfprobe: IOSurfaceGetPropertyMaximum(Width/Height/BytesPerRow) = 0, and BaseBoard's
+[IOSurface bs_IOSurfaceWithWidth:height:options:] returns nil for any size (it refuses sizes above the maxima), so
PosterBoard's wallpaper snapshots had no surface (0213 trace: -[PBFHomeScreenPreviewImageBuilder _imageForView:]).
research/iosurface/: selector 13's 24-byte block is _ioSurfaceLimits: +0 offset align mask, +4 row align mask,
+8 max BytesPerRow, +0xc max Width, +0x10 max Height (as-is), u16 +0x16 = 1 (checked). 0046 filled only +0/+4/+0x16.
xnu/patches/0214-guacamole-iosurface-limits-maxima.patch: +8 = 16384*16, +0xc = +0x10 = 16384 (create()'s own limits).
Test: surfprobe -> maxima 16384 and the bs_IOSurface lines give surfaces; then the photo / default / Pride wallpapers.

- 2026-11-09 Codex applied 0214 after the existing 0197–0202 kernel series and built with
  `XNU_LOCAL=work/xnu-local-latest sh tools/xnu-local-build.sh` (no patch errors). Kernel:
  `work/xnu-local-latest/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet`, SHA-256
  `b8f05c6a9e9285363fe0a24a495f5700819bffec41559a9853df59a5ef0eb854`.
- Boot image: `outputs/guacamole-0197-0198-0199-0201-0202-0214-iosurface-limits.img`, SHA-256
  `f7db46577cd569aa78c9a0c1f903bc0ce0c2681510a6c7192eb4d6605fa765d1`. Reused the 0202 image's ADT and boot
  arguments, refreshed `sweettime`; verified `rd=ufsroot0 ufsroot=ios` are present and
  `sweetfb_refresh=60 sweetfb_vsync=60` are absent. Image built but not booted.

## 2026-11-09: 0215 IOSurface plane row width

- Applied 0215 after 0214 with `tools/xnu-local-build.sh`; kernel build succeeded and reverse-apply check passed.
  Kernel SHA-256: `70974e3bcb90e94d4ea304dcd16ca5d23ea64023965e06eec1a3d1b00db22ce6`.
- Boot image: `outputs/guacamole-0197-0198-0199-0201-0202-0214-0215-iosurface-plane-row-offset.img`, SHA-256
  `f0a59ac1ddb6cff748a37656750f64f74f750368191a256f7f3a9b5d057b990d`. Reused the 0214 image's ADT and boot
  arguments, refreshed `sweettime`; required root args are present and forced refresh/vsync args are absent.
  Image built but not booted.

## 2026-11-09: surfprobe (PosterBoard's nil snapshot surface)

PosterBoard's surface comes from +[IOSurface bs_IOSurfaceWithWidth:height:options:] (BaseBoard), sized from the
layer's bounds x contentsScale, plane 0. It returns nil here. `userland/jb/surfprobe.m` prints IOSurface property maxima/alignments and IOSurfaceRoot registry properties, creates a plain BGRA reference surface, and calls the helper with an `&1` options word at the requested size, 1125x2436, and 64x64. Rebuilt `userland/build.sh`; fixed a CFDictionary-to-NSDictionary bridge access in `surfprobe.m` that blocked this build. Installed `out/surfprobe` as `/var/local/bin/surfprobe` on 192.168.0.51 (root:wheel, 0755), verified SHA-256 `ec0ec31b61bb5aaa0d5feb03c3f9883dcc1e4b6e9db7d5536682b3d65bf36860`. The named BaseBoard helper disassembly and PosterBoard's option interpretation are in `research/posterboard/BaseBoard-IOSurface-helper.txt` and `README.md`. PosterBoard passes a pointer to a 64-bit value 1; it takes the non-planar 4-byte-per-element path. No kernel or Metal bundle change.

## 2026-11-09: no-IOSurface caller = PosterBoard itself (0213 result)

PosterBoard asks for a 375x811 BGRA8 (format 80) texture over a nil IOSurface: a 1/3-scale snapshot of the 1125x2436
screen. Caller frames, from the dispatch queue: PosterBoard (UUID FBD32EAC-FBBF-3F56-A0BA-5BD44D2E2725) image offsets
+465104 (0x718d0) -> +465424 (0x71a10). Disassembled and named both containing functions in `research/posterboard/PosterBoard-0x718d0.txt` and
`PosterBoard-0x71a10.txt`; `README.md` records the analysis. Verified the extracted image UUID. The IOSurface argument
is newly requested from `+[IOSurface bs_IOSurfaceWithWidth:height:options:]` using the view layer's bounds multiplied
by its contentsScale; the returned object is passed as `iosurface:` with plane 0. Extracted PosterBoard Mach-O is also
in that folder.
No crash reports from the wallpaper processes this run (only MobileSlideShow at 15:50, earlier).

## 2026-11-09: 0213 (Metal frontend): who passes a nil IOSurface

With 0211/0212: PhotosPosterProvider's heaps overflow to the device as intended (3x "resource made outside the heap"),
then 3 compute commits and no more; no 420f refusal this run. PosterBoard and SBRendererService both log "surface
texture refused: no IOSurface". 0213 logs the caller's stack (16 frames, first two times per process) with the
descriptor's size and format.

- Rebuilt with `sh userland/metal2/build.sh`; ad-hoc signature verification passed. Installed on `192.168.0.51` at `/System/Library/Extensions/SweetMetal2.bundle`; previous bundle preserved as `SweetMetal2.bundle.pre-0213-20261109`. Installed `SweetMetal2`, Turnip, and `_CodeSignature/CodeResources` hashes match the local build: `8332f7b0ff441d15208c34efb7cd2d87d97e27d6aafe76d6b82916569f777571`, `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`, and `9f7aea4c96dd1fb9c4b3c0769554ecede146d1fc025d1a81e6dd788f4bba79c4`. Archive `outputs/guacamole-SweetMetal2-0213.tar.gz`, SHA-256 `a282a42979d7db282ec1d75ebc1369bf17e7df8491151db45a67333df352a969`. No reboot or kernel change.

## 2026-11-09: 0211 / 0212 (Metal frontend): PhotosPosterProvider's black photo

mtl2.log after setting a photo wallpaper (0210 installed):
- PhotosPosterProvider: 16x "heap texture refused: OutOfMemory(Heap)". MPS sizes heaps counting on makeAliasable
  reuse, which this runtime's heaps lack. 0211: an automatic heap that is full makes the texture/buffer from the device
  instead (still reports this heap); placement heaps (an offset) still refuse. Logs "heap full (...): resource made
  outside the heap" (first 3).
- PhotosPosterProvider: '420f' 606x1145 plane 1 as RG8 303x573, row 968: "PastBuffer counted 1291646 length 1114112"
  (the chroma plane's reported row runs past the surface). 0212: logs every plane's offset/size/row once per surface,
  and retries with a 64-aligned row of the plane's own width when that fits ("row X does not fit, row Y used").
  If colours come out wrong, that guess is wrong: the plane log says what the surface really is (kernel IOSurface?).
- SBRendererService: "surface texture refused: no IOSurface" (0209's split): it is handed nil; probably the poster's
  rendered surface that PhotosPosterProvider never produced.

- Rebuilt with `sh userland/metal2/build.sh`; ad-hoc signature verification passed. Installed on `192.168.0.51` at `/System/Library/Extensions/SweetMetal2.bundle`; previous bundle preserved as `SweetMetal2.bundle.pre-0211-0212-20261109`. Installed `SweetMetal2`, Turnip, and `_CodeSignature/CodeResources` hashes match the local build: `bb8c8e51b7067c2e84911ee3d400beca97d81f2f429560161969256652df9e28`, `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`, and `9f7aea4c96dd1fb9c4b3c0769554ecede146d1fc025d1a81e6dd788f4bba79c4`. Archive `outputs/guacamole-SweetMetal2-0211-0212.tar.gz`, SHA-256 `cb16b6a1288291c55b346d0c172f3707126d4acc6c1f8aeb9c98093daa3137e8`. No reboot or kernel change.

## 2026-11-09: 0209 / 0210 (Metal frontend): black photo wallpaper

0209: "surface texture refused" now says whether the IOSurface or the descriptor was nil (SBRendererService's
"plane 0 of 0" meant one of them was nil; plane 0 of an unplanar surface passes).
0210: SBRendererService logged "NBRenderEncoder MTLRenderCommandEncoder missing" and "NBBlitEncoder
MTLBlitCommandEncoder missing": objc_getProtocol only finds protocols a loaded image registered. The adopt list now
falls back to @protocol(...) for the public Metal protocols, so render/blit encoders conform in every process.
- Rebuilt with `sh userland/metal2/build.sh`; ad-hoc signature verification passed. Installed on `192.168.0.51` at `/System/Library/Extensions/SweetMetal2.bundle`; previous bundle preserved as `SweetMetal2.bundle.pre-0210-20261109`. Installed `SweetMetal2`, Turnip, and `_CodeSignature/CodeResources` hashes match the local output: `003c814c4d2448ffe90724056b977d2b5fadd4926c3e9030ef0a55a926714b8b`, `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`, and `9f7aea4c96dd1fb9c4b3c0769554ecede146d1fc025d1a81e6dd788f4bba79c4`. Archive `outputs/guacamole-SweetMetal2-0209-0210.tar.gz`, SHA-256 `ebddbf893f76cc8ac06cbe3002240e14c39438ddbc5a9eb512e6e243b05ea5a4`. No reboot or kernel change.

## 2026-11-09: 0209 surface plane validation (Metal frontend)

- Rebuilt with `sh userland/metal2/build.sh`; ad-hoc signature verifies. `SweetMetal2` SHA-256: `62e1ebbf21a3b2c733bcd8cdfc611422656ab41430465963ab7c61aef0983a8a`; Turnip unchanged: `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`.
- Installed on `192.168.0.51` to `/System/Library/Extensions/SweetMetal2.bundle`. The previous bundle is preserved as `SweetMetal2.bundle.pre-0209-20261109`. Verified installed `SweetMetal2` SHA-256 `62e1ebbf21a3b2c733bcd8cdfc611422656ab41430465963ab7c61aef0983a8a`, Turnip SHA-256 `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`, and `_CodeSignature/CodeResources` SHA-256 `9f7aea4c96dd1fb9c4b3c0769554ecede146d1fc025d1a81e6dd788f4bba79c4`; all match the local build. Archive: `outputs/guacamole-SweetMetal2-0209-surface-plane-validation.tar.gz` (SHA-256 `34b0d637c60eb227a2c876c1eea25e7dc36326c922dc3631dc16146636eb85c3`). No kernel changed and no reboot was performed.

## 2026-11-09: 0208 makeAliasable (Metal frontend)

With 0207 installed (bundle sha256 must match out/), mpsprobe's blur got past heap sizing and stopped at
"-[NBTexture makeAliasable]: unrecognized selector". 0208 adds a no-op -makeAliasable to NB_RESOURCE_SPI (buffers and
textures): the runtime's heaps never reuse memory, so the resource simply keeps it until freed; isAliasable stays NO.
Install check: shasum -a 256 /System/Library/Extensions/SweetMetal2.bundle/SweetMetal2 against out/.
- 2026-11-09 Codex rebuilt with `sh userland/metal2/build.sh` and installed to `192.168.0.70`. `/System/Library/Extensions` was writable; no remount needed. The bundle is ad-hoc signed, `makeAliasable` is present, and the pulled-back bundle including signature resources matches the build. `SweetMetal2` SHA-256: `a20e8aa1a371bd600cbe584fe7dca1aba7b3b30fd9c5a312e3ca2428d2ee6691`; Turnip unchanged: `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`. Previous install preserved as `SweetMetal2.bundle.pre-0208-20261109-0620`. No kernel change or reboot. Archive: `outputs/guacamole-SweetMetal2-0208-makeAliasable.tar.gz`, SHA-256 `bd27ee0ada5205625cefb3413eb51cc78583672cf06656b9801b78a14cc825c5`.

## 2026-11-09: 0207 heapTextureSizeAndAlignWithDescriptor: (Metal frontend)

mpsprobe: the device answers Apple1-3 + Common1-2 only, MPSSupportsMTLDevice = YES and MPSCommandBufferImageCache
initWithCommandBuffer: returns an object, so MPS accepts the device (at least in a plain process). MPSImageGaussianBlur
then stopped at "-[NativeBridgeMetalDevice heapTextureSizeAndAlignWithDescriptor:]: unrecognized selector".
0207 adds it to NBDeviceMethods (generous estimate: 64-texel tiles, 256-byte rows, +25%, page-aligned levels, align 4096).
Next: rebuild the Metal bundle, run mpsprobe again (the blur should finish), then the photo and Pride wallpapers.
- 2026-11-09 Codex rebuilt with `sh userland/metal2/build.sh`; ad-hoc signature verification passed and the new `heapTextureSizeAndAlignWithDescriptor:` selector is present. `SweetMetal2` SHA-256: `8d978c67400ca79cb9ba3e8f489d02eb177a1b9c1908edc21a3321459d18d9e5`; Turnip unchanged at `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`.
- Installed the 06:09 bundle on `192.168.0.70`; `/System/Library/Extensions` was already writable, so no remount was needed. Preserved prior bundle as `SweetMetal2.bundle.pre-0207-20261109-0609`. Pulled the install back and verified all bundle files, including `_CodeSignature/CodeResources`, match the built bundle. Archive: `outputs/guacamole-SweetMetal2-0207-mps-heap-align.tar.gz`, SHA-256 `f9b78a93ec6bb582077519b2af2f3548eac3f7813601f4e8db56b0061e30cb50`. No reboot or kernel change. Four existing pointer-type warnings remain.

## 2026-11-09: mpsprobe (photo / Pride wallpapers, MPSAutoCache nil+0x18)

MPSCommandBufferImageCache initWithCommandBuffer: returns nil when MPSDevice::GetMPSDevice([cb device]) is NULL
(research/metal/MPSDevice-GetMPSDevice.txt). GetMPSDevice asks supportsFamily: 1008 (Apple8) down to 1002 (Apple2):
Apple8/7 true -> accepted; Apple6 true -> only "Apple A13 GPU" by -name, else NULL; Apple5/4/3/2 -> accepted
(Apple3 with a non-Apple name = the A9-class path); none -> NULL. Then maxComputeThreadgroupMemory and maxTextureLayers
only size things (not a refusal). The bridge claims Apple1-3 (TARGET_FAMILIES) plus whatever the runtime's
supports_family says, so the suspect is Apple6 answered true (or the cache refusing for another reason).
userland/jb/mpsprobe.m (build.sh) prints the device's family answers, MPSSupportsMTLDevice, the cache init result and
runs one MPSImageGaussianBlur. Install to /var/local/bin/mpsprobe and run as mobile or root.
- 2026-11-09 Codex rebuilt userland with `sh userland/build.sh`; installed `out/mpsprobe` to `/var/local/bin/mpsprobe` on `192.168.0.70` via `.new` + rename. Pulled it back and byte-compared successfully. SHA-256: `0f790bf5559aaff069964535e6d177538e09963ed60409716bc630584ba24d4c`. No kernel or Metal bundle changed.

# Active handoff — 0206 Metal object protocols (MPS) (2026-11-09, Claude)

- Photo wallpaper / Pride: SIGSEGV 0x18 in MPSAutoCache::MPSAutoCache (from +[_PIDynamicGuidedProcessorKernel
  processWithInputs:...] via CIImageProcessorKernel). Codex's named disassembly (research/metal/MPSAutoCache-ctor.txt,
  MPS-cache-init.txt): the ctor takes [cb userDictionary][@"MPSCommandBufferImageCache"], else
  [[MPSCommandBufferImageCache alloc] initWithCommandBuffer:cb], then reads +0x18 of it. initWithCommandBuffer:
  returns nil if cb does not conformsToProtocol:MTLCommandBuffer, or MPSDevice::GetMPSDevice([cb device]) is NULL;
  else uses retainedReferences, userDictionary, addHandlerToCommandBuffer: (ours has them).
- 0206 (NativeBridgeMetalFrontend.mm, the 0178 adopt list): NBCommandBuffer MTLCommandBuffer, NBQueue
  MTLCommandQueue, NBBuffer/NBTexture MTLBuffer|MTLTexture + MTLResource, encoders their MTL*CommandEncoder +
  MTLCommandEncoder (public protocols; logged "adopted"). If it still crashes the same way, it is GetMPSDevice
  rejecting our device (MPS's supported-GPU check) -> next: MPSDevice::GetMPSDevice disassembly.

# Active handoff — 0205 NBTexture -allocatedSize (2026-11-09, Claude)

- After 0203/0204 the photo wallpaper gets one step further: new "MISSING NBTexture -allocatedSize" (PhotosPosterProvider).
- 0205 (NativeBridgeMetalFrontend.mm, NBTexture): allocatedSize = rowBytes x height x depth x slices over a
  buffer/surface, else sum over mips of texels x bytes-per-texel (by MTLPixelFormat range) x depth/slices/samples
  (cube x6). Bundle rebuild + reboot. Clear /var/local/mtl2.log before the next test (counts are cumulative).
- 2026-11-09 Codex rebuilt 0203+0204+0205 with `userland/metal2/build.sh`; ad-hoc signatures verified. `SweetMetal2`
  SHA-256 `17675d3c64456614fef31e3e1653f548d7725eab8ea69caab8d75d4d4a49d60e`, Turnip SHA-256
  `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`. Installed at
  `/System/Library/Extensions/SweetMetal2.bundle` on `192.168.0.70`; phone hashes match and `allocatedSize` is present.
  Previous bundle preserved as `SweetMetal2.bundle.pre-0205-20261109-0521`. No reboot performed in this request.
  Archive: `outputs/guacamole-SweetMetal2-0205-allocatedSize.tar.gz`, SHA-256
  `188913a827539962f2c1cf4aca2b5504d964c7515e4084284a453645a1a31102`.

# Active handoff — 0204 missing Metal selectors batch (2026-11-09, Claude)

- 0203 worked (no more stitching). Next PhotosPosterProvider abort: '-[NBTexture isDrawable]: unrecognized selector'.
  mtl2.log MISSING (all processes): MapKit.SnapshotService NBLibrary -newFunctionWithName:constantValues:pipelineLibrary:error:
  (43), NBFunction -libraryData (PhotosPosterProvider 5, MobileSafari 3: Core Image stitching, covered by 0203 in all
  processes), NBTexture -isDrawable (2), FindMy NBRenderEncoder -setDepthClipMode: (1).
- 0204 (NativeBridgeMetalFrontend.mm): NBTexture -isDrawable (label == "CAMetalLayer Display Drawable");
  NBLibrary -newFunctionWithName:constantValues:pipelineLibrary:error: -> the constantValues:error: form;
  NBRenderEncoder -setDepthClipMode: (clip = default no-op; clamp logged once, not applied). Bundle rebuild + reboot.
- 2026-11-09 Codex rebuilt the bundle with 0203+0204 using `userland/metal2/build.sh`; ad-hoc signatures verified.
  `SweetMetal2` SHA-256 `0b73b3192fee3c3585b5477780e7ceed91e3b250607488e9208f2302986c1f94`, Turnip SHA-256
  `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`. Installed at
  `/System/Library/Extensions/SweetMetal2.bundle`; phone hashes match. Prior install preserved at
  `SweetMetal2.bundle.pre-0203-0204-20261109-0511`. Archive:
  `outputs/guacamole-SweetMetal2-0203-0204-metal-selectors.tar.gz`, SHA-256
  `88829e20814a3cc8883d99562a863e55424093fe8c07412a21ca18aeeed4dfa0`. `launchctl reboot system` returned
  successfully; SSH at `192.168.0.70` has not returned after 60 seconds. Post-reboot check is pending Wi-Fi.

# Active handoff — 0202 picker opens; 0203 Core Image without stitching (2026-11-09, Claude)

- With 0202 + sweet-containers the photo picker opens. Picking a photo: PhotosPosterProvider aborts:
  '-[NBFunction libraryData]: unrecognized selector' in Core Image's function stitching (CI::MetalDAG::compile ->
  CI::VisibleFunctionStitchingDescriptor::compile -> -[_MTLDevice newLibraryWithStitchedDescriptor:...] (Apple's base,
  not ours) -> MTLLibraryBuilder::newLibraryWithDAG -> [function libraryData]). Also: Espresso "Failed creating a
  default shader library! GPU mode won't work" (subject segmentation; probably falls back to CPU; not the crash).
- research/metal/CoreImage: CI_ENABLE_METAL_DAG and CI_ENABLE_FUNCTION_STITCHING are getenv'd once (atoi, default
  on); analyze_kernels picks the stitched DAG only with them.
- 0203 (NativeBridgeMetalFrontend.mm): a constructor in the SweetMetal2 bundle does setenv(..., "0", 0) for both,
  so Core Image builds filter chains from Metal source (newLibraryWithSource works). Bundle rebuild + reboot.
  If CI then hits another gap, it will show as a new crash / mtl2 MISSING line in PhotosPosterProvider.

# Active handoff — CoreImage and Metal extracted (2026-11-09)

- Extracted `CoreImage` and `Metal` from the iOS 16.7.16 (20H392) arm64 dyld shared cache at
  `/private/tmp/guac-rootfs-audit/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64` using `ipsw dyld extract`.
- Mach-O arm64 libraries and `strings -a` dumps are in `research/metal/`: `CoreImage`, `CoreImage.strings`,
  `Metal`, and `Metal.strings`. `MPSCore` and `MPSCore.strings` were added 2026-11-09; the extraction is also an
  arm64 Mach-O shared library.

# Active handoff — 0202 containers by signing identity + sweet-containers (2026-11-09, Claude)

- 2026-11-09 Codex applied 0202 after 0201 and built successfully with `tools/xnu-local-build.sh`. The patch needed
  its `PROC_NULL` context updated to the existing 0201 compile fix (`NULL`); after that `git apply --check` passed.
  Patch SHA-256: `48b9cd3f88c5c7c7b70cb39a8eb4fcc4657a1aee99b32af67158cb948f807429`; the hash is recorded in
  `work/xnu-local-latest/applied-patches.txt`. Kernel:
  `work/xnu-local-latest/kernel.0197-plus-0198-plus-0199-plus-0201-plus-0202.development.sweet`, SHA-256
  `4dc1ad11bdf833604f0abb9d9263432db7c65cde201689c21bd8a40890957f94`.
- Boot image: `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0197-0198-0199-0201-0202-sandbox-containers.img`,
  SHA-256 `5084c63911f0e3f82e9a63e4e2fe07f7c308d581eec72603961e8b35969603a7`. Reused the 0199 image's ADT and
  boot arguments, refreshed `sweettime`, retained `rd=ufsroot0 ufsroot=ios` and the 60 Hz GPU-present options,
  and confirmed `sweetfb_refresh=60` and `sweetfb_vsync=60` are absent. Not booted.
- Ran `sh userland/build.sh`; `userland/out/sweet-containers` SHA-256
  `13ba93147de4847fcf75f8ce5f1c1dbd346db5cc588ffaf8e31ba9f150906533`. Installed it as
  `/var/local/bin/sweet-containers` on 192.168.0.70 (root:wheel, 0755); pulled it back and byte-compared it.
  `sweet-containers -1` ran successfully and registered 700 container identities in the running kernel.
- Generated the `--net` launchd configuration from the phone's current plist and installed it atomically at
  `/System/Library/xpc/launchd.plist` (root:admin, 0644); saved the prior file at
  `/var/local/launchd.plist.pre-containers`. Verified the installed plist byte-matches the generated file and
  contains `com.sweet.containers` with `RunAtLoad` and `KeepAlive`, alongside `com.sweet.displayd`. No reboot was
  performed; launchd will load the new persistent job on the next boot.

- sbpath: 0 of 301 processes have a container -> 0201's exec hook records nothing (this launcher passes no
  container at exec, or the hook is not called). 0201 stays (harmless; op 4/0x43/0x42 plumbing).
- libsystem_sandbox: sandbox_set_container_path_for_signing_id[_with_persona] / _for_application_group = op 0xd
  { identity, path, kind 0 signing id / 1 app group, persona }.
- NEW xnu/patches/0202-guacamole-sandbox-containers-by-identity.patch (after 0201; git apply --check OK against
  0201's text): table of 640 {identity[128], path[192]} filled by op 0xd (kind 0); op 4 / 0x43 fall back to the
  process's cs_identity_get() identity when it has no per-process container. Logs first 12 new identities.
- NEW userland/jb/sweet-containers.c (build.sh, -framework CoreFoundation): reads every
  /private/var/mobile/Containers/Data/{Application,PluginKitPlugin,InternalDaemon,System,VPNPlugin,XPCService}/*
  metadata plist (MCMMetadataIdentifier) and calls sandbox_set_container_path_for_signing_id(id, dir); rescans
  every 20 s; `-1` once + prints. Boot job com.sweet.containers in tools/sweet-root-tweaks.py (--net), KeepAlive.
- Test: kernel 0202 + install /var/local/bin/sweet-containers; `sweet-containers -1` (count); `sbpath` (apps and
  extensions now list paths); then choose a photo as wallpaper.

# Active handoff — 0201 in, picker still "error 2"; sbpath diagnostic (2026-11-09, Claude)

- 0201 applied cleanly (Codex). Picker still: 'plugin has invalid sandbox or container: error 2 path ?'.
  research/sandbox/PlugInKit: the check is buf="?"; sandbox_container_path_for_pid(getpid(), buf, 0x400) != 0 ->
  exception with errno. So op 4 for its own pid finds nothing in 0201's table. dmesg has no 'SWEET Sandbox' lines
  at all (ring too busy / not right after boot), so we can't see whether the exec hook records anything.
- NEW userland/jb/sbpath.c (build.sh -> out/sbpath, /usr/local/bin): asks op 4 for every running pid (or given
  pids), prints the processes that have a container + counts. Decides: hook records (apps have paths, only the
  picker missing) vs records nothing (-> fallback needed: e.g. op 4 falls back to a path a userland helper sets
  via op 0x42 from containermanagerd metadata, or the kernel derives it).

- Built with `userland/build.sh`; installed `/usr/local/bin/sbpath` on the phone over SSH. Pulled the installed
  binary back and byte-compared it to `userland/out/sbpath`. SHA-256:
  `455ef123c5b3ee1310d56d1c77d6598fea3aa6ed9ee6ad3646376edf6cb17cc8`. `sbpath 1` returns `error 2` for launchd,
  as expected for a process without an app container.

# Active handoff — 0201 application confirmed; PlugInKit extracted (2026-11-09)

- 0201 was applied during the last kernel build: `tools/xnu-local-build.sh` reported `applying
  xnu/patches/0201-guacamole-sandbox-container-paths.patch` and proceeded to compilation. No `git apply` errors;
  the final patch hash is recorded in `work/xnu-local-latest/applied-patches.txt`. A reverse `git apply --check`
  against the built source also exits 0. The built kernel contains the 0201 sandbox log string.
- Extracted arm64 `PlugInKit` from the mounted iOS 16.7.16 shared cache into `research/sandbox/`; its `strings -a`
  dump is `research/sandbox/PlugInKit.strings`. Research artifacts are git-ignored.

# Active handoff — 0201 sandbox container paths built (2026-11-09)

- 0200 (Metal limits 16384) fixed Photos. Choosing a photo as wallpaper: PhotosPicker.appex aborts because the
  kernel Sandbox stand-in returned ENOENT for container lookups (op 4 and 0x43). `research/sandbox/` has the iOS
  16.7.16 `libsystem_sandbox.dylib` and `ExtensionFoundation` extracts and strings dumps.
- Applied `xnu/patches/0201-guacamole-sandbox-container-paths.patch` after 0199. Its launch hook signature matches
  `security/mac_policy.h`. First compile caught `PROC_NULL` undefined in this file; changed the three null checks
  to `NULL`. Build then succeeded. Local DXB dSYM command needed its `/dev/fd` warning filter disabled because
  this environment rejects Bash process substitution; this affects only symbol-warning filtering.
- Kernel: `work/xnu-local-latest/kernel.0197-plus-0198-plus-0199-plus-0201.development.sweet`, SHA-256
  `43363eb4f36cbb23c90293e9f8bb23cd95c2a3e0a5c543c3a8116dc2b88df766`. Linked image contains both the 0199 panel
  mode marker and 0201 `SWEET Sandbox` log string.
- Boot image: `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0197-0198-0199-0201-sandbox-container.img`,
  SHA-256 `312d686b4a336d74cc914d768b4a79df9ad517a363ca8311953d8704bf418c19`. Reused 0199's ADT and boot args,
  refreshed `sweettime`; verified `rd=ufsroot0 ufsroot=ios` and related root arguments remain and that neither
  `sweetfb_refresh=60` nor `sweetfb_vsync=60` is present. Shim Image and DTB match the prior 0199 image. Not booted.
- Next: temporary-boot this image and test Photos' wallpaper picker; collect the new `SWEET Sandbox` kernel logs
  if the picker still fails.

# Active handoff — iOS 16 sandbox libraries extracted (2026-11-09)

- Extracted `libsystem_sandbox.dylib` and `ExtensionFoundation` from the mounted iOS 16.7.16 20H392
  `dyld_shared_cache_arm64` into `research/sandbox/`. Generated `strings -a` dumps beside each binary; both
  extracts are arm64 Mach-O shared libraries. Research artifacts are git-ignored.

# Active handoff — 0200 Photos crash = Metal device limits 0 (2026-11-09, Claude)

- Photos (MobileSlideShow) aborts: NSInternalInconsistencyException 'Invalid parameter not satisfying:
  !PXPixelSizeAreaIsZero(roundedSize)' (oslog). lastExceptionBacktrace: PXCuratedLibraryUIViewController viewDidLoad
  -> PXGView init -> PXGMetalRenderer _setupMetalIfNeeded -> PXGMetalTextureConverter init -> _createTransparentTexture
  (1x1 CGImage) -> createTextureFromCGImage:transform:options: -> PXGMetalCompatibleImageByRedrawingCGImage -> PXGCreateContext.
- research/photosuicore/PhotosUICore (extracted): createTextureFromCGImage reads [[metalRenderContext device] limits]
  (private MTLDevice SPI, pointer) +0x84/+0x88 as uint32 max 2D texture width/height and caps the redraw size to it;
  our device subclasses _MTLDevice with an empty initLimits -> 0 x 0 -> abort.
- 0200 (NativeBridgeMetalFrontend.mm, native_bridge_metal_open after [cls new]): if -limits returns a pointer inside
  the device object and +0x84/+0x88 are both 0, set them to 16384. Logs "native-metal: limits at .. 2D texture
  16384x16384". Bundle rebuild + reboot (or relaunch apps; backboardd loads it at start). Not compiled by me.

# Active handoff — PhotosUICore extracted from iOS 16.7.16 shared cache (2026-11-09)

- Extracted `/System/Library/PrivateFrameworks/PhotosUICore.framework/PhotosUICore` from the mounted 20H392
  `dyld_shared_cache_arm64` at `/private/tmp/guac-rootfs-audit/System/Library/Caches/com.apple.dyld/` with
  `ipsw dyld extract`. `PhotosUICore` and its `strings -a` dump are in `research/photosuicore/` (research artifacts
  are git-ignored, consistent with the other extracted Apple binaries).

# Active handoff — 0199 90 Hz WORKS (2026-11-09, Claude)

- User: 90 Hz (0197+0198+0199) "feels ok". Photos crashes (long-standing, not new): next to look at.

# Active handoff — 90 Hz WORKS by hand; 0199 makes it the boot default (2026-11-09, Claude)

- Test: `panelbright raw 53 30` -> frames 11.368 ms apart (87.97 Hz); sending a frame takes 10.41 ms (max 10.42)
  in both modes (fits 11.37 ms with ~1 ms margin). 60 Hz baseline 16.711 ms. Back: raw 53 20 or reboot.
- NEW xnu/patches/0199-guacamole-fb-panel-90hz.patch (SweetFramebuffer.cpp, after 0198; dry-run OK on the
  Codex-applied 0198 source research/SweetFramebuffer-0198-applied.cpp; applied and compiled locally): sweet_fb_panel_90hz()
  in start() right before 0198's measurement: maps DSI0 + the buffer page 0x9d124000 (inhibit-cache; just past
  the panel memory the GPU present maps, 0x9c000000 + 0x1124000), checks DSI0 version 20xxxxxx and CTRL enable +
  command mode, sends DCS long write 53 30 (c0390002 ffff3053) like panelbright, waits CMD_DMA_DONE (<=100 ms),
  restores DMA_CTRL/TRIG_CTRL, sleeps 60 ms; then 0198 measures ~87.97 Hz and publishes it to CoreAnimation.
  Logs "SWEET FB: panel switched to 90 Hz (DCS 53 30)". Boot-arg sweetfb90=0: bootloader's 60 Hz.
- Boot image: the working 0197 args WITHOUT sweetfb_refresh=60 sweetfb_vsync=60 (else no 0198/0199 rate).
- 0199 was applied after 0198 to the source tree that already includes 0197. Incremental XNU build succeeded.
  Kernel `work/xnu-local-latest/kernel.0197-plus-0198-plus-0199.development.sweet`, SHA-256
  `83deee3b528e99cb1f97aa4ccd0c8321c619a4e54386949b261bbe148fee524e`.
- Packaged `outputs/guacamole-0197-0198-0199-panel-90hz.img` with the working 0197 root/GPU-present arguments,
  omitting `sweetfb_refresh=60` and `sweetfb_vsync=60`; payload args were checked. SHA-256
  `0d45ec504007538d3cc69ef9c07299138629522eff1a0cf085d684f61b42e655`. Image not booted.
- Watch: backboardd/GPU at ~88 fps (compositing cost +47%); CoreAnimation may still cap apps at 60 (iPhone X).

# Active handoff — 0198 WORKS (with correct boot args); 90 Hz test (2026-11-09, Claude)

- Rebuilt `userland/jb/vsyncprobe.c` with the iOS 16 arm64 SDK and ad-hoc signed it. Replaced
  `/usr/local/bin/vsyncprobe` on the phone at `192.168.0.70` via SSH/SCP staging and rename. Pulled it back and
  byte-compared it to the build. SHA-256: `7a44562a02023fdfae6d64916eb37372d5f406dc0f7082b250b0ebfdc3b38bbf`.
- 0198 boots with the 0197 image's args minus sweetfb_refresh=60 sweetfb_vsync=60 (those override 0198). The
  earlier 0198 "hang" (and A/B) = images built WITHOUT rd=ufsroot0 ufsroot=ios ... (Codex audit). Always build
  from the working image's args. (sweetfb_hwvsync=1 in the args is read by nothing.)
- 90 Hz: Pixel 4 XL's S6E3HC2 panel dtsi (Google kernel/msm, dsi-panel-s6e3hc2-dsc-wqhd-cmd.dtsi): the only
  60/90 difference is DCS 0x53 (WRCTRLD) 0x20 = 60 Hz / 0x30 = 90 Hz; same link clock (962150400), MDP transfer
  time 14000 us (60) / 9000 us (90). Assumed same DDIC family in the OnePlus 7 Pro (1440 wide 90 Hz Samsung).
  Test: `panelbright raw 53 30` then `vsyncprobe` (frame interval should be ~11.1 ms; it now also prints how long
  sending a frame takes: must be < 11 ms). Back: `panelbright raw 53 20` or reboot.
  If it works: iOS still gets 59.85 Hz until 0198 measures 90 at boot -> needs the switch before the framebuffer
  starts (kernel) or a re-measure; next step after the test.

# Active handoff — 0198 boot audit (2026-11-09, Codex)

- Source delta from the applied 0197 tree to the 0198 tree is only
  `iokit/Kernel/arm/SweetFramebuffer.cpp` (120 insertions, 15 deletions). Reconstructed `git diff` is in
  `research/SweetFramebuffer-0198-vs-0197.diff`; patched source copy is `research/SweetFramebuffer-0198-applied.cpp`.
  `0198-build.log` records `applying xnu/patches/0198-guacamole-fb-panel-refresh-lock.patch`; the build script uses
  `git apply` (no fuzz/offset mode), and reverse-checking the applied patch succeeds.
- Image comparison: 0197 fallback and 0198 A/B are all 21,491,712 bytes; Android shim kernel is 188,416 bytes
  and DTB is 4,255,607 bytes with the same hash. Embedded XNU payload is 17,040,784 bytes for 0197 and
  17,040,992 bytes for 0198 (kernel segment file bytes 16,940,616 vs 16,940,824). ADTs are all 99,140 bytes;
  0198 A/B use the same ADT, while 0197's ADT differs only in `/chosen/random-seed`.
- The first 0198 A/B images omitted required root arguments; that was an image-build confound. Rebuilt both A/B
  images from the same 0198 kernel, with `rd=ufsroot0 ufsroot=ios unentitled_ios_sim_launch=1 ignition_level=1
  wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 sweetfbgpu=1` restored, plus the common
  60 Hz GPU-present arguments. They share `sweettime=1794211894`; A adds `sweetfblock=0`, B adds
  `sweetfb_vsync=60 sweetfb_refresh=60`. Corrected A SHA-256:
  `dd83b7f93e22b142c8c05ede87cda32301faf0127bb4b8e4bf34417ee47e4977`; corrected B SHA-256:
  `9de37ebaa92235d172a3a4adb5481432b41d4d62cdea9c5460048cc0a96cf3f8`. The 0197 fallback remains unchanged.
- Rebuilt once more with 0197 reversed and 0198 retained. Kernel:
  `work/xnu-local-latest/kernel.0198-without-0197.development.sweet`, SHA-256
  `6c1658f52ad8f356811d268cd7f766e79bc7bfdfcf6608fef30c04872ed0c8ca` (0198 refresh marker present). The build
  tree's 0197 patch was restored after the build. Corrected no-0197 image A:
  `outputs/guacamole-0198-no0197-A-fblock-disabled.img`, SHA-256
  `90b43224093452e509a49d74eec838b5cdf535ac7646b1f5c7dc90bebdc821e4`; B:
  `outputs/guacamole-0198-no0197-B-vsync-refresh-60.img`, SHA-256
  `0429843d6d52f5fdb235ea636ea431398c69073095080d892bab51c525f05225`. Both carry the required root args and
  `sweetfbgpu=1`; shared `sweettime=1794212073`. The 0197 fallback hash remains
  `7bf17bb629ab98ae2c2d7166d82bc6a7fbe6946315ea4ed75fb01f4d98b1432d`. These new images were not booted.
- Built combined 0197+0198 image with the required root arguments, `sweetfbgpu=1`, and default panel lock enabled
  (no `sweetfblock=0`, `sweetfb_vsync`, or `sweetfb_refresh`):
  `outputs/guacamole-0197-0198-panel-lock.img`, SHA-256
  `fcc35f53583e204b4f3f3b6057a694c279282f1487b17e03975d7b55a95c022c`. Payload boot args were read back and
  checked. The 0197-only fallback remains unchanged; this image was not booted.

- vsyncprobe run 3: INTF1 +0x2b0 = TE line counter (0 .. 0c30, changes on every read), +0x298 another counter
  (0c30 .. 1879), +0x2b4 = 80000001 = autorefresh on, every TE (this is what feeds the command-mode panel; nobody
  kicks CTL_START). Frames 16.712 ms (59.84 Hz) apart.
- NEW xnu/patches/0198-guacamole-fb-panel-refresh-lock.patch (SweetFramebuffer.cpp, after 0185): at start maps
  INTF1 (0x0ae6b000, inhibit-cache), checks
  autorefresh bit + height, times 7 frame starts of the line counter (<= 300 ms busy wait once) -> period.
  IOMFBDisplayRefresh / TimingElements SyncRate publish the measured rate (CoreAnimation runs its own frame clock
  from it; it was 60.000 -> one frame of drift every ~7 s). The vsync timer uses the period and re-arms each tick
  to land just after the frame start (line counter). Logs: "SWEET FB: panel refresh measured: N ns a frame =
  59.8xx Hz", "vsync from the panel", "IOMFBDisplayRefresh 59.8xx Hz". Boot-args: sweetfblock=0 off (as before);
  sweetfb_vsync / sweetfb_refresh still override.
- Build order: 0197 then 0198 (the patches are independent). Built locally from the Wi-Fi-capable `ios-sweet`
  source using the established XNU build tree. Kernel SHA-256:
  `a475575026e54509bb35d80a44731a1c5c53f6fb03d199994bca68669a4be9be`.
- Initial A/B images used the same combined 0197+0198 kernel, template, shim, base args, and sweettime
  (`1794210868`); only the requested
  option(s) differ. A: `outputs/guacamole-0198-A-fblock-disabled.img`, adds `sweetfblock=0`, SHA-256
  `fbfe97c440bb1783d8eba372f4aef0bf38ed0a6ea7e11fa8feeee124be72c99c` (superseded by the corrected image above). B:
  `outputs/guacamole-0198-B-vsync-refresh-60.img`, adds `sweetfb_vsync=60 sweetfb_refresh=60`, SHA-256
  `db63c693dfbfde7e8b27c88323ffff8ed8df3b9a35066e471f8e55db6e8b6c66` (superseded by the corrected image above).
- The previous working 0197-only fallback remains untouched at
  `outputs/guacamole-0197-60hz-gpu-two-layers.img`, SHA-256
  `7bf17bb629ab98ae2c2d7166d82bc6a7fbe6946315ea4ed75fb01f4d98b1432d`. No image was booted or flashed for this
  A/B build request.

# Active handoff — vsyncprobe run 2: panel 59.86 Hz; timing in INTF1 tear check (2026-11-09, Claude)

- DSI0 MDP stream busy rises every 16.70-16.71 ms: 18 frames / 300 ms, 59.86 Hz, steady (while scrolling).
  DSI0 STREAM0_CTRL 05a10039 TOTAL 0c3001e0 (3120 lines x 480 words). PINGPONG0/1 all zero.
- INTF1 (0xae6b800) tear check block populated: +284 1, +288 00180066, +28c ffff, +294 0c30, +298 177f,
  +29c 00050004, +2a0 0c30, +2a4 0c31 (height+1), +2b0 0c30 (layout looks like Linux INTF_TEAR_* shifted +4).
- Consequence: the kernel's vsync timer (60.00 Hz) vs panel 59.86 Hz -> one frame of drift every ~7 s (a hitch).
  Plan (small step): phase-lock the kernel vsync timer to a hardware line counter (period 16.706 ms, tick at a fixed
  line), no interrupt wiring needed. vsyncprobe now also finds which INTF1 word +0x200..+0x2fc changes (and range).
- Codex rebuilt/signed this extended `vsyncprobe` and atomically replaced `/usr/local/bin/vsyncprobe`; pulled-back
  binary byte-matches. SHA-256 `56b81b195af7f0f413e8cfbed7d7e8693ac5bd4e2714dc41d48948cbff2dd0a4`. New build not run yet.

# Active handoff — vsyncprobe run 1: PINGPONG timing all zero (2026-11-09, Claude)

- PINGPONG0/1 (0xae71000/0xae71800) +0x000-0x030 all 0 (no tear check, no autorefresh, LINE_COUNT stuck 0) while
  the screen updates; MDP INTR_EN 0, STATUS 00000100 (bit 8 = PINGPONG0 done, stale). Kernel never kicks CTL_START.
- vsyncprobe extended (read-only): INTF1 (0xae6b800) incl. +0x280.. (INTF tear check on newer DPUs), +0x0ac/+0x2a4
  counters; DSI0 STATUS0 bit 2 (MDP stream busy) sampled 300 ms -> frames actually sent to the panel and their rate;
  DSI0 MDP stream regs +0x58/+0x5c. Rebuild + run while scrolling.
- Codex rebuilt/signed and atomically replaced `/usr/local/bin/vsyncprobe`; installed file pulled back and byte-matched.
  SHA-256 `ff5aaec89b6788dd8ebdc36f00cb05aa91eb7a6b908080fc99387378e56322cd`. The extended probe has not been run yet.

# Active handoff — 0197 WORKS (brightness + Night Shift); Night Shift stuck on fixed (2026-11-09, Claude)

- User: with 0197 brightness and Night Shift work, but Night Shift is always on. sweet-displayd took status byte 0
  OR byte 1 as on. User's -1: on = 01 01, off = 01 00 (02:22, schedule 22:00-07:00 at bytes 8/16): byte 0 =
  inside the schedule, byte 1 = on now. sweet-displayd now uses byte 1 (enabled) only.
- Codex rebuilt/signed and atomically replaced `/var/local/bin/sweet-displayd` (SHA-256
  `5071efc3685cc4de9eb453ed2924548805a3d7f44fbb8cb482cee2dc5e2c2d34`), then ran `killall sweet-displayd`.
  Launchd restarted it: `com.sweet.displayd` is running (pid 685, run count 3); pulled-back binary matches the build.
  Next compare `sweet-displayd -1` with Night Shift on and off (status bytes).

# Active handoff — display driver, small steps: vsync probe (2026-11-09, Claude)

- User asked about a better display driver with 90 Hz; chose to start with the small pieces. Order agreed in
  spirit: real vsync (hardware frame timing) -> direct scanout (SSPP pointed at CA's surface, hardware scaling,
  no copy) -> 90 Hz panel mode -> brightness/Night Shift in the kernel. 0197 is now booted and registered.
- NEW userland/jb/vsyncprobe.c (READ-ONLY; build.sh -> out/vsyncprobe, /usr/local/bin): dumps PINGPONG0/1 tear
  check / sync config / autorefresh registers (Linux dpu_hw_pingpong offsets 0x000-0x030) and MDP INTR/INTR2
  EN/STATUS, then samples PP0 LINE_COUNT for 300 ms and reports frame intervals = the panel's real refresh rate.
- Codex built/signed and installed the first `vsyncprobe` at `/usr/local/bin/vsyncprobe` (SHA-256
  `b46ef0518656181f742e884396541b793bb48b8db695013d499cf61a1e036c67`; later replaced with the INTF1/DSI probe above).
  Read-only run returned zero for all
  PINGPONG0/1 timing registers and no LINE_COUNT cycles in 300 ms (94,657 samples); rerun while content is moving
  to distinguish an idle panel from an incorrect register assumption.

# Active handoff — 0197 AppleARMBacklight so CoreBrightness starts (Night Shift) (2026-11-09, Claude)

- research/corebrightness/CoreBrightness (extracted, read with llvm-objdump): -[BLControl findBacklight] does
  IOServiceGetMatchingServices({IOPropertyMatch: {backlight-control: true}}) -> IOIteratorNext -> 
  -[CBDisplayContaineriOS initWithBacklightService:] (needs IOObjectConformsTo "AppleARMBacklight", registryID)
  -> setupInternalModules (CBDisplayModuleiOS initWithBacklight:queue:display: -> DisplayCreate/DisplayOpen; auto-
  brightness, Night Shift ...). With none: "Built in display/backlight driver has not been found." -> no Night
  Shift, CBBlueLightClient answers nobody, no CoreBrightness prefs. The guacamole ADT has no backlight node.
- NEW xnu/patches/0197-guacamole-applearmbacklight.patch (applied after 0185 and built locally):
  iokit/Kernel/arm/SweetBacklight.cpp: class AppleARMBacklight (IOResources match in KernelConfigTables, after
  AMFI; files.arm64 after SweetAMFI). Properties: the iPhone X /device-tree/backlight node (d22ap.adt, 40 props:
  DBVMin 139/DBVMax 2047, marketing table, restriction curves, ... ; generated table also in
  research/corebrightness/backlight-table.inc.txt), backlight-control = true, IODisplayParameters
  {brightness {min 0, max 65536, value}}. setProperties accepts/keeps everything (IODisplayParameters value merged),
  logs first 80 keys `SWEET BL: <proc> sets <key> (<class>) <n>`; user client accepts all selectors, logs once each.
  Boot-arg sweetbacklight=0 disables it (if backboardd/CoreBrightness then crash-loops).
- Boot confirmation (2026-11-09): `kern.bootargs` sweettime matched the 0197 image. ioreg shows service `backlight`
  as active AppleARMBacklight, `backlight-control = Yes`, `IODisplayParameters`, and its brightness user client.
  `sweet-displayd -1` now says Night Shift answers (on=1, strength 0.443). `com.sweet.displayd` is running (pid 461).
  `dmesg` isn't on the SSH shell PATH; ioreg and CBBlueLightClient confirm registration and the query path. Still
  toggle Night Shift visually; if backboardd crashes, inspect newest .ips and boot with `sweetbacklight=0`.
- Codex built the full patch chain with 0185 followed by 0197 using the local XNU build script, Xcode 27.2 and
  KDK 13.5 (22G74). SweetBacklight compiled and the kernel contains its registration/boot-arg strings. Kernel SHA-256:
  `fb8d12cd859fb6811dc8b0d238ad9fc50a818d6e72a70829932999622b383f43`.
- Packaged the temporary 60 Hz GPU-present image at
  `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0197-60hz-gpu-two-layers.img`, SHA-256
  `7bf17bb629ab98ae2c2d7166d82bc6a7fbe6946315ea4ed75fb01f4d98b1432d`. It keeps the 0185 boot args and defaults
  AppleARMBacklight on (no `sweetbacklight=0`). Booted temporarily with `fastboot boot`; Android partitions were
  not flashed.

# Active handoff — PANEL BRIGHTNESS WORKS; sweet-displayd uses it (2026-11-09, Claude)

- 2026-11-09 Codex rebuilt `userland/out/sweet-displayd` from the current `sweet-displayd.m`, linked Foundation,
  and signed with `sweet-displayd-ents.plist`. Installed `/var/local/bin/sweet-displayd` on the phone atomically;
  pulled it back and confirmed it matches the build (SHA-256 `3b74bd42247deb5247e68579d590b294edddf3ff7ffbe18b331fe4504931ab8a`).
- Added the `NET_JOBS` configuration using `tools/sweet-root-tweaks.py --launchd-plist ... --net` to the phone's
  `/System/Library/xpc/launchd.plist`; verified the installed plist matches and has `RunAtLoad` + `KeepAlive` for
  `com.sweet.displayd`. It is not loaded in the current launchd session (previously booted out); the persistent
  job will be read at next boot. No reboot performed.
- Bootloader's last DSI packet at 0xffd09000: c0390003 89b80051 = DCS long write 0x51 00 b8 (brightness 184).
  panelbright set N works (user tested several values incl. 1): 1 = minimum, 1023 = maximum (user's choice).
  DSI DMA reads plain physical memory (0x9d124000 buffer works).
- sweet-displayd: slider -> panel level 1 + round(1022 * level^2) via DCS 0x51 (only on change; waits <= 100 ms
  for CMD_DMA_DONE); the colour stage (PCC) is now only Night Shift (identity -> PCC off). Rebuild + reinstall
  /var/local/bin/sweet-displayd; start it (or the com.sweet.displayd job).
- Night Shift still waits on iOS's own feature (CBBlueLightClient answers nobody): next = CoreBrightness logs /
  binary (see the 'banding fix test + iOS Night Shift investigation' entry below).

# Active handoff — panelbright: panel is COMMAND mode (2026-11-09, Claude)

- probe: DSI0 HW_VERSION 20030000, CTRL 000001f5 (enable, CMD_MODE_EN, 4 lanes, clk; NO video mode),
  CMD_DMA_CTRL 10000000 (embedded, HS), DMA_BASE ffd09000 / LEN 8 (the bootloader's last packet: looks like a
  physical address -> DSI DMA probably not behind an SMMU), TRIG_CTRL 4 (SW), INTR_CTRL 00000500.
  Buffer page 0x9d124000 all zero. `set 200` correctly refused (wanted video mode).
- Change: send_dcs accepts command mode (CTRL bit0 and bit1|bit2); probe prints the bootloader's last packet.
  Rebuild panelbright; run `panelbright probe` again (for the packet) then `panelbright set 200`.
- Unknown: whether a DMA command collides with the display controller's auto-refresh in command mode (Qualcomm's
  driver pauses autorefresh around commands). If the screen freezes: reboot.

# Active handoff — panel brightness over DSI0 (2026-11-09, Claude)

- User approved real panel brightness. NEW userland/jb/panelbright.c (build.sh -> out/panelbright, /usr/local/bin):
  `probe` (READ-ONLY: DSI0 0x0ae94000 registers + command buffer page), `set N` (DCS long write 0x51 hi lo,
  N 0-1023), `raw HEX..` (any DCS long write). Mechanism = Linux msm video-mode transfer: CTRL |= CMD_MODE_EN,
  CMD_DMA_CTRL embedded (bit28) HS, TRIG_CTRL DMA trigger SW (4), DMA_BASE/LEN, TRIG_DMA=1, poll INTR_CTRL bit0
  (CMD_DMA_DONE, 100 ms), restore CTRL/CMD_DMA_CTRL/TRIG_CTRL. Offsets = DSI v1 + 4 (Linux io_offset; LK mipi_dsi.h).
  Packet buffer: 0x9d124000 (splash 0x9c000000 + 3120*5760 page-aligned; low DDR bank, unused by XNU; inside the
  region the display already reads, in case MDSS's SMMU only maps the bootloader's splash).
- `set` refuses unless DSI0 HW_VERSION is 0x20xxxxxx and CTRL has enable+video. Risk: panel may blank or show
  garbage until reboot if the DMA reads the wrong memory. Waiting for the probe output before the first `set`.
- If it works: sweet-displayd sends the slider as the panel level (PCC back to Night Shift only).

# Active handoff — software dimming limit; real brightness needs the panel (2026-11-09, Claude)

- User: dither changes nothing; dimming wrecks colours (red black at 10%). Cause: PCC scales encoded pixel values,
  so dimmed content lands in the AMOLED's lowest codes (black crush / poor near-black colour). Not fixable in the
  colour stage. sweet-displayd now maps the slider to 50%-100% only (k = 0.5 + 0.5 level).
- Real brightness: panel DCS 0x51 (set display brightness) over DSI0 from the kernel or /dev/pmem, without a
  display driver (DSI command DMA/FIFO while the bootloader's video mode runs; SMMU of MDSS unknown). Not started:
  waiting for the user's go-ahead (risk: wrong DSI writes can blank the panel until reboot).

# Active handoff — banding fix test + iOS Night Shift investigation (2026-11-09, Claude)

- sweet-displayd -1: brightness follows (BKS works) but CBBlueLightClient getBlueLightStatus: returns NO, all 0.
  No CoreBrightness preferences file exists (/var/mobile, /var/root). SpringBoard/Preferences carry only the
  entitlement com.apple.backboard.displaybrightness. => iOS's Night Shift itself is not running on the port
  (CoreBrightness in backboardd presumably found no display/backlight it supports). User chose: make iOS's own
  Night Shift work (not a private toggle).
- User: dimming bands/blotches at low brightness (PCC scaling of 8-bit pixels on AMOLED).
  dispcolor now has `dither on|temporal|off`: PINGPONG0/1 dither block (Linux dpu: PP + 0x30e0: +0 enable,
  +4 depth 2 bits/comp (2 = 8 bit, bit 8 temporal), +8 4x4 matrix 15,7,13,5,3,11,1,9,12,4,14,6,0,8,2,10).
  Test: dispcolor brightness 10; dispcolor dither on (then temporal, then off) and compare. If it helps,
  sweet-displayd should enable it.
- Night Shift next steps:
  1. `out/oslog` is now built with `flame/oslog-ents.plist` entitlements and installed as `/usr/local/bin/oslog`
     on `192.168.0.70`; pulled-back copy matches. SHA-256 `8f28d2afa73b5533c8848dc7414928ef0fb2ab15c51b54a28a4f1854eabe1e9a`.
     Capture CoreBrightness's start-up messages:
     oslog -t 40 -l backboardd > /var/root/bb.log &  then killall backboardd, wait 40 s,
     grep -a -i -E 'bright|bluelight|CBDisplay|night|backlight|aab|display module' /var/root/bb.log | head -80
  2. CoreBrightness binary for offline reading: if `ipsw` and the 20H392 shared cache are available on the Mac,
     `ipsw dyld extract <dyld_shared_cache_arm64> CoreBrightness -o research/corebrightness/` (+ --strings dump),
     put it in this folder for Claude (llvm-objdump here).

# Active handoff — sweet-displayd: brightness WORKS; Night Shift fix attempt (2026-11-09, Claude)

- User: brightness slider works through sweet-displayd; Night Shift does not.
- Change: "on" = status byte 0 (active) OR byte 1 (enabled) (macOS tools use enabled); strength < 0.01 while on
  -> 0.5; `-1` now prints the first 40 status bytes. Rebuild sweet-displayd, then with Night Shift ON run
  `sweet-displayd -1` and with it OFF again, and compare (ask the user for both outputs if still not working).

# Active handoff — sweet-displayd: iOS brightness + Night Shift -> DSPP PCC (2026-11-09, Claude)

- dispcolor WORKS (user: rgb, brightness, night, night+bright, off all as expected). PCC path confirmed.
- NEW userland/jb/sweet-displayd.m (+ sweet-displayd-ents.plist; build.sh builds and signs out/sweet-displayd):
  polls 10x/s BKSDisplayBrightnessGetCurrent() (slider) and CBBlueLightClient getBlueLightStatus:/getStrength:
  (Night Shift), writes DSPP0/1 PCC + CTL0 flush on change (same checks as dispcolor; identity -> PCC off).
  Brightness curve: scale = max(0.03, level^1.8); Night Shift full = G 0.70, B 0.40 (x strength).
  `sweet-displayd -1` prints what iOS reports and exits (writes nothing); `-v` logs every change.
- Install: /var/local/bin/sweet-displayd. Boot job com.sweet.displayd added to tools/sweet-root-tweaks.py NET_JOBS
  (--net), KeepAlive, output /var/local/displayd.out. Not compiled by me (no SDK); entitlement names are guesses.
- Unknown until tested: whether BKSDisplayBrightnessGetCurrent follows the slider with no backlight hardware,
  and whether CBBlueLightClient answers from a root daemon.

# Active handoff — dispcolor: DSPP PCC by hand (2026-11-09, Claude)

- dispprobe result: MDSS 0x50000001; MDP base 0x0ae01000 (TOP+0 = 50000001). CTL0 is the only active path:
  LAYER(LM0) 01000005, LAYER(LM1) 01000028, TOP 00020000, INTF_ACTIVE 2 (INTF1 = DSI0). LM0 and LM1 OUT_SIZE
  0c3002d0 = 720x3120 each (split 1440x3120). DSPP0/1: all 0, PCC off, coefficients 0. Output stopped after the
  DSPP2 row (PCC2 read): possibly a bus hang on an unused block -> never touch DSPP2+.
- NEW userland/jb/dispcolor.c (build.sh: out/dispcolor -> /usr/local/bin): writes ONLY DSPP0/1 PCC (diagonal
  S3.15 matrix, 0x8000 = 1.0) + CTL0 FLUSH bits 13|14, after checking version/INTF_ACTIVE/LM sizes match the probe.
  dispcolor show | rgb R G B | brightness PCT | night PCT [BRIGHT] | off. Undo = `dispcolor off` or reboot.
- If `rgb` shows no change: FLUSH may need bit 17 (CTL) too, or PCC wants the DSPP op-mode enable; report `show`
  output and CTL0 FLUSH value printed after the write.
- Next after it works: a kernel backlight service (iPhone X: backlight node next to disp0) so CoreBrightness sends
  slider / Night Shift values, applied with the same registers from the kernel.

# Active handoff — brightness / Night Shift: display controller probe (2026-11-09, Claude)

- Brightness slider / Night Shift change nothing: while moving the slider and toggling Night Shift the fake IOMFB
  got only selectors 4/5/6 (+12, 79): CoreBrightness never drives the display (iPhone X uses a backlight service
  next to disp0, which the port does not publish; IOK trace of backboardd was no longer in dmesg).
- Plan (no display driver needed): the DPU's DSPP PCC (3x3 colour matrix after each layer mixer, as Linux night
  light) for Night Shift and AMOLED dimming; then a kernel backlight service so iOS's slider/Night Shift reach it.
- Step 1: userland/jb/dispprobe.c (READ-ONLY, /dev/pmem; build.sh builds out/dispprobe -> /usr/local/bin).
  Dumps MDSS version, MDP TOP, CTL0-5, LM0-5, DSPP0-3 + PCC, PP0-3, INTF0-3 for MDP base 0x0ae01000 and 0x0ae00000.
  Run `dispprobe` and paste the whole output to Claude. Do NOT write PCC registers before that is read.

# Active handoff — 0196 WORKS; default direction now counter-clockwise (2026-11-09, Claude)

- User: Angry Birds Classic now runs and draws in landscape, but upside down with the clockwise turn.
  Workaround on the installed bundle: touch /var/local/sweet-rotate-ccw; killall backboardd.
- Source changed: counter-clockwise is the default; the override file is now /var/local/sweet-rotate-cw.
  After the next bundle rebuild, delete /var/local/sweet-rotate-ccw (no longer read).
- Open: a game that picks the other landscape orientation would be upside down; the proper fix is to take the
  direction from the interface orientation (CA's descriptor rotation SPI value is logged as "descriptor rotation N").

# Active handoff — 0196 rotated CoreAnimation framebuffer (landscape apps) (2026-11-09, Claude)

- AB Classic stale screen = the AB2 cause, confirmed: backboardd mtl2.log repeats `texture over buffer refused:
  RowPitch { bytes_per_row: 4608, row_bytes: 9744 }` (CA asks its 1125x2436 framebuffer surfaces as 2436x1125
  textures while a landscape app is in front). backboardd composes with Metal, NOT libsweet-gl: 0195's eagl.c log
  lines never print (harmless). `rotated view asked` (0189) did not print either -> the installed bundle may predate it.
- 0196 (userland/metal2/NativeBridgeMetalFrontend.mm, bundle rebuild): on that refusal (descriptor w/h = surface h/w,
  not planar) the device returns a private landscape texture of ours ("shadow", cached per surface/format/size, it
  keeps CA's drawing so loads/partial redraws work; -iosurface answers the surface) instead of nil. Every command
  buffer whose render pass targets a shadow (or a view of it) gets, at commit, one full-screen pass drawing the shadow
  turned 90 degrees into a portrait texture over the surface. The turning shader is MSL compiled at first use with
  newLibraryWithSource (works per mtltest). Direction: clockwise by default (landscape top row -> surface's right
  column; phone held with home edge on the right). `touch /var/local/sweet-rotate-ccw` flips it,
  `touch /var/local/sweet-norotate` disables 0196 (read when a shadow is made; respring to apply).
- Logs (mtl2.log, backboardd): `rotated framebuffer: surface 0x.. 1125x2436 asked as 2436x1125 ...: a landscape
  texture of ours, turned clockwise ... (descriptor rotation N)`, `rotated framebuffer: turn N into surface 0x..`
  (1st-3rd, every 600th); failures: `the turning shader does not compile`, `no pipeline for format`.
- Not compiled by me (no iOS SDK here). Install: rebuild SweetMetal2.bundle (userland/metal2/build.sh), install,
  reboot (backboardd loads the bundle at start). Expected: AB Classic (and AB2) draw in landscape; if upside down,
  touch /var/local/sweet-rotate-ccw and killall backboardd.

# Active handoff — 0195 window-server log for big app surfaces; mgtool setint/unset (2026-11-09, Claude)

- 0194 installed; AB Classic still shows a stale picture. Its present lines: main thread, kCFRunLoopDefaultMode,
  3600 presents in ~1 min -> the app side is fine (0194's main-thread theory was wrong; flush + seed bump kept).
- Kernel during the blank: 10-38 swaps/s, every swap ONE layer = surface 2 (CA's 1125x2436 framebuffer), not black.
  So backboardd composes the game into its own framebuffer and redraws, but its picture of the game never changes.
  backboardd logged nothing in gl.log (its -texImageIOSurface lines are capped at 24 since boot).
- 0195 (eagl.c, logging only): in the window server, for surfaces >= 2,000,000 pixels shown as textures:
  "big surface N (...) as texture T: ... : its own memory (shared) | a copy" at import (first 60), and
  "big surface N (texture T, shared|copy): bound B times, refilled R, seed S (last copied C), frame F" at bind
  1, 30 and every 600 (first 80). Rebuild libsweet-gl.dylib; backboardd must restart to load it (reboot or
  killall backboardd). Then run AB ~10 s and: grep -a 'big surface' /var/local/gl.log | tail -30
- mgtool: ArtworkDeviceSubType is not in CacheExtra (MobileGestalt computes it). Added `mgtool setint NAME INT`
  (adds or replaces) and `mgtool unset NAME`. setint ArtworkDeviceSubType 2556 did nothing: the real place is
  an ENTRY of the ArtworkTraits answer (CacheExtra key oPeik/9e8lQWMszEjbPzng, the one Cowabunga edits).
  mgtool now takes GROUP/ENTRY and prints dictionaries: mgtool get ArtworkTraits; mgtool setint
  ArtworkTraits/ArtworkDeviceSubType 2556 (refuses if ArtworkTraits is not cached). Remove the useless top-level
  one: mgtool unset ArtworkDeviceSubType. Rebuild out/mgtool -> /usr/local/bin/mgtool.

# Active handoff — 0194 app frames reach the screen (2026-11-09, Claude)

- Also: mgtool is not on the phone any more (searched /var/local, /var/root, /var/tmp, /usr/local, /var/jb).
  build.sh now builds out/mgtool (userland/jb/mgtool.c). Install to /usr/local/bin/mgtool (mkdir -p /usr/local/bin).
  The user wants to try the Dynamic Island: mgtool set ArtworkDeviceSubType 2556 (undo: 2436), killall SpringBoard.

- 0193 worked: Angry Birds Classic no longer crashes at startup (no new .ips; no AL line, so either the real OpenAL
  device opened or audio setup has not been reached yet).
- New symptom (the same as AB had before 0191): blank screen; one frame shows per lock/unlock. gl.log shows
  "present 600 of renderbuffer 5": the game presents continuously, the frames do not reach the display.
- 0194 in eagl.c -presentRenderbuffer: (1) after glFinish, IOSurfaceLock/Unlock (write) to move the surface's seed,
  as all other GPU writes in eagl.c do; (2) [CATransaction flush] on the main thread too (was: other threads only).
  On the main thread the explicit commit is nested in the run loop's implicit transaction; a game running its own
  loop never lets the run loop commit it. The present log line now says which thread and which run loop mode.
- Install: rebuild libsweet-gl.dylib (libsweet-glapp unchanged since 0193); .new + mv -f. Check:
  grep -c -a 'run loop mode' /usr/lib/libsweet-gl.dylib
- Watch Cut the Rope for regressions (it presents from a display link on the main thread).

# Active handoff — 0193 silent OpenAL for apps (2026-11-08, Claude)

- Angry Birds Classic crash (SIGSEGV at 0x10, AngryBirdsClassic-2026-11-08-235018.ips) is NOT graphics: its GL
  context, shaders and textures work. Main thread, -[UIApplication _handleDelegateCallbacks...] -> game code ->
  "createStartUpAssets" (0x10007f2f4) reads app->[0x30] (game::AudioSystemComponent), which is NULL, at 0x100720ce8
  (ldr x8,[x0,#0x10]). It is NULL because alcOpenDevice(NULL) fails (no audio on the port): the game throws
  "Failed to open OpenAL device.", catches it and carries on without audio.
- FIX: userland/gpu/nullal.c, built into libsweet-glapp.dylib (build.sh: + gpu/nullal.c, -framework OpenAL).
  Interposes the 21 OpenAL calls the game imports. Real device opens -> all calls pass through unchanged.
  It does not -> a fake device/context and a timing-only model of sources/buffers (plays nothing; buffers finish
  at their real length, so streaming and one-shot sounds behave). Logs once to /var/local/gl.log:
  "AL[pid name]: redwood-al: OpenAL has no device (default): sounds go to a silent stand-in".
- Install: rebuild libsweet-glapp.dylib only (libsweet-gl unchanged); install via .new + mv -f (signature cache).
  No change to the game binary (it already loads libsweet-glapp). Check: grep -c -a redwood-al /usr/lib/libsweet-glapp.dylib
- Host-tested the model (stream of 3 x 0.1 s buffers, one-shot stop). Not compiled for iOS by me (no SDK here).

# Active handoff — 0192 app shaders untouched (2026-11-08, Claude)

- RESULT: Cut the Rope Experiments WORKS (user, 23:50) with libsweet-gl (0190-0192) + libsweet-glapp (0191) and both
  load commands in ctr-jni. OpenGL ES apps now run on the GPU. Same recipe for any GL app (e.g. Angry Birds Classic).
- Still open: commit all of this (kernel patches 0169-0185, frontend + eagl.c + glapp.c + tools/insert-dylib.py);
  GPU hang recovery (daily-wheel shader hang); AB2 landscape (CA rotated framebuffer textures, 0189 trace);
  sharegroup contexts are not shared yet.

- 0191 result (Cut the Rope): app GL calls reach Mesa; setCurrentContext ours; drawables created (renderbuffer 1:
  375x812, renderbuffer 3: 1125x2436, 3 surfaces each). Every app shader failed: "#version must appear on the first
  line": my_glShaderSource rewrites CoreAnimation's shaders (#version 300 es + vertex_start/fragment_start macros +
  all_highp) ahead of the app's own "#version 100". Follow-on errors 0x501/0x502 = programs missing.
- CHANGE `userland/gpu/eagl.c`: in apps (in_window_server false, set in install()) glShaderSource passes the app's
  text unchanged (kept for the failure log); say_long() now goes through say(), so shader texts reach gl.log.
  Other CA-specific hooks checked: internal_format only maps explicit GL_BGRA; plain/group draws need program 0 or CA
  group textures; deferred clear only for texture-backed framebuffers -> left as is.
- Test: rebuild libsweet-gl (only it; install .new + mv), reopen ctr-jni,
  `grep -a ctr-jni /var/local/gl.log | grep -vE 'replaced|asked for number' | grep -iE "shader|present|error|drawable" | head -30`.

# Active handoff — 0191 app GL calls reach Mesa (Cut the Rope) (2026-11-08, Claude)

- gl.log result (0190 build, after fixing install): libsweet-gl loads in ctr-jni, 13/13 methods replaced, context
  created on FD640 (ES 3.2 Mesa), but `-renderbufferStorage: renderbuffer 0 ... refused` and every present "has no
  drawable": the app's own gl* calls (OpenGLES.framework exports) never reached our table, because they look up the
  context in OpenGLES's internal state, which our contexts never fill (CoreAnimation calls the table directly).
- Install lesson: replacing a signed dylib IN PLACE keeps the kernel's cached signature -> dyld "code signature does
  not cover entire file". Always copy to NAME.new, then `mv -f NAME.new NAME`.
- CHANGES (not compiled yet):
  - `userland/gpu/eagl.c`: +[EAGLContext setCurrentContext:] / +currentContext replaced in apps (thread-local
    app_current); exported `void **sweet_gl_current_table(void)`.
  - NEW `userland/gpu/glapp.c` -> libsweet-glapp.dylib: dyld __DATA,__interpose of OpenGLES's gl* exports onto the
    current thread's table; linked against OpenGLES + libsweet-gl only. Cross-compiled here with clang
    -target arm64-apple-ios16.0: 433 interpose entries, glBindRenderbuffer -> table[0x14d8/8](table[0], ...).
  - `gpu/mkeagl.py`: new section EAGL_GEN_INTERPOSE; optional 5th arg = names iOS's OpenGLES exports (only those
    are interposed; anything else could bind to Mesa and recurse). `build.sh` makes out/opengles-exports.txt from
    the SDK's OpenGLES.tbd and builds libsweet-glapp.dylib.
- Steps: userland/build.sh; install BOTH dylibs via .new + mv; add the second load command to the app:
  `tools/insert-dylib.py ctr-jni /usr/lib/libsweet-glapp.dylib`, codesign -f -s -, copy back via .new + mv.
  Expect gl.log: `+[EAGLContext setCurrentContext:] ours`, `an app's drawable: renderbuffer N (N != 0)`,
  `first present ... read 0x8cd5 draw 0x8cd5`, `present 600 ...`.

# Active handoff — 0190 OpenGL ES for apps (Cut the Rope, Angry Birds Classic) (2026-11-08, Claude)

- Cut the Rope Experiments (/Applications/ctr-jni.app, exe ctr-jni): links Metal and OpenGLES; MTKView and
  MTLCreateSystemDefaultDevice present, but at runtime it only creates the Metal device (mtl2.log: no library, queue,
  drawable or pipeline) and draws with OpenGL ES (CAEAGLLayer, renderbufferStorage, presentRenderbuffer) -> no GL
  in apps -> white screen. Same gap as Angry Birds Classic.
- CHANGE `userland/gpu/eagl.c` (NOT compiled yet; build with userland/build.sh, libsweet-gl.dylib):
  - app contexts are created as ES 3 underneath (ES2 apps still told 2) so the present can use glBlitFramebuffer;
  - new EAGLContext methods (apps only; skipped in backboardd): initWithAPI:, initWithAPI:sharegroup: (sharegroup
    NOT shared, logged), renderbufferStorage:fromDrawable: (Mesa RGBA8 renderbuffer of the layer's pixel size + 3
    BGRA IOSurfaces made textures of their own memory via redwood_gl_surface_texture),
    presentRenderbuffer: (+atTime:, +afterMinimumDuration:): blit the renderbuffer upside down into the next surface's
    texture, glFinish, layer.contents = surface inside a CATransaction (flush off the main thread).
  - log lines: `redwood-gl: an app's drawable: ...`, `first present of renderbuffer ... read/draw status`,
    `present N of renderbuffer ...`.
- NEW `tools/insert-dylib.py BINARY /usr/lib/libsweet-gl.dylib`: adds an LC_LOAD_DYLIB (thin/fat arm64), tested on
  mtltest (llvm-objdump shows the new dylib; second run reports "already loads").
- Steps: build + install libsweet-gl.dylib to /usr/lib on the phone; copy the app executable to the Mac
  (scp root@PHONE:/Applications/ctr-jni.app/ctr-jni .), keep a .orig copy, run insert-dylib.py, `codesign -f -s -`,
  copy back, `chmod 755`. Same for /Applications/AngryBirdsClassic.app/AngryBirdsClassic.
  Logs: libsweet-gl's say() output goes to stderr and (since this change) to /var/local/gl.log if it exists
  (`touch /var/local/gl.log; chmod 666 /var/local/gl.log`), lines "GL[pid name]: redwood-gl: ...".
  First result: library installed (23:18), ctr-jni patched; run from SSH: libsweet-gl loads and replaces all 13
  EAGLContext methods (no scene from SSH, so no context is made there). Home-screen launch: stderr not visible -> gl.log.
- Risks: sharegroup contexts (async texture loading) get no shared objects; multithreaded GL relies on eagl.c's
  single global `current`; first build may need compile fixes.

# Active handoff — AB2 parked; Cut the Rope next (2026-11-08, Claude)

- AB2: user reports the game now demands an app update and still shows black; parked. Pending: install the bundle
  with the 0188/0189 traces and check `rotated view asked` (CA's 2436x1125 views of the portrait framebuffer).
- Cut the Rope Experiments (process ctr-jni): white screen; previous run had a Turnip device but no render
  pipelines -> suspected OpenGL ES app (like Angry Birds Classic). Waiting for executable/framework check.
  If GL: build app-side EAGL (inject libsweet-gl + CAEAGLLayer renderbufferStorage:fromDrawable: /
  presentRenderbuffer:), which would cover AB Classic too.

# Active handoff — AB2 black = CA's rotated framebuffer textures refused (2026-11-08, Claude)

- Before this rebuild the phone ran the 22:29 bundle (no `watched surface` string); the new diagnostic build below
  is now installed.
- That old bundle already shows the cause: with a landscape app in front, backboardd asks for its portrait framebuffer
  surfaces 0x2/0x3 (1125x2436, row 4608) as 2436x1125 textures (format 80) -> `texture over buffer refused:
  RowPitch { bytes_per_row: 4608, row_bytes: 9744 }`. CA then has no render target -> black screen.
  Hypothesis: Apple's driver supports a private texture rotation (descriptor SPI) so CA renders "landscape" into the
  portrait scanout surface; our frontend ignores it.
- CHANGE (frontend, bundle rebuild): on such a refusal (descriptor w/h = surface h/w) log once (max 6)
  `rotated view asked: surface .. as WxH usage storage type; rotation=.. swizzle=.. ...` (descriptor SPI values).
- Planned fix if confirmed: a landscape shadow texture per surface for rendering, rotated into the surface by a
  compute kernel (compiled at runtime via newLibraryWithSource) at the end of each command buffer, and rotated back
  in before passes that load it.
- Rebuilt with the latest frontend probes, verified the ad-hoc signature and diagnostic strings, and installed over
  SSH. Archive: `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-SweetMetal2-ab2-rotated-framebuffer-probe.tar.gz`,
  SHA-256 `8164d0e714623b63f324a85c10edb872a360f3e14e19d9820c96e7c5b634d560`.
- Previous bundle preserved as `/System/Library/Extensions/SweetMetal2.bundle.pre-ab2-probe-20261108-224909`.
  Reboot before testing so backboardd loads the `watched surface` and `rotated view asked` probes.

# Active handoff — AB2: watch the game's surface in backboardd (2026-11-08, Claude)

- Landscape probe result: AB2's drawable IOSurface 58 (0x3a) is stored 1125x2436 (portrait) and backboardd sees it
  with content (CPU and GPU 0/64 black). The big-texture trace hit its 60-entry cap on CA's own blur/downsample
  passes over surfaces 2/3/0x17/0x18 and never showed a use of the game's surfaces.
- CHANGE (frontend, bundle rebuild): surfaces whose "seen here" CPU probe finds content (black < 32/64) go on a
  watch set; every binding of such a surface (any encoder, any stage, incl. texture views) logs
  `watched surface 0x.. bound: <encoder class> stage S index I view WxH fmt type | pipe ... | target ...`, and blit
  copies from/to it log `watched surface blit: ...` (first 80 total).
- Test: bundle rebuild + reboot, `: > /var/local/mtl2.log`, open AB2 ~20 s,
  `grep -a backboardd /var/local/mtl2.log | grep -E 'watched surface|seen here' | head -40`.

# Active handoff — Angry Birds 2 black: backboardd's rotated composite (2026-11-08, Claude)

- With the MTLBlitOption fix AB2 runs and renders (pre-present 240+ frames, 0 black; landscape drawables 2436x1125).
  Screen black: every swap is ONE layer = CA framebuffer surface 3 with rec[0x467]=1, 64/64 black at display. So for
  a landscape app CA does not scan the game layer out directly; backboardd composites it (rotated) into its portrait
  framebuffer surface, and that composite is black.
- CHANGE (frontend, bundle rebuild): the "seen here" CPU/GPU probe and the big-texture draw trace now trigger on
  w*h >= 2,000,000 (they required h >= 2000 and skipped the 2436x1125 landscape drawables).
- Test: rebuild/install bundle, reboot (backboardd loads it at start), `: > /var/local/mtl2.log`, open AB2 ~20 s,
  `grep -a backboardd /var/local/mtl2.log | grep -E 'seen here|big-texture draw'`.

# Active handoff — Angry Birds (2026-11-08, Claude)

- User: 0185 works "well enough" (flicker gone, overlays visible). Now Angry Birds Classic and Angry Birds 2 crash.
- /var/local/mtl2-clients contains "*" (every process gets a Metal device).
- Angry Birds Classic (GLES2 only): crash 22:06:51, EXC_CRASH SIGABRT, abort() from std::terminate inside a
  FrontBoard main-queue block during launch (uncaught C++/ObjC exception, ~0.3 s after launch). Consistent with the
  app's own "Failed to create EAGLContext" path: no OpenGL ES driver in app processes. libsweet-gl (eagl.c) is a
  window-server-only EAGL (replaces initWithAPI/API/setParameter/getMacroContextPrivate/texImageIOSurface/
  attachImage); it has no CAEAGLLayer path (renderbufferStorage:fromDrawable:, presentRenderbuffer:) and is only
  injected into backboardd. Running GL apps needs: app-side injection + a CAEAGLLayer drawable implementation.
- SubwaySurf report 22:06:54 is FrontBoard 0x8BADF00D "failed to terminate gracefully after 5.0s" in background
  (not a game crash).
- Angry Birds 2 (com.rovio.baba, Unity 6000.3.6f1, il2cpp, Metal via UnityFramework; IPA carries injected
  CydiaSubstrate/iGameGod/AngryBirds2.dylib tweak): ran fine from SSH, but SpringBoard never exec'd it.
  Info.plist UIRequiredDeviceCapabilities = arm64, metal, gamekit; MobileGestalt answers fixed with
  userland/jb mgtool (setbool metal/gamekit true) -> now launches, shows the notification prompt, then crashes
  Crash 22:27:20: EXC_BAD_ACCESS at 0xc0 after the frontend logged
  `MISSING NBBlitEncoder -copyFromBuffer:...toTexture:...destinationOrigin:options:` (every launch).
  CHANGE (frontend, bundle rebuild): added the MTLBlitOption forms of copyFromBuffer:toTexture: and
  copyFromTexture:toBuffer:, forwarding to the existing copies (aspect options logged once, not honoured).
- Rebuilt the bundle, verified its signature and blit-option selectors, then installed over SSH. Archive:
  `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-SweetMetal2-blit-options.tar.gz`,
  SHA-256 `79f3865c21cb07c40ebbfcfe59bfee42bf388f1e37570e6f332f1fc828853f32`.
- Previous bundle preserved as `/System/Library/Extensions/SweetMetal2.bundle.pre-blit-options-20261108-222944`.
  Reboot before retesting Angry Birds 2 so it loads the new bundle.

# Active handoff — 0185 layer order: higher index on top (2026-11-08, Claude)

- After 0184 (flicker gone) the game "froze" mid-run: GPU fine (no HANG), game process alive but idle (1.7% CPU),
  last 51 swaps set only CA's layer 1 and kept the game's layer 0; WebKit.GPU had just allocated ~150 MB (likely an
  ad/overlay). `killall SubwaySurf` -> home screen back: CA turns the layer off correctly on app exit (0184 handles
  it). So during the "freeze" CA still had layer 0 on and showed something in layer 1, which 0171's "layer 0 on top"
  covered. Conclusion: higher index = top (app Metal layer under CA's layer with a transparent hole).
- NEW `xnu/patches/0185-guacamole-fb-higher-layer-on-top.patch` (after 0184): order[0] = highest index; a 2-layer
  frame whose top layer samples are all alpha 0 (and not flagged opaque) shows the lower layer alone on the GPU
  (`5s: two-layer frames with a transparent top layer (lower layer alone): N`); a partly transparent top goes to the
  CPU compose, which now draws layer 0 first, blends upper layers, and leaves out all-transparent upper layers.
  Boot-arg `sweetfbtop=0` restores 0171's order. Known gap: 8x8 sampling can miss tiny overlays (home indicator).
- Test: build 0169-0185; play incl. menus and an ad/popup; check flicker stays gone, overlays visible, and
  `dmesg | grep 'SWEET FB: 5s' | tail` (composed frames cost CPU time; if frequent, GPU blending is the next step).
- Built kernel with the previous XNU patches through 0184 plus 0185 using Xcode 27.2 / macOS 13.5 KDK 22G74.
- Boot image: `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0185-60hz-gpu-two-layers.img`,
  SHA-256 `7d73b4c9d42e5f3ae99b2710b431a4fb6919eb98705ad01d33ade571af0c724f`. It uses the known 60 Hz GPU-present
  args plus `sweetgpuhangms=150`; `sweetfbtop` is omitted (higher index on top by default). Not transferred or booted.

# Active handoff — ROOT CAUSE of the Subway Surfers flicker: IOMFB layers persist (2026-11-08, Claude)

- RESULT (user, 0169-0184): FLICKER GONE. A freeze happened mid-game (diagnosis pending; likely the GPU hang
  seen at the daily wheel, see 0179 HANG report).
- 0183 result: every black frame is a swap with ONE enabled layer, layer 1 = CA framebuffer surface 2/3, no layer 0.
  IOMobileFramebuffer keeps layers across swaps: a swap only sets the layers that changed (rec+0x104). CA sends only
  its UI layer (1) when just the UI changed and expects the game layer (0) to stay. Our AppleCLCD treated every swap
  as the whole screen -> CA's layer alone, its transparent hole where the game is -> black. Explains: menus flicker
  more (UI changes more), backboardd never samples the game texture (it never has to), game frames always fine.
- NEW `xnu/patches/0184-guacamole-fb-layers-persist.patch` (SweetFramebuffer.cpp, after 0183): swap() merges the
  incoming record with the remembered layers (id, rects a/b, transform, rec[0x467+l]) for layers the swap does not
  set; rec+0x108 bits = turn layer off; id 0 = off. Boot-arg `sweetfbpersist=0` = old behaviour.
  Logs the first 30 merges: `SWEET FB: swap N sets 0x.. off 0x.. kept 0x.. -> layers on 0x.. (surfaces ...)` and a
  5 s line `swaps that kept an earlier layer: N`. Note: 0169's "re-swapped pending surface" counter now also counts
  kept layers (harmless).
- Risk to check: when the game closes, CA must turn layer 0 off (NULL surface or 0x108). If the last game frame
  stays on screen after leaving the game, the "off" signal is something else; the merge log shows it.
- Test: build 0169-0184, play ~30 s incl. menus; flicker should be gone and `display-time samples` other-top-layer
  black ~0. Then leave the game and confirm the home screen shows normally.
- Built kernel with the previous XNU patches through 0183 plus 0184, using Xcode 27.2 / macOS 13.5 KDK 22G74.
  Boot image uses 60 Hz GPU-present args plus `sweetgpuhangms=150`; persistence is enabled by default
  (`sweetfbpersist=0` omitted), and ring waiting remains off.
- Image: `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0184-60hz-gpu-two-layers.img`,
  SHA-256 `d74172928049346deed5f27318003f4c9b54b029aaf146e2e281618a102d7550`. Not transferred or booted.

# Active handoff — backboardd never draws the game's texture in black frames (2026-11-08, Claude)

- Draw-trace result (bundle with 'big-texture draw' confirmed installed): while 60-68 black "other top layer"
  frames per 5 s were shown, backboardd made NO draw sampling a >=1000x2000 surface. CoreAnimation does not draw the
  game into those compositions at all; it presumably expects the game as a separate display layer.
- NEW `xnu/patches/0183-guacamole-fb-black-frame-layout.patch` (SweetFramebuffer.cpp, after 0179; kernel patch
  numbers 0180-0182 were used as labels for frontend changes): black-at-display lines now the first 40 then every
  150th, each followed by every enabled layer of that swap (`SWEET FB:   layer q surface id a .. b .. xform flags`).
- Test: build 0169-0183 kernel, play ~30 s, `dmesg | grep -A4 'black at display' | tail -40`.
- Built the kernel with the prior XNU patches through 0179 plus 0183 using Xcode 27.2 / macOS 13.5 KDK 22G74.
  Verified the image contains the new per-layer black-frame layout logs and boot arg `sweetgpuhangms=150`.
- Boot image: `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0183-60hz-gpu-two-layers.img`,
  SHA-256 `98f11e70190bcb68c5a1a8cc4541e6a3337f61e303cba75b4055e573620f4948`. It uses the usual 60 Hz GPU-present
  args; `sweetmtlringwait` is omitted. Not transferred or booted.

# Active handoff — the black is in CoreAnimation's draw of the game's texture (2026-11-08, Claude)

- Probe result: game drawables 0x2f/0x30/0x31 in backboardd: 0/64 black by CPU AND 0/64 by GPU (blit through the
  imported texture). Mapping and GPU import are correct. The composition draw that uses the texture yields black.
- CHANGE (frontend, bundle rebuild): draw trace. Pipelines remember label/vertex+fragment function names/colour
  format/blend factors/write mask; samplers their filter/address/normalized/border; render encoders their colour
  target. Every draw while a >=1000x2000 surface is bound for the fragment stage logs each distinct combination once
  (max 60): `native-metal: big-texture draw (draw N xI): t0=surf 0x2f 1125x2436 fmt 80 | label=... vf=... ff=...
  fmt=... blend=... mask=... | samplers ... | target ...`.
- Test: rebuild/install bundle, reboot, `: > /var/local/mtl2.log`, play ~20 s,
  `grep -a backboardd /var/local/mtl2.log | grep 'big-texture draw'`.
- Rebuilt with the draw-trace frontend change, signature-verified, and installed over SSH. Archive:
  `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-SweetMetal2-big-texture-draw-trace.tar.gz`,
  SHA-256 `88ffd2fd25845cf826deafe398d06661fd04dd3ada9af2502fb6d087e2492b40`.
- Previous bundle preserved as `/System/Library/Extensions/SweetMetal2.bundle.pre-draw-trace-20261108-210725`.
  Reboot before testing so the services load this build.

# Active handoff — backboardd sees the game's pixels on the CPU (2026-11-08, Claude)

- Result of the "seen here" probe in backboardd: game drawables 0x2f/0x30/0x31 (47/48/49, the game layer's
  surfaces) 0/64 black through backboardd's own CPU mapping. Other 1125x2436 surfaces (2/3 = CA framebuffer surfaces,
  etc.) 64/64 black at creation (not yet drawn). No "texture refused", no "command buffer refused" in backboardd.
  So backboardd's IOSurface mapping is correct; the black compositions come from the GPU side (import or draw).
- CHANGE (frontend, bundle rebuild): the probe also blits the same 64 points through the new texture with this
  process's GPU (own queue, waitUntilCompleted) and logs `... N/64 black by CPU, M/64 by GPU (65 = probe failed)`.
  GPU black while CPU has content -> the GPU import (pinned pages / Turnip host-pointer import) is not the
  surface's memory. GPU content too -> the composition draw itself (pipeline/sampling) is the problem.

# Active handoff — two separate bugs: wheel GPU hang, black compositions (2026-11-08, Claude)

- HANG report (0179, daily-wheel screen): stuck job = SubwaySurf (3833), CP inside an IB1 with 0x3c3 dwords left,
  rptr == wptr, RBBM_STATUS 0xe795e5 (CP, TSE, RAS, RB, CCU, LRZ, PC_DCALL, VFD, VPC, UCHE, SP, HLSQ busy; A2D idle):
  a 3D draw whose shader never finishes. SMMU FSR 0x400 = format field only, no fault; FAR stale. No GPU watchdog,
  so the phone stays frozen. pipelines.log tail before the freeze: Unity shaders TextMeshPro/Mobile/Distance Field,
  UI/Default, SYBO/Bend/Specials/Coin Glow, SYBO/Bend/Combined (no obvious wheel-only shader yet).
  TODO: kernel GPU hang recovery (hangcheck + reset + fail the job) like msm's a6xx_recover; find the looping shader.
- Flicker is a SEPARATE bug: with a cleared mtl2.log, backboardd DeviceLost = 0 and no refused submits, yet
  composited ("other top layer") frames are still 100% black. Earlier DeviceLost lines were from old boots.
- CHANGE (userland, bundle rebuild): `newTextureWithDescriptor:iosurface:plane:` returned nil SILENTLY when
  IOSurfaceGetBaseAddress() is 0/misaligned in the calling process. Now logs once per surface ID:
  `native-metal: surface 0x.. 'BGRA' WxH texture refused: base 0x0 alloc N (no mapping in this process?)`.
  (Correction: the kernel `SWEET SURF: wk ... -> 0x0` lines are IOReturn success for WebKit surfaces only; not
  evidence. The refused grep in backboardd was empty.)
- CHANGE 2 (same file): when a process makes a texture from a >=1000x2000 BGRA/RGBA surface, once per surface ID it
  samples 64 points through its own mapping (dcache invalidated) and logs
  `native-metal: surface 0x.. WxH seen here: N/64 black (base, alloc, seed)`. In backboardd for the game's drawables:
  mostly black -> backboardd's mapping is not the game's memory (kernel IOSurface mapping); content -> GPU-side
  import/draw problem. Test: rebuild bundle, play ~20 s, `grep -a backboardd /var/local/mtl2.log | grep -E 'seen here|texture refused'`.
- Rebuilt with the current userland sources and verified the ad-hoc signature. Archive:
  `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-SweetMetal2-surface-map-sample.tar.gz`,
  SHA-256 `1095bc94df5cddb2f643732df33eb22cd55374206c5644324de842d8bf02f7db`.
- Installed over SSH at `/System/Library/Extensions/SweetMetal2.bundle`; previous bundle preserved as
  `SweetMetal2.bundle.pre-surface-sample-20261108-203822`. The running services have not been restarted; reload the
  bundle via reboot or service restart before testing.

# Active handoff — 0179 hang report survives (2026-11-08, Claude)

- 0178 frontend result: "is not a MTLFunction" boot spam GONE; composited frames still 100% black (not the cause;
  keep the fix). Daily-wheel screen in Subway Surfers freezes the UI reproducibly, also without sweetmtlringwait:
  `GPU done 9436 of 9482`, game waits 9437 (its own job), killing the game does not recover (no GPU reset exists).
- Phone runs payload-guac-0177.bin (contains HANG strings; boot args sweetgpuhangms=150) yet no HANG line in dmesg:
  the 12-report cap was spent on short loading stalls (150 ms), and the 0175 1 ms-wait log flood rotated them out.
- NEW `xnu/patches/0179-guacamole-gpu-hang-report-keep-long.patch` (after 0177): WaitFence timeouts < 20 ms are
  not logged (Turnip's 1 ms loop); stalls >= 2 s are always reported (`SWEET GPU: HANG (long): ...`), own cap of 12,
  separate from the short-stall reports.
- Test: build 0169-0179, keep sweetgpuhangms=150, open the game until the wheel freezes it, then
  `dmesg | grep -E 'SWEET GPU: (HANG|report)'` (and the `GPU present: window` boot line if still there).
- TODO after diagnosis: GPU hang recovery (reset CP/ring, fail the stuck job's fences) so one hang does not
  freeze the phone until reboot.
- Built the kernel with all XNU patches 0169-0177 plus 0179 (0178 is the userland-only Metal frontend change),
  using Xcode 27.2 / macOS 13.5 KDK 22G74. Kernel strings confirm long-hang reporting and the ring-wait toggle.
- Boot image `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0179-60hz-gpu-two-layers.img`,
  SHA-256 `b7b41e2e9ef5b8a80b93b16d31a0bc664c0f10ae691ec96ec7a417713df22339`. It has the usual 60 Hz GPU-present
  args plus `sweetgpuhangms=150`, with `sweetmtlringwait` omitted. Not transferred or booted.

# Active handoff — 0178 MTLFunction conformance in the Metal frontend (2026-11-08, Claude)

- User: with 0176/0177 safe boot no freeze, still flicker. Freeze seems tied to Subway Surfers' daily-wheel
  screen (does not always appear). Boot console always shows backboardd QuartzCore spam:
  `fragmentFunction is not a MTLFunction.` / `vertexFunction is not a MTLFunction.` (also in NOTES cgtest 2026-11-06).
- Suspect: Metal's pipeline descriptor rejects NBFunction (no formal protocol conformance; same as the earlier
  "texture is not a MTLTextureImplementation" fixed by class_addProtocol), so CoreAnimation lacks some pipelines
  and skips those draws -> backboardd's compositions of the game black, home indicator fine.
- CHANGE (userland only, bundle rebuild): `NativeBridgeMetalFrontend.mm` adds MTLFunction to NBFunction and
  MTLLibrary to NBLibrary via class_addProtocol, logging `native-metal: NBFunction MTLFunction adopted`.
  Public protocols only (private SPI conformance could route callers into unimplemented SPI).
- Test: rebuild SweetMetal2.bundle, install, reboot (kernel unchanged, 0169-0177 fine). Check the boot console no
  longer shows "is not a MTLFunction", flicker status, and `dmesg | grep 'display-time samples' | tail` while playing
  (other top layer black should drop).

# Active handoff — 0177 catch short GPU stalls (2026-11-08, Claude)

- User: on a boot without `sweetmtlringwait=1` the game flickers again (expected: ring refusal -> DeviceLost).
  Working model: one underlying fault, a game GPU job that stalls; refusing work turns it into DeviceLost/flicker,
  waiting for ring room (0174) turns it into a permanent hang. Need the CP state during the stall.
- NEW `xnu/patches/0177-guacamole-gpu-hang-threshold-bootarg.patch` (sweet_gpu.c, after 0176): boot-arg
  `sweetgpuhangms` (default 2000, min 20) sets the stall time that triggers the 0176 HANG report; max 12 reports.
- Test (no freeze expected): build 0169-0177, boot args + `sweetgpuhangms=150` (NO sweetmtlringwait), play until
  it flickers, then `dmesg | grep -E 'SWEET GPU: (HANG|report)|SWEET MTL: .* method 5 failed'`.
- Built the kernel with the full 0169-0177 patch chain using Xcode 27.2 / macOS 13.5 KDK 22G74, then rebuilt
  the 60 Hz GPU-present boot image. Verified the kernel contains the HANG report, threshold boot-arg and
  opt-in ring-wait strings.
- Image: `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0177-60hz-gpu-two-layers.img`,
  SHA-256 `9a286a3b31953655f7680d4cda647ab7c6e1c6e3f0a941b98d2ac3972bff814d`. Boot args include
  `sweetgpuhangms=150`; `sweetmtlringwait` is omitted (default off) as specified for the no-freeze test.
  Not transferred or booted.

# Active handoff — 0176 GPU hang report; 0174 ring wait now opt-in (2026-11-08, Claude)

- With 0169-0175 (0174 = Submit waits for ring room): after clearing mtl2.log, backboardd DeviceLost = 0, but the
  UI FROZE in Subway Surfers. SSH still worked. dmesg: `GPU done 3746 of 3755` constant in every line: the GPU
  is hung on job 3747, the first fence SubwaySurf waits on. backboardd, WebKit.GPU and the game all spin in 1 ms
  WaitFence loops behind it. Only functional change from the non-freezing build is 0174.
  Hypothesis (unverified): a game job waits on the GPU (CP_WAIT_REG_MEM: event/semaphore) for a signal that is
  submitted later or set from the CPU by a thread that is blocked; earlier the ring-full refusal (-> DeviceLost)
  broke the cycle instead.
- NEW `xnu/patches/0176-guacamole-gpu-hang-report-ringwait-optin.patch` (sweet_gpu.c + SweetAccelerator.cpp,
  after 0175): 0174's ring wait only with `sweetmtlringwait=1` (default off = old refuse-when-full). Kernel keeps
  a 128-entry history of submits (fence, pid, first IB iova/dwords, IB count) and, when the done fence has not
  moved for 2 s with later jobs queued, prints once: `SWEET GPU: HANG: ...` RBBM_STATUS/STATUS3, CP rptr/wptr,
  HW_FAULT, INT, STATUS_1, IB1/IB2 base + remaining dwords, redwood_gpu_report (SMMU FSR/FAR), and the jobs
  around the stuck one (">>" = stuck). Display blits queued from swap() show backboardd's pid; their IB iova lies
  in the display's GPU window (`SWEET FB: GPU present: window ...` at boot).
- Test 1 (safe, default): flicker expected back, no freeze. Test 2: add `sweetmtlringwait=1`, reproduce the freeze,
  then `dmesg | grep 'SWEET GPU: HANG\|SWEET GPU: report\|GPU present: window'`.

# Active handoff — 0175 log only real GPU failures (2026-11-08, Claude)

- 0174 result: thousands/s of `method 6 failed: 0xe00002d6` (kIOReturnTimeout) in every process. These are Turnip's
  WaitFence polls with timeout 0 ("not done yet"), normal; the flood hid the real failures (backboardd's first 40
  lines were already gone). mtl2.log DeviceLost count 103470 is cumulative (file not cleared), so 0174's
  ring-wait effect is unknown. Composited frames still 100% black.
- NEW `xnu/patches/0175-guacamole-mtl-log-real-failures.patch` (SweetAccelerator.cpp + bsd/dev/sweet_gpu.c, after
  0174): polls are not logged; every other failure (incl. WaitFence with a real timeout) logs fence, wait ms,
  GPU done fence vs last submitted (`GPU done X of Y`: X stuck far behind Y = GPU hang), pool free.
  First 60 per client, then every 100th. New `sweet_gpu_done_fence()` in sweet_gpu.c.
- Test: `: > /var/local/mtl2.log`, reboot into 0169-0175, reproduce, then
  `dmesg | grep 'SWEET MTL: .*failed'` and
  `grep -a backboardd /var/local/mtl2.log | grep -B40 -m1 DeviceLost` (what backboardd did right before the loss).
- Built kernel and boot image locally with Xcode 27.2 / macOS 13.5 KDK 22G74. Verified the kernel contains the
  detailed failure log (`fence`, `wait`, `GPU done X of Y`, pool free) and ring-space wait message.
- Boot image `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0175-60hz-gpu-two-layers.img`,
  SHA-256 `3f0f45a78b0494b7d9f16f6b960b3759ab0f79fdc6e1e6d244b27385d5bc9441`. It uses the usual 60 Hz GPU-present
  args with kernel patches 0169-0175. Not transferred or booted.

# Active handoff — 0174 backboardd's Metal device is lost (2026-11-08, Claude)

- backboardd's mtl2.log: dozens of `commandBuffer refused: command buffer: DeviceLost` (also texture / no-copy
  buffer refused: DeviceLost). Once Turnip marks the VkDevice lost it stays lost for that process, so every
  composition backboardd draws afterwards fails -> black (0173: 274/274 composited frames black). This is the
  root of the Subway Surfers flicker. backboardd also held 162 -> 229 MB of GPU pool.
- The kernel never reports faults (fFaults is never incremented), so the loss comes from a failed method:
  Submit returns kIOReturnBusy at once when the 32 KB ring is full, or kIOReturnTimeout/NotReady; or WaitFence
  times out. Likeliest: ring full under game + backboardd + display blits.
- NEW `xnu/patches/0174-guacamole-mtl-submit-wait-and-failure-log.patch` (SweetAccelerator.cpp, after 0173):
  Submit waits up to 2 s for ring room instead of refusing (logs `waited N ms for ring space` when >= 20 ms);
  every failed method is logged: `SWEET MTL: <proc> method N failed: 0x... (#n; GPU pool free N MB)`
  (first 40 per client, then every 1000th).
- Test: build 0169-0174, play/menus until flicker would start, then
  `dmesg | grep -E 'SWEET MTL: .*(failed|ring space)|display-time samples'` and
  `grep -a backboardd /var/local/mtl2.log | grep -c DeviceLost`. If DeviceLost still appears, the failed-method
  lines name the cause (SweetGPUABI.h: 2 BoAlloc, 3 BoMap, 5 Submit, 6 WaitFence, 16 BoImport).
- Built locally with Xcode 27.2 / macOS 13.5 KDK 22G74. Kernel has the 0174 ring-wait and failure-log strings;
  the rebuilt Metal bundle retains the 0173 pre-present sampling and passes strict codesign verification.
- Artifacts (not transferred or booted):
  - `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0174-60hz-gpu-two-layers.img`, SHA-256
    `9c5c61a6f3e4ec581ce73ea4fd64ef7b4dd324be74d50e0ba7834e1d9505d9e0`.
  - `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-SweetMetal2-0174-device-lost-diagnostics.tar.gz`, SHA-256
    `13e3e0de55b3ebd72f76b1ce47bbad2ee00c949e26c12a6b2eb7ffadbb91152e`.
- Image keeps the known 60 Hz GPU-present arguments; 0174 adds ring-space waiting and per-client failed-method logs.

# Active handoff — 0173 where does the black come from (2026-11-08, Claude)

- RESULT (decisive): pre-present (game process, after completion) 1080 frames, 0 black. Display-time: game-layer
  frames 0 black of ~880; frames whose top layer is NOT the game layer (CA's own framebuffer surface 2/3, i.e.
  backboardd composited the game itself) 274 of 274 black. Flicker = alternation between direct game-layer frames
  (good) and backboardd-composited frames (game content black, home indicator drawn). Menus composite more often →
  more flicker. Root cause is in backboardd's compositing of the game's IOSurface (texture import / sampling in
  the backboardd Metal frontend, or IOSurface lookup/mapping), not the game's rendering and not the display copy.
  Next: backboardd mtl2.log refusals/missing selectors and SWEET SURF kernel lines for the game's drawable surfaces.

- 0172 user report: game is a bit better while running, menus flicker MORE; "still the original issue".
  Slow-motion video (120 fps) frames: (a) menus: whole screen black except iOS home indicator and a thin edge strip;
  (b) gameplay: HUD (score, coins, pause, GET) intact while the 3D scene is black, sometimes with one clean
  rectangle of scene (GMEM-bin-like) drawn. The display-path hypotheses (0169-0172) did not explain this.
- The frontend's old "frame grid" sampler showed 0 black regions, but it is NOT valid for Subway Surfers: every
  drawable is presented from a present-only command buffer (work=0/0/0/0) and the sample ran before waiting for
  the prior work buffers, i.e. possibly on the previous frame.
- NEW measurement at both ends:
  - `xnu/patches/0173-guacamole-fb-display-black-samples.patch` (after 0172): presentGPU samples the top layer
    8x8 (rows cache-invalidated) as the blit is queued. 5 s line `display-time samples: game layer N frames, N black;
    other top layer N frames, N black`, plus up to 40 `SWEET FB: black at display: ...` lines.
  - `userland/metal2/NativeBridgeMetalFrontend.mm`: sample (8x8 via getBytes) inside the present block AFTER prior
    work completed, right before `[drawable present]`: `pre-present black frame #...` (first 20) and
  `pre-present totals: N frames, N black` every 120 frames (into /var/local/mtl2.log).
- Built locally with Xcode 27.2 / macOS 13.5 KDK 22G74. Kernel contains both display-time sample strings; Metal bundle
  contains both pre-present sample strings and passes `codesign --verify --deep --strict`.
- Artifacts (not installed or booted):
  - `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0173-60hz-gpu-two-layers.img`, SHA-256
    `53631f0cd10b9a9461aa606155bdedd0d44daffe27314be0a65970f43d168857`.
  - `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-SweetMetal2-0173-black-samples.tar.gz`, SHA-256
    `326d5fcf7d35bb0353f996b02be2de681f2f96b1e83b6dd84af18ff505d96c86`.
- Image uses the known 60 Hz GPU-present arguments (`sweetfbgpu=1 sweetfbsync=1 sweetfbcompose=1
  sweetfbopaqueflag=1 sweetfb_refresh=60 sweetfb_vsync=60`) with the same existing 0172 kernel behavior plus
  0173 display sampling. `sweetmtlsyncsubmit` remains off.
- Reading: pre-present black > 0 → the game's finished frames are black (rendering/sync inside metal2vulkan/Turnip).
  Pre-present 0 but display-time black > 0 → the surface changes after hand-over (drawable reused/re-rendered while
  still on screen, or cache). "other top layer" black with home indicator → CA's own composite is black.

# Active handoff — 0172 two layers on the GPU (2026-11-08, Claude)

- 0172 hardware result (Subway Surfers): every frame on the GPU, CPU-composed 0, 51-59 swaps/s in play,
  SwapWait never timed out. Flicker status after 0172: awaiting user's visual report.
- 0171 hardware result: game visible; long stretches at ~60 fps with every frame GPU-blitted (top layer alone).
  Other stretches still CPU-composed (27-38 ms/frame) because (1) the game layer's alpha is not all 255
  (samples 255:61 mid:3) and (2) the game layer is sometimes 1125x2226 over CA's layer (bottom strip visible).
  User has not yet said whether flicker is gone with 0171.
- NEW `xnu/patches/0172-guacamole-fb-two-layers-on-gpu.patch` (after 0171; compiled locally with Xcode 27.2 and
  macOS 13.5 KDK 22G74):
  - rec[0x467+layer] != 0 (set only on the game layer) is treated as "opaque, ignore alpha" (`sweetfbopaqueflag=0`
    disables). Also used by the CPU compose (copy, not blend).
  - presentGPU handles up to two layers with an opaque top: top layer blitted whole, bottom layer only in the
    parts of its rect not covered by the top (up to 4 cut rectangles), so every panel pixel is written once
    (no black flash between blits). Panel rows of adjacent cuts meet exactly (checked: top rows 0-2851, strip 2852-3119).
  - FBGPU_SLOTS 4 -> 8 (window still fits: 64 MB + 8 x 16 MB < 256 MB); slot eviction avoids the other layer's slot.
  - Log: `SWEET FB: GPU present: N layer(s), N blit(s) ...` once for the first frame and once for the first cut frame.
- Test: same args, play ~30 s, `dmesg | grep 'SWEET FB: 5s\|SWEET FB: GPU present\|SWEET FB: compose'`.
  Expect `composed` ~0 in game and the multi-layer GPU count to cover the game frames.
- Packaged image `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0172-60hz-gpu-two-layers.img`,
  SHA-256 `0154e7ecd84e32a956f820503e844111185a6056f2d3a17ab265e19b49644b53`. It uses the 60 Hz GPU-present,
  sync and composition arguments plus `sweetfbopaqueflag=1`. Kernel compile and image packaging succeeded; not yet
  booted on hardware.

# Active handoff — 0171 layer order + opaque top layer on GPU (2026-11-08, Claude)

- 0170 hardware result: Subway Surfers ALL BLACK (normal for ~0.2 s after leaving Control Center). Compose log:
  layer 0 = game drawable (surface 53/55, 1125x2436 BGRA, flag byte rec[0x467+layer]=1, alpha all 255),
  layer 1 = CoreAnimation framebuffer surface 2/3 (alpha all 255 or all 0). 0170 drew layer 0 lowest, so layer 1
  covered the game (and the all-0-alpha heuristic made it black). Conclusion: the record's layer 0 is the TOP.
  This also explains the original flicker: old present() pasted layer 1 over the game on every multi-layer frame.
- NEW `xnu/patches/0171-guacamole-fb-layer-order-top-only.patch` (after 0170; compiled locally with Xcode 27.2 and
  macOS 13.5 KDK 22G74):
  composition goes from the highest index up (layer 0 on top); and in presentGPU, a multi-layer frame whose top
  layer covers the whole logical display with all-255 alpha samples (rows cache-invalidated individually) is
  GPU-blitted alone, no CPU composition. New 5 s line: `multi-layer frames shown as their opaque top layer on the GPU: N`.
- Test: same args, play ~30 s, `dmesg | grep 'SWEET FB: 5s\|SWEET FB: compose'`. Expect most game frames in
  `top layer on the GPU`, few/no `composed`.
- Packaged separate image `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0171-60hz-gpu-top-layer.img`,
  SHA-256 `bf8898488c01cdec0866a3f2025140ea09abc4d582d066423dbec049af4b75b2`. It uses the same 60 Hz GPU-present,
  sync and composition arguments as 0170. Kernel compile and image packaging succeeded; not yet booted on hardware.

# Active handoff — 0170 multi-layer composition (2026-11-08, Claude)

- 0169 hardware result (Subway Surfers, user still sees flicker): `no new GPU work at swap` ~0, `forced` 0,
  `re-swapped` 0 → the submit-after-swap and unfinished-frame hypotheses are NOT the cause. backboardd calls SwapWait
  twice per swap (selectors 4:N 5:N 6:2N). In game windows, frames alternate between one-layer swaps (GPU blit at
  swap) and multi-layer swaps (`skip layers` 97/136, `CPU-copied Metal frames` 48/68, e.g. "shown at swap 8, at
  vsync 68"). The CPU `present()` pasted every layer opaque (alpha ignored), ignored rect a's source size, and wrote
  the panel while an earlier GPU blit could still land on top. That alternation is the new prime suspect.
- NEW `xnu/patches/0170-guacamole-fb-compose-layers.patch` (applies after 0169; compiled locally with Xcode 27.2 and
  macOS 13.5 KDK 22G74):
  boot-arg `sweetfbcompose` (default 1, 0 = old paste). Multi-layer frames (or a layer whose rect a size != rect b
  size) are composed in a cached 1125x2436 buffer: layer 0 lowest, scaled from rect a to rect b, upper layers
  premultiplied source-over (an upper layer whose 64 alpha samples are all 0 is treated as opaque); waits for running
  GPU blits; then one scaled copy to the panel. The GPU blit now uses rect a's size as its source rectangle.
  Logs: `SWEET FB: compose swap ... layer ... a .. b .. xform .. flags .. alpha 0:N 255:N mid:N -> opaque|blended`
  (first 12, then the first of each 5 s window) and `SWEET FB: 5s: composed N frames (us each) ...`.
- Test: boot 0169+0170 kernel, play ~30 s, collect `dmesg | grep 'SWEET FB: compose\|SWEET FB: 5s'`.
  If the game shows a layer missing/black, the layer order or alpha assumption is wrong: the compose lines give the
  layer formats/rects/flags to correct it. Expect lower fps on multi-layer frames (CPU compose).
- Packaged separate image `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0170-60hz-gpu-compose.img`,
  SHA-256 `5b996f6e84ffadc46176bb065ace91180ff9ce3e985606d75b08cb71c0343101`. It uses the usual 60 Hz GPU-present
  arguments with `sweetfbsync=1 sweetfbcompose=1`. Kernel compile and image packaging succeeded; not yet booted on hardware.
- Reconstructed file after 0170: `work/claude-recon/SweetFramebuffer.after0170.cpp` (reference only).

# Active handoff — 0169 frame hand-over sync (2026-11-08, Claude)

- Goal: Subway Surfers black-region flicker. Hypothesis (NOT yet confirmed on hardware): the display shows frames
  whose drawing is unfinished or being redrawn, because (a) `AppleCLCD` never tells CoreAnimation when a swap is
  on the panel (only vsync notifications; `SwapWait` sel 6 was a no-op), (b) 0145's fence is the global last GPU
  job at swap time, which can predate the frame's own Metal submit (metal2vulkan may submit after CA swaps), and
  (c) after 6 vsyncs 0145 showed an unfinished frame anyway. Fits all earlier A/Bs: CPU copy, vsync-only, 90 Hz,
  1 Hz all still flickered; software screenshots are clean.
- NEW `xnu/patches/0169-guacamole-fb-frame-handover-sync.patch` (SweetFramebuffer.cpp + SweetAccelerator.cpp).
  Applies cleanly after 0168 and is now compiled locally with Xcode 27.2 and macOS 13.5 KDK 22G74.
  - SweetAccelerator: per-pid table of last submitted fence + submit count (`sweet_mtl_pid_submits`).
  - Boot-arg `sweetfbsync` (default 1; `sweetfbsync=0` = previous behaviour):
    swap waits <= 4 ms for the swapping process's (backboardd's) Metal job if none arrived since its last swap;
    CPU-path fence = that process's own last job, not everyone's; unfinished frames wait up to 60 vsyncs, not 6;
    `SwapWait` (sel 6, [swap id, timeout, 0]) blocks until that swap is shown or queued behind its drawing (<= 50 ms).
  - New 5 s log line (always on): `SWEET FB: 5s: sync 1; no new GPU work at swap N (late N); forced unfinished N;
    re-swapped pending surface N; CPU-copied Metal frames N (skip setup/layers/none/surface/bounds/slot); SwapWait N
    (timed out N)`.
- Userland (optional, off by default): `userland/metal2/NativeBridgeMetalFrontend.mm`, backboardd only, boot-arg
  `sweetmtlsyncsubmit=1` makes a commit that draws into IOSurfaces wait (runtime-level `nmb_command_buffer_wait_scheduled`)
  until submitted. Rebuilt with `userland/metal2/build.sh`; this flag remains off unless added to boot args.
- Test: boot the usual 60 Hz GPU-present args + 0169 kernel, play Subway Surfers ~30 s, then
  `dmesg | grep 'SWEET FB: 5s'`. Read: `late`>0 or `no new GPU work` high → submit-after-swap race is real (then also try
  `sweetmtlsyncsubmit=1`); `forced`>0 under old behaviour → showing unfinished frames; `re-swapped pending surface`>0 →
  CA redraws surfaces before the display shows them (SwapWait/swap-complete matters); `CPU-copied Metal frames` and its
  skip reasons say whether the game goes through the CPU path. A/B with `sweetfbsync=0` on the same kernel.
- Reference copies of the reconstructed (0001–0168 applied) files: `work/claude-recon/` (not part of the build).
- Build artifacts: `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-0169-60hz-gpupresent.img`
  (SHA-256 `27532d3e026ce17e1108bf7c7daa9bf660f0eb2e08a5cddb1f13adbc4760c681`) and
  `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-SweetMetal2-0169-handover-sync.tar.gz`
  (SHA-256 `20a5c83e3742f5ef34516ecded74825333edd432d6593bf3a8be18d18f7f3503`). Image uses 60 Hz GPU framebuffer
  presentation and `sweetfbsync=1`; Metal bundle was rebuilt from the current local sources and ad-hoc signature
  verified. Neither artifact has been installed or booted on the phone.
- Not done: real swap-complete notification (type number/payload of IOMobileFramebuffer's swap-done callback
  still needs reverse engineering); SDE double-buffer page flip (only if a clean tear line remains after this).

# Active handoff — guacamole Metal2 cold-boot initialization (2026-11-07)

- User reports both the OpenGL-off and OpenGL-on boot images now seem to render
  with Metal after the `_RB4` fallback update. Cut the Rope still opens to a
  white screen; cause is unresolved.
- Installed Angry Birds Classic 8.0.3 (`com.clickgamer.AngryBirds`) from
  `/Users/coreycusick/Downloads/Angry Birds Classic.ipa` to
  `/Applications/AngryBirdsClassic.app`. Its executable is arm64, requires
  OpenGL ES 2, and imports OpenGLES.framework; it has no Metal path. Repeated
  launch reports show `SIGABRT` / `Abort trap: 6` about 0.1 s after launch,
  unwinding through the C++/Objective-C terminate handler. The `.ips` reports
  omit the exception reason. The IPA contains the diagnostic string
  `Failed to create EAGLContext`, so failure to initialize the app's own GL
  context is a likely cause. The `sweetgl` boot path loads Mesa into
  `backboardd` only; it does not establish an EAGL context for Angry Birds.
  Need app-process EAGL logging/injection to confirm. The phone went offline on
  SSH (`192.168.0.50` host down) during this check, so no per-app shim change
  was made.
- Root cause hypothesis from source: RedwoodGPU is brought up asynchronously
  after `sweetgpu=11`; Metal's first open can happen before the backend is
  ready. `SweetMetal2Device` wraps initialization in `dispatch_once`, so a
  failed first open is cached for that process. `sweetglwait=120` waits only
  in the OpenGL/backboardd injection path and does not delay Metal.
- Added a Metal-specific wait in `userland/metal2/SweetMetal2.mm`: before its
  one-shot Turnip open, it polls the `RedwoodGPU` readiness service. Default
  timeout is 120 seconds; `sweetmtlwait=N` overrides it (0 disables waiting).
  This requires no kernel or boot-image changes. It applies when a process
  first requests Metal; it does not eagerly launch a Metal client on its own.
- Bundle built with the existing iOS SDK, Turnip and native bridge. Archive:
  `outputs/guacamole-SweetMetal2-bootwait.tar.gz`, SHA256
  `a0212f0bc7a4ccd59e115204def0fc82708e624a391093db98fcbd2122369f12`.
  Ad-hoc code signature verified. Installed over SSH at
  `/System/Library/Extensions/SweetMetal2.bundle`; previous bundle preserved
  as `SweetMetal2.bundle.pre-bootwait-20261107` (root:wheel ownership fixed).
  The user rebooted after this build. Logs show Metal opens successfully, but
  they also show the `_RB4` import failure below.
- Added a port-specific mapping of the compositor's `RGB8_444_2P` request to
  BGRA8 in `sweetNarrow`, for both textures and pipeline attachments. The
  rejected `_RB4` surface has 4-byte rows (1920 bytes for width 465); the
  kernel exposes the UI backing as BGRA8. Built and installed this new bundle
  over SSH at `/System/Library/Extensions/SweetMetal2.bundle`, preserving the
  wait-only bundle as `SweetMetal2.bundle.pre-rb4-20261107`. Archive:
  `outputs/guacamole-SweetMetal2-rb4-fallback.tar.gz`, SHA256
  `e9f4d3353d6f6871daccf360f8dcea220c26c9fb4325dae3d187638a1c2ec76b`.
  User reports Metal appears to work on both tested boot images; detailed
  post-install compositor log has not yet been collected.

- Installed `/Users/coreycusick/Downloads/Angry Birds Classic.ipa` on the
  phone at `/Applications/AngryBirdsClassic.app` and registered it with
  `/var/jb/usr/bin/uicache`. IPA is decrypted (`cryptid=0`), version 8.0.3,
  `com.clickgamer.AngryBirds`, arm64 executable, OpenGL ES 2 requirement, no
  Metal framework. User is currently testing an OpenGL-on boot image; next is
  for them to launch Angry Birds and report its display. The IPA remains in
  `/var/local/AngryBirdsClassic.ipa` on the phone.

- Cut the Rope Experiments (`com.zeptolab.ctrexperimentslite`) still shows a
  white screen. A previous `ctr-jni` run created a Turnip device, but
  `/var/local/m2v-cache/ctr-jni/pipelines.log` contained only family capability
  records and no render pipelines. This game issue is separate from cold-boot
  Metal initialization and is unresolved. A GL-on test image was prepared but
  is not the current plan; respect the user's report that enabling OpenGL
  globally breaks many things.

- Implemented `MTLDevice.newLibraryWithSource` in the Metal frontend. It calls
  iOS's inherited `_MTLDevice` compiler, reads the resulting metallib through
  `_MTLLibrary.libraryDataContents`, and opens that data as an `NBLibrary`; both
  synchronous and completion-handler forms are bridged. Built
  `outputs/guacamole-SweetMetal2-sourcecompile.tar.gz` (SHA256
  `7fe4c2270b66487a77bbae3d9942e8bd47c24fa8d70f259562cdcb20f90c4043`) and
  installed it on the phone. The prior bundle is preserved at
  `/System/Library/Extensions/SweetMetal2.bundle.pre-sourcecompile-20261106`.
- Ran the existing `/var/local/tests/mtltest` against the installed bundle:
  device creation, blit fill, source compilation, compute pipeline creation,
  and 4096-element GPU compute all PASS (0 wrong words). Restarted backboardd
  and SpringBoard; both now create the Turnip device without refusal. System UI
  frame acceleration still needs a visual/usage check. No kernel or boot args
  changed. The compiler byte accessor is a private iOS 16 API.

- Built one-time code-signing-off boot image
  `outputs/guacamole-metal2vulkan-0216828513-csoff-once.img`, SHA256
  `908bc85d3b46e1214990220fe240d014aa229d66a9c7e4bf82b916372bd2877e`.
  Derived from the existing `guacamole-metal2vulkan-0216828513.img`; verified
  Android kernel/shim and DTB are byte-identical, and the only payload change
  is appending `sweetcsoff=1` to the existing boot arguments. No kernel, rootfs,
  Wi-Fi, or Metal files changed. Image has not been transferred or booted.
- User reports the staged system apps are now installed. Built
  `outputs/guacamole-metal2vulkan-0216828513-gl-off.img`, SHA256
  `cdaebfa3c7b156a8f6fc7c6adc9bc739cc924199a5efe81c1ac36dc711db786e`, from
  the normal image with the exact `sweetgl=0 sweetglfn=0 sweetbbd=0` arguments.
  Kernel and DTB are byte-identical; `sweetcsoff` is absent. Not transferred or
  booted.
- Rebuilt as `outputs/guacamole-metal2vulkan-0216828513-gl-off-wait120.img`,
  SHA256 `e0855b52b754ad839a3ebed9551c135328f2918c86bb07836ce30255ec7803eb`,
  with the corrected `sweetglwait=120`; all other arguments are unchanged.
  Kernel and DTB remain byte-identical. Not transferred or booted.
- User booted the GL-off/wait-120 image and reports Metal initialization did not
  change. Prior confirmed compositor issue was refusal of IOSurface format 542
  (`RGB8_444_2P`); subsequent standalone tests below narrowed the runtime gaps.
- SSH later became reachable; installed the test archive and ran the three
  tests. `mtltest`: device creation and blit fill pass; compute pipeline fails
  `foreign function` because `newLibraryWithSource` is not bridged (runtime
  explicitly reports `NotImplemented(LibrarySource)`). `surftest`: BGRA8 and
  BGR10_XR IOSurface texture reads/clears all pass. `cgtest`: FAIL, 6272/960000
  bytes differ; QuartzCore logs `fragmentFunction is not a MTLFunction` and
  `vertexFunction is not a MTLFunction`.
- Ran `m2vtest` on the phone against QuartzCore's 1014944-byte
  `default.metallib`: device/compute/render/import self-tests pass (0/1000,
  0/64, 0/4096 wrong); QuartzCore compilation: 3 kernels pass, 4 tile kernels
  request unsupported tile attachments; 13 render pairs pass and 8 fail from
  missing vertex-attribute/color-attachment descriptor inputs in this probe.
  Main next gap is source-library bridging (`MTLDevice newLibraryWithSource`),
  then test the specific QuartzCore/app pipeline descriptors. No kernel or boot
  arguments changed during these tests.

- Latest finding: the device now creates Turnip in system processes, but
  `backboardd` refuses IOSurface texture format 542 (`RGB8_444_2P`) as an
  unsupported buffer format. The bridge had been marking every IOSurface as
  non-planar and dropping its YCbCr surface facts at the C/Rust boundary.
- Updated `NativeMetalObjects` to carry IOSurface plane index, planar flag,
  surface OSType/dimensions, and plane 1 offset/row into the runtime. It only
  attaches YCbCr facts for a planar surface and a YCbCr Metal format. Bundle
  built and installed at `/System/Library/Extensions/SweetMetal2.bundle`;
  previous installs preserved as `.pre-iosurface-facts-20261106` and
  `.pre-iosurface-facts-v1-20261106`. Bundle archive in `outputs/` has SHA256
  `c86bfa129fa7b27902c5b9935f3b3d2f375d6cb724190b97bca3d7ca44a5d518`.
- No kernel source/image changes. The process must reload the installed bundle
  before the new path is exercised. Next: after reboot or process restart,
  inspect the `mtl2.log` texture result. The current kernel maps `b3a8` to a
  one-plane BGRA surface unless `sweetsurf10=1`; if format 542 is still refused
  over BGRA, that is a separate layout/format emulation gap and needs a surface
  dump (OSType, plane count, plane offsets/rows, allocation size) before
  implementing it.
- Source patch is uncommitted in `userland/metal2/NativeBridgeMetalFrontend.mm`,
  `bridge/NativeMetalObjects.h`, and `bridge/NativeMetalObjects.rs`. Previous
  `userDictionary` change remains committed as `9710929`.

- Source checkout: `OtVaL228/ios-sweet`, branch `guacamole`, HEAD
  `021682851328a343e373e1dfdd5e8fe11eae30fc`. The running kernel is from this
  source; the local build also uses the expected Xcode compatibility patch
  `xnu/local/0001-iorpc-kernelcontent.patch`. No kernel source changes were made
  for this userland update.
- Device logs from `PridePosterExtension` show successful Turnip device creation,
  then `MISSING NBCommandBuffer -userDictionary`. Added a mutable
  `userDictionary` to `NBCommandBuffer` in
  `userland/metal2/NativeBridgeMetalFrontend.mm` and initialized it per command
  buffer. Bundle rebuilt with `userland/metal2/build.sh`; archive SHA256
  `4452b57fd8c14ede3d3db64c95278f3320b0ec8bcff3afb7e254f0c9067e7b2a`.
- Installed bundle on phone at `/System/Library/Extensions/SweetMetal2.bundle`;
  previous bundle saved as `SweetMetal2.bundle.pre-userdict-20261106`. File
  checksums match the local build. Terminated old PridePosterExtension PID 713
  so a later launch can load the replacement. No reboot or test run performed.
- Historical: earlier `backboardd`, SpringBoard, and `chronod` attempts
  returned `IOServiceOpen 0xe00002c2`. Current logs now show system processes
  creating Turnip devices; the active blocker is IOSurface format 542 above.

# iOS 16 on sweet (Redmi Note 10 Pro, SM7150) - working notes

## Agent handoff / active work (2026-10-15, latest)

### >>> UPDATE 2026-10-18 (read first, then the 10-17 handoff below) <<<

**STATE at end of 2026-10-18 (evening):**
- On the phone: userdata = **image v15** (`/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v15.img/.simg`; = v14 + `/private/var/preferences/SystemConfiguration/preferences.plist` with one network service "USB Ethernet": Interface DeviceName ue0 Type Ethernet, IPv4 Manual 192.168.7.2/24 router 192.168.7.1, DNS 1.1.1.1 8.8.8.8, set "Automatic" as CurrentSet). Fallback image v14 (v12/v13 deleted).
- Kernels: **0078** = all 8 cores (`cpus=8`, cpu6/7 = A76, homogeneous SMP; mkadt now emits 8 CPU nodes; ADT `shim/out/sweet-0078hi.adt`); **0079** = display off/on via IOMFB selector 12 kern_RequestPowerChange(state) (fade over 6 vsyncs, then black; verified: power button gives "SWEET FB: display off/on"); **0080** = `SWEET NET: <proc> route add/delete/change ...` and `... add/delete address ...` trace (built, `shim/out/shim-boot-0080s.img`, NOT booted yet).
- **Internet over USB: not working yet.** Host side is ready (temporary iptables: FORWARD accept for 192.168.7.0/24 + MASQUERADE, comment "ios-sweet"; lost on host reboot). With v15 the phone sent no DNS and no outbound traffic (only mDNS/ARP) -> configd probably did not apply the service (or did, without default route/DNS). Next: boot 0080s on v15, watch `SWEET NET` lines (does configd add 192.168.7.2 / default route on ue0?). If configd ignores ue0 (not an IOKit IONetworkInterface; configd 1241.140.6 source in `research/configd-1241`, SCNetworkInterface createInterface needs IONetworkInterface -> controller (IOMACAddress, SelfNamed) -> bus), add IOKit stand-ins and/or rename the ifnet to en*.
- Live log listener (`nc -ukl 5140`) may still be running, writing `logs/live-0078s.txt`.

**LIVE LOG over USB (0077, boot 0077s): no more recovery round-trips.** Boot an image with `sweetusb=2`, run `tools/sweet-usbnet.sh logs/live-NAME.txt` on the PC: it waits for the phone's CDC-ECM interface, sets 192.168.7.1/24 (sudo) and prints/saves the kernel console the phone streams every 100 ms as UDP to 192.168.7.1:5140 (starts with the last 64 KiB). `ping 192.168.7.2` checks the link. Current image: `shim/out/shim-boot-0077s.img` (kernel 0077, ADT sweet-0069hi, cpus=6, sweetfb_noconsole=1, sweetfb_hz=30, sweetusb=2). The recovery pull (`tools/sweet-log-pull.sh`) is still the way after a panic/hang before USB comes up.

**🎉 USB networking works (boot 0076s, 18:24 local): `ping 192.168.7.2` from the PC answers in ~1 ms, 0 % loss (200-packet burst, 1500-byte frames, 512/1024-byte frames).** 0075: CDC-ECM function (bulk 0x81/0x01, interrupt 0x82 NETWORK_CONNECTION; host MAC 02:00:5e:77:ee:01) + kernel ethernet ifnet `ue0` (legacy output, header strip like if_fake) with 192.168.7.2/24 set from the kernel (proto_plumb + SIOCAIFADDR on an in-kernel socket). 0076: received frames into one cluster mbuf (mbuf_allocpacket gave a first mbuf of length 0 -> panic "Failed mbuf validity check: len -14"). Host side (Linux cdc_ether, interface `enp0s20f0u3u1`): `nmcli dev set IF managed no; sudo ip addr add 192.168.7.1/24 dev IF; sudo ip link set IF up`. Boot image `shim/out/shim-boot-0076s.img` (`sweetusb=2`). iOS itself does not see ue0 yet (configd needs an IONetworkInterface in IOKit).

**USB device mode works (boot 0074s, 17:45 local): the host enumerates the phone at high speed as `1209:0001 ios-sweet "Sweet iOS (USB bring-up)"`.** 0072 read-only probe: ABL leaves the USB30_PRIM GDSC and all 7 USB clocks on, DWC3 v3.20a (GSNPSID 0x5533320a) in device mode (GCTL 0x00112000), halted (DCTL RUN_STOP 0, DSTS 0x00d3de04), event buffer still at ABL's 0xfdfc8000, QUSB2 PHY alive. 0073 (`sweetusb=2`): DCTL soft reset, own event buffer (polled, interrupt masked), ep0 DEPSTARTCFG/SETEPCONFIG/SETTRANSFRESOURCE, setup TRB, RUN_STOP; control-transfer state machine as Linux ep0.c; descriptors: vendor class, no data endpoints. It ran but no event came: **0074** forces the VBUS session in the Qualcomm wrapper (QSCRATCH 0xa6f8800: HS_PHY_CTRL |= UTMI_OTG_VBUS_VALID(20)|SW_SESSVLD_SEL(28), SS_PHY_CTRL |= LANE0_PWR_PRESENT(24), as Linux dwc3_qcom_vbus_override_enable) -> connect + enumeration. Image `shim/out/shim-boot-0074s.img`. Black Phone app: main thread waits in -[VMVoicemailManager fetchAccounts] (sync XPC; voicemail needs the modem).

**Navigation works (boot 0071s on v14, log `logs/boot-0071s.txt`):** swipe up from the bottom edge returns to the home screen; app switcher works. Two fixes: **0070** pid_hibernate (syscall 435 was nosys on the macOS config -> SIGSYS killed SpringBoard in -[SBToAppsWorkspaceTransaction _didComplete] when an app left the foreground; the only CONFIG_EMBEDDED-only entry in syscalls.master); **0071** touches starting within 48 px of an edge carry kIOHIDDigitizerEventFromEdgeTip (UIKit `_UIEventHIDTouchEdgeTypeForHIDEvent` reads the digitizer EventMask 0xb0007 bits 0x800/0x1000/0x2000/0x40000). Diagnosed, not fixed: Settings (Preferences) SIGTRAP in PodcastsUI func_1cdc47c58: force-unwrap of `URL.volumeTotalCapacity` (nil on our HFS+ root). Phone/Photos/Messages show black (MobilePhone launches, no crash) - next: stack samples via boot-arg `sweetsample=MobilePhone,MobileSlideShow,MobileSMS,SpringBoard` (image `shim/out/shim-boot-0071s.img`).

**🎉🎉 MILESTONE 2026-10-18 ~15:30 local: iOS 16.7 SPRINGBOARD HOME SCREEN ON THE PHONE** (boot `shim/out/shim-boot-0069s.img` = kernel 0069 + ADT `shim/out/sweet-0069hi.adt`, image **v14**; log `logs/boot-0069s-v14-HOMESCREEN.txt`). Home screen with a few apps, Settings opens, Control Center / lock screen / notification center work, touch + buttons work, 6 A55 cores (`cpus=6`), `sweetfb_noconsole=1 sweetfb_hz=30`. Known issue: cannot leave Settings (home gesture / AssistiveTouch home do nothing) - to investigate.
What it took today (in order): 0058 HID services (Apple IOHIDFamily-1915 user client), 0059 iPhone X identity (hw.product etc.), 0060 SPMI keys + GENI touch, 0061 1125x2436 presented display, 0062 RPMh L11C touch power + IOPMPowerSource battery, 0063 GENI FIFO packing (Goodix GT9896), 0064 PRIV_WORK_INTERVAL grant (SpringBoard "Failed to create work interval"), 0065 SMP via PSCI (6 A55), 0066 crash map regions, 0067 console mute without cons_ops write, 0068 IOSurface lookup of an already-mapped surface returns base -1 (backboardd snapshot crash), 0069 UFS log flush 10 s. Image v13: CommCenter removed from launchd.plist, BKNoWatchdogs. **Image v14** = v13 + Setup skipped (`/var/mobile/Library/Preferences/com.apple.purplebuddy.plist` SetupDone/SetupFinishedAllSteps true, SetupVersion 11, daemonizeNeedsToRun false; purplebuddy.notbackedup mini-buddy flags false; managedconfiguration.notbackedup LockdownActivationIndicatesCloudConfigurationAvailable false; springboard MCForceShowCloudConfigurationUI false) + mobileactivationd data ark seeded (`/private/var/containers/Data/System/D90C94C4-F94E-4FCF-8A28-A93BE3846D8E/Library/internal/data_ark.plist`, -ActivationState Activated etc.) + MediaToolbox D22/D221/Default SubServerConfiguration StartRecorderServer=false (mediaserverd crash loop: portrait-filter prewarm called NULL GL). **Activation:** ADT /chosen certificate-production-status 0, effective-production-status-ap 0, certificate-security-mode 1, effective-security-mode-sep 1 -> mobileactivationd's native Hactivation branch (Codex `codex/activation/report.md`; alternative 4-byte daemon patch documented there). Fallback image v13; v12 deleted.

**🎉 TOUCH WORKS (boot 0063u on image v13, 2026-10-18 ~13:15 local):** tapping (i) on Setup's Hello opens the device-info sheet; swipe-up is recognised (spinner, then back to Hello - under investigation). Chain: RPMh L11C vote (0062, `bsd/dev/sweet_rpmh.c`) -> GT9896 @0x5d on GENI SE7 (0063 fixed FIFO packing: register address went out byte-swapped) -> sweet_hid_touch -> SweetHIDTouch -> backboardd. Battery icon OK (0062 SweetBattery IOPMPowerSource, static 100 %). **Image v13** (= v12 + CommCenter jobs removed from launchd.plist + `BKNoWatchdogs`=true and `FBLaunchWatchdogExceptions{com.apple.purplebuddy:100}` in /var/mobile/Library/Preferences/com.apple.springboard.plist): Setup no longer killed by the scene-create watchdog (it blocked in CoreTelephony getSubscriptionInfo -> CommCenter, Codex `codex/setup/report.md`; that report also lists how to skip Setup: SetupDone/SetupFinishedAllSteps/SetupVersion=11 in com.apple.purplebuddy.plist + activation check). v9 deleted, fallback = v12. Input latency ~1 min with 1 CPU -> Codex task 11 = SMP via PSCI (`codex/smp/`).

**INPUT WORKS (boot 0060u on a freshly flashed v12, log `logs/boot-0060u.txt`):** power, volume up, volume down reach iOS (PMIC -> sweet_keys.c -> SweetHIDButtons -> backboardd). Power locks/unlocks (screen "off" shows the kernel console because the panel never really turns off). **Device identity fixed: iPhone X** (home indicator, Face-ID-style Setup). Earlier test (0059u, `sweethidtest=15` fake Home presses) advanced Setup from Hello to the language list.
- **0058** HID: `iokit/Kernel/arm/hid/`: IOHIDEventServiceUserClient + IOHIDEventServiceQueue + IOHIDEvent compiled UNCHANGED from Apple IOHIDFamily-1915.140.3 (= exact version of the iOS 16.7 IOHIDLib, clone in `research/iohid-1915`) over our minimal IOHIDEventService (SweetHID.cpp: SweetHIDButtons consumer page 0x0C power 0x30 / vol 0xE9/0xEA / menu 0x40; SweetHIDTouch digitizer collections). C hooks `sweet_hid_key(key, down)` (0 power, 1 vol+, 2 vol-, 3 home) and `sweet_hid_touch(finger, down, x, y)`. HID logs go to IOLog. Boot-arg `sweethidtest=N` = fake Home press every N s.
- **0059** identity: hw.machine/hw.model/hw.target/hw.product from the ADT (macOS-config kernel used to answer "iPad8,6"/"Redmi Note 10 Pro"). mkadt: root compatible `D22AP|iPhone10,3|xiaomi,sweet|...`, model iPhone10,3, target-type D22, `/product` copied from the real D22AP device tree (`research/ipsw-x/d22ap.adt`, extracted from the IPSW, NOT in git), `/buttons home-button-type 2`. Codex report `codex/mg/report.md`: MG ProductType = sysctl **hw.product** (unrecognized -> iPod), HWModelStr = hw.target, HomeButtonType = `/buttons/home-button-type`; disk cache `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist` (keyed by build + UUID only -> stale iPod answers survived; reflashed clean v12 to clear it).
- **0060** drivers by Codex (`codex/input/hw-report.md`): `bsd/dev/sweet_keys.c` (SPMI arbiter v5 observer reads, PON 0x810 bits KPDPWR/RESIN SID0, PM6150L GPIO2 SID4 0xc110; works), `bsd/dev/sweet_touch.c` (GENI SE7 I2C @0xa84000, starts 45 s after boot). Touch result: SE7 fw ok (proto 3) but **0x5d and 0x38 both time out -> touch IC unpowered** (L11C LDO needs an RPMh vote). Codex task 8 = RPMh client (`codex/rpmh/`), task 7 = battery (`codex/battery/`).
- **0061** (CI 39117078444): red status bar "rdar:45025538" = UIKit "No status bar visual provider for screen" (360x800 pt is no real iPhone). AppleCLCD now presents **1125x2436** (iPhone X) and shows the centered 1080x2400; touch mapped the same; ADT canvas 1125x2436. `sweetfb_native=1` reverts.
- Images: `shim/out/shim-boot-0060b.img` (0060 + /buttons ADT `sweet-0060hi.adt`). Next: 0061 image with ADT from current mkadt.

### >>> HANDOFF 2026-10-17 late (read this first) <<<

**STATE: iOS 16.7 Setup.app "Hello" screen is ON THE PHONE** (boot `shim/out/shim-boot-0057u.img` on image v12). Photo `/run/media/dipa/android/ios-sweet-work/MILESTONE-setup-hello-2026-10-17.jpg`. SpringBoard + backboardd + Setup run; nothing crashes in the GUI path. Remaining crash/relaunch loops: installcoordinationd (should be fixed by 0057, unverified), mediaserverd (VirtualAudio removed in v12, unverified), mobileactivationd (~100/boot, not analysed), bluetoothd + fairplayd.H2 relaunch constantly (no hardware).

**What is on the phone now:** userdata = image **v12** (`/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v12.img` + `.simg` + `.sha256`; v9 kept as fallback, v8/v10/v11 deleted or superseded). Kernel **0057** (`xnu/out-0057`, CI `39067532732`). ADT **`shim/out/sweet-0057hi.adt`** (QEMU twin `research/q0057-hi.adt`). Boot image **`shim/out/shim-boot-0057u.img`**. Boot args: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1`.

**Build a phone image** (kernel K, ADT A): `python3 tools/mkpayload.py $K $A shim/out/payload-X.bin --cmdline "$B" && (cd shim && ./build.sh ../shim/out/payload-X.bin && cp out/shim-boot.img out/shim-boot-X.img)` then check `python3 tools/unpack_bootv2.py shim/out/shim-boot-X.img shim/out/check-X && cmp shim/out/check-X/ramdisk shim/out/payload-X.bin`. QEMU smoke test: `python3 tools/mkpayload.py $K research/q0057-hi.adt $S/csq/payloadX.bin --ramdisk $S/csq/root-fork.img --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"` + `PATH=~/swift/usr/bin:$PATH python3 tools/qemu-sweet.py ... --secs 40 --kernel $K --fail-on-panic` (S = scratchpad; root-fork.img lives in the session scratchpad csq/ dir and may be gone in a new session).

**Kernel workflow:** edit `research/bt28/xnu/...` (keep the pre-edit copy in `research/preNNNN/`), syntax check `python3 tools/xnu-cc.py research/run28.log research/bt28 <file>`, make `xnu/patches/NNNN-*.patch` with `diff -u pre bt28 | sed` path rewrite, add new/changed files to `research/check-patch-order.py` list, run it (must say applied + MATCH), commit + push -> GitHub Actions `xnu` builds (~8 min) -> `gh run download <id> -R dipa-studio/ios-sweet -n xnu-kernel -D xnu/out-NNNN`. **Repo is PUBLIC since 2026-10-17** (private macOS runner minutes ran out: "recent account payments have failed"); public = free runners. Commit trailer: only `Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>`.

**Phone workflow (user does buttons, Claude runs commands):**
- Boot test kernel: phone in fastboot -> `fastboot boot shim/out/shim-boot-X.img`. NEVER `fastboot getvar`.
- **Read logs WITHOUT filming:** after a test the user holds Power+VolDown -> fastboot -> `fastboot boot recovery/recovery.img` (OrangeFox, temp) -> wait for `adb shell true` -> `tools/sweet-log-pull.sh /run/media/dipa/android/ios-sweet-work/logs/<name>.txt` (reads the 4 MiB console ring the kernel keeps in the last 1025 blocks of userdata; patch 0047). Then `adb reboot bootloader` brings the phone back to fastboot without the user.
- **Flash a root image:** `fastboot flash -S 512M userdata <img>.simg` (7 min, 25 chunks). After a big flash the Xiaomi ABL fastboot hangs: next `fastboot boot` stalls at "Sending" -> the USER must re-enter fastboot by hand (Power+VolDown). Do not retry commands against the hung ABL. A flash right after `adb reboot bootloader` once failed on chunk 1 (USB protocol error): just retry. User must NOT unplug during flash (once interrupted at 25/25 -> reflash).
- **Edit the phone root without reflashing (same-length only):** find the file's raw offset in the local image (it is one extent; search the image for its first 4 KiB), compare with `adb exec-out dd if=/dev/block/sda34 ...`, then `adb push` changed 4 KiB blocks and `dd of=/dev/block/sda34 seek=... conv=notrunc,fsync`, read back. Recovery kernel has no hfsplus.
- **Pull the whole phone root** (12 min): `adb exec-out "dd if=/dev/block/sda34 bs=1048576 count=16384" > img`; mount ro with udisksctl or `sudo mount -t hfsplus -o loop,ro`. To write into a pulled image: `sudo fsck.hfsplus -fy` first (volume is never cleanly unmounted -> Linux mounts ro even with -o force), then `sudo mount -t hfsplus -o loop,force`.
- Making images: copy latest img with `cp --sparse=always`, mount rw with force, change, `sync; umount`, `sha256sum`, `img2simg`. Keep only the latest image + one fallback on the SSD (/home is ~98% full; big work files live in `/run/media/dipa/android/ios-sweet-work/`, old kernels/images archived in `.../ios-sweet-work/archive/`).

**Diagnostics built into the kernel (all SWEET-only):** persistent console log (0047, boot-arg sweetlog=0 off; flush every ~1 s); `|name| text` copies of /dev/null writes for processes in `sweetnull` (default backboardd,SpringBoard; 0048); `IOK name:` IOKit lookup/open trace for `sweetiolog` (default backboardd,SpringBoard; 0049/0054, 400 lines max); `SWEET Sandbox: proc op N` (0050); `STR ...` heap text dump on SIGABRT/SIGTRAP for `sweetscan` (default backboardd,SpringBoard; 0051/0053) - this is how NSException/BSAssert reasons were found; `SMP name tN pc .. bt ..` user stack sampler for `sweetsample` (default SpringBoard,backboardd; first at 90 s then every 120 s, 40 samples, 16 frames; 0054/0055); `SWEET FB: N swaps` every 1000 swaps; `CRASH name ...` crash reports (0043/0044). Symbolicate cache addresses with `ipsw dyld a2s research/dsc/dyld_shared_cache_arm64 <addr - slide>`; main-binary addresses: addr - mh + 0x100000000.

**Patches added today (0046-0057, all CI-built):** 0046 IOSurface (IOCoreSurfaceRoot) + IOMobileFramebuffer (AppleCLCD on /arm-io/disp0) + arm-io nubs; 0047 persistent log; 0048 /dev/null tee; 0049 IOKit trace; 0050 permissive "Sandbox" MAC policy; 0051/0053 heap text on crash; 0052 RENAME_SWAP via exchangedata on HFS; 0054/0055 sampler, swap counter, AKS selectors 35 (extended device state = 17) and 101; 0056 panel copy from the vsync timer at 10 Hz (sweetfb_hz) instead of every swap; 0057 no config_ext_resolver (ACL group entries resolve without memberd). ADT (tools/mkadt.py): /arm-io/disp0, /chosen/display-scale 3, /chosen/unique-chip-id (synthetic 0x5357454554000001) + chip-id 0x8015 -> IOPlatformUUID A9683613-3704-53F2-95D0-98A8FB574E78. Image v9-v12: /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}; launchd.plist StandardOut/ErrorPath renamed to ...PatX (pointing jobs at /dev/console stops them launching: EACCES); v12: LastSystemVersion=20H392 in plain /var/mobile/Library/Preferences/com.apple.migration.plist (stops the 1-hour data migration that ran every boot), VirtualAudio.plugin moved to /Library/Audio/Plug-Ins/HAL.disabled/.

**Codex helper** (`codex exec --skip-git-repo-check -m gpt-6.1-sol -c model_reasoning_effort=high -s workspace-write -C /run/media/dipa/android/ios-sweet-work/codex -o OUT "$(cat task.md)" < /dev/null`, run detached with setsid nohup; config already defaults to gpt-6.1-sol high). Reports in `/run/media/dipa/android/ios-sweet-work/codex/`: mediaserverd-report.md, installcoordinationd-report.md (-> 0057), ufs-performance.md (ranked UFS speed plan), **ufs/0058-sweet-ufs-faster.patch** (READY, not yet applied: ufslogsecs default 10 s log flush batching, optional ufscache=1 directional cache ops, optional ufswait=1 spin-then-sleep; syntax-checked, host-stub verified; see ufs/README.md). Apply it as patch 0058 after review.

**Claude safety-classifier note:** several messages about QuartzCore renderer selection / writing an HFS reader tool were cut off by an automatic classifier (false positives, user agrees). Keep those topics short or let them be; they are not blockers now (rendering already works).

**NEXT STEPS (in order):**
1. **Input** (Setup stuck on Hello): touch + power + volume as IOHIDEventService/IOHIDDevice services backboardd matches (log shows backboardd matching IOHIDDevice with DeviceUsagePairs and IOService with HIDServiceSupport=true). Phone hardware: Goodix/Focaltech touch over I2C/SPI (check live DT `dtb/`), buttons via PMIC (pon) and GPIO keys. Start with a fake HID service (e.g. power button via a timer or boot-arg) to learn the ABI, then real drivers.
2. **Device identity**: MobileGestalt reports iPod + Home button. Add /product node / product-type (iPhone10,3, D22AP), device class iPhone, home-button-less (Face ID) properties in mkadt; find exact MG keys (MGIOMFBSupport.c, `IODeviceTree:/product` lookups seen in backboardd trace).
3. **Battery**: AppleSmartBattery/IOPMPowerSource style service (status bar shows empty red battery).
4. Apply Codex 0058 (UFS speed), consider cpus>1, check whether installcoordinationd/mediaserverd loops are gone, analyse mobileactivationd.
5. Hide kernel console over the GUI when not debugging: boot-arg sweetfb_noconsole=1 (but then panics are not on screen).

- **🎉 MILESTONE: iOS GUI ON THE PHONE - Setup.app "Hello" screen (2026-10-17 ~21:30 UTC, boot 0057u on image v12).** Photo: `/run/media/dipa/android/ios-sweet-work/MILESTONE-setup-hello-2026-10-17.jpg` (animated "dobar d(an)" hello lettering, status bar "iPod" + battery icon, bottom "Pritisnite Home za otvaranje" = "Press Home to open" (locale hr), (i) button). Rendered by iOS 16.7 SpringBoard/Setup through CoreAnimation -> backboardd -> kernel IOSurface (IOCoreSurfaceRoot) -> kernel AppleCLCD swaps -> ABL panel memory. What made it: data migration finally skipped (v12: `LastSystemVersion` 20H392 in the plain `/var/mobile/Library/Preferences/com.apple.migration.plist`; ADT `/chosen/unique-chip-id` + `chip-id` for IOPlatformUUID), 0057 (no ext resolver -> ACL containers work), plus everything before (0046 display/IOSurface, 0050 Sandbox policy, display-scale 3, containers dirs, RENAME_SWAP, AKS 35/101, 10 Hz present).
  **Missing (next work, in rough order):**
  - **Input: no touch, no power button, no volume buttons** (no HID drivers; backboardd looks for IOHIDDevice / HIDServiceSupport services). Setup is stuck on "Hello".
  - **Device identity: it thinks it is an iPod with a Home button** (status bar "iPod", "Press Home to open"): MobileGestalt has no product/model identity (no /product node, product-type, etc.) - give it iPhone10,3 / home-button-less (Face ID class) identity.
  - **No battery detection** (no battery/charger/PMIC service; status bar battery shows empty/red).
  - Locale came up Croatian (hr) - harmless.
  - Performance: cpus=1, polled UFS (Codex UFS patch in progress: `codex/ufs/`), backboardd presents at 60 Hz.
- **README refreshed (2026-10-17):** read the full notes and updated the public overview through patch 0045, v7 hardware observations and prepared v8 diagnostics. Documents real Apple userspace, the missing display/IOSurface GUI work, limited security services, highmem, persistent userdata root, current tooling and build examples. Documentation only; no kernel build, tests, flashing or phone boot performed.

- **v11 still migrated (boot 0056u, 20:32 UTC, `logs/boot-0056u-v11.txt`).** On iOS kCFPreferencesCurrentHost lands in the plain plist: all DMEnvironment setters (setLastBuildVersionPref, setUserDataDispositionPref, ...) use CurrentHost and the phone's `/var/mobile/Library/Preferences/com.apple.migration.plist` holds DMUserDataDisposition; datamigrator's XPC service runs with `_RoleAccount: mobile`. **Image v12** = v11 + `LastSystemVersion` = 20H392 added to that plain `com.apple.migration.plist` (keeps DMUserDataDisposition, 501:501 0600) + `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` moved to `/Library/Audio/Plug-Ins/HAL.disabled/` (Codex: stops the mediaserverd SIGSEGV loop; untested). **installcoordinationd loop explained (Codex `codex/installcoordinationd-report.md`):** os_crash "Failed to create data directory" EACCES for `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.installcoordinationd/Library/InstallCoordination`: `Library`/`Library/Caches` in the container are ACL-only (uid/gid 0xfffffffe, mode 0000, ACE for group GUID ...012B = gid 299 systemusers); with `config_ext_resolver` the kernel asks an identity resolver (memberd/opendirectoryd) that iOS does not run -> EWOULDBLOCK -> ACE skipped -> EACCES. **Patch 0057**: drop config_ext_resolver from FILESYS_BASE (compatibility UID/GID<->GUID mapping is used instead), likely fixes other ACL-protected containers too. Codex task 3: UFS driver performance design (`codex/ufs-performance.md`).
- **mediaserverd SIGSEGV loop explained (Codex report `/run/media/dipa/android/ios-sweet-work/codex/mediaserverd-report.md`):** the fault (FAR 0x390) is in the HAL plug-in `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio` (ApplyBeamFormerPreference <- VirtualAudio_PlugIn::SetPropertyData <- HALS_AHPPlugIn::ObjectSetPropertyData <- AudioObjectSetPropertyData <- MediaExperience vaemVADSerializationListener): the default virtual audio device ('vdef', VirtualAudioDevice_Default) does not exist (no audio hardware/driver, no /product/audio). Proposed workaround for GUI bring-up: move VirtualAudio.plugin out of /Library/Audio/Plug-Ins/HAL/ in the next root image (untested). v11 reflashed OK after an interrupted flash (USB unplugged at chunk 25/25). Codex task 2: installcoordinationd crash loop.
- **ROOT CAUSE of the endless migration: no IOPlatformUUID (2026-10-17).** datamigrator's `-[DMMigrator isMigrationNeededWithBuildVersion:lastBuildVersion:clientPID:]` gets lastBuildVersion from `-[DMEnvironment lastBuildVersionPref]` = `CFPreferencesCopyValue("LastSystemVersion", "com.apple.migration", kCFPreferencesCurrentUser, kCFPreferencesCurrentHost)` -> a ByHost file. The phone had `/var/mobile/Library/Preferences/com.apple.migration.plist` (only DMUserDataDisposition) and NO `Preferences/ByHost` directory at all. On arm XNU publishes IOPlatformUUID (gethostuuid) only from `/chosen/unique-chip-id` (8 bytes) + `/chosen/chip-id` (4 bytes) (IOPlatformExpertDevice::generatePlatformUUID, called from IOPlatformExpert::start); the SWEET ADT had neither, so every CurrentHost preference write failed. Fix: mkadt adds `unique-chip-id` = 0x5357454554000001 (synthetic) and `chip-id` = 0x8015 -> UUID `A9683613-3704-53F2-95D0-98A8FB574E78` (same SHA1 namespace computation as the kernel). ADT `shim/out/sweet-0057hi.adt`, boot image `shim/out/shim-boot-0056u.img` (0056 kernel + 0057hi ADT, QEMU OK). **Image v11** = v10 + `Preferences/ByHost/com.apple.migration.<UUID upper and lower>.plist` {LastSystemVersion: 20H392} (501:501, dir 0755 files 0600). Flashing v11 2026-10-17 ~20:10 UTC. Codex (gpt-6.1-sol high) analysing the mediaserverd SIGSEGV loop in `/run/media/dipa/android/ios-sweet-work/codex/` (report `mediaserverd-report.md`).
- **v10 did NOT stop the migration (boot 0056n on v10, 19:18 UTC, `logs/boot-0056n-v10.txt`):** datamigrator + migrationpluginwrapper ran again, backboardd drew the progress UI (13000+ swaps in ~5 min). So `com.apple.DataMigration.plist`/LastSystemVersion is not the deciding input. What is known about the decision (datamigrator XPC service `DataMigration.framework/XPCServices/com.apple.datamigrator.xpc/com.apple.datamigrator`, strings): `Migration sentinel present. Migration necessary.` (sentinel `/private/var/datamigrator/migrate`, created and removed by datamigrator itself; absent in v10), `System build version changed from %@ to %@. Migration necessary.` / `...unchanged... not necessary`, `Disposition dictionary with build version %@ is out of date relative to current build version %@`, `SyntheticDidUpgrade`, DMUserDataDispositionManager; previous build source not found yet (not in the binary's plain strings; likely in DataMigration.framework's DMUserDataDispositionManager / preferences domain `com.apple.migration`). backboardd calls `DMPerformMigrationIfNeeded` and holds SpringBoard in `checkInAndWaitForDataMigration` until it returns. NEXT: find where the previous build version / disposition dictionary is stored (disassemble DataMigration.framework DMUserDataDispositionManager + datamigrator's isMigrationNeeded path), write it into the image; or get datamigrator's os_log out (OS_ACTIVITY_DT_MODE needs a launchd env change).
- **Data migration never records completion -> skip it (image v10, 2026-10-17).** Boot 0056n (18:16 UTC, `logs/boot-0056n.txt`, 30 min): 10 Hz panel copies, backboardd still presents at ~60 Hz (107k swaps), SpringBoard still in `BKSSystemShellService _checkIn` at sample 14. Every migration run takes 1+ hour on cpus=1 and repeats each boot. DataMigration decides from `/var/mobile/Library/Preferences/com.apple.DataMigration.plist` key `LastSystemVersion` (vs ProductBuildVersion 20H392); the pulled phone root (21:01 local) had NO such file. **v10** = that pulled phone state + fsck.hfsplus -fy (volume was marked not cleanly unmounted -> Linux mounts it read-only even with -o force; fsck found only hard-link counts + minor volume header, repaired) + `com.apple.DataMigration.plist` {LastSystemVersion: 20H392} (501:501 0600). `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v10.img` (+ .sha256, .simg); v8 files deleted, v9 kept as fallback.
- **Repo made PUBLIC (2026-10-17, user request) so GitHub-hosted macOS runners are free:** the 0056 CI run was refused ("recent account payments have failed or your spending limit needs to be increased"). Checked before: no device dumps, serials, IMEI or Apple binaries in the history (.gitignore keeps backup/, dtb/, recovery/, research/ out); commit author emails are public now. 0056 rerun `39056660104` passed -> `xnu/out-0056`, QEMU OK, image `shim/out/shim-boot-0056n.img` booted 18:16 UTC.
- **Boot 0055n (17:16 UTC, ~65 min, `logs/boot-0055n.txt`): migration bar reached 99% in ~30 min (was ~60), then crawled.** Root cause of the slowness: backboardd presents the progress UI at ~58 Hz (**167,000 swaps in ~48 min**) and AppleCLCD copied the full 10 MB frame into the panel on every swap -> ~600 MB/s memcpy on the single CPU (cpus=1), starving everything (polled UFS I/O too). Sampler (16 frames): **SpringBoard waits in `-[BKSSystemShellService _checkIn]`** (synchronous BSXPC call to backboardd, from FBSystemShellInitialize <- SBSystemAppMain) - backboardd holds the system shell check-in until the data migration ends, so the migration is the gate to SpringBoard UI. **Patch 0056**: swap only records the latest swap record; the vsync timer copies it to the panel every 60/sweetfb_hz ticks (default 10 Hz); `SWEET FB: N swaps` every 1000.
- **Boot 0054n (16:53 UTC, ~12 min, `logs/boot-0054n.txt`, patch 0054 sampler):** the data migration runs AGAIN on every boot (it completed once in the 0053s run) - completion is not being recorded. backboardd keeps swapping the progress UI (3700+ swaps in ~10 min, `SWEET FB: N swaps`). Sample during migration: SpringBoard main thread blocked in a synchronous XPC call (`mach_msg2_trap <- dispatch_mach_send_and_wait_for_reply <- xpc_connection_send_message_with_reply_sync <- -[BSXPCServiceConnectionMessage _sendSynchronously:] <- BSXPCServiceConnectionProxy ...`). Unsupported AKS selectors seen: 101 `aks_assert_consume`, 93 `aks_get_internal_info`, 34 `_aks_stash_load` (datamigrator), **35 `aks_get_extended_device_state`** (SpringBoard, chronod, remindd, searchd, symptomsd...). **Patch 0055**: AKS 35 answered like 17 (same DER device state), 101 succeeds; sampler backtraces 16 frames, 40 samples (~80 min).
- **MILESTONE: FIRST iOS PIXELS ON THE PANEL (boot 0053s long run, 15:33 UTC, ~60 min; photos IMG_20261017_183029/183222/183419; log `/run/media/dipa/android/ios-sweet-work/logs/boot-0053s-long.txt`, 407 KB, 1428 flushes).** The iOS data-migration boot screen (Apple logo + progress bar) was shown and the bar advanced to full in ~60 minutes. It is drawn by **backboardd through our kernel display path**: `SWEET SURF: backboardd create id 2/3 1080x2400 fmt 0x42475241 (BGRA) bpe 4 bpr 4352 size 10452992 keys 11`, IOSurface sels 9, 27, 5, 34 and traps 2/3 (lock/unlock), IOMFB SwapBegin (4) / SwapEnd (5, 1132-byte struct) / GetLayerDefaultSurface (3) / 68 / 55, then `SWEET FB: swap N layer 0 surface 2|3 1080x2400 ... a 0,0 1080x2400 b 0,0 1080x2400` alternating (double buffering; only the first 16 swaps are logged). Triggered by `com.apple.datamigrator` (AKS sels 101/93/34 return unsupported) and `com.apple.migrationpluginwrapper`. Probably ProgressUI-style CoreGraphics drawing into IOSurfaces (no GPU used). After the migration: SpringBoard and backboardd stay up (exec once each, no crash), nothing new drawn; crash loops only for mediaserverd, installcoordinationd, mobileactivationd (~108 each in the hour); bluetoothd and fairplayd.H2 relaunch constantly (no hardware). Kernel console text overlays the image (sweetfb_noconsole=1 would hide it).
- **MILESTONE: SpringBoard and backboardd both stay up (boot 0053s on v9, 15:29 UTC, `logs/boot-0053s.txt`).** With /chosen/display-scale = 3: `exec SpringBoard` once, `exec backboardd` once, no CRASH for either; MobileGestaltHelper reads IOMFB size/dot pitch; backboardd opens AppleCLCD (sels 57,15,22,29,33,79,74 as before). No IOSurface creates and no swaps: backboardd keeps polling `IOProviderClass IOAcceleratorES` (-> 0 found) and AGXAccelerator/IOAcceleratorESLegacy - no GPU, so no CoreAnimation renderer yet. Log goes quiet after ~47 s (only 47 flushes): the system idles. NEXT WALL: CoreAnimation rendering without a GPU (CPU path) so the window server produces frames.
- **SpringBoard's invalid mode = scale 0 (boot 0053n, 15:19 UTC, `logs/boot-0053n.txt`, heap scan):** `FBSDisplayMode cannot be configured as requested : <FBSDisplayMode: ...; infxinf@0x (1080x2400/0) 60Hz sRGB SDR>` - pixel size and 60 Hz are fine, scale is 0. libMobileGestalt (MGIOMFBSupport.c, sub_19c560a54) reads `display-scale` (u32, default 0) and `display-rotation` from **IODeviceTree:/chosen**, pixel size via IOMobileFramebufferGetDisplaySize (MobileGestaltHelper sel 8/28 in the log). mkadt now adds `/chosen/display-scale` = 3 (1080x2400 @3x = 360x800 pt). ADT `shim/out/sweet-0054hi.adt` (QEMU `research/q0054-hi.adt`), image `shim/out/shim-boot-0053s.img` = 0053 kernel + 0054hi ADT. Possible catch: MobileGestalt may have cached scale 0 in `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/...`.
- **MILESTONE: backboardd survives and opens the display (boot 0052n on v9, 15:07 UTC, `logs/boot-0052n.txt`).** No `CRASH backboardd`. It walks `IODeviceTree:/arm-io` children, matches IOMobileFramebuffer, `open AppleCLCD type 0 -> 0x0`, calls IOMFB selectors 57, 15, 22, 29, 33, 79 (GetBlock, out 1580/40), 74 (SupportedFrameInfo), 12, 6, 27, 17, 84, 50; opens IOCoreSurfaceRoot (sel 13); looks for AGXAccelerator / IOAcceleratorES / IOAcceleratorESLegacy (none, repeatedly), `IODeviceTree:/product` (none, repeatedly), `/buttons`, aop-smart-cover, IOPMrootDomain. No swaps yet. **SpringBoard now gets past display discovery** and dies (sig 5) in `-[FBSDisplayMode _initWithWidth:height:preferredScale:scaleOverride:refreshRate:gamut:hdr:rotation:virtual:validityCheck:]+580` <- `_initWithCADisplayMode:scale:rotation:` <- `FBSDisplayConfiguration _initWithImmutableDisplay:` <- `FBSDisplaySource initWithDisplay:` <- `FBSDisplayMonitor` (slide 0x14504000): "FBSDisplayMode cannot be configured as requested". Checks: width/height >= 2, preferredScale > 0, scaleOverride >= 0, refreshRate non-zero, valid gamut/HDR/rotation. Suspects: refresh rate 0 (IOMFB timing blocks answered with zeros) and/or scale 0 (MobileGestalt main-screen-scale; no /product node in the ADT). **Patch 0053**: heap text scan also on SIGTRAP and for SpringBoard (default sweetscan list backboardd,SpringBoard) to read the exact mode description.
- **Image v9 (2026-10-17) = v8 + `/private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}` (root:wheel 0755; on iOS these live on the data volume, absent from the rootfs) + launchd.plist with StandardOutPath/StandardErrorPath renamed to ...PatX (the in-place phone fix; daemons' stdout reaches the log via the 0048 /dev/null tee).** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v9.img` SHA256 aa9f9139...6e67, `.simg` 12,145,303,864 bytes; written through `sudo mount -t hfsplus -o loop,force`. **Patch 0052** (`xnu/patches/0052-sweet-hfs-rename-swap.patch`, CI `39042761826` -> `xnu/out-0052`, QEMU OK, image `shim/out/shim-boot-0052n.img`): vn_rename falls back to VNOP_EXCHANGE when VNOP_RENAMEX(RENAME_SWAP) returns ENOTSUP for two regular files on one volume (HFS exchange swaps data and cnode descriptors = swap semantics). Flashing v9 started 2026-10-17 ~15:00 UTC.
- **backboardd EXCEPTION FOUND (boot 0051n, 14:41 UTC, `logs/boot-0051n.txt`, patch 0051 heap scan):** `*** Terminating app due to uncaught exception 'NSInvalidArgumentException', reason: '*** -[NSURL initFileURLWithPath:]: nil string parameter'`. First-throw stack (slide 0x337f0000): `+[NSURL fileURLWithPath:]` <- `-[BSSystemContainerForCurrentProcessPathProvider libraryPath]` <- `cachesPath` <- backboardd main+0x1d858 (StartWindowServer, shader cache path). BaseBoard: libraryPath = `BSSystemSharedResourcesDirectoryForIdentifier` = `BSSystemSharedDirectoryForIdentifier(id)` + "/Library"; that calls `container_system_path_for_identifier` (libsystem_containermanager -> `_container_legacy_lookup` -> containermanagerd XPC), which returned NULL (BaseBoard logs "Error getting shared system container ..."). With 0050 the sandbox denials are gone. containermanagerd.log.0 in the pulled (pre-0050) image: "Did not receive any persona attributes", "Expected to find at least one persona", notify_register_dispatch failed 9; `/private/var/containers` does NOT exist in the root image (on iOS it lives on the data volume). usermanagerd does create kernel personas 99/199/1000, but fails to save `/private/var/keybags/usersession.kb` and `persona.kb`: "Failed to atomically swap writing file: Operation not supported (45)" (HFS has no RENAME_SWAP). NEXT: create `/private/var/containers` (and check what containermanagerd needs for system group containers), fix or emulate RENAME_SWAP on HFS, then re-check backboardd.
- **Disk housekeeping (2026-10-17):** /home was at 4 GiB free. Moved (not deleted) `xnu/out-0010..0044` and old `shim/out/*.img`/`payload-*.bin` (everything before 0046) to `/run/media/dipa/android/ios-sweet-work/archive/{xnu,shim-out}` (6.3 GiB); /home now 8.7 GiB free. Large work files (phone-root.img 16 GiB, logs, tracev3 dumps) live in `/run/media/dipa/android/ios-sweet-work/`. **Patch 0051** (`xnu/patches/0051-sweet-crash-heap-strings.patch`, CI `39041440935`): on SIGABRT of a process in boot-arg `sweetscan` (default backboardd) the crash log prints heap text runs as `STR ...` (to find the NSException reason). Boot 0051n 14:41 UTC.
- **Boot 0049n (14:11 UTC, `logs/boot-0049n.txt`, patch 0049 trace): backboardd's IOKit lookups before it dies** (each run): `IODeviceTree:/arm-io/pearl-sep` -> none, matching ApplePearlSEPDriver -> none, `IOService:/IOResources/AppleKeyStore` open OK, matching IOHIDDevice (DeviceUsagePairs) / `HIDServiceSupport` notifications, `usb-drd,t8027` notification; **never any IOMobileFramebuffer / display lookup** - the exception happens before display detection. launchd (tracev3) shows backboardd's mach lookups denied with `error = 159: Sandbox restriction`: com.apple.mobilegestalt.xpc, com.apple.iohideventsystem, com.apple.system.notification_center, com.apple.logd, containermanagerd, ACCHWComponentAuthService, oscard. libsystem_sandbox uses `__mac_syscall("Sandbox", op, arg)` (ops: 1 init, 2 check, 3 note, 4 container path, 5/6/7/8 extensions, 0x15 check_bulk, ...); with no policy named "Sandbox" every call gets ENOPOLICY. **Patch 0050** (`xnu/patches/0050-sweet-sandbox-policy.patch`): built-in permissive MAC policy "Sandbox" in sweet_amfi.c (mpo_policy_syscall returns 0, container/extension ops ENOENT; logs first use of each op as `SWEET Sandbox: proc op N`).
- **Phone root pulled (16 GiB, 12 min, `adb exec-out dd if=/dev/block/sda34`) to `/run/media/dipa/android/ios-sweet-work/phone-root.img` (loop-mounted ro at /run/media/dipa/iOSRoot).** No .ips crash reports. logd tracev3 (Persist, LZ4 `bv41` blocks, decompressed strings in `ios-sweet-work/tracev3/all.txt`): /private/var/db/uuidtext is EMPTY so only launchd's own messages are readable. Findings: v8 launch failure = `Service could not initialize: posix_spawn(/usr/libexec/backboardd), error 0xd - Permission denied` (xpcproxy exit 78) - /dev/console not openable for the mobile user; backboardd[98] spawned `com.apple.MTLCompilerService` before SIGABRT; **launchd denies many mach lookups with `error = 159: Sandbox restriction`** (mediaserverd -> com.apple.backboard.display.services, remoted, mobiletimerd -> sleepd, identityservicesd -> apsd) and **tccd exits OS_REASON_SANDBOX "profile not found"** - no Sandbox kext/profiles on SWEET; likely cause of many daemon crash loops (next wall after the display). CF's uncaught-exception text never reaches stderr for daemons (no CF handler; libc++abi prints only the type). **Patch 0049** (`xnu/patches/0049-sweet-iokit-trace.patch`): `IOK name: ...` console lines for registry path lookups, child iterators, matching (with result count/class), matching notifications and IOServiceOpen of the processes in boot-arg `sweetiolog` (default backboardd), max 400 lines.
- **CONFIRMED: launchd StandardOut/ErrorPath=/dev/console stops backboardd from launching.** After the in-place rename (boot 0047n-b, log `logs/boot-0047n-b.txt`) backboardd launches again and dies as on v7: sig 6, `___pthread_kill <- abort <- demangling_terminate_handler <- _objc_terminate <- std::terminate <- _dispatch_client_callout <- _dispatch_once_callout <- main+0x9334` (window-server dispatch_once). It never opened AppleCLCD or IOCoreSurfaceRoot (no `SWEET FB/SURF: backboardd` lines) -> throws before/at display detection. **Patch 0048** (`xnu/patches/0048-sweet-devnull-tee.patch`, CI `39037749668` -> `xnu/out-0048`, image `shim/out/shim-boot-0048n.img`): writes to /dev/null by processes named in boot-arg `sweetnull` (default `backboardd,SpringBoard`) are printed as `|name| text`. Boot 0048n (13:48 UTC, log `logs/boot-0048n.txt`): only `|backboardd| libc++abi: terminating due to uncaught exception of type NSException` (CF's reason line goes to os_log, not stderr, despite CFLOG_FORCE_STDERR/OS_ACTIVITY_DT_MODE). Next: pull the whole 16 GiB root from recovery (`adb exec-out dd if=/dev/block/sda34`) to `/run/media/dipa/android/ios-sweet-work/phone-root.img` and read ReportCrash .ips files / logd tracev3 for the exception reason.
- **Persistent log WORKS (boot 0047n on v8, 13:31 UTC; pulled from OrangeFox: 70 KB, 111 flushes, `/run/media/dipa/android/ios-sweet-work/logs/boot-0047n.txt`).** In 3 min **backboardd was never exec'd** (v7 ran it), ~70 bare `exec xpcproxy`, SpringBoard loops on FBSDisplayMonitor (no display), mediaserverd sig 11 loop, locationd, lockdownd sig 5. Suspect the v8 plist (StandardOut/ErrorPath=/dev/console on backboardd). **In-place plist edit from recovery (no reflash):** launchd.plist is one extent at image/partition byte offset 0x1e025f000 (195 blocks; verified bytes identical on phone vs local v8 mount); renamed StandardOutPath->StandardOutPatX and StandardErrorPath->StandardErrorPatX (same length, both jobs) in blocks 68 and 105 via `adb push` + `dd of=/dev/block/sda34 seek=... conv=notrunc,fsync`, read back VERIFIED. Phone now differs from local v8 image in those 2 blocks (files in `/run/media/dipa/android/ios-sweet-work/`). Test boot 0047n 13:38:42 UTC. Recovery kernel has no hfsplus (raw block edits only).
- **Patch 0047: persistent console log (CI `39036478663` -> `xnu/out-0047`, QEMU no panic; phone image `shim/out/shim-boot-0047n.img` = 0047 + sweet-0046hi.adt + 0045n cmdline).** `_cnputs` (all console output: kernel printf + daemons' /dev/console) also appends to a 4 MiB ring in sweet_ufs.c; after the UFS partition is ready and writable (ufsrw=1) a thread writes new ring blocks + header to the LAST 1025 blocks (4 KiB) of userdata every ~1 s and SYNCHRONIZE CACHE (prints `SWEET LOG: console log to the last 4100 KiB ...`). Read: phone to fastboot -> `fastboot boot recovery/recovery.img` (OrangeFox) -> `tools/sweet-log-pull.sh OUT.txt` (adb dd of that area + `tools/sweet-log.py`). Not yet verified on the phone. Panics after the last flush are not in it (screen still shows them). 2x font attempt reverted: XNU's v_scale only scales the progress icon.
- **Boot 0046n on v8 (13:05 UTC, VID_20261017_150522_HSR_120):** phone prints `SWEET SURF: IOCoreSurfaceRoot registered` and `SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0x9c000000, default surface 1` (nubs + match work on hardware). Visible userspace: early boot tasks, launchd_cache_loader, `exec SpringBoard`; no `exec backboardd` / `SWEET FB: backboardd` line seen in the readable frames (video blurry). Frame 00:47: lower half of the screen shows OLDER console text (16:01:12) below newer lines -> unconfirmed hint that a swap copied an old snapshot of the panel (CA reads the default/displayed surface = panel memory). SpringBoard still dies in `-[FBSDisplayMonitor _initWithBookendObserver:transformer:]+1160` (sig 5, slide 0x30960000). **Shim: 2x console font** (`shim/loader.c` Video.v_depth scale field = 1) so phone videos are readable; image `shim/out/shim-boot-0046b.img` = 0046n + 2x font.
- **Patch 0046: built-in IOSurface + IOMobileFramebuffer (2026-10-17, offline, NOT phone-tested).** `xnu/patches/0046-sweet-iosurface-iomfb.patch`: new `iokit/Kernel/arm/SweetSurface.{h,cpp}` (IOCoreSurfaceRoot + IOSurfaceRootUserClient: sel 13/0/4/34/35, traps 0-5, wired buffers mapped into the caller, per-client refs) and `SweetFramebuffer.cpp` (AppleCLCD : IOMobileFramebuffer on /arm-io/disp0: sizes, swap = CPU copy of enabled layers into the ABL panel memory, 60 Hz vsync via sendAsyncResult64WithOptions(CanDrop), panel memory published as the default IOSurface, kernel text console paused on the first swap only with `sweetfb_noconsole=1` (default keeps the console: pausing switches it to serial and would hide panics)); SweetPlatformExpert creates nubs for /arm-io children (not gic) attached to the platform expert (IOPlatformDevice::compareName casts its provider to IOPlatformExpert, so nubs under the arm-io nub never matched - first CI build showed that in QEMU). mkadt: /arm-io/disp0 {device_type display-subsystem, compatible sweet,display, canvas-width 1080, canvas-height 2400}. CI `39021572169` -> `xnu/out-0046`. **QEMU** (`research/q0046-hi.adt`, RAM disk init, msgbuf): `SWEET SURF: IOCoreSurfaceRoot registered`, `SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0x9c000000, default surface 1`, init alive, no panic. Phone image **`shim/out/shim-boot-0046n.img`** (SHA256 9c4d2fe9..., CI `39024108588` with the review fixes: swap bounds check, zeroed 64-bit async ref, port release, 1.5 GiB wired budget, ref leak/race fixes; QEMU re-checked) = 0046 kernel + `shim/out/sweet-0046hi.adt` + the 0045n cmdline; boot it on image v8 (backboardd/SpringBoard log to console) and film. Expected new console lines: `SWEET SURF: <proc> create id ...`, `SWEET FB: backboardd sel N ...`, `SWEET FB: swap ...`. Open question for the next wall: QuartzCore's IOMFB server builds its renderer from Metal first (`CAMetalContextCreate`), GLES only when enabled; how the CPU renderer gets selected on a device without a GPU is not yet established.
- **IOSurface values ABI (offline RE, `/run/media/dipa/android/ios-sweet-work/iosurface-values.md`) - TODO in SweetSurface (0046 only accepts and drops values, so IOSurfaceCopyValue always returns NULL):** sel 9 SetValue in = {u32 id, u32 pad, IOCFSerialize([value, key]) or ([dict] = merge)}, sel 0xb RemoveValue in = {u32 id, u32 pad, key\0} (empty key = remove all); both out 4 bytes = new generation, client accepts only (gen+2)|1. Kernel after each change: gen = (gen+2)|1 (odd, never 0) -> state+0x20, serialized size (>=1) -> state+0x28. sel 0xa in {u32 id, u32 0, u8 0}, out {u32 ?, u32 gen, serialized non-bulk dict}; too small -> 0xe00002db + needed size (client retries x16). sel 0x1b in 0x88: bulk struct 0x70 @0, u64 valid size @0x70, u64 changed mask @0x78, u32 id @0x80 -> copy masked fields into state+0x30 (bit0 32B @0, bit1 16B @0x20, bit2 8B @0x30, bits3-10 1B @0x38..0x3f, bits11-14 24B @0x40/4B @0x58/8B @0x5c/1B @0x64).
- **IOMFB display matching confirmed (offline RE, `/run/media/dipa/android/ios-sweet-work/iomfb-match.md`):** the framework never matches on "AppleCLCD"; any registered IOMobileFramebuffer subclass works. `iomfb_match_callback` reads only `dfr`/`AppleTV`/`external` (CFBoolean, service itself, no parents); internal main display = all absent/false (type 0, name "primary"). Display count = /arm-io children whose device_type starts with "display-subsystem" (count 0 -> CreateDisplayList returns NULL; more nodes than services -> 30 s wait). IOMobileFramebufferOpen fails only on IOServiceOpen failure; canvas-width/height read as 4-byte CFData from the IOService-plane parent (optional). CopyProperty = IORegistryEntrySearchCFProperty on the service only. Patch 0046 (disp0 node, `external`=false, AppleCLCD : IOMobileFramebuffer) fits all of this.
- **IOMobileFramebuffer swap + notification ABI (2026-10-17, offline).** Swap record = fb+0x18, **0x46c bytes**, zeroed by SwapBegin and sent whole by `kern_SwapEnd` = IOConnectCallStructMethod(sel 5, rec, 0x46c, no output). **SwapBegin** = sel 4, no input, 1 scalar out = swap id (stored at rec+0x50). `kern_SwapSetLayer(fb, layer<4, IOSurfaceRef, CGRect a, CGRect b, flags)`: rec+0x54+4*layer = **IOSurface id**; rec+0x104 bit layer = layer enabled (rec+0x108 bit cleared); rect a (int x,y,w,h, scaled) at rec+0x64+16*layer, rect b at rec+0xc4+16*layer; rec+0x114+4*layer = transform from flags&0xf; flag bits 5/31/6/7/8/9 set bytes rec+0x2ca/0x2cc/0x2e3/0x2e5/0x2e6/0x467+layer. A NULL surface only sets the enable bit with zero id. SwapWait = sel 6 scalars [swap id, timeout, 0].
  - **Notifications**: `IOMobileFramebufferScheduleWithDispatchQueue`/`GetRunLoopSource` create an IONotificationPort and `IOConnectSetNotificationPort(conn, type, port, 0)` (kernel `registerNotificationPort`). `kern_EnableNotifications(fb, type, cb, ctx)` = **sel 72 scalars [NotifyCallbackFuncs[type], fb, type, 0]** -> the kernel keeps an async reference {port, func, refcon=fb} per type and fires it with `sendAsyncResult64`. Types: 0 HotPlug, 1 Power, 3 FrameInfo (args[0] = mask of 31-bit keys, args[1] u32, then values), 4 CRC, **5 Vsync**: 6 args, each a 32-bit half in a u64 slot: (ts lo, ts hi), (t2 lo, hi) passed through the time-ingest helper, (t3 lo, hi); the framework then calls the client's vsync callback(fb, ts, t2', t3, ctx). Vsync callbacks are dropped while the notification port has queued messages.

- **IOSurface.framework user-client ABI reversed (2026-10-17, offline, step 2 of the GUI plan).** `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image IOSurface` (selector scan script: scratchpad `selscan.py`).
  - Connect (`_iosConnectInitalize`, pthread_once): `IOServiceGetMatchingService(IOServiceNameMatching("IOCoreSurfaceRoot"))`, retried with sleep(1) up to 241 times (a missing service stalls every client ~4 min, no crash); `IOServiceOpen(type 0)`; then **sel 13** (no inputs, struct out 0x18 bytes = `_ioSurfaceLimits`) and requires u16 at out+0x16 == 1 (ABI version); out+0 = offset/plane-base alignment mask, out+4 = bytes-per-row alignment mask (used as (x+m)&~m), +0..+0x14 also copied into IOSurface remote-method init, otherwise logs and closes the connection.
  - **Create = sel 0**: scalar in [parent id or 0], struct in = `IOCFSerialize(props, kIOCFSerializeToBinary)` (OSUnserializeBinary format; keys IOSurfaceWidth/Height/BytesPerRow/BytesPerElement/PixelFormat/AllocSize/PlaneInfo/CacheMode/IsGlobal/...), struct out **0xa68 bytes** ("lock result"). **Lookup = sel 4**: scalar in [id], same struct out. LookupFromMachPort = sel 0x22 (scalar in [port name]); CreateMachPort = sel 0x23 (scalar in [id, options], scalar out [port name]).
  - Lock result (copied to client+0x68, size 0x68 + planes*0x80, min 0xe8): 0x00 u64 base address **mapped in the caller's task**; 0x08 u64 task VA of a per-surface shared state page (+0 u64 registry id, +8/+0xa u16 bound accel ids, +0xc u32 seed, +0x14 u8 tiled, +0x20 u32 value generation, +0x24 YCbCr matrix, +0x28 u32 non-bulk values size, +0x2d u8 displayable, +0x2e u8 sysmem-only, +0x30 0x70 bytes bulk attachments); 0x10 u64 task VA of timestamp page (4 u64 indexed timestamps, +0x20 detach code); 0x18 u32 surface id; 0x1c u32 alloc size; 0x20 width; 0x24 height; 0x28 bytes per row; 0x2c buffer offset; 0x30 pixel format; 0x34 u32 (dcache flush length); 0x38 plane count; 0x40 u16 bytes/element; 0x42 u8 element width; 0x43 u8 element height; 0x44 cache mode; 0x4c u8 subsampling; 0x4d u8 prohibit-use count; 0x50 parent id; 0x54 slice count; 0x58 u64 trace id; 0x60 u64 protection options; planes[n] at 0x68+n*0x80 (+0 width, +4 height, +8 bytes/row, +0xc offset, +0x14 size, +0x18 u16 bpe, +0x1a/+0x1b u8 element w/h, +0x1c u8 component count, +0x1d address format, +0x54 u8 compression type (non-zero -> no CPU base address)).
  - **Traps** (IOConnectTrapN -> `IOUserClient::getTargetAndTrapForIndex`): 0 IncrementUseCount(id, category), 1 DecrementUseCount(id, category), 2 **Lock**(id, options, &seed_out), 3 **Unlock**(id, options, &seed_out), 4 Release(id), 5 Release after a failed create/lookup setup (id).
  - Other selectors: 5 SetYCbCrMatrix, 6 WrapClientImage, 7 WrapClientMemory, 9 SetValue (serialized), 0xa UpdateNonBulkValueCache (struct in 9), 0xb RemoveValue, 0xc BindAccel, 0x11 SetSurfaceNotify (async), 0x12 RemoveSurfaceNotify, 0x13 Log, 0x14 SetPurgeable, 0x15 SetOwnership, 0x16 SetTiled, 0x18 SetTimestamp, 0x19 GetTileFormat, 0x1a CopyDataValueBytes, 0x1b SetBulkAttachments (struct 0x88), 0x1e/0x2e-0x31 (via outlined helpers), 0x1f SetCompressedTileDataRegionMemoryUsed, 0x20 GetGraphicsCommPageAddress, 0x21 SetIndexedTimestamp, 0x24-0x26/0x2d IOSurfaceAccelerator, 0x27-0x29 shared events, 0x2a SignalEvent, 0x2b Set/InitDetachModeCode, 0x2c SetOwnershipIdentity, 0x32-0x34 data properties.
  - => SWEET kernel service: class/name **IOCoreSurfaceRoot** + IOSurfaceRootUserClient; minimum = sel 13, 0, 4, 9/0xa/0xb (values), traps 0-5, 0x22/0x23 later for cross-process surfaces (CA sends surfaces to backboardd by mach port). Memory: IOBufferMemoryDescriptor per surface, mapped into each task with createMappingInTask; ids from a global table.

- **Image v8: daemon logs on the console.** `tools/patch-launchd-plist.py IN OUT LABEL...` rewrites launchd.plist (binary, plistlib): renames every `_PanicOnCrash` to `_PanicOnCrasX` and gives the listed jobs `StandardOutPath`/`StandardErrorPath` `/dev/console` (devfs console is 0622, writable by mobile) plus `OS_ACTIVITY_DT_MODE=YES` (os_log mirrored to stderr) and `CFLOG_FORCE_STDERR=1`. v8 = v7 + that plist for com.apple.backboardd and com.apple.SpringBoard, written through the mount (796310 bytes, root:80 0644), SHA256 in `ios16-root-20H392-v8.img.sha256`, `.simg` ready; boot with `shim/out/shim-boot-0045n.img`. Goal: see backboardd's uncaught exception text and the window server's display probing. ODDITY (unresolved): the v5 raw-image rename at offset 0x1e025f000 is present in the v5-v7 image bytes, but the plist read through a Linux mount of v7 still had the original `_PanicOnCrash` keys; filefrag shows the file as one extent. Never patch file contents through raw offsets again; write through the mount.
- **IOMobileFramebuffer.framework reversed (step 1 of the GUI plan).** `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image IOMobileFramebuffer`.
  - Display discovery (`IOMobileFramebufferCreateDisplayList` -> `getDisplayListNumber`): iterates children of `IODeviceTree:/arm-io`, counts nodes whose `device_type` is "display-subsystem" or "ext-display-subsystem" (or `compatible` "paravirtualizedgraphics,gpu"); then `iomfb_populate_display_infos` adds an IOServiceAddMatchingNotification(IOServiceFirstMatch, **IOServiceMatching("IOMobileFramebuffer")**) and runs the run loop up to 30 s until that many services matched. `iomfb_match_callback` reads service properties `dfr`, `AppleTV`, `external` (CFBoolean) and names displays (external ones "%s-%d").
  - `IOMobileFramebufferOpen(service, task, type, &fb)`: `__ioMobileFramebufferAlloc` (CFPreferences com.apple.iokit.IOMobileGraphicsFamily: canvas_width/height, override_display_width/height, override_panel_width/height, benchmark, enable_ktrace), `IOServiceOpen(service, task, type)` (GetMainDisplay passes type 0), `getEDTProperty` reads `canvas-width`/`canvas-height` from the service's IODeviceTree parent, then fills a table of `kern_*` function pointers.
  - User client selectors (`kern_*` -> IOConnectCall*): 3 _kern_GetLayerDefaultSurface[ScalarMethod_stub]; 4 _kern_SwapBegin[ScalarMethod_stub]; 5 _kern_SwapEnd[StructMethod_stub]; 6 _kern_SwapWaitWithTimeout[ScalarMethod_stub] _kern_SwapWait[ScalarMethod_stub]; 7 _kern_GetID[ScalarMethod_stub]; 8 _kern_GetDisplaySize[ScalarMethod_stub]; 10 _kern_SetTVOutMode[ScalarMethod_stub]; 11 _kern_SetWSSInfo[ScalarMethod_stub]; 12 _kern_RequestPowerChange[ScalarMethod_stub]; 13 _kern_EnableStatistics[ScalarMethod_stub]; 14 _kern_SetFlags[ScalarMethod_stub]; 15 _kern_SetDebugFlags[ScalarMethod_stub]; 16 _kern_SetTVOutSignalType[ScalarMethod_stub]; 17 _kern_SetGammaTable[StructMethod_stub]; 18 _IOMobileFramebufferIsMainDisplay[ScalarMethod_stub]; 19 _kern_SetWhiteOnBlackMode[ScalarMethod_stub]; 20 _kern_SwapSignal[ScalarMethod_stub]; 22 _kern_SetDisplayDevice[ScalarMethod_stub]; 23 _kern_SetDigitalOutMode[ScalarMethod_stub]; 24 _kern_GetHDCPDownstreamState[StructMethod_stub]; 25 _kern_GetDigitalOutState[ScalarMethod_stub]; 26 _kern_printDisplayRegs[ScalarMethod_stub]; 27 _kern_GetGammaTable[StructMethod_stub]; 28 _kern_GetDotPitch[ScalarMethod_stub]; 29 _kern_GetDisplayArea[StructMethod_stub]; 30 _kern_EnableDisableDithering[ScalarMethod_stub]; 31 _kern_SetUnderrunColor[ScalarMethod_stub]; 33 _kern_EnableDisableVideoPowerSavings[ScalarMethod_stub]; 34 _kern_SetVideoDACGain[ScalarMethod_stub]; 35 _kern_SetLine21Data[ScalarMethod_stub]; 41 _IOMobileFramebufferEnableMirroring[ScalarMethod_stub]; 42 _IOMobileFrameBufferGetMirroringCapability[StructMethod_stub]; 43 _IOMobileFramebufferSetRenderingAngle[StructMethod_stub]; 44 _IOMobileFramebufferSetMirrorContentRegion[StructMethod_stub]; 47 _kern_HDCPSendRequest[ScalarMethod_stub]; 48 _kern_HDCPGetReply[ScalarMethod_stub]; 49 _kern_SurfaceIsReplaceable[ScalarMethod_stub]; 50 _kern_SetBrightnessCorrection[ScalarMethod_stub]; 51 _kern_SetColorRemapMode[ScalarMethod_stub]; 52 _kern_SwapCancel[ScalarMethod_stub]; 53 _IOMobileFramebufferSetCanvasSize[ScalarMethod_stub]; 54 _IOMobileFramebufferGetMatrix[Method_stub]; 55 _IOMobileFramebufferSetMatrix[Method_stub]; 56 _IOMobileFramebufferKernelTests[StructMethod_stub]; 57 _kern_GetColorRemapMode[ScalarMethod_stub]; 66 _kern_SPLCSetBrightness[ScalarMethod_stub]; 67 _kern_SPLCGetBrightness[ScalarMethod_stub]; 68 _kern_SetParameter[Method_stub]; 69 _kern_SwapSetColorMatrix[StructMethod_stub]; 72 _kern_DisableNotifications[ScalarMethod_stub] _kern_EnableNotifications[ScalarMethod_stub]; 73 _kern_ChangeFrameInfo[ScalarMethod_stub]; 74 _kern_SupportedFrameInfo[Method_stub]; 75 _kern_FactoryPortal[StructMethod_stub]; 77 _kern_GetCurrentAbsoluteTime[ScalarMethod_stub]; 78 _kern_SetBlock[Method_stub]; 79 _kern_GetBlock[Method_stub]; 80 _kern_GetLinkQuality[ScalarMethod_stub]; 81 _kern_SwapCancelAll[ScalarMethod_stub]; 82 _kern_GetProtectionOptions[ScalarMethod_stub]; 83 _kern_CopyLayerDisplayedSurface[ScalarMethod_stub]; 84 _kern_SetContrast[StructMethod_stub]; 85 _kern_GetBufBlock[Method_stub]; 86 _IOMFBGainEncoderFinishEncoding[Method_stub]; UNKNOWN {}
  - => Kernel needs: an `/arm-io` child node with device_type "display-subsystem" (+ canvas-width/height) and an IOService of class **IOMobileFramebuffer** (subclass name match) attached under it, opened with type 0, implementing at least GetDisplaySize(8), IsMainDisplay(18), GetID(7), GetLayerDefaultSurface(3), SwapBegin(4)/SwapEnd(5 struct)/SwapWait(6), Enable/DisableNotifications(72), frame info(73/74), CopyLayerDisplayedSurface(83), plus IOSurface for the swapped buffers. NEXT: CoreAnimation's AppleDisplay/IOMFBDisplay usage of these (constructor, first swap, vsync), then IOSurface.framework's IOSurfaceRoot selectors.
- **GUI PLAN (decided 2026-10-16): CPU rendering + kernel IOMobileFramebuffer.** User: CPU rendering is fine for now (goal = prove the GUI runs), may later research an Apple-style GPU driver for the Adreno 618 -> keep display output and renderer separable. QuartzCore findings (symbols from the cache's .symbols subcache: `ipsw dyld symaddr research/dsc/dyld_shared_cache_arm64 --image QuartzCore`; strings: `ipsw dyld macho ... QuartzCore --strings`):
  - Renderers: `sw_new_context` (CA::OGL::SWContext, full CPU renderer incl. blur, images, IOSurface-backed surfaces, make_buffer_current(WindowServer::Surface)), `gles_new_context`, `metal_new_context`, `new_null_context`. Env knobs include CA_ENABLE_OGL, CA_DISPLAY_TYPE, CA_FORCE_LOCAL_SERVER, CA_FORCE_TIMER, CA_CANVAS_WIDTH/HEIGHT, CA_MAIN_DISPLAY_SCALE/ORIENTATION, CA_DISABLE_RENDER, CA_ALWAYS_RENDER (selection logic not traced yet).
  - `-[CAWindowServer _detectDisplays]` (0x1884e3150) iterates static tables `open_funcs`/`count_funcs` = AppleCLCDDisplay, AppleRGBOutDisplay, AppleWirelessDisplay only; each `count()` = `display_count_of_type(IOMFBDisplayType)` over **`IOMobileFramebufferCreateDisplayList()`**, `open(i)` = `create_display_of_type` -> `CA::WindowServer::AppleDisplay(CFString name, ..., IOMFBDisplayType)` (an IOMFBDisplay driven by IOMFBServer: vsync_callback, frame_info_callback, swaps). `CA::WindowServer::VirtualDisplay` exists but is never auto-detected (explicit API only). => the main display MUST come from an IOMobileFramebuffer kernel service.
  - NEXT: reverse IOMobileFramebuffer.framework: IOMobileFramebufferCreateDisplayList / IOMobileFramebufferOpen (service matching, display type property), the user-client selectors used by IOMFBServer (display size/modes, swap begin/set-layer/end with IOSurface ids, vsync notifications), then the IOSurface kernel side (IOSurfaceRoot user client: create/lookup/lock, memory mapping) that both IOMFB swaps and CA surfaces need. Kernel: SWEET IOMobileFramebuffer service that copies the swapped IOSurface to the 1080x2400 framebuffer at 0x9c000000.
- **logd fixed; backboardd = DISPLAY WALL (boot 0045n on v7, 21:08:57 UTC, frames VID_20261016_230850 00:55-01:28, slide 0x4c3ec000, `research/crash-0045n-v7.txt`).** No `CRASH logd` any more; new daemons run (runningboardd, WirelessRadioManagerd, networkserviceproxy, dasd, cloudpaird, magicswitchd, atc, askpermissiond, ManagedSettingsAgent, containermanagerd uses AKS 0/17). New: locationd sig 11 at CFRunLoopAddSource+132 (NULL source) from its main+0x2af138 in a dispatch_once. **backboardd** sig 6: abort <- std::terminate <- _objc_terminate <- dispatch_once <- main+0x9334 = the "start %{public}@..." wrapper (0x100009264) for **"window server"** (block 0x10000a770 -> dispatch_once body 0x10001d7f0 = StartWindowServer): MTLSetShaderCachePath, `[CAWindowServer serverWithOptions:@{kCAWindowServerDisableUpdatesOnMainDisplay: @YES}]`, `setRendererFlags:`, **`mainDisplay`** -> `displayId` -> `[CADisplay displayWithDisplayId:]`, setWindowServerDisplay:, bounds, pointScale, currentMode, preferredScale, nativeOrientation, touchScales... With no display, mainDisplay is nil and it throws. (HID system and GS start fine before it.) SpringBoard dies on the same missing display (FBSDisplayMonitor). QuartzCore display backends (from the cache's .symbols subcache, `ipsw dyld str`): `CA::WindowServer::AppleCLCDDisplay` (IOMobileFramebuffer hardware) and **`CA::WindowServer::VirtualDisplay`** (open(), new_server, clone_update). `CA_DISPLAY_TYPE` and `CA_FORCE_LOCAL_SERVER` strings exist in QuartzCore but have no direct xref (likely read via a table of CA_* env names). NEXT: find how CA::WindowServer::Server enumerates displays (IOMobileFramebuffer services vs VirtualDisplay / CA_DISPLAY_TYPE) and choose: (a) kernel IOMobileFramebuffer service + IOSurface, or (b) a VirtualDisplay rendered by CA's CPU path (CA::OGL::SW) and copied to the framebuffer.
- **watchdogd + CommCenter + identityservicesd fixed (boot 0045n on v6, 20:41:30 UTC, frames VID_20261016_224133 00:39-01:13, slide 0x20fec000).** `SWEET WDT: watchdogd sel 0 in 0/0 out 1/0 -> ok`, no watchdogd/CommCenter/identityservicesd crashes (CommCenter/identityservicesd now call AKS 0/17/7). usermanagerd boot task runs (UMD_INIT creates primary session uid 501 "mobile", personas 99 System and 199 Default registered with the kernel); `Failed to atomically swap writing file /private/var//keybags/usersession.kb: Operation not supported (45)` (HFS lacks the atomic swap rename; non-fatal so far). Remaining: SpringBoard (FBSDisplayMonitor, no display), backboardd (uncaught exception), mediaserverd, lockdownd (crash inside obfuscated code, main+0x4c1b4?), logd. **logd**: main+0x18160 = `BUG IN LIBTRACE: Failed to migrate directory` from 0x10001801c(dir 0): 0x100016538 requires the dir to exist with uid/gid/mode from the table at 0x10002df14 (0: /private/var/db/diagnostics 272:272 0751, 1: /private/var/db/uuidtext 272:272 0755, 2: /private/var/db/diagnostics/timesync 272:272 0755) plus a `.migrated` file, else XPC "operation 6" to a helper and crash. **Image v7** = v6 + those dirs/markers (also in mkiosroot.sh), SHA256 in `ios16-root-20H392-v7.img.sha256`, `.simg` ready. Boot with `shim/out/shim-boot-0045n.img`.
- **Exit-reason text works (boot 0044n, frames VID_20261016_221726 00:32-00:53, slide 0x22d5c000).** CommCenter/identityservicesd: `couldn't dlopen libobjc-trampolines.dylib: dlopen(/usr/lib/libobjc-trampolines.dylib, 0x0106): tried: '/usr/lib/libobjc-trampolines.dylib' (no such file, not in dyld cache)`: the file only exists in the OS cryptex and dyld does not search cryptexes at ignition_level=1. 19 cryptex files have no root counterpart (OS: DriverKit dyld cache + .symbols, VisualTestKit, libffi-trampolines, libobjc-trampolines, libstdc++ 6.0.9 + 2 symlinks; App: SafariSupport.bundle, AuthenticationServicesAgent, com.apple.Safari.History, passwordbreachd, safarifetcherd, webbookmarksd, webinspectord, webpushd) -> hard-linked (symlinks copied: Linux hfsplus cannot hard-link symlinks); `tools/mkiosroot.sh` does it generically. watchdogd still crashed: the `-wdt_no_pmu` / `/arm-io/wdt no-pmu` check (0x10000a01c) only runs when os_variant_has_internal_diagnostics; otherwise 0x100009b18 looks up IOServiceMatching("IOService") + IOPropertyMatch {IOPMUPrimary: 1} (NSConstantDictionary) and crashes if absent, then IORegistryEntrySetCFProperty(boot stage 0xff) (failure only logged). Patch **0045**: IOWatchdog sets IOPMUPrimary = 1 and accepts setProperties; CI `38961814194` -> `xnu/out-0045`. **Image v6** = v5 + the 19 cryptex links, SHA256 7c610c6a...297a, `ios16-root-20H392-v6.simg` (full flash needed). Boot image `shim/out/shim-boot-0045n.img` (0045 + sweet-0044hi.adt). Still open: SpringBoard (no display), backboardd (uncaught exception, reason none), mediaserverd (Fig NULL dict), logd (main+0x18160), lockdownd (main+0x4c1b4).
- **System survives daemon crashes (boot 0043p, video frames VID_20261016_215006 00:25/00:42/00:54).** With the v5 plist patch no panic: launchd keeps launching (runningboardd, powerd, tccd, healthd, peopled, cryptexd, routined, aksd, mediaremoted, familynotificationd, nanoprefsyncd, navd, cloudpaird, passd, amsaccountsd, accessoryupdaterd, wcd ...). Shared cache slide that boot 0x406b4000 (`research/crash-0043p.txt`). watchdogd still crashed at main+0xb82c from the PMU step: `-wdt_no_pmu` is only honored when os_variant_has_internal_diagnostics; customer builds read `no-pmu` from IODeviceTree:/arm-io/wdt (IORegistryEntrySearchCFProperty) -> mkadt adds /arm-io/wdt {no-pmu}. **cryptexd** sig 6: abort_with_payload <- os_crash_fmt <- `__darwin_arm_secure_boot_chip.cold.7` ("panic: failed to get crypto algorithm") <- `_img4_chip_select_personalized_ap`: reads /chosen use-ddi-secure-boot, allow-ecid-mismatch, crypto-hash-method ("sha1" | "sha2-384"), /chosen/asmb lp-smb0/1 -> mkadt adds crypto-hash-method "sha2-384". **identityservicesd** sig 6 = same `_objc_fatal` in imp_implementationWithBlock as CommCenter (block trampolines; message needed). lockdownd sig 5 at main+0x4c1b4. Patch **0044**: crash report adds `CRASH name reason ns N code X: text` (exit reason description = abort_with_payload text). CI `38959096811` -> `xnu/out-0044`, QEMU OK. Image `shim/out/shim-boot-0044n.img` = 0044 + `shim/out/sweet-0044hi.adt` (wdt no-pmu, crypto-hash-method) + cmdline (wdt=-1, no -wdt_no_pmu).
- **First symbolicated daemon crashes (boot 0043u on journaled v4, 19:38:53 UTC; video frame chat image 36; transcribed in `research/crash-0043u.txt`; shared cache slide 0x40d60000 for every process).** (Gemini's video transcription invented values, use frames only.) `tools/sweet-crash.py research/dsc/dyld_shared_cache_arm64 x research/crash-0043u.txt`:
  - **watchdogd** pc main+0xb82c (`_os_crash` trap helper) from 0x100009bb0 = set PMU boot stage via IORegistry (no PMU) -> fix: boot-args `wdt=-1 -wdt_no_pmu` (Apple's own test mode writes "%s wdt=-1 -wdt_no_pmu"; watchdogd logs "skipping setting PMU boot stage because -wdt_no_pmu found in boot-args or no-pmu found in EDT"). Kernel effect of wdt=-1: sched hygiene debug off, zone_exhausted_timeout -1 (harmless).
  - **SpringBoard** sig 5 in `-[FBSDisplayMonitor _initWithBookendObserver:transformer:]+1160` <- `+[FBDisplayManager sharedInstance]` <- `FBSystemShellInitialize` <- `SBSystemAppMain`: no main display. GUI WALL: display service (IOMobileFramebuffer / CoreAnimation display).
  - **backboardd** sig 6: abort <- std::terminate <- _objc_terminate (uncaught exception; message not shown).
  - **CommCenter** sig 6: `_objc_fatal` in `imp_implementationWithBlock` (from `+[_PFRoutines initialize]`, CoreData): objc block trampolines cannot be allocated -> likely kernel gap (trampoline page remap); will hit many processes.
  - **mediaserverd** sig 11: CFDictionaryGetValue(NULL) in FigNotificationListenerStartNotifications (routing session manager).
  - **logd** sig 5 at its main+0x18160 (bt main+0x9dfc); lockdownd sig 5 (not yet symbolicated).
  - Image v5 = v4 + launchd.plist (`/System/Library/xpc/launchd.plist`, contiguous at image offset 0x1e025f000, 1456980 bytes) with the 7 `_PanicOnCrash` keys renamed `_PanicOnCrasX` in place (raw image patch, no metadata change) -> `diff-v4-v5.simg` = 7 blocks; v5 SHA256 78f96974...185a. Boot image `shim/out/shim-boot-0043p.img` = 0043 + `wdt=-1 -wdt_no_pmu`.
- **0043 console crash reports (QEMU verified); image `shim/out/shim-boot-0043u.img`; NEXT: flash v4 + boot 0043u.** Boot 0042u (video photo IMG_20261016_211646): short exec lines work; watchdogd crashes with NO `SWEET WDT:` line (fails before its first IOWatchdog call: lookup or IOServiceOpen, or elsewhere); lockdownd, SpringBoard, mediaserverd, CommCenter, identityservicesd, backboardd, watchdogd all crash repeatedly ("Too many corpses being created" = corpse limit, no crash reports). AKS callers: lockdownd (0, 7), imagent/sleepd (0, 17, 7), backboardd (0, 17), UserEventAgent (0, 7, 102, 103 -> unsupported). Patch **0043**: `sweet_crash_log` (osfmk/kern/exception.c, called from proc_prepareexit for crash exits on the current proc) prints `CRASH name[pid] sig S code (exc<<32|code) sub X slide SR mh MH`, `CRASH name pc lr sp fp far`, `CRASH name bt` (10 fp frames); AKS sel 103 notify_state_is_set -> 0. CI `38954520419` -> `xnu/out-0043`. QEMU test: init forks, child `brk #1`: `CRASH launchd[2] sig 5 code 0x600000001 sub 0x1000004f0 slide 0x0`, pc correct, parent keeps running (pid 1 crashes panic before proc_prepareexit, so test with a child). `tools/sweet-crash.py CACHE ROOT [report]` symbolicates typed-in CRASH lines (`ipsw dyld a2s` with addr - slide; main binary offsets = addr - mh + 0x100000000).
- **v3 reflashed 18:50-18:55 UTC (`research/flash-userdata-v3b.log`, 24/24 OKAY); 0042u booted 19:01:17 UTC on the clean disk (`research/boot0042u-2.log`), SLOW MO VID pending (watchdogd failure reason).** ABL hangs on the first `fastboot boot` after every big flash; user re-enters fastboot. User gave standing approval to reflash/boot without asking (memory standing-flash-approval). Commits now end with `Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>` (user request; never a Claude trailer).
  - watchdogd flow: connect_to_kext (0x100005408): IOServiceGetMatchingService("IOWatchdog") fails -> "failed to discover" + _os_crash; IOServiceOpen(type 1) fails -> "IOServiceOpen failed with error : 0x%x" + _os_crash. Then sel 0 (out 1 scalar): 0 -> "userspace monitoring disabled", 0x100009bb0, sleep 2, exit(0); nonzero -> "userspace monitoring enabled", normal operation.
  - **Image v4 (JOURNALED):** `tools/mkiosroot.sh` now does `mkfs.hfsplus -J`, mounts with `-o loop,force`, adds the /System/Library/Caches/com.apple.dyld hard links (44) and /private/var/keybags/systembag.kb (0700/0600 root). Built from the IPSW dmgs (OS 087-86698-044, SystemOS 087-86593-047, AppOS 087-86715-047) -> `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v4.img` SHA256 fba4aeae...ec0f9 + `.simg`. Header: attr 0x80002100 (unmounted, journaled), journal info block 129, journal 8 MiB at 532480 flagged need-init. Linux `fsck.hfsplus -n` reports "Volume header needs minor repair" on any fresh journaled volume even after -y (quirk). QEMU: 64 MB journaled test root (`rd=md0 rootrw=1`, 0042 kernel): `hfs: Initializing the journal (joffset 0x3000 sz 0x80000)` -> mounted -> init runs, no panic. NEXT: flash v4 (full), boot 0042u.
- **SPRINGBOARD STARTS (boot 0041u 18:39:53 UTC, photo IMG_20261016_204142).** backboardd, bluetoothd, fairplayd.H2, SpringBoard (pid 111, crashed: "Too many corpses"), xpcproxy run; `SWEET AKS: backboardd sel 0 -> 0`, `sel 17 in 1/0 out 0/4096 -> 0`. Panic: watchdogd[113] exited, namespace 2 subcode 5 (SIGTRAP = its _os_crash): watchdogd crashes on purpose when IOServiceGetMatchingService("IOWatchdog") fails ("connect_to_kext() unexpectedly failed") or IOServiceOpen(type 1) fails ("IOServiceOpen failed with error"); IOWatchdog DOES register (QEMU msgbuf: `SWEET WDT: IOWatchdog registered`), cause not on screen yet -> need video. Note `wdt_test=no_user_checkin` makes watchdogd log "detected testing boot-args to not run, exiting" (path not reached here?). **HFS CORRUPTION:** `hfs_swap_BTNode: offset #0 invalid (0xBC1D) (blockSize 0x2000 numRecords 81)`, `node=19677 fileID=4` (catalog), `hfs_mark_inconsistent` repeated. Likely cause: non-journaled HFS + unclean panics while writing (0038e wrote a 2.7 GB core then panicked; every boot since ends in panic). Plan: reflash v3 (clean), longer term HFS journaling. Patch **0042**: one short `exec NAME` console line instead of the 3-line "Allowing spawn of iOS binary" notice; CI `38949451276` -> `xnu/out-0042`; image `shim/out/shim-boot-0042u.img` (same cmdline as 0041u).
- **LAUNCHD DAEMONS RUN (boot 0039u, 18:29:13 UTC, photo IMG_20261016_203052).** With `launchd_unsecure_cache=1` all boot tasks pass and launchd starts LaunchDaemons (pids > 100: identityservicesd 114 and lockdownd 106 crash, "Too many corpses being created"). Fatal: `watchdogd: ERROR: failed to discover watchdog KEXT service` -> exit (namespace 2 = signal, subcode 5) -> `_PanicOnCrash` panic. watchdogd: IOServiceMatching("IOWatchdog"), IOServiceOpen type 1, IOConnectCallScalarMethod sel 0/1 (check in)/3/4 (disable/enable userspace monitoring)/5, IOConnectCallStructMethod sel 2 (1 KiB in) and 9 (small out); VM path ("detected virtual machine environment and no watchdog KEXT found, exiting...") only when it detects a VM. Console also flooded with `hfs: set hardlink bit on vol=iOSRoot cnid=321318` (Linux-made hard links lack kHFSHasLinkChainBit; repaired only in memory on each lookup). Patches **0040** (`iokit/Kernel/arm/SweetWatchdog.cpp`: IOWatchdog + IOWatchdogUserClient, all selectors succeed with zeroed outputs, each logged once as `SWEET WDT: ...`) and **0041** (sweet_hfs hfs_catalog.c: the three link-repair printfs limited to 8 per boot); CI `38948245372` PASSED -> `xnu/out-0041` (QEMU regression OK). Image `shim/out/shim-boot-0041u.img` (0041 + sweet-0038hi.adt + ufsrw rootrw + launchd_unsecure_cache=1) booted 18:39:53 UTC (`research/boot0041u.log`), photo pending.
- **v3 FLASHED + keybag passes; next wall AMFI (launchd_cache_loader).** User said "flash, nothing to lose": `fastboot -S 512M flash userdata ios16-root-20H392-v3.simg` 17:51:52-17:57:01 UTC, 24/24 chunks OKAY (`research/flash-userdata-v3.log`). userdata = v3 (SHA256 fa84274c...22df) until the next rw boot. ABL fastboot then hung ("Sending" forever, `reboot bootloader` timed out) until the user re-entered fastboot by hand (common Xiaomi ABL crash). Boot 0038e 18:06:15 UTC (`research/boot0038e-3.log`), photos IMG_20261016_200812/200838: keybag OK, then boot tasks tzinit (`Bad zoneinfo link`, resets to system partition, OK), finish-restore, finish-demo-restore, sysstatuscheck, prng_seedctl (`failed to load kernel prng seed`, `failed to load virtual random (-147)`, AllowCrash), auearlyboot, then **launchd_cache_loader: `abort: Unable to find AMFI`** -> 2.7 GB core written to /cores in 91 s -> launchd panic `launchd_cache_loader[22] exited -- exit reason namespace 7 subcode 0x1`. launchd_cache_loader: SecStaticCodeCheckValidity of /System/Library/xpc/launchd.plist with detached .sig, then IOServiceMatching("AppleMobileFileIntegrity") + IOServiceOpen type 0 + IOConnectCallMethod sel 6 (1 scalar = 1, struct in = cdhash) = trust the cdhash, then sends the cache to launchd. Boot-arg `launchd_unsecure_cache=1` skips signature + AMFI ("Using unsecure cache"); the value is parsed with strtoul base 10 (helper 0x100002da8), so a path = 0 = off: boot 0039w (18:17:13 UTC, `=/System/Library/xpc/launchd.plist`, photo IMG_20261016_202008) still aborted `Unable to find AMFI` (core dump now skipped: `Failed to generate core file ... error 14`, so 0039 works). Image `shim/out/shim-boot-0039u.img` uses `launchd_unsecure_cache=1`. Patch **0039**: do_coredump = 0 on SWEET (kern.coredump sysctl still works); CI `38945267341` -> `xnu/out-0039` (_do_coredump now in __common). Image `shim/out/shim-boot-0039w.img` = 0039 + sweet-0038hi.adt + ufsrw rootrw + `launchd_unsecure_cache=/System/Library/xpc/launchd.plist`. TODO later: real AppleMobileFileIntegrity IOService (sel 6 trust cdhash etc.).
- **Image v3 ready, awaiting user "flash".** Boot 0038w 17:38:10 UTC, photo (chat image 30): `SWEET AKS: keybagd sel 0 -> 0x0`, `sel 14 out 1/0 -> 0x0` (x2), `MKB_INIT: mount is with NO CPROTECT ... does not support Data Protection`, `System Keybag loaded`; then keybagd still opens /private/var/keybags/systembag.kb (0x100012d3c, for BackupKeyBagKeys) -> ENOENT, `no-effaceable-storage` not set -> `Can't load the keybag. tears in rain... Time to die` -> `FATAL KEYBAG ERROR: kb_load` -> reboot -> Halt/Restart Timed Out panic. Fix: ADT `/defaults/no-effaceable-storage` (mkadt.py; `shim/out/sweet-0038hi.adt`, `research/q0038-hi.adt`, QEMU OK) + file `/private/var/keybags/systembag.kb` (0600 root, dir 0700; XML plist {KeyBagKeys: data}). **v3 image** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v3.img` = v2 + that file, fsck OK, SHA256 fa84274c...22df; `ios16-root-20H392-v3.simg` (img2simg: zero blocks become FILL chunks, so a full flash overwrites every block the rw boots changed). Flash: `fastboot -S 512M flash userdata ios16-root-20H392-v3.simg`, then boot `shim/out/shim-boot-0038e.img` (0038 + sweet-0038hi.adt + ufsrw=1 rootrw=1).
- **0038 AppleKeyStore (CI 38938744130 PASSED -> `xnu/out-0038`; QEMU msgbuf: `SWEET AKS: AppleKeyStore registered (no passcode, unlocked)`; phone image `shim/out/shim-boot-0038w.img` booted 17:38:10 UTC, `research/boot0038w.log`, photo pending).** QEMU full kernel log trick (fb.txt is only the last screen): `llvm-nm KERNEL | grep _msgbufp` -> `--hmp "x /1gx ADDR"` gives struct msgbuf address (static, 0xfffffe0007b56678 on 0038) -> `--hmp "x /3gx STRUCT"` gives bufc (3rd quad) -> `--hmp "memsave BUFC 0x20000 \"FILE\""` (quote the file name), `tr -d "\0" < FILE`. Same inputs give the same addresses. Boot 0037w 17:08:35 UTC, photo IMG_20261016_191052: bindfs fixed mount; mount-phase-1/2 pass; `cc_fips_test` boot task: all corecrypto FIPS POST tests PASSED in userspace; then keybag boot task `keybagd --init`: `MKB_INIT: FATAL KEYBAG ERROR: failed to load system bag`, `Could not get options entry from the device tree` (x2, IODeviceTree:/options = NVRAM, non-fatal), `REBOOTING INTO RECOVERY MODE` -> reboot -> panic `Halt/Restart Timed Out @IOPlatformExpert.cpp:884` (no restart path on SWEET).
  - keybagd (static libaks): load_devicebag (0x100012c30) calls aks_get_system (sel 14, 0-1 scalar in, 1 out) and loads /private/var/keybags/systembag.kb only if the result is 0xe00002f0 (NotFound); any other error -> fatal; SUCCESS -> skip the file (userspace-reboot path). systembag.kb is a plist {KeyBagKeys}; decrypted with effaceable storage unless IODeviceTree:/defaults has `no-effaceable-storage`. Image has no /private/var/keybags. has_data_protection: statfs of /private/var; no cprotect on our HFS -> device-bag volume mapping skipped.
  - libaks connection: IORegistryEntryFromPath / IOServiceMatching("AppleKeyStore"), IOServiceOpen type 0, then selector 0 (no args) must succeed.
  - **AppleKeyStore selector map (iOS 16.7 AppleKeyStore.framework, via `ipsw dyld disass`):** 0 open, 2 create_bag, 3 save_bag, 4 unload_bag, 5 set_system_with_passcode, 6 load_bag, 7 get_lock_state (1 in, 1 out; bit 2 = unlocked since boot), 9 unlock_device, 10 wrap_key, 11 unwrap_key, 12 unlock_bag/test_stashed_kek, 13 lock_bag, 14 get_system, 15 change_secret_se, 16 internal_state, 17 get_device_state (1 in, struct out <=4K: DER SET of SEQUENCE{UTF8 key, INTEGER}: ss state, sls lock_state (MKBGetDeviceLockState returns it), sb backoff, sbo, sfa failed attempts, sfao, sgs generation, sas assertions, sgpe grace, srcd countdown, sr reserved), 18 recover_with_escrow_bag, 19 obliterate_class_d, 20 drain_backup_keys, 21/22 set/clear_backup_bag, 23 get_bag_uuid, 24 rewrap_key_for_backup, 26/27 assert_hold/drop, 30/31 set/get_configuration, 33-37 stash, 38 auth_token_create, 40 generation, 41 fdr_hmac_data, 42 verify_password, 43 _aks_operation, 44 remote_session, 49/50 signing keys, 51/53/54 stash enable/persist/escrow, 55 unload_session_bags, 57/58 remote peer, 59-62 smartcard, 63 migrate_s_key, 65 set_keybag_for_volume_with_cookie_persona, 66/67 sync bags, 68 copy_volume_cookie_persona, 69-73 se_*, 74-79/84/85/99 fv_* (FileVault/VEK), 80/81 absinthe, 82 make_public_backup_bag, 83 drop_auxiliary_auth, 86 kext get/set options, 87 invalidate_bag, 88-91/94/95/106/107 se_*, 92 lower_iteration_count, 93 get_internal_info, 96-98/117 keybag_persona create/list/delete, 100/101 assert promote/consume, 102/103 notify_state set/is_set, 104 fairplay, 105 change_secret_epilogue, 108 delete_xart_leak, 110-113 backup volume, 114 sealed_hashes_set, 115 prewarm_sps, 118-128 AKSIdentity*/AKSGetLastUser.
  - Patch **0038**: `iokit/Kernel/arm/SweetKeyStore.cpp` (class AppleKeyStore on IOResources, IOUserClientClass AppleKeyStoreUserClient; personality in KernelConfigTables.cpp): 0/4/5/9/12/13/26/27/55/102 success, 14 -> handle 0, 7 -> 0x4, 17 -> DER state (ss 4, sls 0), others kIOReturnUnsupported; logs `SWEET AKS: <proc> sel N arg0 in a/b out c/d -> ret` for every call.
  - Tools: shared cache copied to `research/dsc/` (git-ignored; ipsw needs a writable dir for its .a2s symbol cache); `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image AppleKeyStore|MobileKeyBag`; scratchpad `annot2.py BIN DIS FIXUPS` annotates llvm-objdump output with strings and import names (`ipsw macho info --fixups`).
- **0037 bindfs (phone boot 0037w pending photo).** Boot 0036mw 16:57:47 UTC, photo IMG_20261016_190102: WRITABLE ROOT WORKS: `/dev/ufsroot0 on / (hfs, local, noatime)` after mount_hfs update (GetMasterBlock EBUSY harmless), coredump of mount written to /cores (rw). Then `mount: failed to mount /private/preboot/5CBE3601.../usr/standalone/firmware -> ...: Operation not supported by device (19)` -> exit 74 -> launchd panic `mount[2] exited`. Cause: the macOS-derived config had nullfs but no bindfs (ENODEV = unknown fs type). Caller passes env 0 -> bind not required, ENOENT (missing source dir) tolerated. Patch **0037** adds `bindfs` to FILESYS_BASE in MASTER.arm64.sweet; CI `38936550483` -> `xnu/out-0037` (bindfs_vfsops present; QEMU probe regression OK). Image `shim/out/shim-boot-0037w.img` (0037 + sweet-0036hi.adt + ufsrw=1 rootrw=1) booted 17:08:35 UTC (`research/boot0037w.log`). NOTE: userdata has now been modified by rw boots; it no longer equals the v2 image, so sparsediff with v2 as OLD is not safe any more (full reflash or a new baseline needed).
- **0036 phone: launchd boot tasks run; mount -P 1 needs /chosen/boot-manifest-hash.** Boot 0036hr 16:15:32 UTC, photo IMG_20261016_185236: platform check passes; fsck boot task runs (`fsck: not booting main or diagnostic OS. Skipping fsck on OS container`); mount-phase-1: `failed to get filesystem info` x2 (no IODeviceTree:/filesystems/fstab, non-fatal), `mount: data volume missing, but not required in env: 0`, mount_hfs update of /: `GetMasterBlock: Error 16 opening /dev/ufsroot0` (EBUSY, mounted block dev, non-fatal) then mount() EROFS (read-only boot) -> `mount: / failed with 1`; then `failed to get boot-manifest-hash` -> errx exit 66 -> launchd panic `mount[3] exited`. /sbin/mount: fn 0x100005f90 reads IODeviceTree:/chosen boot-manifest-hash (hex), bindfs-mounts /private/preboot/<HEX>/usr/standalone/firmware (ENOENT tolerated unless required). `tools/mkadt.py` now adds `boot-manifest-hash` = SHA-384("sweet boot manifest") (5CBE3601...5205). ADTs `shim/out/sweet-0036hi.adt`, `research/q0036-hi.adt` (QEMU probe OK, csops 0x26000001). Image `shim/out/shim-boot-0036mw.img` (0036 + new ADT + ufsrw=1 rootrw=1), booted 16:57:47 UTC (`research/boot0036mw.log`): FIRST WRITABLE BOOT of userdata (restore source: v2 image on SSD).
- **0036 CS_SIGNED (phone boot 0036hr pending photo).** Phone boot 0035hr 2026-10-16 15:57:02 UTC (`research/boot0035hr.log`, photo IMG_20261016_180249): highmem works on hardware; shared cache maps (trust-cache warnings only), Apple launchd runs (`Darwin Bootstrapper Version 7.0.0`, `entering ondemand mode`), starts boot task fsck, then userspace panic `boot task is not a platform binary (fsck, /sbin/fsck)`. launchd+0x3fea0: `csops(pid, CS_OPS_STATUS)` must have bit 0x04000000. Cause: nothing set CS_SIGNED (AMFI does it in vnode_check_signature), so csproc_get_blob() returns NULL for every process and csops never reports CS_PLATFORM_BINARY (also breaks per-process cdhash/entitlement queries). QEMU probe init (`csops` status printer, scratchpad csq/) on 0035: `0x02000001`; patch **0036** (`*cs_flags |= CS_SIGNED` in the SWEET AMFI hook), CI `38929635791` -> `xnu/out-0036`: `0x26000001`. Images `shim/out/shim-boot-0036hr.img` / `-0036hw.img` (same cmdlines as 0035). 0036hr booted 16:15:32 UTC (`research/boot0036hr.log`). Next launchd boot tasks (embedded __config plist): fsck -qL and mount -P 1/2 (RequireSuccess); no /etc/fstab on the image.
- **0035 ENTITLEMENTS + PLATFORM BINARIES (QEMU verified).** `xnu/patches/0035-sweet-amfi-entitlements-platform-binaries.patch`: new `iokit/Kernel/arm/SweetEntitlements.cpp` (OSObject subclass: OSDictionary from OSUnserializeXML of the signature's XML entitlements + original XML for CS_OPS_ENTITLEMENTS_BLOB); `bsd/kern/sweet_amfi.c` answers queryEntitlementBoolean/String(+WithProc, string also matches array members), copyEntitlementAsOSObject(+WithProc, retained), asdict (retained), get_xml (kalloc_data copy); built-in MAC policy "SWEET AMFI" (registered from bsd_init before vfs/exec) with mpo_vnode_check_signature: every signed binary on the MNT_ROOTFS mount gets CS_PLATFORM_BINARY (stand-in for AMFI's static trust cache: iOS system binaries are ad-hoc, cd_flags 0x2, platform byte 0), and attaches entitlements ONLY for the writable copy (cs_blob != csb_ro_addr; revalidation passes the RO blob). LESSON: first version attached in adjustContextWithoutMonitor -> QEMU `unable to adjust OSEntitlements context without monitor: 53` because ubc_cs_convert_to_multilevel_hash (4K-page signatures on 16K kernel) sets csb_entitlements_blob = NULL; ALL iOS 16 binaries (launchd, backboardd, SpringBoard) use 4K code pages, so this would have broken launchd on the phone. Boot-arg `sweet_amfi_debug=1` logs `SWEET AMFI: <ident>: N entitlements (test: ...)`. `tools/mkinit.py OBJ OUT [ENTS.plist]` adds an XML entitlements blob (special slot -5; default output byte-identical). QEMU `research/q0035-ent` (RAM disk `research/root-hfs-ent.img`, init with 3 test entitlements, highmem): `org.sweet.init: 3 entitlements (test: bool 1 string 1 array 1)` then init PID 1. MAC policies loaded: 0034 = 1, 0035 = 2 (QMP `mac_policy_list`). CI `38909807230` -> `xnu/out-0035`. Phone images: `shim/out/shim-boot-0035hr.img` (highmem, ro), `-0035hw.img` (highmem, +ufsrw=1 rootrw=1), `-0035r.img` (low mem fallback).
- **PHONE TEST ORDER (updated):** 0035hr -> if instant reset, 0035r -> then 0035hw.

- **HIGHMEM: XNU in the upper DDR bank, ~3.95 GiB (was 1.2 GiB).** Live FDT memory = <0x80000000 2G> + <0x1_00000000 0xfcc00000>; all fixed reserved-memory carve-outs are in the low bank (highest: ramdump_fb 0xaf000000, rmtfs ~0xfde01000), dynamic pools have alloc-ranges <= 0xffffffff, UFS node has no `iommus` (Linux DMAs anywhere). `tools/mkadt.py --highmem` sets /chosen dram-base 0x100000000 dram-size 0xfcc00000; `tools/mkpayload.py` now takes phys_base/mem_size from the ADT (default payloads byte-identical, verified); `tools/qemu-sweet.py` sizes -m from the payload (8G for highmem). Patch **0034** (`0034-sweet-ufs-dma-above-4g.patch`): UFS refuses DMA memory above 4G without UFSHCI CAP.64AS (bit 24), logs `DMA at 0x...`. CI `38905889883` success -> `xnu/out-0034`. **QEMU verified** (`research/q0034-hi`, ADT `research/q0034-hi.adt` --qemu --highmem, HFS RAM disk): phys_base 0x100000000, init PID 1 heartbeat, no panic; `max_mem` = `mem_size` = 0xfcc00000 read via QMP (`research/q0034-hi-mem`). Phone images: `shim/out/shim-boot-0034hr.img` (read-only) / `-0034hw.img` (+ufsrw=1 rootrw=1), ADT `shim/out/sweet-0034hi.adt` (asmb SIP + sweet-ufs + highmem). PHONE RISK: unknown runtime TZ/XPU protection in the upper bank would show as an instant reset; fall back to 0033r/w. Watchdog ruled out for the overnight reset: msm-watchdog bark is 20 s (qcom,bark-time 0x4e20) yet panic screens stay up for many minutes.

- **Offline research 2026-10-16 (user away, phone unavailable). Images rebuilt** `shim/out/shim-boot-0033r.img` / `-0033w.img` now also carry `wdt_test=no_user_checkin`. NEXT ON PHONE: boot 0033r (expect past in6_nigroup), then 0033w (first writes: rw root fixes HFS link-chain bits itself and gives /var for launchd sockets /var/run/syslog, /var/run/lockdown.sock).
- **launchd.plist** (`/System/Library/xpc/launchd.plist`, compiled; 532 daemons, 66 RunAtLoad/KeepAlive, list in scratchpad `early-daemons.txt`). `_PanicOnCrash` daemons: containermanagerd/usermanagerd (InternalOnly), lockdownd/remoted/trustd/remotepairingdeviced (AutomationOnly), **watchdogd unconditional** (KeepAlive SuccessfulExit=false). watchdogd needs IOKit `IOWatchdog` kext service; if absent it exits only when `kern.hv_vmm_present` (ADT `/defaults/vmm-present`, hvg_sysctl.c) or with boot-arg `wdt_test=no_user_checkin` ("detected testing boot-args to not run, exiting"; reads kern.bootargs). Using the boot-arg (vmm-present would change many daemons; keep as an option).
- **GUI path research (QuartzCore in the shared cache, `ipsw dyld str CACHE STR`, ~10 s/search):** CA::WindowServer has IOMFBServer/IOMFBDisplay (Apple DCP/IOMobileFramebuffer), AppleCLCDDisplay, AppleRGBOutDisplay, **VirtualServer/VirtualDisplay** and **MemorySurface**, EAGLServer. Renderers: CA::OGL::MetalContext, GLContext and a **CPU software rasterizer CA::OGL::SW** (samplers/blend modes; "image is too large for software renderer"). Env knobs: `CA_ENABLE_OGL`, `CA_DISPLAY_TYPE`, `CA_FORCE_LOCAL_SERVER` (249 CA_* vars). Hypothesis (unverified): backboardd's render server could run a VirtualServer/MemorySurface with the SW renderer and we copy the surface to the ABL framebuffer -> needs disassembly of CA::WindowServer server creation. Expected other GUI walls: IOSurface (closed-source kext; needs a reimplementation), IOMobileFramebuffer user client, IOHID touch events, AppleKeyStore/keybag (SEP) for lock state, MobileGestalt answers (AppleCLCD etc.).

- **0032r RESULT (boot 05:07:50 UTC, `research/boot0032r.log`, photo IMG_20261016_071200): SHARED CACHE MAPPED, LAUNCHD RUNS.** With SIP relaxed (ADT asmb) the `not in trust cache` lines are only warnings for every subcache (.31 ... .42.dyldlinkedit); no SIP rejection; libSystem loads; launchd configures networking (`skip attaching fsw to lo0 using legacy TX model`) -> kernel data abort far 0x28 in `SHA1Init` (symbolized as sha1_init) from `in6_nigroup` (IPv6 on lo0): g_crypto_funcs NULL. Kernel addresses are unslid: `~/swift/usr/bin/llvm-symbolizer --obj=KERNEL ADDR` works directly.
- **0032** BearSSL SHA-1 (`0032-sweet-bearssl-sha1.patch`), **0033** BearSSL SHA-384/512 (`0033-sweet-bearssl-sha384-sha512.patch`, BearSSL ctx stored in SHA512_CTX, static-asserted to fit; ubc_subr.c uses SHA-384 for code directories). `0020-stage-bearssl.sh` now also stages hash/sha1.c, hash/sha2big.c, codec/{dec,enc}{32,64}be.c. CI `38854909053` success (0032-only run `38854802073` superseded). Kernel `xnu/out-0033` (SHA1Init -> _br_sha1_init verified by disassembly; nm symbols have a leading underscore). Images (ADT sweet-0032): `shim/out/shim-boot-0033r.img` (read-only), `shim-boot-0033w.img` (+ufsrw=1 rootrw=1). Still NULL g_crypto_funcs users: des, chacha20poly1305, rsa, aesxts, rand, flow_divert.
- `research/check-patch-order.py` now skips patches touching CI-staged sweet_hfs sources (0031) instead of stopping.

- **0031r RESULT (boot 05:03:52 UTC 2026-10-16, `research/boot0031r.log`, photo IMG_20261016_070508): SHARED CACHE FOUND.** dyld now opens `dyld_shared_cache_arm64` (v2 image hard links), kernel refuses to map it: `shared_region: ... map(...'dyld_shared_cache_arm64'): not in trust cache` -> `vnode is not SIP-protected` (vm_unix.c: macOS arm64 path needs trust cache, else root-owned + SF_RESTRICTED unless CSR_ALLOW_UNRESTRICTED_FS) -> `dyld cache '(null)' not loaded: syscall to map cache into shared region failed` -> libSystem not loaded -> coredump to /cores fails error 30 (EROFS) -> panic initproc failed -> "MACH Reboot" (spins; screen kept). Lots of `hfs: set hardlink bit on vol=iOSRoot cnid=... linkid=...` spam (Linux-made hard links lack the link-chain bit; read-only mount cannot persist the fix). FIX: ADT `/chosen/asmb` with `lp-sip0`=0x6f (u64; incl. CSR_ALLOW_UNRESTRICTED_FS) and `lp-sip1`=1 (unauthenticated root), `tools/mkadt.py` -> `shim/out/sweet-0032.adt`. Images with kernel 0031: `shim/out/shim-boot-0032r.img` (read-only) and `shim-boot-0032w.img` (+ufsrw=1 rootrw=1).

- **2026-10-16 morning:** userdata now = **v2** image (`diff-v1-v2.simg` flashed 21:06:50 UTC 2026-10-15, OKAY, `research/flash-userdata-diff-v2.log`); future diffs use `ios16-root-20H392-v2.img` as OLD. Booted `shim-boot-0029k.img` (ignition_level=1) right after (`research/boot0029k-v2.log`); **no photo** (user asleep). Next morning the phone was in PixelOS recovery = some hardware reset (ABL then booted Android, which cannot use userdata). OBSERVATION: our kernel cannot reset the SoC itself (`PE_halt_restart` is never set; reboot/halt print "MACH Reboot"/"CPU halted" and spin), so the reset came from outside XNU: Qualcomm watchdog or a TZ/XPU violation. Unknown which; re-boot 0029k and watch/video until the reset. Do NOT tap factory reset/format data in recovery (would wipe the iOS image).
- **0030** (`xnu/patches/0030-sweet-ufs-writes.patch`, CI `38852655542`): UFS WRITE(10) + read-modify-write for partial 4 KiB blocks + SYNCHRONIZE CACHE (DKIOCSYNCHRONIZE), only with boot-arg `ufsrw=1`; `ufs_write_blocks` re-checks partition bounds before every write (only place data goes out).
- **0031** (`xnu/patches/0031-sweet-read-write-root.patch`, CI `38852837176`): boot-arg `rootrw=1` clears MNT_RDONLY before the root mount (no data volume on sweet: /private/var is on the root; image has no /etc/fstab); Apple HFS accepts a dirty non-journaled ROOT read-write on SWEET (logs "mounting dirty non-journaled root read-write"). Later: journaled HFS for crash safety. Kernel `xnu/out-0031` (CI `38852837176` success, 5m31s; 0030 run cancelled by the newer push). Boot images (phone ADT `sweet-0028.adt`, ramdisk byte-matched): `shim/out/shim-boot-0031r.img` = 0029k args (read-only, ignition_level=1) and `shim/out/shim-boot-0031w.img` = same + `ufsrw=1 rootrw=1`. PLAN: boot 0031r first and film until the reset (diagnose overnight reset), then 0031w (first ever writes to userdata; v2 image on the SSD is the restore source). `research/check-patch-order.py` cannot check 0031 (staged sweet_hfs files are not in its index); CI stage scripts run first, so CI is the check.

- **ignition_level=1 result** (boot 21:01:39 UTC, `research/boot0029k.log`, photo IMG_20261015_230308): preboot/cryptex stages skipped (need >= 2/3), `dylib_cache: finding shared cache on system volume: ignition level = 1, search root fd = 4, subpath = System/Library/Caches/com.apple.dyld/` -> not found -> `ignition boot failed: 19` -> no shared cache -> libSystem not loaded. FIX IN PROGRESS: image v2 = copy of the flashed image + `/System/Library/Caches/com.apple.dyld/` with HARD LINKS to the 44 cryptex cache files (no extra space; avoids symlink-following doubts), then `tools/sparsediff.py ios16-root-20H392.img ios16-root-20H392-v2.img diff.simg` and flash only the diff. Boot with `ignition_level=1` (shim-boot-0029k.img).
- **ignition_level=0 result** (boot 20:58:07 UTC, `research/boot0029j.log`, photo IMG_20261015_230037): all stages skipped as "ignition level insufficient" (hello/preboot/cryptex1 sniff >= 2, select os cryptex path/graft >= 3, **dyld_cache >= 1**), `ignition boot failed: 89`, "ignition partially disabled", then same `Library not loaded: /usr/lib/libSystem.B.dylib (no such file, no dyld cache)`. => dyld gets the shared cache directory from ignition's dyld_cache stage. Next: `ignition_level=1` (`shim/out/shim-boot-0029k.img`) to skip preboot/cryptex stages but run dyld_cache; its log "finding shared cache on %s ... subpath = %s" will show where it searches. If it searches rootfs /System/Library/Caches/com.apple.dyld, add a symlink there (image rebuild; consider a diff-only sparse flash tool to avoid rewriting 12 GB).
- **APPLE dyld/launchd RUNNING (libignition) with `unentitled_ios_sim_launch=1`** (boot 20:53:24 UTC, `research/boot0029i.log`, photo IMG_20261015_225617): kernel prints "Allowing spawn of iOS binary ... boot-arg set to true"; `shared_region: ... check_np ... vm_shared_region_start_address() failed` (no region yet, normal); dyld's built-in **libignition** (Darwin Ignition Sequence 1.0.0, libignition-51~1974; prints rd=ufsroot0, hardware target sweet, ignition level 0x5, platform 0x2, boot spec local, 7 stages): `hello from launchd.1` -> `preboot: mounting preboot` -> `failed to lookup preboot: 25` (ENOTTY: looks for a separate Preboot volume by role) -> ignition fails -> dyld `Library not loaded: /usr/lib/libSystem.B.dylib ... (no such file, no dyld cache)` -> launchd exit -> panic initproc failed. dyld boot-args found via strings: `ignition_level`, `ignition_force_dylib_root` (forced rootfs/livefs/cryptex fallback), `ignition_halt_after`, `ignition_live_app_graft`; dyld cache search paths include /System/Cryptexes/OS, /System/Volumes/Preboot/Cryptexes/OS/, /private/preboot/Cryptexes/OS/ (present in our image). Next try `ignition_level=0` (`shim/out/shim-boot-0029j.img`).
- **APPLE launchd EXECUTED FROM UFS (2026-10-15).** Flashed iOS image: `img2simg` -> `ios16-root-20H392.simg` (12.1 GB) then `fastboot -S 512M flash userdata ...simg` (24 chunks, 307 s, `research/flash-userdata-ios16.log`; a raw-image flash was killed by earlyoom at 2.6 GB RSS - always flash the sparse image). ABL fastboot hung on getvar after the flash; power+vol-down fixed it. Boot 0029u 20:49:04 UTC (`research/boot0029u-ios16.log`), photo IMG_20261015_225134: `hfs: mounted iOSRoot` -> load_init_program /sbin/launchd -> panic `unexpected SIGKILL of launchd (CS_KILLED) with reason -- namespace 9 code 0xe` = OS_REASON_EXEC / EXEC_EXIT_REASON_WRONG_PLATFORM: our kernel is XNU_TARGET_OS_OSX, iOS-platform binaries are "alien" and refused unless spawn attr platform set or DEVELOPMENT TUNABLE `unentitled_ios_sim_launch`. Next try: same kernel, cmdline + `unentitled_ios_sim_launch=1` (`shim/out/shim-boot-0029i.img`). LONGER TERM: macOS-config kernel marks iOS maps alien (vm_map_mark_alien) and differs from iOS in many policies; an iPhoneOS-config kernel build may eventually be needed.
- **iOS 16 ROOT IMAGE BUILT:** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392.img` (16 GiB case-sensitive HFS+, label iOSRoot, 12 GB used, fsck OK after `fsck.hfsplus -y` rewrote 32 non-ASCII localized WorkflowKit Shortcut names Linux decomposes differently). Built by `tools/mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]` (needs sudo; apfs-fuse as root; OS dmg is UDIF -> link must end .dmg). Contents: iPhone X 20H392 OS root + SystemOS cryptex at /private/preboot/Cryptexes/OS (so /System/Cryptexes/OS -> cache, 44 files, dyld_v1 arm64) + AppOS at .../App. /sbin/launchd and /usr/lib/dyld root:wheel 0755. NEXT: (confirm) `fastboot flash userdata ios16-root-20H392.img` (auto-sparse), boot 0029u (rd=ufsroot0) -> Apple launchd attempt.
- **MILESTONE ON HARDWARE: BOOT FROM INTERNAL FLASH.** 0029 CI `38797924912` PASSED, `xnu/out-0029` SHA256 `a68bc847...2a37`. Image `shim/out/shim-boot-0029u.img` (payload-0029u = 0029 kernel + `shim/out/sweet-0028.adt` + `rd=ufsroot0`, NO RAM disk). Boots 20:01:34 and 20:12:37 UTC (`research/boot0029u*.log`), photo IMG_20261015_221403: `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB, read-only` -> `BSD root: ufsroot0, major 1, minor 0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `SWEET: root volume is not authenticated (error 25); continuing unsealed` -> `load_init_program ... /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> heartbeat (ran for minutes unattended). userdata currently holds the 8 MB `research/root-hfs.img`. NEXT: build a multi-GB HFS+ image with the iOS 16 (iPhone X 20H392) userland (OS rootfs + SystemOS cryptex contents incl. dyld shared cache at /System/Library/Caches/com.apple.dyld; preserve modes/owners/symlinks; decompress LZBITMAP via apfs-fuse), flash it to userdata (sparse, needs user OK), then try Apple's dyld/launchd. Driver is read-only (root mounts RDONLY), fine for first launchd attempts.
- **userdata FLASHED with the 8 MB HFS test volume** (user said "flash it", 2026-10-15 19:53:17 UTC, `research/flash-userdata-hfs-test.log`: `fastboot flash userdata research/root-hfs.img` OKAY; pre-checks: one sweet device, partition-size:userdata 0x1AC4BFB000, partition-type f2fs, image fsck OK, SHA256 3626ea69...). Android userdata is now destroyed (expected/approved). Boot 0028u at 19:53:20 (`research/boot0028u-hfs.log`), photo IMG_20261015_215541: `BSD root: ufsroot0` -> **`hfs: mounted SweetRoot on device b(1, 0)` from internal UFS** -> panic `rootvp not authenticated after mounting @bsd_init.c:931` (arm64 macOS-config requires FSIOC_KERNEL_ROOTAUTH sealed root for non-ramdisk roots). Patch **0029** (`dd1c284`): SWEET logs `SWEET: root volume is not authenticated (error N); continuing unsealed` instead of panicking (Apple configs keep the panic; pivot_root still requires auth). CI **`38797924912`** running. Next: download out-0029, phone image with sweet-0028.adt + rd=ufsroot0 (no RAM disk), boot -> expect init from UFS.
- **UFS ROOT DRIVER VERIFIED ON HARDWARE (read-only).** 0028 CI `38796139570` PASSED, `xnu/out-0028` SHA256 `8b981640...9da6`; QEMU HFS RAM-disk regression `research/q0028-hfs` still runs init. Phone boot 2026-10-15 19:50:24 UTC (`research/boot0028u.log`, image `shim/out/shim-boot-0028u.img`, phone ADT `shim/out/sweet-0028.adt` with /sweet-ufs, NO RAM disk, `rd=ufsroot0`), photos IMG_20261015_215151/215220: `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB, read-only` -> `BSD root: ufsroot0, major 1, minor 0` -> `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : f188` (Android-encrypted data, as expected) -> `cannot mount root, errno = 19`, kernel retries mountroot (no panic). Nothing written to flash. NEXT (destructive, needs user confirmation right before): `fastboot flash userdata research/root-hfs.img` (8 MB HFS with test init) then boot rd=ufsroot0 -> expect init from internal flash.
- **PATCH 0028 (`33aceac`): XNU read-only UFS root device**, CI **`38796139570`** running (next artifacts xnu/out-0028, research/bt29, run29.log). `bsd/dev/sweet_ufs.c`: reuses ABL's UFSHCI state (like the shim probe), takes over UTRL at a kmem_alloc_contig region (DMA layout: UTRL 0x0, UCD 0x1000, 1 MiB bounce 0x4000), verifies UTRLBA readback before any doorbell, IE=0 polled, one SCSI cmd at a time on LUN0 (TUR x3, READ(10) via bounce, PRDT 256 KiB chunks), flush_dcache64 by PA around each command (host not dma-coherent per DT), finds GPT partition by name (`ufsroot=` boot-arg, default `userdata`), exposes `/dev/ufsroot0`/`rufsroot0` (512-byte logical sectors, physical 4 KiB, DKIOC* ioctls), refuses writes (EROFS). IOKitBSDInit: `rd=ufsroot0` -> sweet_ufs_rootdev() (lazy init) else panic. ADT: `tools/mkadt.py` adds `/sweet-ufs` reg <0x1d84000 0x3000> for the PHONE only (not --qemu). Local compile OK, imports resolve. Sandbox research/bt28 (pre0028 originals). FIRST PHONE TEST PLAN (non-destructive): boot 0028 with phone ADT + `rd=ufsroot0` and NO RAM disk: expect `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB` + `BSD root: ufsroot0`, then root mount failure (userdata is still Android-encrypted) -> panic. That validates the driver before flashing an HFS image into userdata (needs explicit user confirmation at that time).
- **SUPERSEDES the 64 GiB iosroot GPT plan: user said "u can delete android" (2026-10-15).** Use the existing `userdata` partition (LUN0 sda34, 4K LBA 0x2B8C00, 0x1AC4B MiB ~107 GiB) directly as the iOS root: NO GPT changes. Keep bootloaders, recovery (OrangeFox), modem/persist/fsg/modemst untouched. Write the HFS image later with `fastboot flash userdata` (sparse, auto-split under the 768 MiB limit). Android will no longer boot normally (expected; reinstallable). Order: XNU UFS driver first, read-only against the current GPT; flash only once it can locate userdata. Nothing erased yet.
- **DECIDED (user, 2026-10-15): 64 GiB `iosroot` on LUN0 (sda), taken from userdata (wipes Android user data; Android/super kept).** Planned GPT (4K LBAs): userdata 0x2B8C00..0xD7D6FF (~43 GiB, keep type/unique GUIDs and attributes); iosroot 0xD7D700..0x1D7D6FF (0x1000000 blocks = 64 GiB, 1 MiB aligned); backup GPT area unchanged. Procedure: user backs up phone data -> boot OrangeFox recovery (adb) -> back up GPT (sgdisk --backup + raw dd of LBA 0-5 and last 6 blocks) to research/ -> sgdisk edit -> Format Data -> verify Android boots to setup. NOT EXECUTED YET.
- **INTERNAL UFS WORKS FROM THE SHIM (read-only), user authorized internal-UFS approach incl. flashing (2026-10-15, "i allow everything").** Shim probe `shim/ufs.c` (commit `12ed3ed` + filter): ufshc@1d84000 is UFSHCI 2.1 (ver 0x210, cap 0x1587031F, 32 slots), ABL leaves HCE=1, HCS=0x10F (device present, lists ready, UIC ready); probe takes over UTRL at its own buffer (verifies UTRLBA readback before any doorbell), TEST UNIT READY/READ CAPACITY/READ(10) all succeed. LUN0 = 0x1D7D800 x 4 KiB (~126.6 GB), valid GPT (64 entries at LBA 2). Photos IMG_20261015_212916/213325/213429. Big LUN0 partitions (start in 4K LBAs / MiB): super 0xCA00/0x2200 (8.5 GiB), logdump 0x22CA00/64, minidump 0x230A00/128, rawdump 0x238A00/128, recovery 0x240C00/128, cache 0x248C00/384 ext4, exaid 0x260C00/384, cust 0x278C00/1024 ext4, **userdata 0x2B8C00/0x1AC4B MiB (~107 GiB), fs unreadable = encrypted (metadata encryption)**. cache..cust contiguous = only 1.75 GiB (too small for iOS ~10+ GiB). Plan needing user consent: shrink userdata in GPT (wipes Android data) and add an `iosroot` partition at the end of LUN0. LUNs 1-5 not listed yet (sdb..sdf backed up in backup/lun).
- **fastboot limit measured (2026-10-15, read-only getvar):** `max-download-size: 805306368` (768 MiB); `max-fetch-size` unsupported; `is-userspace: no` (ABL fastboot). A `fastboot boot` image (shim+kernel+RAM disk) must be < 768 MiB, so the 2.5 GiB iOS shared cache cannot ride in the boot RAM disk even if XNU gets all RAM. Real userland needs storage access (UFS driver, or USB host) or a much smaller cache. Unlocking full RAM is still needed later (GUI).
- **iOS 16 USERLAND SOURCE (Claude, 2026-10-15).** Chose **iPhone X (iPhone10,3/10,6, D22/D221AP)** iOS **16.7.16 (20H392)** over iPhone 8: same A11 (no PAC -> plain arm64, runs on Kryo 470/ARMv8.2), last iOS for A11; X userland is home-button-less and tall-screen (1125x2436 vs sweet 1080x2400), better fit for later GUI. Note the X has its OWN SystemOS cryptex (different digest from iPhone 8's). IPSW URL in `research/ipsw-x/URL.txt` (+ BuildManifest). Images on the user's external SSD (NOT in git, do not format that disk: it holds the user's photos): `/run/media/dipa/android/ios-sweet-ipsw/iPhone10,3_16.7.16_20H392/` — `087-86593-047.dmg` = Cryptex1,SystemOS (2,743,074,816 B), `087-86698-044.dmg` = OS rootfs (4,730,814,044 B), both zip CRC-32 verified, plus Firmware/*.root_hash/.trustcache/.mtree. AppOS cryptex is 087-86715-047.dmg (10 MB, same as iPhone 8).
  - Images are raw APFS; files use LZBITMAP (decmpfs type 14). 7z cannot decode. Tools: `ipsw` 3.1.738 (AUR ipsw-bin, user-approved) needs apfs-fuse on Linux; **apfs-fuse** built in user space (system cmake broken: cmake 4.4.4 wants libjsoncpp.so.27, jsoncpp 1.9.6 installed = partial upgrade, NOT fixed) with pip cmake + `-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint"`, installed to `~/.local/bin/apfs-fuse` (+apfs-dump-quick). Mount via a comma-free symlink (apfs-fuse splits options on ',' in `iPhone10,3`; exFAT has no symlinks -> put link elsewhere): `apfs-fuse LINK/087-86698-044.dmg MNT` -> `MNT/root/...`; unmount `fusermount3 -u MNT`.
  - Verified from headers: `dyld_shared_cache_arm64` magic `dyld_v1   arm64`; `/sbin/launchd`, `/usr/lib/dyld` = cputype 0x100000c subtype 0 (plain arm64). libSystem exists only inside the cache.
  - **BLOCKER for real userland via RAM disk:** shared cache = 3.10 GiB in 44 files (~2.5 GiB without the 592 MB `.symbols`), but the kernel manages only 1.20 GiB (0xb0000000..0xfd000000). Options: (A) give XNU the phone's full ~6 GB (banks 0x80000000+2G and 0x100000000+0xfcc00000, carve-out holes need pmap support) AND check fastboot max-download-size for a multi-GB boot image; (B) storage driver (Qualcomm UFS) to read a root from flash (no flashing allowed: would need a file/partition strategy); (C) run against individual dylibs extracted from the cache (uncertain).
- **MILESTONE ON HARDWARE: HFS+ ROOT FILESYSTEM -> PID 1.** Boot 2026-10-15 18:22:12 UTC (`research/boot0027b.log`, image `shim/out/shim-boot-0027b.img`), photo `IMG_20261015_202334.jpg`: `BSD root: md0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> repeated `SWEET init: still alive`, no panic. Kernel 0027 (built-in Apple OSS HFS + BearSSL MD5), init with SP alignment fix. NEXT: real iOS 16 userland on the HFS image (dyld, libSystem/dyld shared cache, launchd). Known gaps: entitlements (none granted), SHA-1/SHA-384/512 providers, no storage driver (RAM disk only), root mounted read-only.
- **PHONE HFS ROOT (0027) + SIGBUS ROOT-CAUSED.** Phone boot 2026-10-15 17:49:06 UTC (`research/boot0027.log`, photo `IMG_20261015_200031.jpg`): `hfs: mounted SweetRoot on device b(1, 0)` (HFS works on hardware), init printed its first line, then `pid 1 exited -- exit reason namespace 2 subcode 0xa` = SIGNAL/SIGBUS -> `initproc failed to start` panic. Diagnostic boot 18:08:13 UTC (`research/boot0027d.log`, image `shim/out/shim-boot-0027d.img`, diag init `research/diag-init` from scratch diag-init.S with [a][b][c] markers, `cs_debug=1`; photo `IMG_20261015_201547.jpg`): output `PID [a][b]` then SIGBUS -> fault at first `strb w21,[sp]`; no code-signing messages. Cause: kernel enters a static binary with sp at argc, not necessarily 16-aligned (depends on arg/apple string lengths, differs mockfs vs HFS); EL0 SP alignment check -> EXC_ARM_SP_ALIGN -> SIGBUS (sleh handle_sp_align). QEMU does not enforce SP alignment. NOT a kernel bug: real binaries start in dyld which aligns. Fix `1a8fc15`: sweet-init aligns sp at entry. Rebuilt `research/root-hfs.img`, QEMU `research/q0027-hfs2` OK. Phone image `shim/out/shim-boot-0027b.img` SHA256 `098ba46c...c5c` (payload-0027b, ramdisk match check-0027b) ready, awaiting FASTBOOT. Lesson: Assembly `;` is a comment in Apple arm64 asm (use .macro, not ;-joined #define).
- **HFS ROOT WORKS (QEMU).** 0027 CI `38779441895` PASSED, `xnu/out-0027` SHA256 `a8643886...5f6a`. QEMU `research/q0027-hfs` (RAM disk = `research/root-hfs.img`, rd=md0): `BSD root: md0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` + heartbeat, no panic. Phone image `shim/out/shim-boot-0027.img` (payload-0027 = 0027 kernel + sweet-0021.adt + root-hfs.img + rd=md0, ramdisk byte-match check-0027) ready; awaiting FASTBOOT for phone test.
- **HFS ROOT WORK (Claude), patches 0026-0027.**
  - 0026 (`f0e990c` + link fix `64c96f4`): Apple OSS **hfs-627.100.6** (commit `31d5ec238dfab485e78a9a30b8b92d13e8914f03`, paired with xnu-8796.141.3 in macos-135 release.json) staged by `xnu/patches/0026-stage-hfs.sh` into `bsd/sweet_hfs` (core + hfs_encodings; AppleKeyStore include/key prototypes guarded by CONFIG_PROTECT; SDK TargetConditionals copied). New config option `sweet_hfs` (MASTER + MASTER.arm64.sweet); 40 `optional sweet_hfs` files; Makefile.template foreach adds `_CFLAGS_ADD = -include sweet_hfs_compat.h -I...` (MUST come after kernel -D flags; compat undefs BSD/XNU_KERNEL_PRIVATE and the CONFIG_* HFS tests so kext-config.h decides: macOS profile, no content protection). `sweet_hfs_iokit.c` replaces hfs_iokit.cpp/hfs_encodings_kext.cpp: `sweet_hfs_register()` called in bsd_init after vfsinit (encodings init, vfs_fsadd VFS_TBLCANMOUNTROOT, zones, sysctls); is_ejectable 0, iterate_media none, serial KERN_NOT_SUPPORTED, OSKextGetCurrentLoadTag -> kOSKextInvalidLoadTag (renamed via compat macro). hfs_encodings' own `hfs_allocated` renamed per object (separate kexts on Apple; first CI `38777325964` failed on that duplicate at link). mockfs now validates Mach-O magic (its TODO) so non-Mach-O roots fall through to HFS (new fs entries come AFTER mockfs in vfsconf). CI `38778133720` PASSED, `xnu/out-0026` SHA256 `0b49170c...701a`, prints `SWEET HFS: hfs-627.100.6 registered`.
  - HFS+ image: `research/root-hfs.img` (8 MB, mkfs.hfsplus from AUR hfsprogs installed with user approval; loop-mounted with sudo; /sbin/launchd = research/sweet-init, /dev; fsck.hfsplus OK). QEMU `research/q0026-hfs`: `BSD root: md0` then data abort in MD5Init from hfs_getvoluuid (mount computes volume UUID).
  - 0027 (`14bf2b7`): 0020 stager also copies BearSSL hash/md5.c + codec/dec32le.c/enc32le.c; `corecrypto_md5.c` SWEET branch converts MD5_CTX <-> br_md5_context around br_md5_* calls; wrappers sweet_md5.c/sweet_dec32le.c/sweet_enc32le.c. Host test 40/40 match md5sum. CI **`38779441895`** running.
  - Local tooling: `tools/xnu-cc.py` has `XNU_CC_POST` (flags after logged ones). Sandbox `research/bt28/xnu` = 0025 CI tree + 0026/0027 edits; originals `research/pre0026`, `research/pre0027`. HFS local objects `research/hfs-objs`. Checker entries updated to bt28.
  - SHA-1 / SHA-384/512 still have no provider.
- **MILESTONE ON HARDWARE: USERSPACE PID 1 RUNS ON THE PHONE.** Boot 2026-10-15 17:16:00 UTC (`research/boot0025.log`: send OKAY0.410s, boot OKAY0.135s), image `shim/out/shim-boot-0025.img` SHA256 `9dbfa0b1...89db0`. Photo `IMG_20261015_191733.jpg`: `BSD root: md0` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> repeated `SWEET init: still alive` (5 s heartbeat), no panic. Kernel 0025 + mockfs RAM disk + built-in no-entitlements AMFI. No flashing performed.
- **NEXT (toward real iOS userland):** real root filesystem with real binaries. Options: (a) build Apple OSS `hfs` (macOS 13.5 tag `hfs-627.100.6`) into SWEET like libpthread, then an HFS RAM disk image; (b) iOS 16 dyld + libSystem/dyld shared cache from the IPSW (needs APFS or repacking onto HFS). Expected hurdles: entitlements (sweet_amfi grants none), SHA-1/MD5/SHA-384/512 providers (BearSSL), missing kexts/daemons. Current init is a static test program, not launchd.
- **MILESTONE: USERSPACE RUNS (QEMU).** 0025 CI `38774788048` PASSED, `xnu/out-0025` SHA256 `5496069b...4877`. QEMU `research/q0025-rd` (150 s, no panic): `BSD root: md0` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> `SWEET init: still alive` every 5 s. Exec, ad-hoc SHA-256 code signature, syscalls, devfs /dev/console, select timeouts all work. Phone image prepared: `shim/out/shim-boot-0025.img` 16,515,072 bytes SHA256 `9dbfa0b15140c0bd733a16c4f727f6a7df44a8741f98d8e266d5f2fa4f689db0` (payload-0025 = 0025 kernel + sweet-0021.adt + research/sweet-init + rd=md0; ramdisk byte-match check-0025). Waiting for user to put phone in FASTBOOT.
- **0024 CI `38773277811` PASSED**, `xnu/out-0024` SHA256 `a0ba964b...4348`. QEMU `research/q0024-rd` and **PHONE (boot 2026-10-15 17:05:55 UTC, `research/boot0024.log`, image `shim/out/shim-boot-0024.img` SHA256 `cf3dac86...6964`, 16,510,976 bytes, payload `shim/out/payload-0024.bin` = 0024 kernel + `shim/out/sweet-0021.adt` + RAM disk `research/sweet-init` + rd=md0; ramdisk byte-match `check-0024`)** agree: `Added memory device md0`, `BSD root: md0`, `load_init_program: attempting to load /sbin/launchd`, then data abort in accelerate_entitlement_queries (PC 0x...77d2458, FAR 0x1a0) = NULL AMFI interface. Photo `IMG_20261015_190715.jpg`. RAM disk root is physically verified.
- **0025 (`2518c79`)**: new `bsd/kern/sweet_amfi.c` registers a built-in AMFI interface at EARLY_BOOT/MIDDLE: no entitlements (adjustContextWithoutMonitor succeeds without creating a context, queries KERN_DENIED, copies NULL/KERN_NOT_FOUND, get_xml/transmuted false); WithMonitor KERN_NOT_SUPPORTED; OSEntitlements_query and query_context_to_object panic explicitly (CoreEntitlements closed; no kernel caller); TrustCache version 0 (0015 keys on trust_cache_rt==NULL, img4 absent -> still unavailable); CoreEntitlements table zero (PMAP_CS only). Compiles locally; checker updated (bt27 entries). CI **`38774788048`** running. Next artifacts `xnu/out-0025`, `research/bt29`, `research/run29.log`. Real iOS binaries will need real entitlements later (no OSS CoreEntitlements exists).
- **0021-0024 RAM-disk-root iteration (Claude), QEMU only so far, no new phone boot since 0020.** Each kernel tested with `tools/mkpayload.py KERNEL research/q0021.adt OUT --ramdisk research/sweet-init --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"` then `tools/qemu-sweet.py OUT DIR --secs 150 --kernel KERNEL --fail-on-panic` (PATH needs ~/swift/usr/bin for llvm-nm). Fresh ADTs: `research/q0021.adt` (QEMU), `shim/out/sweet-0021.adt` (phone).
  - 0021 CI `38769532000` PASSED, `xnu/out-0021` SHA256 `2b7500a1...962e`, `research/bt25`/`run25.log`. QEMU `research/q0021-rd`: md0 root, then assert `iomediaClass` in IOService::publishHiddenMedia.
  - 0022 (`20ece9c`, publishHiddenMedia without IOMedia class) CI `38770666158` PASSED, `xnu/out-0022` SHA256 `a4d0e787...30c5`, `research/bt26`/`run26.log`. QEMU `research/q0022-rd`: data abort in SHA256_Init (NULL g_crypto_funcs) from vm_kernel_addrhash_internal.
  - 0023 (`c8089d1`, libkern SHA256_Init/Update/Final use built-in ccsha256_ltc_di + ccdigest_init/update on SWEET; also serves code-signing SHA-256; SHA-1/MD5/SHA-384/512 still have NO provider - BearSSL from 0020 could supply them later) CI `38771876423` PASSED, `xnu/out-0023` SHA256 `d763fed7...1003`, `research/bt27`/`run27.log`. QEMU `research/q0023-rd`: **mockfs mounted md0, `load_init_program: attempting to load /sbin/launchd`**, then panic `mockfs_fsnode_vnode failed to create fictitious pages ... rvalue = 5` (mockfs.c:351).
  - 0024 (`3293c80`): mockfs used memdev mi_base (4K page number of a VIRTUAL kernel address for IOKit RAMDisk, truncated to 32 bits) << PAGE_SHIFT as a physical address. Now zero-copy pager mapping only when mi_phys (with 4K units); virtual md uses pagein/blockmap/strategy -> memdev bcopy. memdev's own mdPhys path assumes 4K ppnums (unsafe on 16K arm64), so do NOT switch the RAMDisk to physical. CI **`38773277811`** running. Next artifacts `xnu/out-0024`, `research/bt28`, `research/run28.log`.
  - Sandboxes: bt25/bt26/bt27 contain the next patch's edited files; pristine copies in `research/pre0022`..`pre0024`.
- **PATCH 0021 (Claude): RAM disk root via mockfs -> first userspace program.** Commits `9c37771` (initial; CI `38768500280` FAILED: mockfs_fsnode.c kfree_type(mockfs_fsnode_t) typed-alloc static assert; local xnu-cc stubs `__builtin_xnu_types_compatible` so it can't catch this) and `193772f` (fix), CI **`38769532000`** running: https://github.com/dipa-studio/ios-sweet/actions/runs/38769532000. Next artifacts: `xnu/out-0021`, `research/bt25`, `research/run25.log`.
  - Kernel: `config/MASTER.arm64.sweet` FILESYS_BASE += `mockfs` (XNU's built-in FS: root device is itself a Mach-O, served as /sbin/launchd, plus /dev for devfs). Upstream mockfs bit-rot fixed: kalloc_type/kfree_type used the POINTER typedef mockfs_fsnode_t (nodes were 8 bytes) -> `struct mockfs_fsnode`. `IOService::setRootMedia` asserted IOBlockStorageDriver class exists (IOStorageFamily absent): SWEET-only, a missing class means root=NULL (same as IOSetImageBoot); Apple path keeps assert. All 21 patches apply (`research/check-patch-order.py`); 5 TUs compile locally with -DMOCKFS (`research/mockfs-objs`), imports resolve vs 0020 kernel. Sandbox `research/bt24/xnu` now holds 0021 versions of those 3 files; pristine 0020 copies in `research/pre0021`.
  - Tools: `tools/mkpayload.py --ramdisk IMAGE` appends IMAGE as an extra payload segment right after the kernel, moves kernel_end past it (shim unchanged: ADT+boot_args follow it, image stays below topOfKernelData), and adds ADT `/chosen/memory-map` `RAMDisk`=<phys,size>. Boot with `rd=md0`. Without --ramdisk output is byte-identical to existing 0020 QEMU and phone payloads (verified). `tools/sweet-init.S` + `tools/mkinit.py`: PIC static init (open /dev/console, print PID, heartbeat every 5 s via select, never exits) wrapped into a static arm64 Mach-O (__PAGEZERO, __TEXT@0x100000000, LC_UNIXTHREAD, ad-hoc SHA-256 CodeDirectory v0x20400, 4K pages). Build: `~/swift/usr/bin/clang -target arm64-apple-ios16.0 -c tools/sweet-init.S -o X.o && tools/mkinit.py X.o research/sweet-init` (ld64.lld cannot link static iOS). Signature not verified by a real codesign; the kernel will tell.
  - QEMU with 0020 kernel + RAM disk (`research/q0020-rd.bin`, rd=md0, 150 s `research/q0020-rd150`): `Added memory device md0`, **`BSD root: md0, major 1, minor 0`** (plumbing works), then the setRootMedia assert (now fixed in 0021). NOTE: QEMU needs >60 s to reach IOFindBSDRoot (60 s run showed nothing after stf0; likely RTC/inittodr wait), so use --secs 150.
  - NEXT: download 0021, QEMU 150 s with ramdisk; expect mockfs mount then init exec -> `*** SWEET userspace: init running as PID 1 ***`. Then phone boot with same payload (phone ADT shim/out/sweet-0020.adt pattern + --ramdisk + rd=md0).
- **0020 PHONE RESULT (user transcription of screen, 2026-10-15): KERNEL INIT COMPLETE, WAITING FOR ROOT.** After Skywalk self-tests PASSED, kminit, ifnet_attach lo0/gif0/stf0, screen prints `BSD`, then `Waiting on <dict><key>IOProviderClass</key><string>IOMedia</string><key>Content</key><string>Apple_HFS</string></dict>` and `Failed to write boot-picker-bringup-reason to NVRAM.` No panic. This is IOFindBSDRoot waiting for a root device (none exists); NVRAM warning is expected (no NVRAM driver). QEMU `cpu_idle` snapshot is presumably the same wait (not GDB-confirmed; milestone GDB run was cancelled by user since phone answered it). Draft `research/check-milestones20.gdb` (hbreaks inittodr/vfs_mountroot/IOFindBSDRoot/bsd_utaskbootstrap) left uncommitted/ignored.
- **NEXT MILESTONE: root filesystem -> launchd.** Plan: (1) RAM disk: shim places an image in RAM, ADT /chosen/memory-map `RAMDisk` entry, boot-arg `rd=md0` (XNU md driver). (2) HFS is a separate kext on Apple; build Apple OSS `hfs` (tag paired with macOS 13.5 in distribution-macOS macos-135 release.json) into SWEET like libpthread 0019. APFS is closed. (3) Minimal HFS image with iOS 16 launchd (later: dyld/shared cache; AMFI/code-signing absent will be a later hurdle). User's end goal is full iOS GUI.
- **0020 (AES) VERIFIED + PHONE BOOTED (Claude took over after Codex usage limit).** Actions `38765422261` (`ab9e23f`) PASSED; kernel `xnu/out-0020/kernel.development.sweet` checksum OK; buildtree `research/bt24`, log `research/run24.log`. Actual-kernel AES checks PASS (`research/check-aes20.gdb` / `.log`, 12 extra CBC cases match OpenSSL). 45s QEMU `research/q0020`: no panic, zero unknown glyphs, past old TCP/AES panic -> Skywalk self-tests PASSED -> bsd_autoconf kminit -> ifnet_attach lo0/gif0/stf0; final snapshot PC in `cpu_idle` (idle or waiting; NOT yet diagnosed, could be root-device wait). Image `shim/out/shim-boot-0020.img` 16,461,824 bytes SHA256 `775200cb02d95fc750981516b79d91dcdc96184bbe67a38f9d57c59b4a391828`, ramdisk byte-matches `payload-0020.bin` (`check-0020`). **Phone temporarily booted 2026-10-15 16:10:00 UTC**: one FASTBOOT product sweet, send OKAY0.407s, boot OKAY0.134s, total0.549s, exit0 (`research/boot0020.log`). Awaiting user photo; do not reboot the phone before it. NEXT: read photo; diagnose what QEMU waits on after stf0 (GDB backtrace of threads / longer run). Codex's STOP request below is complete.
- User requests STOP after completing the current0020 AES iteration (Actions/QEMU/phone), then leave a precise committed handoff for another AI. User says FASTBOOT ready again after0019 photo. Do not start a new blocker fix after this iteration.
- Goal: iOS16-generation XNU on Redmi Note10Pro sweet/SM7150. Always commit, use gh/Actions for kernel builds, keep this handoff current. Temporary fastboot boot only; preserve Android/backups. No userland/root filesystem boot yet.
- **Patch0020 update:** initial `e71098d` / Actions `38765012466` superseded by scalar-only compile flags. Added -mgeneral-regs-only to all six provider/BearSSL TUs and corecrypto_aes.c; object disassembly now confirms no SIMD. Updated implementation `ab9e23f`, Actions `38765422261` queued/running: https://github.com/dipa-studio/ios-sweet/actions/runs/38765422261. Do not launch another run. portable BearSSL AES CBC and legacy one-block ECB wired directly into SWEET corecrypto_aes.c. Genuine bitslice implementation, no CPU AES/SIMD requirement, heap or fake global crypto table. Source fetched by `0020-stage-bearssl.sh` from official BearSSL git at immutable `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`; MIT license retained, downloaded source stays out of git. Kernel limits include adapter only. All128/192/256-bit keys, CBC encrypt/decrypt, copied contexts, in-place/out-of-place; zero block count and size overflow handled, invalid keys clear context. Seven TUs compile to arm64 objects (`research/aes-objs`), imports resolve, all20 patches apply and sources match (`research/patch0020-check.log`). Sandbox `research/bt23/xnu`, originals `research/pre0020`. NEXT: Actions then actual-kernel NIST/random CBC/ECB checks and >=45s QEMU before another phone boot. Next artifacts out-0020 / bt24 / run24.log. 0020 updated CI pending. Local sources/checks match the scalar-only patch.
- **Latest built kernel0019**, implementation `256d3f0`, patches0001-0019. Actions `38763072907` PASSED (4m5s): https://github.com/dipa-studio/ios-sweet/actions/runs/38763072907. Kernel `xnu/out-0019/kernel.development.sweet`, SHA256 `a27df7568f03fb541cf0e8024c0a594fa178a85054fcf9eba29158cbd4c5ecc0`; checksum verified. Log/source `research/run23.log` / `research/bt23` (no nested .git). Seven pthread source/build/header files and staged adaptations byte-match local compile inputs.
- **0019 hardware photo received** `IMG_20261015_175306.jpg`: pthread registration visible, passes mbinit and DLIL checksum/scheduling, same AES fault as QEMU: PC`0xfffffe00079174e0` (`aes_encrypt_key128`), LR`0xfffffe000767c3a4` (`tcp_init`), FAR0x78, x8=0. Disassembly proves NULL g_crypto_funcs->ccaes_cbc_encrypt. Photo processed; no more0019 observation pending. General crypto table remains absent, other hash/mode operations may still fault after0020. DO NOT bypass TCP or install fake crypto. XNU already contains SHA256/HMAC implementations; AES source there is a dispatch wrapper. Full future provider interface libkern/libkern/crypto/register_crypto.h. Apple/corecrypto default README describes evaluation-only licensing, so no external corecrypto was fetched or staged; BearSSL selected for this portable AES integration.
- **0019 pthread FIXED in QEMU and observed beyond its init on phone**: 45s QEMU `research/q0019`, zero unknown glyphs, then AES panic. Actual-kernel API checks PASS `research/check-pthread19.gdb` / `.log`, guest `research/q0019-api`: function/callback table identity, actual provider pthread_init returns, locks/global hash/threadcall/typed zones initialized, thread_selfid via syscall shim + buffer bounds, process hash init/delete via shims restores NULL. Monitor quit prints expected remote-disconnect after PASS. All fixture guests stopped.
- **0019 provider**: genuine Apple libpthread-514.100.2 commit `a6c70ca0e544347a0377babf748f831577ec24f9`, exact paired release with xnu8796.141.3 in distribution-macOS/macos-135/release.json (`research/macos-135-release.json`). 0019 shell stage runs before patches (workflow copies *.sh); three SWEET wrapper TUs use kext BSD header view, vm_fault_external, renamed provider-side pthread_kern/current_uthread; register real function table at EARLY_BOOT SECOND, real pthread_init later. Stager applies bounded address casts, zero user addresses, proper panic format. SWEET opaque ksyn storage enlarged48->56 for trailing kwe_uth, checked with provider static assertion. Local pre-CI TargetConditionals fixture was syntax-only; artifact bt23 carries actual SDK header and CI is authoritative.
- **Phone0019 temporarily booted** 2026-10-15 15:51:36 UTC: exactly one FASTBOOT/product sweet verified; send OKAY0.406s, boot OKAY0.134s, total0.546s, exit0 (`research/boot0019.log`). Image `shim/out/shim-boot-0019.img`, 16,457,728 bytes, SHA256 `19429a57e486b856880df9e27327ed488e0139345311fdb0350974e72103a9a1`. Phone ADT/payload `shim/out/sweet-0019.adt` / `payload-0019.bin` timer19, unpacked ramdisk byte-match (`check-0019`). EntryPA0xb146b488, kernelendPA0xb2000000, boot_argsPA0xb2004000/top0xb2008000. Last user FASTBOOT+USB reply preceded this boot; do not assume phone is still in FASTBOOT.
- **CPU0018 physically verified** by `IMG_20261015_173951 (2).jpg`: boot CPU0 registered, real SGI1 verified, initialization complete -> BSD/MAC/mbinit -> old pthread-table panic. SweetCPU uses legacy IOCPU, ml_processor_register/processor_start/initCPU, real SGI1 clears SIGPdisabled before max CPUs completion; no forced flags/skipped waits. CPU APIs and two-CPU rejection passed (`research/check-cpu18.gdb`, `q0018-api`, `q0018-smp`). Single core; SMP/suspend unsupported. CURRENT SWEET has NO USE_APPLEARMSMP.
- QEMU next run: generate fresh `tools/mkadt.py research/q0020.adt --qemu` (timer27), mkpayload with `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`, then `tools/qemu-sweet.py PAYLOAD OUT --secs 45 --kernel KERNEL --fail-on-panic`. 12s can miss later panics because IORTC waits30s. Runner snapshot only: panic exit2, blank/incomplete exit3; absence of panic does not mean complete boot. Qualcomm XPU/ABL/panel and phone SMC PSCI require hardware checks.
- Local checker `tools/xnu-cc.py research/run23.log research/bt23 FILTER --per-file`, XNU_CC_OBJDIR for objects. For new BearSSL TUs add XNU_CC_EXTRA with -I bt23/xnu/libkern/crypto/sweet_bearssl/{inc,src}. Patch checker `research/check-patch-order.py` uses temporary upstream index; source fixtures/artifacts ignored under research. GDB mask registers to unsigned64, use function-entry manual-call return traps, discard manipulated guests. No QEMU guest intentionally left running.

## Session log and earlier handoffs (superseded by the active summary above)

### Prior 0019 transition notes (superseded)

- Goal: iOS 16 generation XNU on sweet. Always commit completed work, use gh/Actions for kernel builds, keep this handoff current. Temporary fastboot boot only; preserve Android/backups.
- **0019 build PASSED** Actions `38763072907` (4m5s), implementation `256d3f0`. Kernel `xnu/out-0019/kernel.development.sweet`, SHA256 `a27df7568f03fb541cf0e8024c0a594fa178a85054fcf9eba29158cbd4c5ecc0`; checksum verified. Log/source `research/run23.log` / `research/bt23`; all seven patched files and staged provider adaptations match local compile inputs. Prefer bt23 as the next baseline.
- **0019 pthread FIXED in QEMU**: 45s run `research/q0019` registers provider, passes mbinit, DLIL SUM16 checks and DLIL thread scheduling. Actual-kernel GDB checks `research/check-pthread19.gdb` / `.log` PASS: function/callback table identity, real _pthread_init returns, locks/global hash/cleanup threadcall/typed zones non-NULL, thread_selfid via actual syscall shim with output bounds, process hash init/delete through actual shims restores NULL. Guest stopped after tests; no fixture left running. GDB monitor quit prints expected remote disconnect after PASS.
- **Current next blocker:** QEMU0019 panic data abort FAR0x78 at `aes_encrypt_key128` PC`0xfffffe00079174e0`, called by `tcp_init` LR`0xfffffe000767c3a4`. Disassembly confirms `g_crypto_funcs == NULL`, loading ccaes_cbc_encrypt at+0x78. General crypto registration is still absent; DO NOT bypass TCP or supply fake crypto. Next integrate a genuine compatible crypto implementation/provider (AES first needed here, other digest/mode APIs also used). XNU contains SHA256/HMAC but no AES implementation; its corecrypto_aes.c is a dispatch wrapper. Full crypto table is in libkern/libkern/crypto/register_crypto.h.
- **0019 phone image prepared and verified**, user reports FASTBOOT+USB again; exactly one device/product sweet verified. `shim/out/shim-boot-0019.img`, 16,457,728 bytes, SHA256 `19429a57e486b856880df9e27327ed488e0139345311fdb0350974e72103a9a1`; unpacked ramdisk matches phone payload (`shim/out/check-0019`, `payload-0019.bin`, `sweet-0019.adt`, timer19). EntryPA0xb146b488; kernelendPA0xb2000000; boot_argsPA0xb2004000/top0xb2008000. Usual debug cmdline cpus=1/-noprogress. **Temporarily booted 2026-10-15 15:51:36 UTC**: send OKAY0.406s, boot OKAY0.134s, total0.546s, exit0 (`research/boot0019.log`). User asked for photo after45s. Await the photo; DO NOT reboot again before observation. Bootloader acceptance only; no0019 physical progress observed yet.
- **Hardware photo confirms 0018 progress** (`IMG_20261015_173951 (2).jpg`): real phone prints `SWEET CPU: boot CPU 0 registered, SGI1 verified, initialization complete`, enters BSD/MAC, reaches `mbinit: done`, then the same missing-pthread-function-table panic as QEMU at pthread_shims.c:318. CPU completion/interrupt delivery is now physically observed. The photo was received; no further phone boot performed yet.
- **Patch0019 committed/pushed `256d3f0`; Actions `38763072907` queued/running:** https://github.com/dipa-studio/ios-sweet/actions/runs/38763072907. Do not launch a duplicate. genuine Apple OSS libpthread-514.100.2 provider (immutable commit `a6c70ca0e544347a0377babf748f831577ec24f9`), paired with xnu-8796.141.3 by Apple's `distribution-macOS/macos-135/release.json` (`research/macos-135-release.json`). `0019-stage-libpthread.sh` fetches/stages it before patches; workflow now copies shell scripts too. SWEET-only wrappers compile all three real kernel sources, rename provider-side symbols, use kext header view and vm_fault_external, register the real function table at EARLY_BOOT SECOND. Existing pthread_init then runs the actual provider initialization. No pthread wait/syscall is bypassed. Source staging includes bounded explicit address casts/zero user addresses and panic-format fixes. SWEET kernel opaque ksyn storage enlarged from48 to56 bytes to contain this provider's trailing kwe_uth, with a provider size assertion. Local compile of three provider TUs plus pthread_shims/kern_fork passed; imports resolve and provider exports do not collide with existing kernel symbols. All19 patches apply and changed sources match (`research/patch0019-check.log`). Source sandbox `research/bt22/xnu`, originals `research/pre0019`, objects `research/pthread-objs`. Local TargetConditionals fixture only has target macros for syntax; CI stages the real macOS SDK header and is authoritative. Next Actions artifacts: out-0019 / bt23 / run23.log; run QEMU >=45 seconds. 0019 CI is pending; no 0019 QEMU run yet.
- **Latest kernel implementation `e7d9550`, patches0001-0018; CI `38760004398` PASSED (3m56s)**: https://github.com/dipa-studio/ios-sweet/actions/runs/38760004398. No build is pending. Kernel `xnu/out-0018/kernel.development.sweet`, SHA256 `b3c8d59acc88f93a2c836eb2527a05ed4fbaac7c580a2ea8c3b795137f4aadf1`; checksum verified. Source/log `research/bt22` / `research/run22.log` (baseline for patch0019; no nested .git). New driver/config sources match CI exactly.
- **CPU wait FIXED in QEMU**: SweetCPU uses legacy IOCPU, validates single boot CPU, ml_processor_register -> processor_start -> initCPU real SGI1 -> scheduler/IOKit completion -> ml_set_max_cpus. It does NOT force a flag or skip a wait. CPU APIs passed: Running CPU0, processor_array[0] association, SGI signalling enabled, name, unsupported secondary-start error (`research/check-cpu18.gdb` / `.log`, `research/q0018-api`). Single-core only; SMP and suspend unsupported. Two-CPU topology fails explicitly (`research/q0018-smp`).
- **Next blocker reproduced in 45s QEMU** (`research/q0018-long`, zero unknown glyphs): BSD initialization reaches MAC framework, network mcache and `mbinit: done`, then **`pthread kernel extension not loaded (function table is NULL). @pthread_shims.c:318`**. Do not bypass pthread_init: find/integrate the matching open-source libpthread kernel implementation and register its real function table. A 12s run `research/q0018` looked panic-free before the 30s IORTC wait elapsed; use >=45s or GDB checkpoints for the next iteration. No userland/root filesystem boot yet.
- **Phone 0018 TEMPORARILY BOOTED** 2026-10-15 **15:31:15 UTC**: exactly one FASTBOOT device verified product sweet; send OKAY0.400s, boot OKAY0.134s, total0.539s, exit0. Local log `research/boot0018.log`. This confirms bootloader acceptance only. User was asked for a photo; await it, DO NOT reboot the phone again before their observation. No flashing performed.
- Current hardware image `shim/out/shim-boot-0018.img`, **16,437,248 bytes**, SHA256 **`b7a23595d143a76c72c9fe2cfcc5d77a8f16987b5af1a2703bbffdfa6922908c`**. Phone ADT/payload `shim/out/sweet-0018.adt` / `shim/out/payload-0018.bin` (timer19), unpacked ramdisk exactly matches payload (`shim/out/check-0018`). EntryPA0xb146b488; kernelendPA0xb1ffc000; boot_argsPA0xb2000000/top0xb2004000. Command line `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`.
- QEMU payload/ADT `research/q0018.bin` / `research/q0018.adt` (timer27). Run `python3 tools/qemu-sweet.py research/q0018.bin research/NEW-OUT --secs 45 --kernel xnu/out-0018/kernel.development.sweet --fail-on-panic`; fresh ADT seed for a new payload. Output fb.png/fb.txt/regs.txt/qemu.log/result.json. Visible panic exit2; blank/incomplete decode exit3; no_panic_observed is snapshot-only, not successful boot. -noprogress prevents the graphical indicator corrupting console text. Qualcomm XPU/bootloader/panel and SMC PSCI still need physical validation.
- Patches0014-0017 fixed PRNG registration/direct SHA256, explicit unavailable trust-cache service with rejected loads/no matches, built-in SweetPlatformExpert, and timer PPI from ADT (phone19/default19, QEMU27). All their checks/artifacts are preserved in the history below. CURRENT SWEET has NO USE_APPLEARMSMP; do not assume gCPUIC/gAIC/gPMGR AppleARMSMP globals exist.
- Local single-file checker: tools/xnu-cc.py research/run22.log research/bt22 FILTER --per-file, output via XNU_CC_OBJDIR. Patch checker `research/check-patch-order.py` applies all18 via temporary index, matches sandbox sources. Sandbox for0018 `research/bt21/xnu`, originals `research/pre0018`, objects `research/cpu-objs`. First compiler issues were missing machine/atomic.h and nonexistent getNameSymbol; fixed and both TUs compiled successfully before CI. Fixtures/artifacts under research are local-only/ignored.
- GDB: mask int(register)&((1<<64)-1) for unsigned kernel pointers; use function-entry manual-call return traps, discard manipulated guests. No QEMU fixture is intentionally left running.

### Earlier active handoff snapshot (superseded)


- **0018 milestone:** Actions `38760004398` PASSED (3m56s), checksums passed, new CPU/build files byte-match CI tree. Kernel `xnu/out-0018/kernel.development.sweet`, SHA256 `b3c8d59acc88f93a2c836eb2527a05ed4fbaac7c580a2ea8c3b795137f4aadf1`; log/tree `research/run22.log` / `research/bt22`. QEMU `research/q0018` 12s: boot CPU registered, SGI1 verified, CPU initialization complete, **BSD initialization entered** (copyright, MAC framework, buffer headers), no panic/zero unknown glyphs. Direct CPU API checks PASS (`research/check-cpu18.gdb`, `.log`, `research/q0018-api`): CPU0 Running, processor matches processor_array[0], real SGI enabled, CPU name correct, secondary-start returns KERN_NOT_SUPPORTED. Two-CPU ADT correctly panics before registration (`research/q0018-smp`). Longer 45s run `research/q0018-long` still running; next BSD wait/fault not yet identified.
- **Prepared 0018 phone image:** `shim/out/shim-boot-0018.img`, 16,437,248 bytes, SHA256 `b7a23595d143a76c72c9fe2cfcc5d77a8f16987b5af1a2703bbffdfa6922908c`. Phone ADT/payload `shim/out/sweet-0018.adt` / `shim/out/payload-0018.bin`, usual debug args with cpus=1 and -noprogress. Unpacked ramdisk byte-compared (`shim/out/check-0018`). Entry PA remains 0xb146b488; kernel end PA 0xb1ffc000, boot_args 0xb2000000/top0xb2004000. User reports FASTBOOT+USB ready; read-only check found exactly one product sweet. NEXT: temporary fastboot boot of this verified image, record send/boot results, then await hardware photo without rebooting again.
- Patch 0018 committed/pushed **`e7d9550`**; Actions **`38760004398`** queued/running: https://github.com/dipa-studio/ios-sweet/actions/runs/38760004398. Do not launch a duplicate run.
- Active continuation: patch **0018** implemented in `iokit/Kernel/arm/SweetCPU.cpp` + arm64 source list + exact qcom,kryo470 personality. Keeps legacy IOCPU, accepts ONLY the single boot CPU, fills ml_processor_info from parsed topology with CPU object as cpu_id, calls ml_processor_register and processor_start. initCPU sends SGI1 via ICC_SGI1R_EL1 and waits (1-second deadline) for real cpu_signal_handler to clear SIGPdisabled. Only after successful startup calls ml_cpu_init_completed/cpusRunning and publishes completion/max CPUs. Secondary startup/suspend are explicitly unsupported. Local C++ compile passed for both TUs (`research/cpu-objs`), all imports resolve, object contains dsb ishst / ICC_SGI1R / isb; all18 patches apply in order and match sources. Sandbox `research/bt21/xnu`, originals `research/pre0018`; next CI artifacts should use `xnu/out-0018`, `research/bt22`, `research/run22.log`. User has the phone available; first run QEMU, then prepare/temporarily boot useful hardware progress.
- Goal: iOS 16 generation XNU on Redmi Note 10 Pro / sweet. User wants persistent progress, commits, kernel builds via `gh` Actions, and this handoff kept current across agent switches. Temporary `fastboot boot` only; preserve Android and backups.
- Latest kernel implementation: **`aa3f991`**, patches **0001-0017**. All four new Actions builds PASSED: 0014 `38748543058` (4m0s), 0015 `38749549779` (4m3s), 0016 `38750733996` (4m3s), 0017 **`38751507437` (4m7s)**. Latest: https://github.com/dipa-studio/ios-sweet/actions/runs/38751507437. No build is pending.
- **Current blocker: CPU initialization has no SWEET CPU driver.** QEMU registers SweetPlatformExpert, then kernel_bootstrap_thread waits in **kperf_init_early -> ml_wait_max_cpus**. Confirmed via actual-kernel GDB call stack (`research/locate-cpu-wait2.log` / `.symbols`) and normal-run `max_cpus_initialized == 2` (MAX_CPUS_WAIT). Do not force the flag or remove the wait. Next implement real CPU registration and completion using XNU's CPU/processor interfaces. CURRENT SWEET uses legacy IOCPU (`USE_APPLEARMSMP` is absent); enabling AppleARMSMP is a separate architectural change requiring GIC/PMGR services. No userland/root filesystem boot yet.
- Fixed this session: PC=0 unregistered PRNG callback (0014, built-in HMAC-SHA256 provider + direct entropy/write_random hashing); absent Image4/AMFI trust-cache service (0015, unavailable runtime, loads rejected and queries no-match); missing platform driver (0016, SweetPlatformExpert exact personality); incorrect QEMU timer IRQ (0017, validated ADT PPI property, phone default19/QEMU27).
- **Latest kernel** `xnu/out-0017/kernel.development.sweet`, SHA256 `055a80190ab998b4df9203f1102072dcc78faad1779a537a514e522b682b582f`. Buildtree/log **`research/bt21` / `research/run21.log`**; all nine changed C/C++/header/config files byte-match local checked sources. Checksums passed. Entry PA `0xb146b488`, kernel end PA `0xb1ff4000`. Preferred baseline for patch 0018: `research/bt21/xnu` (no nested .git).
- **QEMU verified**: `research/q0017-noprogress` ran **30 seconds**, no visible panic, **zero unknown glyphs**, IRQ exceptions returned normally (4 IRQs, tickless idle). It still waits for CPU completion, so this is NOT a completed boot. Earlier 15s run `research/q0017` captured 122 IRQs, timer value27, GICR_ISENABLER0 `0x08000002`, and CPU WAIT; graphical progress overlay corrupted 13 glyphs. Add **`-noprogress`** to keep the console readable. New runner classifies incomplete/blank decoding explicitly (exit3 with --fail-on-panic), visible panic exits2. Snapshot-only; use regs/GDB to diagnose stalls.
- Current QEMU payload/ADT: **`research/q0017-noprogress.bin` / `research/q0017.adt`**. Run: `python3 tools/qemu-sweet.py research/q0017-noprogress.bin research/NEW-OUT --secs 30 --kernel xnu/out-0017/kernel.development.sweet --fail-on-panic`. Generate fresh seed with mkadt for a new payload. QEMU virt models generic ARM execution, not Qualcomm XPU/bootloader/panel or the phone's SMC PSCI conduit.
- **Prepared phone image**: **`shim/out/shim-boot-0017.img`**, 16,416,768 bytes, SHA256 `51ff7fe30b4a6668c3452ffe1da922c5ac30cb8de60625c2a652587ecbd0ec92`. Phone ADT/payload `shim/out/sweet-0017.adt` / `shim/out/payload-0017.bin` (timer19). Unpacked ramdisk exactly matches payload (`shim/out/check-0017`). Command line **`debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`**. NOT sent to phone this session: known CPU-driver wait should be addressed in QEMU first. Last actual phone boot remains 0013; user's latest photo `IMG_20261015_160033.jpg` confirmed the old PRNG PC=0 panic.
- Direct QEMU/GDB API checks: PRNG 600-byte chunking, buffer bounds, changing output, zero-length reads/writes, user reseed, contended refresh, generator1 init/output PASS again on latest0017 (`research/check-prng17.gdb` / `.log`, guest `research/q0017-api`). Trust-cache unavailable-service/argument/output cases PASS on0015 (`research/check-trustcache.gdb` / `.log`). Platform registration + target/product normal/truncated/zero/null/bounds PASS on0016 (`research/check-platform.gdb`, `research/check-platform3.log`). Different platform ID correctly retains IOPanicPlatform fallback (`research/q0016-negative`). Timer PPI32 and size2 correctly panic (`research/q0017-badppi`, `research/q0017-badsize`); omitted property selects19 (`research/q0017-default`). These are API/bring-up checks, not crypto certification or SMP scheduling validation.
- Local checker **`research/check-patch-order.py`** verifies all17 patches and changed source equality using a temporary upstream git index. Fixtures/logs/artifacts under research are local-only and ignored. For GDB unsigned kernel pointers, mask `int(register_value) & ((1<<64)-1)`; x30/x29 may be rendered signed. Stop at a function entry for manual-call return traps, and discard each manipulated guest after API tests. All fixture QEMU guests have been stopped.


- User wants persistent progress toward iOS 16, commits for completed changes, builds through `gh` / GitHub Actions.
- Keep this file current at every milestone so another agent can resume immediately, including when usage limits interrupt a session.
- Patch 0017 committed/pushed `aa3f991`, Actions `38751507437` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38751507437. Next artifacts: xnu/out-0017, research/bt21, research/run21.log. Do not duplicate.
- Patch 0016 kernel downloaded/checksum verified: SHA256 `d06336c54baa06e0df6561a8bd7df98a9b9f7502c9381c62d1a44c045cd1ce46`, `xnu/out-0016/kernel.development.sweet`; buildtree/log `research/bt20` / `research/run20.log`. QEMU `research/q0016` (10s) prints platform expert registered, zero unknown glyphs, no panic observed BUT stalls (timer mapping still wrong until 0017). Direct actual-kernel GDB tests passed for registered instance and target/product names, including maxLength=1/0 and NULL, sentinels intact (`research/check-platform.gdb`, `research/check-platform3.log`, `research/q0016-api3`). First two fixture attempts used premature/wrong return breakpoints; discarded, corrected fixture uses start-return observation then a function-entry return trap. No phone boot yet.
- Patch 0016 CI `38750733996` PASSED (4m3s); artifacts not yet downloaded/booted. Patch 0017 prepared to fix QEMU timer fidelity BEFORE interpreting future no-panic/stall runs: actual `qemu-system-aarch64 -M virt,gic-version=3,dumpdtb=research/qemu-virt.dtb ...` timer property has virtual PPI 11 => INTID 27, while phone uses INTID 19. Kernel formerly hardcoded 19. New optional /arm-io/gic virtual-timer-intid property selects the PPI (validate exact u32 and 16..31); missing property retains phone default 19. One RO-late global drives GIC enable and sleh_irq dispatch. tools/mkadt.py emits phone19/QEMU27. Both C TUs compile (`research/timer-objs`), ADT serialize/parse checks confirm values/addresses, all 17 patches apply and match. Sandbox `research/bt19/xnu`, originals `research/pre0017`; initial local sleh compile failed due read-only exported header not updated, fixed export and BOTH recompiled successfully. CI exports normally.
- Architecture correction for next agent: CURRENT SWEET board_config does NOT define USE_APPLEARMSMP; it compiles the legacy IOCPU path, not cpu_boot_thread/IOPMGR AppleARMSMP. Existing roadmap mentions AppleARMSMP as a future approach. Do not look for gCPUIC/gAIC/gPMGR globals in the current kernel: they are absent. Determine the actual CPU-init wait after platform registration before choosing the next driver architecture.
- Patch 0016 committed/pushed as `0dcdf5c`; Actions `38750733996` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38750733996. Next artifacts: `xnu/out-0016`, `research/bt20`, `research/run20.log`.
- Patch 0016 implemented: new `iokit/Kernel/arm/SweetPlatformExpert.cpp`, arm64 source list, built-in personality in KernelConfigTables.cpp guarded by ARM64_BOARD_CONFIG_SWEET. Matches only xiaomi,sweet at score 10000, keeps IOPanicPlatform fallback for other hardware. Uses IODTPlatformExpert base initialization/power domain and createNubs CPU logical-ID annotation; overrides top-level publishing because base requires nonexistent Apple NVRAM /options. Publishes real CPU/top-level nodes, provides bounded target/product names, and registers the platform service. GIC/PMGR and arm-io child-bus drivers remain separate work. Both C++ TUs compile (`research/platform-objs`), all imports resolve against 0015/own objects, all 16 patches apply and sources match. Sandbox `research/bt19/xnu`; originals `research/pre0016`. NEXT: full Actions/QEMU; inspect CPU boot waits after the platform registers, do not equate absence of panic with successful boot.
- Patch 0015 CI `38749549779` PASSED (4m3s), checksum verified. Kernel `xnu/out-0015/kernel.development.sweet`, SHA256 `ca1b424a596c7058550ad1659080a333e7643270f019656401d80bf2b876645f`; buildtree/log `research/bt19` / `research/run19.log`. QEMU `research/q0015` (10s, zero unknown glyphs) passes trust-cache handling and machine lockdown, then panics `Unable to find driver for this platform: "xiaomi,sweet" @IOPlatformExpert.cpp:2319` (IOPanicPlatform fallback). Next concrete work: add a built-in SWEET IODTPlatformExpert personality/class; keep the fallback for mismatched platforms. No phone boot yet.
- Direct trust-cache API checks PASSED in the actual 0015 kernel under QEMU/GDB (`research/check-trustcache.gdb`, `research/check-trustcache.log`, `research/q0015-api`): runtime pointers NULL, hash/UUID queries return NOT_FOUND, typed/untyped/legacy loads return NOT_SUPPORTED, capabilities outputs zero and bounds preserved, invalid-argument cases still reject. Second PRNG check also verifies contended refresh returns immediately (`research/check-prng2.log`, `research/q0014-api2`).
- Patch 0015 committed/pushed `8f851d4`, Actions run `38749549779` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38749549779. Do not duplicate.
- Direct PRNG API checks passed in the REAL 0014 kernel under QEMU/GDB, stopped before trust-cache init (`research/check-prng.gdb`, log `research/check-prng.log`, guest `research/q0014-api`). Invoked read_random for 600 bytes (chunking and buffer sentinel intact), repeated output differed, zero-length read/write succeeded, write_random(64-byte input) succeeded, secondary generator 1 initialized and generated distinct output. QEMU log has no exceptions during these checks; guest was discarded after the tests. This tests API behavior, not a cryptographic certification or SMP scheduling.
- Patch 0015 implemented in `bsd/kern/kern_trustcache.c`: SWEET with missing Image4/AMFI marks both runtime pointers NULL and logs service unavailability. Existing static loader handles NULL. Public load APIs return KERN_NOT_SUPPORTED; hash/UUID queries return KERN_NOT_FOUND; capabilities outputs are zero with KERN_NOT_SUPPORTED. Invalid-input checks remain, and Apple paths/available-provider initialization retain existing behavior. No trust is granted. Local arm64 compile passed (`research/trustcache-objs`), all 15 patches apply and sources match. Sandbox `research/bt18/xnu`, baseline `research/pre0015`. Next: Actions/QEMU and direct unavailable-service API checks using GDB.
- Patch 0014 CI `38748543058` PASSED (4m0s), artifact checksum passed. Kernel `xnu/out-0014/kernel.development.sweet`, SHA256 `741de20c851755d333759d7e668346f3edb4f325efac4cc7c142d80d1f5ff75c`; buildtree/log `research/bt18` / `research/run18.log`. QEMU run `research/q0014` (10s, zero unknown glyphs) prints `SWEET PRNG: HMAC-SHA256 registered`, passes commpage initialization, loads vnguard policy, THEN panics `image4 interface not available @kern_trustcache.c:253`. PC=0 fault FIXED IN QEMU. No new phone boot performed.
- Next work: SWEET currently has neither AppleImage4 nor AMFI. `trust_cache_runtime_init` unconditionally requires both; unlike BCM2837, SWEET enters this startup path. Investigate an explicitly unavailable trust-cache runtime with rejected loads/no query matches while providers are absent; preserve all Apple-platform checks. Merely skipping the startup call would leave later AMFI/IMG4 dereferences unsafe. Existing load_static_trust_cache already handles trust_cache_rt == NULL.
- QEMU runner now supports `--kernel MATCHING_KERNEL --fail-on-panic`: runtime font extraction from the payload produces exact `fb.txt`, plus `result.json`; a visible panic returns 2. Baseline 0013 re-run `research/q0013-text` detects the same NULL callback panic with zero unknown glyphs. Synthetic glyph roundtrip (including literal `?`) and invalid-VA rejection passed. Screen rows remain circular, not a full log; `no_panic_observed` does not prove successful boot. README has the invocation.
- Patch 0014 committed/pushed as `be61f99`; Actions run `38748543058` is running: https://github.com/dipa-studio/ios-sweet/actions/runs/38748543058. Do not start a duplicate build.
- 2026-10-15 active: hardware photo `IMG_20261015_160033.jpg` confirms the same PC=0 instruction-fetch abort as QEMU (LR `0xfffffe0007443fe4` = read_random). Independently reproduced with kernel 0013 in `research/q0013` (8-second run); prior allocator fix works. Latest tooling commit reviewed: `8d66d2d`.
- Patch 0014 implemented: registers a built-in HMAC-SHA256 DRBG provider at EARLY_BOOT/FIRST, before commpage creation. A shared mutex serializes output/reseed across CPUs (correctness first; no per-CPU scaling yet), initgen validates CPU identities, refresh uses try-lock and the existing entropy filter/health tests. SWEET entropy accumulation and write_random now use the built-in SHA256 directly; no incomplete g_crypto_funcs table is registered. The existing bootloader-seed requirement remains enforced.
- Local validation: both PRNG translation units compile to arm64 objects in `research/prng-objs`; no SHA256 wrapper imports remain. All 14 patches apply in CI order and resulting PRNG sources match local inputs. Sandbox `research/bt17/xnu`, originals `research/pre0014`, checker `research/check-patch-order.py`. NEXT: Actions kernel build, download to `xnu/out-0014`, run the QEMU payload before requesting another phone boot. Baseline QEMU ADT/payload are `research/q0013.adt` / `research/q0013.bin`.
- 2026-10-15 08:15 QEMU EMULATION WORKS (`tools/qemu-sweet.py`, see "QEMU emulation" below). Patch 0013 VERIFIED in QEMU: boot passes kmem_crypto_init and ulock_initialize. NEXT BLOCKER (reproduced in QEMU, expected on hardware too): `pmap_create_commpages -> random() -> RandomULong -> read_random -> prng_funcs.refresh == NULL` (instruction fetch abort at pc 0, lr read_random+0x34). Nobody calls `register_and_init_prng` (closed corecrypto kext does it on Apple). Fix = patch 0014: SWEET-registered cckprng_funcs provider (osfmk/prng/prng_random.c); note random_cpu_init also panics while !prng_ready. The 07:26 hardware boot of 0013 will most likely show this same panic.
- Previous hardware boot: patch 0013 (`a3a46e2`) CI `38686345689` PASSED (3m44s), image `shim/out/shim-boot-0013.img` was temporarily booted on 2026-10-15 at 05:26 UTC: send OKAY (0.407s), boot OKAY (0.134s), total 0.546s. Superseded by the hardware/QEMU PC=0 observation and patch 0014 work above.
- Latest kernel implementation commit: `8f851d4`. Latest successful kernel CI: `38749549779` (patch 0015), https://github.com/dipa-studio/ios-sweet/actions/runs/38749549779.
- CURRENT image: `shim/out/shim-boot-0013.img`, 16,412,672 bytes, SHA256 `110d1c46aee4b60253db24b6535e9781c4cf0fd25ca2a70e9016cf81378cea25`. Kernel `xnu/out-0013/kernel.development.sweet`, SHA256 `f0b1d375995316c550ce7014ff234d8271e9b9ac2857795ceff41b76ba2cd740`, plain arm64. CI log confirms 0013 applied; checksums passed. Entry PA `0xb146b488`, kernel end PA `0xb1ff4000`.
- Current log/buildtree: `research/run17.log`, `research/bt17` (extracted, no nested .git; preferred baseline for patch 0014). Payload/ADT: `shim/out/payload-0013.bin`, `shim/out/sweet-0013.adt` (includes /defaults). Unpacked ramdisk byte-compared with payload in `shim/out/check-0013`.
- Patch-0012 original image: `shim/out/shim-boot-0012.img`, 16,412,672 bytes, SHA256 `cf4b8ec4a65a51888d71c17d09cd0831d6b761b90e2531fdb8e575c32f4fd24a`. Kernel `xnu/out-0012/kernel.development.sweet`, SHA256 `69b78f49f6459cc63a0af44e7cf73c1779ef72130ef72b12ae86c88183ffc742`; checksums passed and sweet_early_relocate symbol present. Entry PA remains `0xb146b488`, kernel end PA `0xb1ff4000`.
- Patch-0012 log/tree: `research/run16.log`, `research/bt16` (extracted, no nested .git; preferred baseline for next patch). CI log confirms 0012 applied. Payload `shim/out/payload-0012.bin`, ADT `shim/out/sweet-0012.adt`; packed ramdisk byte-compared with payload in `shim/out/check-0012`.
- Active payload command line: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam`.
- Latest observed photo: `/home/dipa/Downloads/IMG_20261015_071346.jpg` (local only). Stages 9 (kernel framebuffer VA), 10 (TTBR0 off/TLB flush), 11 (physmap table pointers) and 12 (calling pmap_bootstrap) all visible. Persistent diagnostics are VERIFIED ON HARDWARE.
- pmap.c:1987 is the assert after SecureDTLookupEntry(NULL, "/defaults", &entry). The pmap-max-asids property itself is optional; an empty /defaults uses XNU's MAX_ASIDS. Do not bypass the assertion or guess an ASID limit.
- Previous payload-only fix image: `shim/out/shim-boot-0012-defaults.img`, SHA256 `3ff61a268853c6ea7cbf284833f1346a1d791980ca366b6e1a215a86e040512b` (16,412,672 bytes). Same kernel/CI as 0012; ADT 2516 bytes (`shim/out/sweet-0012-defaults.adt`), root /defaults present with no ASID override. Payload `shim/out/payload-0012-defaults.bin`; unpacked ramdisk exactly matches payload (`shim/out/check-0012-defaults`). This boot reached the allocator crypto NULL-pointer fault below.
- Latest hardware photo: `/home/dipa/Downloads/IMG_20261015_071608.jpg` (local only). Normal serial framebuffer console works. VM bootstrap/zone initialization, logging, scheduler, IPC, KDP and ulock_initialize print before the kmem_crypto_init data abort. gPanicBase warnings are expected because PLATFORM_PANIC_LOG_DISABLED is configured, and are not the faulting call.
- Faulting instruction: `ldr x0, [x8, #0x150]` in crypto_random_kmem_ctx_size; x8 = NULL g_crypto_funcs. Source: libkern/crypto/corecrypto_rand.c and osfmk/vm/vm_kern.c:2448. rand.h explicitly permits NON-cryptographic kmem RNGs, but requires no locking in generation and context size <=256. Do not substitute read_random here: the global PRNG provider is also unregistered and kmem generation must remain lock-free. early_random may seed contexts during single-CPU bootstrap; it uses the built-in HMAC-SHA256 DRBG with the host-generated ADT seed.
- Patch 0013 keeps the replacement entirely private to vm_kern.c: SplitMix64 state is 8 bytes per CPU/sizeclass, initialized using early_random. Uniform selection uses rejection sampling to avoid modulo bias. Other platforms retain the original corecrypto calls; SWEET's generic crypto APIs/registration remain untouched. Reference: https://prng.di.unimi.it/splitmix64.c (Sebastiano Vigna, public domain).
- Validation: vm_kern.c compiles to arm64 (`research/kmem-rng-objs`, using `research/run16.log` + `research/bt16`). Undefined symbols include early_random and NO crypto_random_* references. All 13 patches apply in CI order and match the checked source. Original saved as `research/vm_kern.pre0013.c`.
- RNG host checks: extracted actual new helper in `research/kmem-rng-check.c`; 16,384 outputs match the author's downloaded reference (`research/splitmix64-reference.c`), 147,456 bounded outputs satisfy their limits (including 1 and UINT64_MAX), and a forced rejection advances correctly. Full kernel crypto services and the registered kernel PRNG provider remain future work; this patch only supplies the allocator RNG contract.
- Patch-0011 artifacts: kernel `xnu/out-0011/kernel.development.sweet`, log `research/run15.log`, extracted buildtree `research/bt15` (no nested .git). CI log confirms patch 0011 applied; artifact checksums passed. Kernel is plain arm64, entry VA `0xfffffe000746b488` -> PA `0xb146b488`, kernel end PA still `0xb1ff4000`.
- Patch-0011 image: 16,412,672 bytes, SHA256 `29ad5cbfc752473de4af2c471a24a8b34f92da4bff5afec65c614b90cd3ad53a`. Kernel SHA256 `8ac3a956894b09f7bd0d890de8c5de38c65513287feb1e1bcd12db8ca772ee5c`. Payload/ADT in `shim/out/payload-0011.bin`, `shim/out/sweet-0011.adt`. Header unpacked/checked in `shim/out/check-0011`.
- First patch-0011 attempt timed out waiting for FASTBOOT; superseded by the successful send/boot recorded above. Do not repeat the boot before the user reports the current screen.
- Patch-0011 photo: `/home/dipa/Downloads/IMG_20261015_070218.jpg` (local only). Visible stages 1-8 and old red shim text; entry PA B146B488, boot args PA B1FF8000 / top B1FFC000. No evidence yet locating execution after stage 8.
- Patch-0012 sandbox: `research/bt15/xnu` (NO nested .git). Proposed permanent debug VA: VM_MIN_KERNEL_ADDRESS + ARM_TT_L2_SIZE, in the reserved gap below gVirtBase and above low globals; map the same PA framebuffer as Normal-NC in the final TTBR1 before arm_vm_physmap_init changes phystokv(). Keep the early panic sink until normal kprintf startup.
- Patch 0012 validation: all 4 changed C translation units compiled to arm64 objects in `research/post-ttbr0-objs`; internal header export added to osfmk/arm64/Makefile and recreated locally in EXPORT_HDRS for pe_serial.c. All 12 patches apply in CI order and match all 6 edited files. Baselines are saved under `research/pre0012/`.
- Actual mapping-helper host model (`research/early-map-check.c`, barrier stubbed only for host compilation) confirms: VA `0xfffffe0002000000`, L1 index 2016, L2 index 1; descriptor `0x006000009c000605` = framebuffer PA, Normal-NC, outer-shareable, EL1 RW, PXN+UXN. All other L2 entries preserved. The gap is excluded by ARM_LARGE_MEMORY's pmap_virtual_region(0).
- Next boot command line MUST add `-enable_kprintf_spam`: DEVELOPMENT disables kprintf_output by default even when DB_KPRT is set (`pexpert/arm/pe_kprintf.c`). Planned command line: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam`.

### Patch 0012 screen legend (after original stages 1-8)
- 9: kernel framebuffer VA live; 10: TTBR0 off / TLB flushed; 11: physmap pointers rebound.
- 12: calling pmap_bootstrap; 13: pmap_bootstrap returned; 14: early I/O page tables ready; 15: arm_vm_init returned.
- 16: calling serial_init; 17: fbcon config read / mapping; 18: fbcon mapping returned; 19: serial_init returned; 20: kprintf startup complete.
- Normal serial initialization clears the screen, so earlier stages may disappear before 19. If serial exists, early console disables after 20; otherwise early panic output stays enabled.
- Local source sandbox: `research/bt13/xnu` (git baseline `623b060`, patches 0001-0008 already in baseline; uncommitted export edits correspond to patch 0009). Preserve those edits when producing patch 0010.
- Previous compile log/tree: `research/run13.log`, `research/bt13`; previous kernel: `xnu/out/kernel.development.sweet`. New successful log/tree: `research/run14.log`, `research/bt14`; new kernel: `xnu/out-0010/kernel.development.sweet`.
- Current device access: patch-0012 image was temporarily started from FASTBOOT; no adb connection is expected. Never flash partitions for bring-up; use `fastboot boot` with the shim image.
- Patch `0010` lives in `xnu/patches/0010-sweet-early-framebuffer-debug.patch`. Its exact compiled sources are under `research/bt13/xnu`; generated objects are in `research/early-objs`.
- Local checks passed: all 5 changed/new translation units compiled to arm64 Mach-O objects; new console only imports `_doprnt` and `iso_font` (both exist in the previous kernel). All 10 patches applied to a temporary upstream git index in CI order, and all 7 resulting changed source files matched the local compile inputs.
- Reproduce changed-file compilation: `XNU_CC_OBJDIR="$PWD/research/early-objs" python3 tools/xnu-cc.py research/run13.log research/bt13 arm_init.c arm_vm_init.c kern/debug.c arm64/start.s osfmk/arm64/sweet_early.c --per-file` (create the output directory first). The helper now accepts explicit new source paths absent from the old artifact's Makefiles and rejects empty selections.
- Device command result: `adb reboot bootloader`, then `timeout 30s fastboot boot shim/out/shim-boot-0010.img`: send OKAY (16028 KB), boot OKAY, total 0.541s. This confirms bootloader acceptance only, not XNU progress.
- The successful boot above was on 2026-10-14. On 2026-10-15, `timeout 45s fastboot boot ...` timed out waiting for a device; no image was sent. Boot image SHA256 rechecked and unchanged. No new build is needed to repeat the trial.
- Earlier retry: `timeout 90s fastboot boot shim/out/shim-boot-0010.img` timed out (exit 124). This is superseded by the successful 2026-10-15 repeat boot recorded above; do not repeat it until the user reports the screen.
- User observation confirms MMU-on C entry, platform expert, CPU topology/bootstrap, startup bootstrap and cpu_init all complete. Failure is within arm_vm_physmap_init (non-XNU_MONITOR branch), before stage 8 / TTBR0 teardown. Old red shim text remains behind the early console; this is expected because only written rows are cleared.
- New edit/compile sandbox is `research/bt14/xnu` (extracted artifact, NO nested .git; `git diff` there would act on the parent project). Use a saved baseline file / generated unified diff, or explicitly initialize a local baseline before editing. `research/bt13/xnu` does have its own baseline git repo.
- Exact user-reported panic: `arm_vm_init.c:1606 Assertion failed: (temp_ptov_table[ptov_index - 1].va + temp_ptov_table[ptov_index - 1].len) <= physmap_end`.
- Root cause: MacOSX build sets `ARM_LARGE_MEMORY=1`; `XNU_MONITOR` is off. Block alignment in arm_vm_physmap_slide can add gaps, but the non-PPL large-memory physmap_end omitted the padding already present in the small-memory branch. Patch 0011 reserves PTOV_TABLE_SIZE*ARM_TT_TWIG_SIZE (8*32MB) in both the aperture bound and its L1 allocation; monitor/PPL builds retain zero padding. The assertion is preserved.
- Validation: `arm_vm_init.c` compiled to an arm64 object using `research/run14.log` + `research/bt14` (object in `research/physmap-objs`). All 11 patches apply in CI order and produce exactly that source. Host harness `research/physmap-check.c` extracts the real XNU slide function with page-table-write stubs: all 65,536 page-aligned slides within 1GB checked against the current kernel memory layout; old bound fails 65,504 cases, padded bound fails zero; worst overrun 0x1ffc000 bytes. Baseline source saved as `research/arm_vm_init.pre0011.c`.
- CI: https://github.com/dipa-studio/ios-sweet/actions/runs/38638702083 (success; artifacts and full log downloaded).
- New image: `shim/out/shim-boot-0010.img` (16,412,672 bytes, SHA256 `ed2c270e0081bad5ff517be415f9bfe8805463aea7866732203fa97ac737e64e`). Previous image preserved as `shim/out/shim-boot-pre0010.img`.
- New kernel SHA256: `3ee05270cc291c49589a101678f904b1b69cb2f2575e05398bce8a4e052f409a`. Artifact checksums passed; arm64 (not arm64e), early-console symbols present. Payload entry VA `0xfffffe000746a488` -> PA `0xb146a488`; kernel end PA `0xb1ff4000`. Command line remains `debug=0xc serial=3 cpus=1 keepsyms=1`.

### Patch 0010 screen legend
- Bottom-left white 64x32 tile: assembly entered `start_first_cpu`, before page tables/MMU.
- Text `SWEET EARLY 1`: entered `arm_init` with MMU on; 2: platform expert done; 3: CPU topology done; 4: CPU bootstrap done; 5: startup bootstrap done; 6: CPU init done; 7: entering `arm_vm_init`; 8: leaving bootstrap TTBR0.
- `SWEET EARLY PANIC:` prints a copy of panic arguments before normal panic handling needs per-CPU/thread state.
- In patches 0010/0011 the console disabled before TTBR0 invalidation; patch 0012 replaces that with a persistent kernel VA alias and diagnostics through normal serial/kprintf startup (see its legend above).
- Screen contents require a physical observation from the user: adb cannot capture a framebuffer after Android has been replaced by a bare-metal kernel.

## QEMU emulation (2026-10-15)
- `tools/mkadt.py OUT.adt --qemu` = same ADT with QEMU virt GICD 0x08000000 / GICR 0x080a0000 / CNTFRQ 62.5 MHz.
- `tools/qemu-sweet.py PAYLOAD OUTDIR [--secs N] [--hmp "x /8gx ADDR"] [--gdb]`: host-side copy of shim/loader.c
  (segments + ADT + boot_args at their PAs, 6-instruction stub sets x0 and jumps), `-M virt,gic-version=3 -cpu cortex-a76`
  (same ID_AA64MMFR0 as Kryo 470), 4G RAM so 0x9c000000 fb and 0xb0000000 kernel are plain RAM.
  Outputs `fb.png` (the screen), `regs.txt`, `qemu.log` (exceptions with ESR/FAR/ELR). `--gdb` halts with a stub on :1234
  (system `gdb` + `set architecture aarch64`, `hbreak *VA` works with the MMU on).
- Not emulated: Qualcomm XPUs, ABL, real panel, PSCI conduit (QEMU virt uses hvc). Hardware still decides those.
- Example: `python3 tools/mkadt.py /tmp/q.adt --qemu && python3 tools/mkpayload.py xnu/out-0013/kernel.development.sweet /tmp/q.adt /tmp/pq.bin --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam' && python3 tools/qemu-sweet.py /tmp/pq.bin /tmp/q`

## Layout
- `backup/`  partition dumps + full sdb..sdf LUNs, `SHA256SUMS` (all verified on-device == local)
- `dtb/`     live FDT (from OrangeFox), PixelOS boot.img dtb, dtbo, dts versions
- `shim/`    bare-metal boot shim (`./build.sh` -> `out/shim-boot.img`)
- `research/` mainline sm7150 dts, XNU pe_fiq.c etc, PocketDarwin patches
- `tools/unpack_bootv2.py`

## Device access
- No root in PixelOS 17 (user build, adb root disabled). Root = `fastboot boot recovery/recovery.img` (OrangeFox R12.0_2, temp boot, not flashed).
- `fastboot boot` works for stock boot.img, OrangeFox, and our shim.
- Shim resets itself via PSCI SYSTEM_RESET; IMEM restart_reason 0x77665500 is seen
  (bootreason=bootloader) but ABL still boots Android, so it does not stop in fastboot.

## Hardware facts (live DT + shim measurements)
- Framebuffer (ABL cont splash): 0x9c000000, 1080x2400, stride 4320, a8r8g8b8, panel is DSC *video* mode (no kick needed).
- RAM: 0x80000000+2G, 0x100000000+0xfcc00000. Live reserved-memory = PixelOS base dtb + sweet dtbo fixups.
- GICv3: GICD 0x17a00000, GICR 0x17a60000 (stride 0x20000, 8 PEs). GICD_TYPER 0x007B041D: SecurityExtn=1 (DS=0).
  ABL leaves GICD_CTLR = 0x10 (ARE_NS on, groups off). Writing 0x13 reads back 0x12: EnableGrp1A (bit1) is the one that matters.
- GIC Group 1 VERIFIED (shim 2c): vtimer INTID 19 = 30 IRQs/300ms, ptimer INTID 18 = 30, SGI 1 to self = 10/10.
  ICC_SRE=1 already, ICC_CTLR 0x8440, priority/enable writes stick. GICR_IGROUPR0 and GICD_IGROUPRn read 0 from NS
  even though Group 1 delivery works (RAZ in NS view) - do not use them as a health check.
  INTID 28 shows pending constantly (unknown PPI 12).
- Timer 19.2 MHz. Qualcomm PPI numbering: secure-phys/phys/virt/hyp = PPI 1/2/3/0 -> vtimer INTID 19, ptimer 18 (NOT 27/30).
- PSCI 1.1 via SMC, SMCCC 1.1. CPU_ON ok; SYSTEM_SUSPEND and SYSTEM_RESET2 unsupported.
- SMP: CPU_ON works for all 7 secondaries (MPIDR 0x81000n00).
- Entry EL1, MMU off, x0 = DTB (magic ok). ID_AA64MMFR0 = ...101122: 16K granule YES, 4K YES.
- No serial console: debug UART SE is in SPI mode on sweet. Screen is the only debug channel.

## XNU implications
- XNU GICv3 path (pexpert/arm/pe_fiq.c, VMAPPLE) puts the timer (PPI 27) in Group 0 = FIQ. With SecurityExtn=1,
  Group 0 is secure -> impossible from NS EL1. Need: Group 1 IRQ, ICC_IGRPEN1, INTID 19, and the timer handled in the IRQ path.
- XNU VMAPPLE PSCI uses `hvc`; sweet needs `smc`.
- PocketDarwin NonAppleGenericCpu.patch only swaps -mcpu=apple-h7 for -march=armv8-a.

## Milestone 3 (2026-10-14): 16K MMU verified
- ABL loads the Image at 0x80080000 (base 0x80000000 + 0x80000) regardless of text_offset=0.
- 16K granule, T0SZ=28 (L2 start, 32MB blocks), MAIR 0x44ff00, TCR 0x1409CB51C, PARange=2 (40-bit PA).
- Only own image pages WB (Qualcomm XPUs: never map carve-outs cacheable); fb Normal-NC; rest Device.
- AT S1E1R checks all as expected; unmapped -> translation fault L2.
- Fill fb: 110 ms (MMU off) -> 3.5 ms (NC). Memory loop 70 ms -> 0.7 ms. vtimer + SGI IRQs still fine with MMU on.

## XNU loader design notes (xnu-8796)
- Entry: `_start` -> `start_first_cpu`, x0 = boot_args (PA), MMU off, EL1. boot_args: Revision/Version 2,
  virtBase/physBase/memSize/topOfKernelData, Boot_Video (fb 0x9c000000, rowBytes 4320, 1080x2400, depth 32),
  deviceTreeP/Length (ADT), CommandLine[608], bootFlags, memSizeActual.
- start.s maps [kernel mach header .. physBase+memSize) with cacheable BLOCKS. On Qualcomm every XPU carve-out
  must therefore lie below the kernel: use physBase = 0xb0000000 (above ramdump_fb 0xaf000000+16M,
  cdsp_sec, qseecom), end before rmtfs 0xfde01000 -> memSize ~0x4d000000 for bring-up.
- Base ADT: iPhone 8 d20ap DeviceTree from iOS 16.7.16 IPSW (tools/remotezip.py + tools/adt.py; LZFSE payload).
  Template values are empty: the loader must fill chosen/*, memory-map, cpu*/reg etc. like iBoot.
- XNU build: GitHub Actions (dipa-studio/ios-sweet, xnu.yml) via blacktop/darwin-xnu-build, MACOS_VERSION=13.5.
  New-clang fixes in xnu/patches; Xcode 16 iig is incompatible (IORPC.kernelContent) -> macos-14 runner, oldest Xcode.

## Kernel architecture plan (SWEET machine config)
- First stock build (VMAPPLE) is arm64e (PAC). Kryo 470 = ARMv8.2, no PAC -> SWEET must build plain arm64
  (HAS_APPLE_PAC is only defined for __arm64e__, so arm64 drops PAC automatically).
- Template: VMAPPLE (16K pages, GICv3 code in pe_fiq.c) minus APPLEVIRTUALPLATFORM hypercalls/paravirt PAC,
  plus BCM2837 (generic ARM: no APPLE_ARM64_ARCH_FAMILY Apple sysregs). Do not link the KDK lib<cfg>.os.*.a.
- Timer: XNU expects it on FIQ (sleh_fiq -> rtclock_intr). SWEET: GIC Group 1, so sleh_irq must ack ICC_IAR1,
  route INTID 19 to the same timer path, SGIs to cpu_signal_handler, EOI via ICC_EOIR1.
- SMP/interrupts: open-source AppleARMSMP (USE_APPLEARMSMP) needs an IOInterruptController (AIC role) and an
  IOPMGR (CPU power -> PSCI CPU_ON via SMC). Plan: built-in IOKit classes (SweetGIC, SweetPMGR, platform
  expert) compiled into the kernel and matched via iokit/KernelConfigTables.cpp -> no closed kexts needed.
- Debug: xnu patch 0003 sweet-fbcon = kprintf drawn on the framebuffer (ADT node "sweet-fbcon").
- ADT /arm-io ranges must not map to parent 0 (pe_arm_get_soc_base_phys()==0 disables serial): use parent 0x1000000.

## Status 2026-10-14 late: first XNU boot attempt
- SWEET kernel builds (CI run 38635343732, patches 0001-0009). Verified offline: arm64, no PAC, no >v8.2 code,
  70300 classic relocs all in range (slide 0 = no loader work), entry = __start.
- Boot: shim loader worked (initrd 0x846F0000-0x855FE3E0, BOOTARG 0xB1FF8000, JUMP 0xB146A488) but screen stayed
  red -> xnu died/hung before serial_init (fbcon). kprintf is a no-op before STARTUP_SUB_KPRINTF.
- Historical next-step plan (patch 0010, now implemented and built; see active handoff above): early fb debug channel
  1. start.s start_first_cpu (after mov x20,x0 / mov x21,#0): PA marker band (MMU off, direct store to 0x9c000000).
  2. start.s after both create_bootstrap_mapping calls: add ONE block entry to the existing V=P L2 table
     (x25 + 2*PGBYTES; do NOT call create_bootstrap_mapping again: fb and RAM share L1 entry 0, it would replace
     the L2 table). Entry = TYPE_BLOCK|VALID|SH(OUTER)|ATTRINDX(CACHE_ATTRINDX_WRITECOMB=1)|AF|PNX|NX|0x9c000000,
     index = (va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT.
  3. new osfmk/arm64/sweet_early.c (+files.arm64 entry): sweet_early_mark(n) colored bands at screen bottom and
     sweet_early_putc using iso_font via VA=PA while the bootstrap TTBR0 is live; disable before
     arm_vm_init's set_mmu_ttb(invalid_ttep) (arm_vm_init.c:2148).
  4. markers in arm_init after PE_init_platform, ml_parse_cpu_topology, cpu_bootstrap, kernel_startup_bootstrap,
     cpu_init, before arm_vm_init.
  5. panic hook at top of panic_trap_to_debugger: va_copy + _doprnt(fmt, &copy, sweet_early_putc, 16).
  Verify with tools/xnu-cc.py (compile+symbols) and patch-order simulation before the single CI run.

## 2026-11-07: framebuffer present timing A/B image
- Observation: Subway Surfers frames are clean in software screenshots but show large black regions/flicker on camera stills. That points to the panel presentation path; vblank/scanout interaction is a hypothesis, not confirmed.
- Added `xnu/patches/0157-guacamole-fb-at-swap-toggle.patch`. It adds boot arg `sweetfbatswap` (default `1`) and logs `SWEET FB: GPU present mode ...`. `sweetfbatswap=0` disables the immediate Metal present in `AppleCLCD::swap()` and leaves the existing 60 Hz timer present path active.
- Built locally from the existing `ios-sweet-latest` guacamole source tree and its already-applied Wi-Fi patches. XNU kernel build succeeded with Xcode 27.2/KDK 13.5. Local build-only workaround: removed Bash process substitution from the DXB dSYM warning filter because this sandbox denies `/dev/fd`; this does not change kernel code.
- Packaged `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-metal2vulkan-vsync-only.img` using the existing guacamole shim and fullmetal stock boot template. Kernel payload args preserve the current GL/Metal/Wi-Fi settings and add only `sweetfbatswap=0` (plus refreshed `sweettime`). SHA-256: `042be7c91908d6da38ff17fb6f75e8f2960a939af2b13356f9e560f6db1b41f2`.
- Not yet booted on hardware. Boot temporarily with `fastboot boot /Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-metal2vulkan-vsync-only.img`; after boot, verify `dmesg | grep 'SWEET FB: GPU present mode'` says `vsync-only`, then compare camera flicker. If unchanged, this toggle is not the cause.
- Hardware result: `kern.bootargs` confirmed `sweetfbatswap=0`; the 5-second stats reported `shown at swap 0, at vsync 54`, confirming the tested frames used the timer presentation path. The user reports the flicker remains, so immediate-at-swap presentation is not the sole cause. `vsyncs sent 0` counts registered async framebuffer callbacks, not timer firings or GPU presentations.
- Next isolation image: `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-metal2vulkan-cpu-present.img`, same kernel and Metal/Vulkan app rendering, with `sweetfbgpu=0` to force the final layer composition/copy through the CPU while retaining vsync-only timing. SHA-256: `aebbb92c1ad09d4a0351528d3a783a0c5481214cb623b3b055aec8748e30aada`. Not yet booted on hardware.
- Hardware result: user reports flicker also persists with `sweetfbgpu=0` CPU framebuffer composition, so the final Adreno blit is not the sole cause. Next A/B tests the iOS-published display refresh at 90 Hz (the OnePlus 7 Pro panel's nominal rate) while retaining CPU composition and vsync-only presentation. Image `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-metal2vulkan-cpu-present-90hz.img`, SHA-256 `806c1ffbb876a04b6e076bb1910a8e6892922c77b8ad9166a074102f5a01cb05`; not yet booted.
- The 90 Hz advertised-refresh image made the black region somewhat worse. Since the kernel's presentation timer was still fixed at 60 Hz, I added patch `0158-guacamole-vsync-rate-toggle.patch` with boot arg `sweetfb_vsync` (default 60), which sets the software timer and callback period. Built the next comparison with `sweetfb_refresh=90 sweetfb_vsync=90 sweetfbgpu=0 sweetfbatswap=0`; expected boot log: `SWEET FB: synthetic vsync 90 Hz (11111 us)`. Image `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-metal2vulkan-cpu-present-90hz-timed.img`, SHA-256 `5dcd99fa40a6a69a1760a3c40d36038fd6709f5658c24332de6a2346d99e5221`; not yet booted on hardware.
- Follow-up A/B requested by user: built `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-metal2vulkan-gpu-present-90hz-timed.img`, same 90 Hz advertised refresh and timer, vsync-only mode, but GPU final framebuffer blit re-enabled (no `sweetfbgpu=0`). SHA-256 `0b1ca46898b1d5139404bf6555873b482fa29c13e0cf5beb086ef47282763fae`; not yet booted.
- User requested a return to 60 Hz and GPU framebuffer presentation because the 90 Hz mode felt slower. Built `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-metal2vulkan-60hz-gpu-present.img` from the current local guacamole kernel. Args set `sweetfb_refresh=60 sweetfb_vsync=60 sweetfb_hz=60 sweetfbgpu=1 sweetfbatswap=1`; Metal/OpenGL/Wi-Fi args retained. SHA-256 `ad5d645357afedbc196f050cf47bc3ec8959b1643fa3160a6c6d623c0629128a`. Not yet booted.
- Next diagnostic prepared: `guacamole-metal2vulkan-fb-write-1hz-diagnostic.img` (SHA-256 `d29515040cada97583e2acc8895547394c227c3a8bfe220bb03890f1aba3a3a2`) uses `sweetfb_hz=1`, 60 Hz timer/advertised refresh, GPU final blit, and vsync-only presentation. This intentionally displays only about one updated frame per second, to distinguish flicker during framebuffer writes from flicker while the front buffer is held static. Not yet booted.

## 2026-11-08: return to pre-pageflip source and image
- Reverted the experimental framebuffer-vsync/pageflip patch batch (0159–0167) after the user reported unusably slow UI and a black game. Earlier Metal/Vulkan, OpenGL, and Wi-Fi work remains in the source tree.
- Do not use `guacamole-metal2vulkan-scanout-probe-v6.img`; user reports it does not boot. The last tested, bootable pre-pageflip image is `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-metal2vulkan-teirq.img` (SHA-256 `e755c940fc2dc2535c66ca3f30db9b44304b3b48d73e50cd4e467dbfedb29988`). It booted with Wi-Fi, Metal, OpenGL, and 60 Hz enabled; its known issue is the previously reported flicker.

## 2026-11-08: read-only physical scanout sampler
- The 1 Hz framebuffer update test had already been tried and did not eliminate flicker. This rules out continuous writes as the whole explanation, but does not show whether the panel is scanning the expected buffer contents.
- Added `xnu/patches/0168-guacamole-scanout-memory-sample.patch`. Boot arg `sweetfb_scanoutdiag=1` logs the framebuffer dimensions/stride/address once at driver start, then once per 60 vsync ticks samples five bands (four full-width rows each) directly from the scanout buffer, reporting a hash, nonblack pixel count and mean RGB. It does not write to framebuffer memory. Sampling is disabled unless the boot arg is present.
- Built the XNU `DEVELOPMENT_ARM64_SWEET` kernel locally with Xcode 27.2 and macOS 13.5 KDK. Verified the linked kernel contains both sampler log strings. Existing dSYM-filter `/dev/fd` workaround remains local to the generated DXB build tree.
- Packaged separate temporary image `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-scanout-memory-diag.img`, SHA-256 `4f91bfbc7c5bf4da81f0ca6faf2d26ed80085add533ad0f45f4e085be4ffd995`. It uses the known 60 Hz GPU framebuffer settings and otherwise retains the known Metal/OpenGL/Wi-Fi boot args; only diagnostic addition is `sweetfb_scanoutdiag=1` (and refreshed `sweettime`). It is not booted yet.
- Next: user can temporarily boot it with `fastboot boot /Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-scanout-memory-diag.img`, reproduce flicker, then collect `dmesg | grep 'SWEET FB: scanout' | tail -n 20`. Compare bands across samples: stable hashes/nonblack counts while the camera sees black would point downstream of the framebuffer memory (SDE/panel); changing or black bands would show that the buffer being scanned is itself changing or blanking. This is a diagnostic hypothesis test, not a fix.
- Follow-up to the read-only sampler result: the phone confirms `sweetfb_scanoutdiag=1` is active; repeated samples of all five bands remain all-zero with an identical hash while Subway Surfers is active. This proves only that the CPU-visible `fPixels` mapping reads as zero; it does not prove which memory the panel scans.
- Prepared a separate probe-validation image `/Users/coreycusick/Documents/Codex/2026-11-06/ok/outputs/guacamole-scanout-cpu-map-check.img` (SHA-256 `5b6b81de270612478b12f92922010863f39d2906efefaca62836a7218e2253a8`). Same diagnostic kernel and Metal/OpenGL/Wi-Fi settings, but `sweetfbgpu=0` forces CPU composition into `fPixels`. Not booted. If its sampler becomes nonzero, compare against the GPU framebuffer-present route; if it stays zero, the sampler's mapping/read method is not seeing the content and must be corrected before interpreting panel behavior.
# Active handoff — 0200 Metal bundle installed; reboot requested (2026-11-09)

- Rebuilt `userland/metal2/out/SweetMetal2.bundle` from the current source (0200 Photos device-limits fix), ad-hoc
  signed by `userland/metal2/build.sh`. Build succeeded with four existing incompatible-pointer-type warnings.
  `SweetMetal2` SHA-256: `2aba7a84dc2eb74e7225c0e70466862de20b0c1a547d1119125c5d5621a7eb63`; Turnip SHA-256:
  `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`.
- Installed over SSH at `/System/Library/Extensions/SweetMetal2.bundle`, verified the 0200 limits log string is
  present, and preserved the prior bundle as `SweetMetal2.bundle.pre-0200-photos-limits-20261109`.
- `reboot` is not installed on the phone. Invoked `launchctl reboot`; SSH at `192.168.0.70` has not returned after
  50 seconds, consistent with Wi-Fi needing manual re-enable after boot. Photos test and crash diagnostics pending.


# Active handoff — MPSCore cache disassembly (2026-11-09)

- `research/metal/MPSAutoCache-ctor.txt` contains the 60-instruction MPSAutoCache constructor disassembly from the iOS 16.7.16 (20H392) arm64 dyld shared cache. At 0x1d40afa48 it loads the `MPSCommandBufferImageCache` class, then calls `initWithCommandBuffer:` at 0x1d40afa54.
- `research/metal/MPS-cache-init.txt` contains the named 139-instruction implementation `-[MPSCommandBufferImageCache initWithCommandBuffer:]` at 0x1d40aeda0.

# Active handoff — 0206 Metal object protocols installed (2026-11-09)

- Rebuilt `SweetMetal2.bundle` with the current 0206 `NativeBridgeMetalFrontend.mm` protocol-adoption changes using `sh userland/metal2/build.sh`. Ad-hoc signature verified locally. Four existing incompatible-pointer-type warnings remain.
- `SweetMetal2` SHA-256: `057bf9db25c72e6074323e6177c78923cfc38ec5d202930d421a511afdb02f84`; Turnip SHA-256: `55d231439a2aba105f8cbe03883062979f86a41ffe2fed804aadf87efe9b1af5`.
- Installed to `/System/Library/Extensions/SweetMetal2.bundle` on `192.168.0.70`; pulled both binaries back and byte-compared them to the build. Previous bundle preserved as `SweetMetal2.bundle.pre-0206-20261109`. Archive: `outputs/guacamole-SweetMetal2-0206-mps-protocols.tar.gz`, SHA-256 `429d8d50b60cf5e2cf0949dd8273bdf77ddb7742c91d2ce586c5c85e443a21e9`.
- No reboot performed. Restart/reboot before judging the MPS app behavior so backboardd and app processes load the new bundle.

# Active handoff — MPSDevice support-path disassembly (2026-11-09)

- Added `research/metal/MPSDevice-GetMPSDevice.txt` with the complete named `MPSDevice::GetMPSDevice(id<MTLDevice>)` body from the iOS 16.7.16 (20H392) arm64 dyld shared cache. It is unusually large (428 unique instruction addresses); family/name gating and initializer dispatch are inline.
- Also added the named base `MPSDevice` constructor and `MPSDevice_Common` constructor, plus two private helpers used on this path: the optional `gpuCoreCount` selector helper and a per-family `MPSDevice_Common` initializer that calls `supportsFamily:`.
- The selector checks and model-name comparisons appear in `GetMPSDevice`; the disassembly includes `supportsFamily:` calls and hard-coded Apple GPU names (A13/A14/A12X/M1/A10X/A8X/A10/A9X). No Adreno model string appears in those comparisons. This is evidence about the observed selection path, not proof of the runtime `supportsFamily:` answers returned by SweetMetal2.

# Audio handoff — CoreAudio IOAudio2 client contract (2026-11-10)

- Added `research/audio/coreaudio-ioa2-client-contract.md` and linked it from the audio README.
- Confirmed from iOS 16.7.16 (20H392) CoreAudio: IOAudio2 service matching name and open type, selectors 2/3/5/6/10/11 client call shapes, mapped memory type families, visible registry schema, VirtualAudio's built-in topology expectations, and D22 VAD tuning filenames.
- Unresolved by static client-side analysis: stock D22 primary device UID/name and full runtime registry dictionaries, notification variant layout, and named offsets/semantics for IOAudio2's status-page clock. These are explicitly called out in the report; do not guess them from the distinct AOP path.

# Active handoff — Request I haptics capability switches (2026-11-10)

- Completed the iOS 16.7.16 / 20H392 disassembly in `research/haptics/ios-i/RESULTS.md`.
- Both VibeSynth and Core Haptics ultimately consult `DeviceSupportsClosedLoopHaptics`. VibeSynth also has a legacy CoreAudio `vibe` device fallback only when that answer is false. It caches its capability once. Kernel 0251 does not participate in either check.
- To preserve both MG answers, disabling `libVibeSynthEngine.dylib` loading is the reversible way to force MediaExperience back to its IOKit `vibrator` fallback; SpringBoard’s independent UIKit level-2 gate is `DisableLevel2Feedback` (preference storage/write path not verified). See results for limits and caveats. No phone changes made.
