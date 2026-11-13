#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Independent decoders used only as host test oracles (docs/validation.md). Never linked into firmware.
# Fetches pinned commits into research/protocol and builds them under build/validators. No radio access.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
fetch() { # path url commit
    if [ ! -d "$1/.git" ]; then
        git init -q "$1"
        git -C "$1" remote add origin "$2"
    fi
    if [ "$(git -C "$1" rev-parse HEAD 2>/dev/null || true)" != "$3" ]; then
        git -C "$1" fetch -q --depth=1 origin "$3"
        git -C "$1" checkout -q --detach "$3"
    fi
    test "$(git -C "$1" rev-parse HEAD)" = "$3"
}
fetch research/protocol/dsd-fme https://github.com/lwvmobile/dsd-fme.git 34753963b8855c6c07b79250cb4de4f2c38a4613
fetch research/protocol/MMDVMHost https://github.com/g4klx/MMDVMHost.git 6ffa6d354a2c185316201c27c831c766a25a5653
fetch research/codec/mbelib https://github.com/szechyjs/mbelib.git 9a04ed5c78176a9965f3d43f7aa1b1f5330e771f
mkdir -p build/validators logs
prefix="$root/build/validators/prefix"
rm -rf build/validators/mbelib build/validators/dsd-fme
cp -r research/codec/mbelib build/validators/mbelib
cp -r research/protocol/dsd-fme build/validators/dsd-fme
(cd build/validators/mbelib && mkdir -p build && cd build &&
    cmake .. -DCMAKE_INSTALL_PREFIX="$prefix" > "$root/logs/validators-mbelib.log" 2>&1 &&
    make -j"${JOBS:-4}" >> "$root/logs/validators-mbelib.log" 2>&1 && make install >> "$root/logs/validators-mbelib.log" 2>&1)
# dsd-fme needs libsndfile, ncurses and pulseaudio headers (Ubuntu: libsndfile1-dev libncurses-dev libpulse-dev)
(cd build/validators/dsd-fme && mkdir -p build && cd build &&
    cmake .. -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_BUILD_TYPE=Release > "$root/logs/validators-dsd-fme.log" 2>&1 &&
    make -j"${JOBS:-4}" >> "$root/logs/validators-dsd-fme.log" 2>&1)
echo "built build/validators/dsd-fme/build/dsd-fme"
