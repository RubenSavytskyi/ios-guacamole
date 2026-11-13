#!/bin/sh
# Build the ios-sweet userland helpers for the iOS 16 root (arm64, A11: no
# arm64e), ad-hoc signed: the kernel makes signed binaries on the root
# volume platform binaries (patch 0035), so no other signature is needed.
#
#   userland/build.sh            -> userland/out/ (autorun, GPU tests, backboardd wrapper)
set -e
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -isysroot $SDK -O2 -Wall -Wextra"
mkdir -p out
build() { # OUT SRC... [-- extra flags]
	out=$1; shift
	$CC -o "out/$out" "$@"
	codesign -f -s - "out/$out"
	echo "out/$out"
}
build sweet-autorun autorun/sweet-autorun.c
build redwood-gpu-test gputest/redwood-gpu-test.c -framework IOKit -framework CoreFoundation
build sweet-bbd bbd/sweet-bbd.c
build libsweet-bbd.dylib bbd/bbd-insert.c -dynamiclib -install_name /usr/lib/libsweet-bbd.dylib -framework IOSurface -Wl,-not_for_dyld_shared_cache \
	-framework IOKit -framework CoreFoundation
build libsweet-insert-test.dylib dyldtest/insert-test.c -dynamiclib -install_name /private/var/tmp/libsweet-insert-test.dylib
# removable system apps: runs SystemAppMigrator; signed again with the entitlements it needs
build sweet-sysapps sysapps/sweet-sysapps.m -fobjc-arc -framework Foundation
codesign -f -s - --entitlements sysapps/entitlements.plist out/sweet-sysapps
build sweet-fs fsutil/sweet-fs.c -framework CoreFoundation
# MobileGestalt answer cache editor (metal/gamekit for AB2; ArtworkDeviceSubType for the Dynamic Island); install to /usr/local/bin
build mgtool jb/mgtool.c -framework CoreFoundation
build haptic jb/haptic.c
build auprobe jb/auprobe.c -framework CoreFoundation
build sweet-msd jb/sweet-msd.c
build libsweet-autrace.dylib jb/autrace.c -dynamiclib -install_name /usr/lib/libsweet-autrace.dylib -Wl,-not_for_dyld_shared_cache -Wl,-U,_AudioObjectGetPropertyData,-U,_AudioObjectGetPropertyDataSize,-U,_AudioObjectSetPropertyData,-U,_AudioObjectHasProperty
build vibtest jb/vibtest.c -framework AudioToolbox -framework IOKit -framework CoreFoundation
build mgq jb/mgq.c -lMobileGestalt -framework CoreFoundation
# read-only look at the display controller the bootloader left running (brightness / Night Shift via DSPP PCC)
build dispprobe jb/dispprobe.c
# brightness / Night Shift by hand through DSPP0/1 PCC (CTL0 only; refuses if the controller is not as probed)
build dispcolor jb/dispcolor.c
# the panel's own brightness (DCS 0x51 over DSI0 during the bootloader's video mode); `panelbright probe` first
build panelbright jb/panelbright.c
# read-only: the panel's real frame timing (PINGPONG tear check / autorefresh / line counter)
build vsyncprobe jb/vsyncprobe.c
# read-only: each process's data container as the kernel's Sandbox stand-in answers it (0201)
build sbpath jb/sbpath.c
# containers by signing identity for the Sandbox stand-in (0202): /var/local/bin/sweet-containers, job com.sweet.containers
build sweet-containers jb/sweet-containers.c -framework CoreFoundation
# iOS's brightness slider / Night Shift -> DSPP PCC (install /var/local/bin/sweet-displayd; job com.sweet.displayd)
build sweet-displayd jb/sweet-displayd.m -fobjc-arc -framework Foundation
codesign -f -s - --entitlements jb/sweet-displayd-ents.plist out/sweet-displayd
# why MPS refuses the Metal device (photo / Pride wallpapers crash in MPSAutoCache): /var/local/bin/mpsprobe
build mpsprobe jb/mpsprobe.m -fobjc-arc -framework Foundation -framework Metal
# why +[IOSurface bs_IOSurfaceWithWidth:height:options:] returns nil in PosterBoard (black wallpaper): /var/local/bin/surfprobe
build surfprobe jb/surfprobe.m -fobjc-arc -framework Foundation -framework IOSurface -framework IOKit
# Bluetooth step 1, read-only: WCN3990 UART (QUP wrapper 2 SE3) clocks, pins and engine: /var/local/bin/btprobe
build btprobe bt/btprobe.c wland/pmem.c -Iwland
# Bluetooth step 2: WCN3990 over its UART (clock, pins, GENI UART, power pulses, first HCI): /var/local/bin/btuart
build btuart bt/btuart.c wland/pmem.c -Iwland
# Bluetooth part 2: bluetoothd / BlueTool <-> WCN3998 through a pty at /dev/cu.bluetooth: /var/local/bin/btbridge
build btbridge bt/btbridge.c wland/pmem.c -Iwland -Ibt
# Bluetooth (userland/bt/btd.c, from OtVaL228/ios-sweet guacamole): WCN3990 HCI over the kernel control
# socket com.sweet.bt (xnu 0162, boot-arg sweetbt=1), firmware from /var/local/fw/bt; install
# /var/local/bin/btd (job com.sweet.btd). bluetoothd gets BT_HCI_TRANSPORT=H4:POSIX:com.sweet.bt.
build btd bt/btd.c wland/pmem.c -Iwland
# bluetoothd's BlueTool stand-in: RunBuiltinScript ("boot", "init") -> result 0 (job com.apple.BlueTool)
build bluetoold bt/bluetoold.c -framework CoreFoundation
# Speaker audio (userland/adsp): ADSP PAS boot, APR/AFE/ASM, TFA9874 over /dev/pmem; install
# /var/local/bin/adspd and /var/local/bin/tfa (job com.sweet.adspd, script /var/local/adspd.boot)
build adspd adsp/adspd.c adsp/edge.c wland/pmem.c wland/smem.c wland/qmi.c -Iwland -Iadsp
build tfa adsp/tfa.c wland/pmem.c -Iwland
# SweetCodec: a HAL AudioServerPlugIn (Codec + Hawking) writing the ADSP's ring /var/local/pcm.ring;
# install as /Library/Audio/Plug-Ins/HAL/SweetCodec.driver/{SweetCodec,Info.plist}
# The iOS SDK has no AudioServerPlugIn.h (a macOS SDK header); borrow it, link only CoreFoundation
mkdir -p out/SweetCodec.driver
MACSDK=$(xcrun --sdk macosx --show-sdk-path)
INC=out/hal-include
mkdir -p $INC/CoreAudio $INC/CoreAudioTypes
cp "$MACSDK/System/Library/Frameworks/CoreAudio.framework/Headers/"*.h $INC/CoreAudio/ 2>/dev/null || true
cp "$MACSDK/System/Library/Frameworks/CoreAudioTypes.framework/Headers/"*.h $INC/CoreAudioTypes/ 2>/dev/null || true
$CC -I$INC -o out/SweetCodec.driver/SweetCodec -bundle audio/sweetcodec.c -framework CoreFoundation
codesign -f -s - out/SweetCodec.driver/SweetCodec
cp audio/Info.plist out/SweetCodec.driver/Info.plist
echo out/SweetCodec.driver
# A2DP: what the kernel answers to bluetoothd's user-pipe Skywalk nexus (research/bluetooth/a2dp-skywalk.md)
build nxprobe bt/nxprobe.c
# nxprobe-bt: the same, signed with bluetoothd's entitlements (bt/bluetoothd-ents.plist, dumped from the iOS root
# with `codesign -d --entitlements :- .../usr/sbin/bluetoothd`), to run as mobile like bluetoothd
if [ -f bt/bluetoothd-ents.plist ]; then
	$CC -o out/nxprobe-bt bt/nxprobe.c && codesign -f -s - --entitlements bt/bluetoothd-ents.plist out/nxprobe-bt && echo out/nxprobe-bt
