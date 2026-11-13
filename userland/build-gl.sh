#!/bin/sh
# Build only what the GPU path needs: the backboardd wrapper (DYLD_INSERT) and
# libsweet-gl (EAGLContext on Mesa's freedreno, drawing into IOSurfaces).
#
#   userland/mesa/mk-mesa.sh   first (libOSMesa.8.dylib + gl.xml + exports)
#   userland/build-gl.sh       -> userland/out/
#
# The rest of userland/build.sh is not needed for graphics; keeping this
# small means a Mesa or OpenGLES problem cannot hide behind another tool.
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

# com.apple.backboardd -> /usr/libexec/sweet-bbd -> real backboardd with
# /usr/lib/libsweet-bbd.dylib (it waits for RedwoodGPU, then dlopens
# /usr/lib/libsweet-gl.dylib).
build sweet-bbd bbd/sweet-bbd.c
build libsweet-bbd.dylib bbd/bbd-insert.c -dynamiclib -install_name /usr/lib/libsweet-bbd.dylib \
	-framework IOSurface -Wl,-not_for_dyld_shared_cache -framework IOKit -framework CoreFoundation
# CAEnableDeepFramebuffer / CAEnableFramebufferCompression / CAEnableIOSurfaceCompression:
# the old EAGL path could not draw those, so prefs sets them to NO on the phone.
build prefs jb/prefs.c -framework CoreFoundation

MESA=mesa/build
if [ ! -f $MESA/out/libOSMesa.8.dylib ]; then
	echo "missing $MESA/out/libOSMesa.8.dylib: run userland/mesa/mk-mesa.sh first" >&2
	exit 1
fi
cp $MESA/out/libOSMesa.8.dylib out/
# 0191: the gl* names iOS's OpenGLES exports (from the SDK's .tbd), for app interposers
grep -o '_gl[A-Za-z0-9_]*' "$SDK/System/Library/Frameworks/OpenGLES.framework/OpenGLES.tbd" \
	| sed 's/^_//' | sort -u > out/opengles-exports.txt
python3 gpu/mkeagl.py gpu/eagl_slots.txt $MESA/out/gl.xml $MESA/out/exported-gl.txt gpu/eagl.c \
	out/opengles-exports.txt > out/eagl_gen.h
build libsweet-gl.dylib gpu/eagl.c gpu/gl_osmesa.c -dynamiclib -install_name /usr/lib/libsweet-gl.dylib \
	-Iout -Igpu -I$MESA/mesa-25.0.7/include $MESA/out/libOSMesa.8.dylib -Wl,-rpath,/usr/lib \
	-Wl,-not_for_dyld_shared_cache -Wno-unused-but-set-variable \
	-framework OpenGLES -framework IOKit -framework IOSurface -framework CoreFoundation -lobjc
