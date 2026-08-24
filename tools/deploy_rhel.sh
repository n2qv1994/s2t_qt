#!/usr/bin/env bash
#
# Copy this source tree to the RHEL host and build both halves there.
#
#   tools/deploy_rhel.sh            # copy + build
#   tools/deploy_rhel.sh copy       # copy only
#   tools/deploy_rhel.sh build      # build only (source already on the host)
#
# ssh/scp will ask for the account password once per connection; run this from
# a real terminal.  To be asked only once, install a key first:
#
#   ssh-keygen -t ed25519          # if you have no key yet
#   ssh-copy-id -p 2247 intekcom@222.252.10.175
#
# The remote build itself is tools/build_rhel9.sh, which is part of the copied
# tree - see the package list in its header comment.
set -euo pipefail

HOST=${HOST:-222.252.10.175}
PORT=${PORT:-2247}
USER_=${USER_:-intekcom}
DEST=${DEST:-s2t-qt}           # relative to the remote home directory

src=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
step=${1:-all}

# Windows build output, Qt Creator's per-user state and the git database are
# all machine specific; sending them would be slow and would put MinGW objects
# next to the RHEL ones.
excludes=(
    --exclude=build
    --exclude=build-rhel
    --exclude=.git
    --exclude=.qtcreator
    --exclude='*.o'
    --exclude='moc_*'
    --exclude='*.exe'
    --exclude='*.user'
)

copy() {
    echo "==> copy $src -> $USER_@$HOST:~/$DEST"
    if command -v rsync >/dev/null 2>&1; then
        rsync -az --delete "${excludes[@]}" -e "ssh -p $PORT" \
              "$src/" "$USER_@$HOST:$DEST/"
    else
        # No rsync (Git Bash on Windows has none): stream a tar over ssh.
        # --delete has no equivalent here, so clear the target first.
        ssh -p "$PORT" "$USER_@$HOST" "rm -rf ~/$DEST && mkdir -p ~/$DEST"
        tar -C "$src" -czf - "${excludes[@]}" . \
            | ssh -p "$PORT" "$USER_@$HOST" "tar -C ~/$DEST -xzf -"
    fi
}

build() {
    echo "==> build on $HOST"
    ssh -p "$PORT" "$USER_@$HOST" \
        "chmod +x ~/$DEST/tools/*.sh && OUT=~/$DEST/build-rhel ~/$DEST/tools/build_rhel9.sh"
    echo
    echo "==> selftest (không cần màn hình, không cần mạng)"
    ssh -p "$PORT" "$USER_@$HOST" "~/$DEST/build-rhel/s2t-qt-server/s2t-qt-server --selftest"
    ssh -p "$PORT" "$USER_@$HOST" "~/$DEST/build-rhel/s2t-qt-client/s2t-qt-client --selftest"
}

case $step in
    all)   copy; build ;;
    copy)  copy ;;
    build) build ;;
    *)     echo "dùng: $0 [all|copy|build]" >&2; exit 2 ;;
esac
