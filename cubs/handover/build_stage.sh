#!/bin/bash
set -e
VM="$1"
export PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH"
T="--target=aarch64-linux-gnu"
clang $T -O2 -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -mno-outline-atomics -fsanitize=kcfi -fsanitize-cfi-icall-experimental-normalize-integers -ffixed-x18 -c cubs_stage.c -o cubs_stage.o
clang $T -c -DVERMAGIC="\"vermagic=$VM\"" modmeta_stage.S -o modmeta_stage.o
ld.lld -r -o cubs_stage.ko cubs_stage.o modmeta_stage.o
ls -l cubs_stage.ko
