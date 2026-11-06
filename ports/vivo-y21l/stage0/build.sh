#!/bin/bash
# Assemble the stage0 stubs into flat binaries (the 32-bit part needs no
# linker; the AArch64 payload is linked at 0x8000a000 with rust-lld).
set -euo pipefail
cd "$(dirname "$0")"
LLVM=${LLVM:-/opt/homebrew/opt/llvm/bin}
RUST_LLD=${RUST_LLD:-$HOME/.rustup/toolchains/stable-aarch64-apple-darwin/lib/rustlib/aarch64-apple-darwin/bin/rust-lld}
AS32="$LLVM/llvm-mc -triple=armv7-none-eabi -mattr=+trustzone,+virtualization -filetype=obj"
AS64="$LLVM/llvm-mc -triple=aarch64-none-elf -filetype=obj"
A64FLAGS=(--target=aarch64-none-elf -ffreestanding -nostdlib -fno-pic
	-march=armv8-a -mgeneral-regs-only -mstrict-align -fno-stack-protector
	-fno-asynchronous-unwind-tables -fno-unwind-tables -O2)
mkdir -p out

$AS32 stub.S       -o out/stub.o
$AS32 stub3.S      -o out/stub3.o
$AS32 stub4_arm32.S -o out/stub4_arm32.o
$AS64 stub4_a64.S  -o out/stub4_a64.o
for f in stub stub3 stub4_arm32 stub4_a64; do
	"$LLVM/llvm-objcopy" -O binary --only-section=.text out/$f.o out/$f.bin
done

# v5: C payload, linked at 0x8000a000
"$LLVM/clang" "${A64FLAGS[@]}" -c payload/entry.S -o out/entry.o
"$LLVM/clang" "${A64FLAGS[@]}" -c payload/payload.c -o out/payload.o
"$LLVM/clang" "${A64FLAGS[@]}" -c payload/loader.c -o out/loader.o
"$RUST_LLD" -flavor gnu -T payload/payload.ld --build-id=none \
	-o out/payload.elf out/entry.o out/payload.o out/loader.o
"$LLVM/llvm-objcopy" -O binary out/payload.elf out/payload.bin

# v4 blob: ARM32 stub at 0x80008000, AArch64 payload at 0x8000a000 (offset 0x2000)
python3 - <<'EOF'
arm = open('out/stub4_arm32.bin','rb').read()
a64 = open('out/stub4_a64.bin','rb').read()
assert len(arm) <= 0x2000, len(arm)
open('out/stage0v4.bin','wb').write(arm + b'\0'*(0x2000-len(arm)) + a64)

arm = open('out/stub4_arm32.bin','rb').read()
payload = open('out/payload.bin','rb').read()
assert len(arm) <= 0x2000, len(arm)
total = 0x2000 + len(payload)
assert total <= 0x7c8280, hex(total)
open('out/stage0v5.bin','wb').write(arm + b'\0'*(0x2000-len(arm)) + payload)
print(f"arm32 stub {len(arm)} B, payload {len(payload)} B, "
      f"stage0v5 {total:#x} of 0x7c8280 kernel area")
EOF
ls -l out/stage0v4.bin out/stage0v5.bin
"$LLVM/llvm-readelf" -r out/stub4_arm32.o out/stub4_a64.o out/payload.elf \
	| grep -c relocation || true