fi
# Wi-Fi: ios-flame's wland (userland/wland/README.md): modem + WLFW + WCN3990 radio from userland over /dev/pmem
(cd wland && $CC -Wno-unused-parameter -o ../out/wland wland.c smem.c pmem.c glink.c link.c qmi.c rmtfs.c pdmapper.c \
	tftp.c scm.c wlfw.c radio.c ipnet.c bridge.c) && codesign -f -s - out/wland && echo out/wland
# from ios-flame (userland/flame/README.md): LaunchServices registration, unified log stream
build lsreg flame/lsreg.m -fobjc-arc -framework Foundation -framework MobileCoreServices
codesign -f -s - --entitlements flame/lsreg-ents.plist out/lsreg
build oslog flame/oslog.m -fobjc-arc -framework Foundation
codesign -f -s - --entitlements flame/oslog-ents.plist out/oslog
build netsvc flame/netsvc.m -fobjc-arc -framework Foundation -framework SystemConfiguration
codesign -f -s - --entitlements flame/netsvc-ents.plist out/netsvc
build nettest flame/nettest.m -fobjc-arc -framework Foundation
# a network service for en0 (preferences.plist from before 0139 has none)
build wifisvc wifisvc/wifisvc.c -framework CoreFoundation -framework SystemConfiguration
codesign -f -s - --entitlements wifisvc/entitlements.plist out/wifisvc
# G3: needs userland/mesa/mk-mesa.sh first (libOSMesa.8.dylib + headers)
MESA=mesa/build
if [ -f $MESA/out/libOSMesa.8.dylib ]; then
	# libOSMesa is @rpath/...: next to the test, or in /usr/lib for libsweet-gl
	build osmesa-triangle gputest/osmesa-triangle.c -I$MESA/mesa-25.0.7/include $MESA/out/libOSMesa.8.dylib \
		-Wl,-rpath,@executable_path
	cp $MESA/out/libOSMesa.8.dylib out/
	# G4: EAGLContext on Mesa for backboardd (eagl.c: HackiOS via topaz, MIT)
	# 0191: the gl* names iOS's OpenGLES exports (from the SDK's .tbd), for the app interposers
	grep -o '_gl[A-Za-z0-9_]*' "$SDK/System/Library/Frameworks/OpenGLES.framework/OpenGLES.tbd" | sed 's/^_//' | sort -u \
		> out/opengles-exports.txt
	python3 gpu/mkeagl.py gpu/eagl_slots.txt $MESA/out/gl.xml $MESA/out/exported-gl.txt gpu/eagl.c \
		out/opengles-exports.txt > out/eagl_gen.h
	build libsweet-gl.dylib gpu/eagl.c gpu/gl_osmesa.c -dynamiclib -install_name /usr/lib/libsweet-gl.dylib \
		-Iout -Igpu -I$MESA/mesa-25.0.7/include $MESA/out/libOSMesa.8.dylib -Wl,-rpath,/usr/lib \
		-Wl,-not_for_dyld_shared_cache -Wno-unused-but-set-variable \
		-framework OpenGLES -framework IOKit -framework IOSurface -framework CoreFoundation -lobjc
	# 0193: gpu/nullal.c: a silent OpenAL stand-in when the real device does not open (the port has no audio).
	# 0191: OpenGL ES for apps: OpenGLES.framework's gl* exports interposed onto the thread's eagl table.
	# Linked against OpenGLES only (no Mesa), so the interposed names are iOS's. An app loads it via
	# tools/insert-dylib.py APP_BINARY /usr/lib/libsweet-glapp.dylib.
	build libsweet-glapp.dylib gpu/glapp.c gpu/nullal.c -dynamiclib -install_name /usr/lib/libsweet-glapp.dylib -Iout \
		out/libsweet-gl.dylib -Wl,-not_for_dyld_shared_cache -Wno-unused-function -Wno-unused-parameter \
		-framework OpenGLES -framework OpenAL -Wl,-U,_sweet_gl_current_table -Wno-deprecated-declarations
else
	echo "skipped osmesa-triangle: run userland/mesa/mk-mesa.sh"
fi
