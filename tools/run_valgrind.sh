#!/usr/bin/env bash
#
# Memcheck wrapper for the RHEL 9 host (valgrind 3.26).
#
# The default target is --selftest, and that is deliberate: the self-test
# drives exactly the hand-written code that has no library standing behind it
# - the proto3 codec, HPACK with its dynamic table, and the HTTP/2 framing -
# with no GUI, no device and no network.  A leak or an invalid read there is
# this project's own bug, which is the only kind of finding worth chasing.
#
#   tools/run_valgrind.sh                          # codec + HPACK
#   tools/run_valgrind.sh --selftest-net 127.0.0.1:18700
#   BIN=./s2t_qt tools/run_valgrind.sh --probe 192.168.1.47:8700 --token XXX
#
# Build the binary with debug info first, or every frame reads as "???":
#   qmake6 CONFIG+=memcheck ../s2t_qt.pro && make -j"$(nproc)"
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
bin=${BIN:-./s2t_qt}

if [[ ! -x $bin ]]; then
    echo "không tìm thấy binary: $bin (đặt BIN=... hoặc chạy trong thư mục build)" >&2
    exit 2
fi
if ! command -v valgrind >/dev/null; then
    echo "chưa cài valgrind: sudo dnf install valgrind" >&2
    exit 2
fi

# glib's slab allocator recycles blocks itself and hides real errors from
# memcheck; these two variables are the documented way to switch that off.
export G_SLICE=always-malloc
export G_DEBUG=gc-friendly
# If a GUI mode is ever run under valgrind, do it without a real display.
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-offscreen}

exec valgrind \
    --tool=memcheck \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect \
    --track-origins=yes \
    --num-callers=30 \
    --suppressions="$here/valgrind.supp" \
    --gen-suppressions=all \
    --error-exitcode=99 \
    "$bin" "${@:---selftest}"
