#!/bin/sh
# Native csky-elf GCC/binutils. No radio access. Original archives stay pristine.
set -eu
cd "$(dirname "$0")/.."
task_root=$(pwd)
task_jobs=${JOBS:-8}
task_prefix="$task_root/toolchain"
python3 tools/sources.py fetch
mkdir -p build/binutils build/gcc logs
if [ ! -d research/toolchain/binutils-2.45.1 ]; then
    tar -xf research/toolchain/downloads/binutils-2.45.1.tar.xz -C research/toolchain
fi
if [ ! -d research/toolchain/gcc-14.3.0 ]; then
    tar -xf research/toolchain/downloads/gcc-14.3.0.tar.xz -C research/toolchain
fi
(
    cd build/binutils
    "$task_root/research/toolchain/binutils-2.45.1/configure" --target=csky-elf \
        --prefix="$task_prefix" --disable-nls --disable-werror --disable-gdb \
        --disable-gprofng --with-system-zlib > "$task_root/logs/binutils-configure.log" 2>&1
    make -j"$task_jobs" > "$task_root/logs/binutils-build.log" 2>&1
    make install > "$task_root/logs/binutils-install.log" 2>&1
)
(
    cd research/toolchain/gcc-14.3.0
    # This pinned release script verifies its prerequisite archives with SHA-512.
    ./contrib/download_prerequisites --no-isl > "$task_root/logs/gcc-prerequisites.log" 2>&1
)
(
    cd build/gcc
    PATH="$task_prefix/bin:$PATH"
    export PATH
    "$task_root/research/toolchain/gcc-14.3.0/configure" --target=csky-elf \
        --prefix="$task_prefix" --enable-languages=c,c++ --without-headers --with-newlib \
        --disable-bootstrap --disable-multilib --disable-nls --disable-shared --disable-threads \
        --disable-libssp --disable-libquadmath --disable-libgomp --disable-libatomic \
        --with-system-zlib --with-cpu=ck803s --with-float=soft > "$task_root/logs/gcc-configure.log" 2>&1
    make -j"$task_jobs" all-gcc > "$task_root/logs/gcc-build.log" 2>&1
    make install-gcc > "$task_root/logs/gcc-install.log" 2>&1
    make -j"$task_jobs" all-target-libgcc > "$task_root/logs/libgcc-build.log" 2>&1
    make install-target-libgcc > "$task_root/logs/libgcc-install.log" 2>&1
)
"$task_prefix/bin/csky-elf-gcc" --version
