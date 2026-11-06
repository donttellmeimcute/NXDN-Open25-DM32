#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
task_cc=${CSKY_CC:-toolchain/bin/csky-elf-gcc}
task_ar=${CSKY_AR:-toolchain/bin/csky-elf-ar}
task_size=${CSKY_SIZE:-toolchain/bin/csky-elf-size}
task_nm=${CSKY_NM:-toolchain/bin/csky-elf-nm}
task_out=${P25_TARGET_OUT:-build/p25-ck803s}
mkdir -p "$task_out"
"$task_cc" -mcpu=ck803s -mlittle-endian -mno-high-registers -msoft-float -std=c99 -ffreestanding \
    -Wall -Wextra -Werror -pedantic -Os -ffunction-sections -fdata-sections \
    -fstack-usage -c src/p25/p25.c -o "$task_out/p25.o"
"$task_ar" rcsD "$task_out/libdm32_p25.a" "$task_out/p25.o"
"$task_size" -A "$task_out/p25.o" > "$task_out/size.txt"
"$task_nm" -u "$task_out/p25.o" > "$task_out/undefined.txt"
"$task_cc" --version > "$task_out/compiler.txt"
cat > "$task_out/abi_sizes.c" <<'EOF'
#include "p25.h"
const unsigned char target_size_p25_rx[sizeof(p25_rx)] = {0};
const unsigned char target_size_p25_decoded[sizeof(p25_decoded)] = {0};
const unsigned char target_size_p25_call[sizeof(p25_call)] = {0};
EOF
"$task_cc" -mcpu=ck803s -mlittle-endian -mno-high-registers -msoft-float -ffreestanding \
    -Isrc/p25 -c "$task_out/abi_sizes.c" -o "$task_out/abi_sizes.o"
"$task_nm" -S "$task_out/abi_sizes.o" > "$task_out/abi_sizes.txt"
cat "$task_out/size.txt"
cat "$task_out/undefined.txt"
