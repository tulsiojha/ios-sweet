#!/bin/bash
# Sweet-style payload pipeline for the vivo Y21L (msm8916):
#   stage0 (ARM32 stub + AArch64 payload) -> boot.img kernel slot
#   SWEETXNU payload (Mach-O kernel + msm8916 ADT) -> boot.img ramdisk
#   slot; LK loads it to 0x82000000 and stage0's loader boots it.
#
#   mkpayload.sh [KERNEL_MACHO] [OUT_IMG]
#
# With no KERNEL_MACHO it links ktest.S, whose "kernel" writes
# qcom,pshold so the phone powers off - the pipeline pass signal.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
LLVM=${LLVM:-/opt/homebrew/opt/llvm/bin}
mkdir -p out

"$ROOT/ports/vivo-y21l/stage0/build.sh"

kernel=${1:-}
if [ -z "$kernel" ]; then
	"$LLVM/llvm-mc" -triple=arm64-apple-macos11 -filetype=obj ktest.S -o out/ktest.o
	xcrun ld -arch arm64 -platform_version macos 11.0 11.0 -static \
		-e _start -o out/ktest out/ktest.o
	kernel=out/ktest
fi

python3 mkadt.py out/vivo.adt
python3 "$ROOT/tools/mkpayload.py" "$kernel" out/vivo.adt out/payload.bin \
	--cmdline "debug=0x8 serial=3 keepsyms=1 usbcon=1"
python3 "$ROOT/tools/vivo-repack-boot.py" \
	"$ROOT/device-info/vivo-stock/boot.img" \
	"$ROOT/ports/vivo-y21l/stage0/out/stage0v5.bin" \
	"${2:-out/vivo-boot.img}" --ramdisk-blob out/payload.bin --grow-ramdisk