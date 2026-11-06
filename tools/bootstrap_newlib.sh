#!/bin/sh
# Complete the existing native CK803S toolchain with a pinned, real target libc.
set -eu
cd "$(dirname "$0")/.."
task_root=$(pwd)
task_archive="$task_root/research/toolchain/downloads/newlib-4.5.0.20241231.tar.gz"
mkdir -p research/toolchain/downloads build/newlib logs
if [ ! -f "$task_archive" ]; then
    curl -fL https://sourceware.org/pub/newlib/newlib-4.5.0.20241231.tar.gz -o "$task_archive"
fi
python3 - "$task_archive" <<'PY'
import hashlib, sys
assert hashlib.sha256(open(sys.argv[1], 'rb').read()).hexdigest() == '33f12605e0054965996c25c1382b3e463b0af91799001f5bb8c0630f2ec8c852', 'newlib hash mismatch'
PY
if [ ! -d research/toolchain/newlib-4.5.0.20241231 ]; then
    tar xf "$task_archive" -C research/toolchain
fi
export PATH="$task_root/toolchain/bin:$PATH"
export MAKEINFO=true
cd build/newlib
"$task_root/research/toolchain/newlib-4.5.0.20241231/configure" --target=csky-elf \
    --prefix="$task_root/toolchain" --disable-multilib --enable-newlib-nano-malloc \
    --disable-newlib-supplied-syscalls --enable-newlib-reent-small --disable-nls \
    CFLAGS_FOR_TARGET="-mcpu=ck803s -mno-high-registers -mlittle-endian -msoft-float -Os -g -Wno-error=implicit-function-declaration" \
    > "$task_root/logs/newlib-configure.log" 2>&1
make -j"${JOBS:-8}" MAKEINFO=true > "$task_root/logs/newlib-build.log" 2>&1
make MAKEINFO=true install > "$task_root/logs/newlib-install.log" 2>&1
echo "Installed pinned CK803S newlib (no high registers)."
