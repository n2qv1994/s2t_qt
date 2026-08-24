#!/usr/bin/env bash
#
# Build s2t_qt on RHEL 9 (tested target: 9.8, gcc 11.5, glibc 2.34, x86_64).
#
#   tools/build_rhel9.sh              # release build into ../build-rhel
#   tools/build_rhel9.sh memcheck     # -O1 -g3 -fno-omit-frame-pointer
#
# Packages needed once, as root:
#
#   dnf install gcc-c++ make \
#               qt6-qtbase-devel qt6-qtmultimedia-devel \
#               qt6-qtbase-gui
#   # optional, each for one thing only:
#   dnf install nodejs      # tools/mock_adapter.js, for --selftest-net
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
if [[ ${1:-} == memcheck ]]; then
    extra=(CONFIG+=memcheck)
fi

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
echo "out   : $out"

mkdir -p "$out"
cd "$out"
"$qmake" "${extra[@]}" "$src/s2t_qt.pro"
make -j"$(nproc)"

echo
echo "xong: $out/s2t_qt"
echo "kiểm tra nhanh (không cần màn hình, không cần mạng):"
echo "  $out/s2t_qt --selftest"
