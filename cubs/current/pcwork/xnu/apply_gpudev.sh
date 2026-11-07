#!/bin/bash
# Run on the Mac: add /dev/cubs-gpu (redwood_gpu.c) to the XNU tree and build the static iOS client gpuc.
# usage: bash apply_gpudev.sh   (after copying redwood_gpu.c and gpuc.c to ~)
set -e
S=~/unixnu-pixel11/hackios-upstream/xnu-src/xnu-8796.121.2
cp ~/redwood_gpu.c $S/bsd/dev/redwood_gpu.c
grep -q redwood_gpu.c $S/bsd/conf/files || sed -i '' 's#^bsd/dev/redwood_input.c\(.*\)$#bsd/dev/redwood_input.c\1\nbsd/dev/redwood_gpu.c\t\tstandard#' $S/bsd/conf/files
grep -q redwood_gpu_init $S/bsd/dev/redwood_input.c || python3 - <<XX
p="$S/bsd/dev/redwood_input.c"
s=open(p).read()
s=s.replace("void\nredwood_input_init(void)\n{","extern void redwood_gpu_init(void);\n\nvoid\nredwood_input_init(void)\n{",1)
s=s.replace("\tredwood_adsp_init();\n}","\tredwood_adsp_init();\n\tredwood_gpu_init();\n}",1)
open(p,"w").write(s)
XX
grep -n "redwood_gpu" $S/bsd/conf/files $S/bsd/dev/redwood_input.c
# the static client, built like the other raw-syscall programs of userland/build.sh
cd ~/unixnu-pixel11/hackios-upstream/userland
xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -e _start -o ~/gpuc ~/gpuc.c || echo "gpuc: check the entry symbol name used by hello.c"
ls -l ~/gpuc
