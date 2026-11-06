#!/bin/bash
# Assemble the ARM32 stage0 stub into a flat binary (no linker needed).
set -euo pipefail
cd "$(dirname "$0")"
LLVM=${LLVM:-/opt/homebrew/opt/llvm/bin}
mkdir -p out
"$LLVM/llvm-mc" -triple=armv7-none-eabi -filetype=obj stub.S -o out/stub.o
"$LLVM/llvm-objcopy" -O binary --only-section=.text out/stub.o out/stub.bin
ls -l out/stub.bin
"$LLVM/llvm-objdump" -d out/stub.o | sed -n '1,40p'
