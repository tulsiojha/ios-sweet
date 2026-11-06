#!/bin/bash
# Assemble the stage0 stubs into flat binaries (no linker needed).
set -euo pipefail
cd "$(dirname "$0")"
LLVM=${LLVM:-/opt/homebrew/opt/llvm/bin}
mkdir -p out
AS32="$LLVM/llvm-mc -triple=armv7-none-eabi -mattr=+trustzone,+virtualization -filetype=obj"
AS64="$LLVM/llvm-mc -triple=aarch64-none-elf -filetype=obj"

$AS32 stub.S       -o out/stub.o
$AS32 stub3.S      -o out/stub3.o
$AS32 stub4_arm32.S -o out/stub4_arm32.o
$AS64 stub4_a64.S  -o out/stub4_a64.o
for f in stub stub3 stub4_arm32 stub4_a64; do
	"$LLVM/llvm-objcopy" -O binary --only-section=.text out/$f.o out/$f.bin
done
# v4 blob: ARM32 stub at 0x80008000, AArch64 payload at 0x8000a000 (offset 0x2000)
python3 - <<'EOF'
arm = open('out/stub4_arm32.bin','rb').read()
a64 = open('out/stub4_a64.bin','rb').read()
assert len(arm) <= 0x2000, len(arm)
open('out/stage0v4.bin','wb').write(arm + b'\0'*(0x2000-len(arm)) + a64)
EOF
ls -l out/stub.bin out/stub3.bin out/stage0v4.bin
"$LLVM/llvm-readelf" -r out/stub4_arm32.o out/stub4_a64.o | grep -c relocation || true
