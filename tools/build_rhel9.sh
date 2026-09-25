#!/usr/bin/env bash
#
# Build both halves on RHEL 9 (tested target: 9.8, gcc 11.5, glibc 2.34, x86_64).
#
#   tools/build_rhel9.sh              # release build into ../build-rhel
#   tools/build_rhel9.sh memcheck     # -O1 -g3 -fno-omit-frame-pointer
#   tools/build_rhel9.sh server       # only s2t-qt-server (no Qt Multimedia,
#                                     # no GUI - what a headless host needs)
#   tools/build_rhel9.sh client       # only s2t-qt-client
#
# The top-level .pro is a subdirs project, so the default builds both.  The
# server half is the one that goes on a machine near the GPU box; the client
# half is the one that goes on an operator's workstation, and neither needs
# the other's dependencies.
#
# Packages needed once, as root:
#
#   dnf install gcc-c++ make qt6-qtbase-devel          # server needs only this
#   dnf install qt6-qtmultimedia-devel qt6-qtbase-gui  # plus this, for the client
#   # optional, each for one thing only:
#   dnf install nodejs      # tools/mock_adapter.js, for the client --selftest-net
#   dnf install valgrind gdb
#   dnf install ffmpeg-free # .m4a replay; plain 16-bit PCM WAV needs nothing
#
# RHEL 9 ships Qt 5 and Qt 6 side by side and plain `qmake` is the Qt 5 one,
# which cannot build this app - so the Qt 6 qmake is located explicitly here
# rather than trusted to be first on PATH.
set -euo pipefail

src=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
out=${OUT:-$(cd "$src/.." && pwd)/build-rhel}
extra=()
project="$src/s2t_qt.pro"
what="cả hai"

case "${1:-}" in
    memcheck)
        extra=(CONFIG+=memcheck)
        ;;
    server)
        project="$src/s2t-qt-server/s2t-qt-server.pro"
        what="s2t-qt-server"
        ;;
    client)
        project="$src/s2t-qt-client/s2t-qt-client.pro"
        what="s2t-qt-client"
        ;;
    "")
        ;;
    *)
        echo "tham số không hiểu: $1 (dùng: memcheck | server | client)" >&2
        exit 2
        ;;
esac

qmake=""
for candidate in "${QMAKE6:-}" qmake6 qmake-qt6 /usr/lib64/qt6/bin/qmake; do
    [[ -z $candidate ]] && continue
    path=$(command -v "$candidate" 2>/dev/null || true)
    [[ -z $path ]] && continue
    version=$("$path" -query QT_VERSION 2>/dev/null || true)
    if [[ $version == 6.* ]]; then
        qmake=$path
        break
    fi
done
if [[ -z $qmake ]]; then
    echo "không tìm thấy qmake của Qt 6." >&2
    echo "  sudo dnf install qt6-qtbase-devel qt6-qtmultimedia-devel" >&2
    echo "  (hoặc đặt QMAKE6=/đường/dẫn/tới/qmake)" >&2
    exit 2
fi

echo "qmake : $qmake ($("$qmake" -query QT_VERSION))"
echo "gcc   : $(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion)"
echo "build : $what"
echo "out   : $out"

mkdir -p "$out"
cd "$out"
"$qmake" "${extra[@]}" "$project"
make -j"$(nproc)"

echo
# A subdirs build puts each target under its own directory; a single-project
# build puts it straight in $out.  Report whichever actually exists rather
# than printing a path that may not be there.
for binary in \
    "$out/s2t-qt-server/s2t-qt-server" "$out/s2t-qt-server" \
    "$out/s2t-qt-client/s2t-qt-client" "$out/s2t-qt-client"; do
    [[ -x $binary && -f $binary ]] && echo "xong: $binary"
done

echo
echo "kiểm tra nhanh (không cần màn hình, không cần mạng):"
echo "  <đường dẫn>/s2t-qt-server --selftest"
echo "  <đường dẫn>/s2t-qt-client --selftest"
