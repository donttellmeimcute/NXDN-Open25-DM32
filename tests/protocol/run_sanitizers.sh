#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/dm32-p25-sanitize.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT HUP INT TERM
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc/p25 \
    src/p25/p25.c tests/protocol/fuzz_p25.c -o "$task_tmp/fuzz-p25"
"$task_tmp/fuzz-p25"
printf 'PASS P25 AddressSanitizer + UndefinedBehaviorSanitizer\n'
