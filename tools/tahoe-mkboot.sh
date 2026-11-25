#!/bin/bash
# Wrap a Tahoe (26.5) XNU kernel into a fastboot-bootable guacamole image.
#
#   tools/tahoe-mkboot.sh KERNEL OUT.img
#
# KERNEL  kernel.development.sweet from the CI artifact (guacamole-xnu run)
# OUT.img written here; nothing is flashed. Then: fastboot boot OUT.img
#
# The image reuses, byte for byte, the shim Image, the phone DTB and the boot
# header of the image that printed the 0015 markers on the phone, plus the D22
# ADT that image's payload embedded. Only the kernel changes, so a new marker
# on the screen can only come from the new kernel.
#
# TEMPLATE (default ../../tahoe-boot) must contain:
#   template.img  the known-good boot image (header + DTB source)
#   shim.Image    its kernel section (arm64 Image of the shim)
#   d22.adt       the ADT its payload embedded
# Sanity check: rebuilding template.img from its own parts is byte-identical.
#
# ARGS defaults to the proven first-boot set; the boot image command line
# stays "console=null" because the shim owns the screen.
set -euo pipefail
[ $# -ge 2 ] || { sed -n '2,21p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
KERNEL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
OUT=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")

TEMPLATE=${TEMPLATE:-$HERE/../../tahoe-boot}
ARGS=${ARGS:-"debug=0xc serial=3 -v cpus=1 keepsyms=1 wdt=-1 -noprogress"}

for f in "$KERNEL" "$TEMPLATE/template.img" "$TEMPLATE/shim.Image" "$TEMPLATE/d22.adt"; do
	[ -f "$f" ] || { echo "missing: $f"; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

python3 "$HERE/mkpayload.py" "$KERNEL" "$TEMPLATE/d22.adt" "$work/payload.bin" \
	--cmdline "$ARGS"
python3 "$HERE/mkguacboot.py" "$TEMPLATE/template.img" "$TEMPLATE/shim.Image" \
	"$OUT" --ramdisk "$work/payload.bin" --cmdline "console=null"

echo "boot-args: $ARGS"
echo "next: fastboot boot $OUT"
