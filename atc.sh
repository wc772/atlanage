#!/bin/bash

set -e
cd "$(dirname "$0")"
f="$1"
if [ -z "$f" ]; then echo "用法: ./atc.sh <源.at> [输出名]"; exit 1; fi
base=$(basename "$f" .at)
out="${2:-$base}"
U="$$"
./AT-V1 "$f" > "_atc_${U}.asm" 2>/dev/null || { echo "COMPILE FAIL"; rm -f "_atc_${U}.asm"; exit 1; }
./AT-V1 -obj "_atc_${U}.asm" -out="_atc_${U}.obj" >/dev/null 2>&1 || { echo "OBJ FAIL"; exit 1; }
./AT-V1 -link "_atc_${U}.obj" -out="${out}.exe" >/dev/null 2>&1 || { echo "LINK FAIL"; exit 1; }
rm -f "_atc_${U}.asm" "_atc_${U}.obj"
echo "→ ${out}.exe"