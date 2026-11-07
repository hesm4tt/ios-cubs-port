#!/bin/bash
# build the hand-made module on the Mac; usage: build.sh "<vermagic string>"
set -e
VM="$1"
export PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH"
T="--target=aarch64-linux-gnu"
clang $T -O2 -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -mno-outline-atomics -fsanitize=kcfi -fsanitize-cfi-icall-experimental-normalize-integers -ffixed-x18 -c cubs_ho.c -o cubs_ho.o
clang $T -c -DVERMAGIC="\"vermagic=$VM\"" modmeta.S -o modmeta.o
ld.lld -r -o cubs_handover.ko cubs_ho.o modmeta.o
ls -l cubs_handover.ko
