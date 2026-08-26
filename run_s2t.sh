#!/usr/bin/env bash
#
# Set the environment and the parameters, then run the two halves of s2t.
#
# Qt 6 on the RHEL host is NOT a distro package: it is an installer tree at
# ~/Qt/6.11.2/gcc_64 and nothing puts it on PATH.  That is the whole reason the
# environment block below exists - without it the failure is one line reading
# "libQt6Core.so.6: cannot open shared object file", which names a symptom and
# not a cause.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() {
    cat <<'HELP'
Cách dùng: ./run_s2t.sh [lệnh]

  (không có)  server chạy nền + client giao diện, đóng client là dừng cả hai
  server      chỉ Server buffer, ở tiền cảnh
  client      chỉ giao diện điều hành viên
  selftest    hai bài self-test: không cần mạng, không cần màn hình
  build       build lại (đặt sẵn QMAKE6 và OUT); thêm server|client|memcheck
  config      ghi cấu hình client cho khớp với server ở đây (có sao lưu)
  env         in môi trường và tham số đã chốt rồi thoát
  stop        dừng server do lần chạy nền trước để lại

Mọi giá trị đều là biến môi trường ghi đè được, không phải sửa tệp này:

  TOKEN=abc ./run_s2t.sh
  LISTEN=0.0.0.0:8800 TOKEN=abc ./run_s2t.sh server
  BACKEND=riva UPSTREAM=192.168.1.47:50051 ./run_s2t.sh server
  JOURNAL_DIR= ./run_s2t.sh server        # tắt nhật ký, hàng đợi chỉ trong RAM

  QT_ROOT UPSTREAM_TOKEN MODEL LANGUAGE ENROLL_URL BUFFER_SECONDS
  STATE_DIR JOURNAL_DIR DB_DIR LOG_DIR BUILD_DIR CLIENT_CONFIG DISPLAY
  S2T_LOG_MODE S2T_LOG_LEVEL
HELP
}

# ------------------------------------------------------------------ môi trường

QT_ROOT=${QT_ROOT:-$HOME/Qt/6.11.2/gcc_64}
if [[ -d $QT_ROOT/lib ]]; then
    export PATH="$QT_ROOT/bin:$PATH"
    export LD_LIBRARY_PATH="$QT_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export QT_PLUGIN_PATH="$QT_ROOT/plugins"
    export QMAKE6="${QMAKE6:-$QT_ROOT/bin/qmake}"
fi

# Vietnamese strings are compiled in as UTF-8; a C/POSIX locale turns them into
# question marks in the terminal without anything else looking wrong.
case ${LANG:-} in
    *UTF-8*|*utf8*|*UTF8*) ;;
    *) export LANG=en_US.UTF-8 ;;
esac

# X session on this host is :1 (the one AnyDesk reaches).  Only the client
# needs it; the server never opens a display.
export DISPLAY=${DISPLAY:-:1}

# Hai biến của bộ nhật ký dùng chung - xem shared/core/Logger.h.
export S2T_LOG_MODE=${S2T_LOG_MODE:-debug}
export S2T_LOG_LEVEL=${S2T_LOG_LEVEL:-info}

# -------------------------------------------------------------------- tham số

# Chú ý dấu: ${X:-mặc định} coi rỗng như chưa đặt, ${X-mặc định} thì không.
# Những khoá mà "rỗng" là một lựa chọn thật - tắt token, tắt đăng ký giọng, để
# backend tự chọn mô hình - phải dùng dạng không có dấu hai chấm, nếu không thì
# `TOKEN= ./run_s2t.sh` lặng lẽ nhận lại giá trị mặc định.
LISTEN=${LISTEN:-127.0.0.1:8800}          # nơi client kết nối tới
TOKEN=${TOKEN-s2t-local}                  # token client phải gửi kèm; rỗng = tắt
BACKEND=${BACKEND:-triton}                # triton | riva
UPSTREAM=${UPSTREAM:-192.168.1.47:8011}   # Triton: cổng gRPC 8011, KHÔNG phải HTTP 8010
UPSTREAM_TOKEN=${UPSTREAM_TOKEN-}
MODEL=${MODEL-asr_diar_session}           # rỗng = mặc định của backend
LANGUAGE=${LANGUAGE-vi-VN}                # chỉ Riva dùng tới
ENROLL_URL=${ENROLL_URL-http://127.0.0.1:8790}   # rỗng = tắt đăng ký giọng
BUFFER_SECONDS=${BUFFER_SECONDS:-300}

# Ba thư mục, và phân biệt chúng là điều quan trọng nhất ở đây:
#   journal  = HÀNG ĐỢI audio chưa lên tới tầng suy luận, xoá khi đã lên
#   database = BẢN LƯU cuộc họp, giữ lại (~115 MB mỗi giờ họp)
#   logs     = nhật ký của lần chạy nền
# Đừng trỏ hai cái đầu vào cùng một chỗ: một bên xoá, một bên giữ.
STATE_DIR=${STATE_DIR:-$HOME/.local/share/s2t-qt-server}
JOURNAL_DIR=${JOURNAL_DIR-$STATE_DIR/journal}
DB_DIR=${DB_DIR-$STATE_DIR/database}
LOG_DIR=${LOG_DIR:-$STATE_DIR/logs}
PID_FILE=$STATE_DIR/s2t-qt-server.pid
SERVER_LOG=$LOG_DIR/server.log

BUILD_DIR=${BUILD_DIR:-${OUT:-$root/build-rhel}}
CLIENT_CONFIG=${CLIENT_CONFIG:-$HOME/.config/s2t/s2t_qt.conf}

# ------------------------------------------------------------------- tiện ích

die() { echo "$*" >&2; exit 2; }

# A subdirs build puts each target under its own directory; `build_rhel9.sh
# server` puts it straight in the build dir.  Accept either.
find_binary() {
    local name=$1 candidate
    for candidate in "$BUILD_DIR/$name/$name" "$BUILD_DIR/$name"; do
        [[ -f $candidate && -x $candidate ]] && { echo "$candidate"; return 0; }
    done
    return 1
}

require_binary() {
    local name=$1 path
    if ! path=$(find_binary "$name"); then
        die "không thấy $name trong $BUILD_DIR
  build trước đã:  $0 build"
    fi
    echo "$path"
}

server_args() {
    local args=(--listen "$LISTEN"
                --backend "$BACKEND"
                --upstream "$UPSTREAM"
                --buffer-seconds "$BUFFER_SECONDS")
    [[ -n $TOKEN ]]          && args+=(--token "$TOKEN")
    [[ -n $UPSTREAM_TOKEN ]] && args+=(--upstream-token "$UPSTREAM_TOKEN")
    [[ -n $MODEL ]]          && args+=(--model "$MODEL")
    [[ -n $ENROLL_URL ]]     && args+=(--enroll-url "$ENROLL_URL")
    [[ -n $DB_DIR ]]         && args+=(--database-dir "$DB_DIR")
    [[ -n $JOURNAL_DIR ]]    && args+=(--journal-dir "$JOURNAL_DIR")
    [[ $BACKEND == riva && -n $LANGUAGE ]] && args+=(--language "$LANGUAGE")
    printf '%s\n' "${args[@]}"
}

# Token rỗng nghĩa là chấp nhận mọi người gọi tới được cổng này.  Chấp nhận
# được khi chỉ nghe trên loopback; trên địa chỉ dùng chung thì không.
check_token() {
    local host=${LISTEN%:*}
    if [[ -z $TOKEN && $host != 127.0.0.1 && $host != localhost && $host != ::1 ]]; then
        die "TOKEN rỗng mà lại nghe trên $host - như vậy là mở cổng cho mọi người.
  đặt TOKEN=... hoặc đổi LISTEN về 127.0.0.1:8800"
    fi
}

# Cổng đã có người nghe chưa.  Đáng kiểm trước khi khởi động, vì QTcpServer đặt
# SO_REUSEADDR: một server đang nghe 0.0.0.0:8800 KHÔNG chặn được một server
# thứ hai bind 127.0.0.1:8800, và khi đó client nối vào cái nào là chuyện của
# bảng định tuyến chứ không phải của ai.  Khoá trong journal_dir không cứu được
# chỗ này - nó chỉ giữ thư mục nhật ký, không giữ cổng.
port_in_use() {
    local hostport=$1 host port
    host=${hostport%:*}; port=${hostport##*:}
    [[ $host == 0.0.0.0 || $host == "::" ]] && host=127.0.0.1
    if (exec 3<>"/dev/tcp/$host/$port") 2>/dev/null; then
        exec 3<&- 3>&-
        return 0
    fi
    return 1
}

check_port_free() {
    port_in_use "$LISTEN" || return 0
    echo "đã có thứ gì đó nghe ở $LISTEN." >&2
    if command -v ss >/dev/null 2>&1; then
        ss -lntp 2>/dev/null | grep -E "[:.]${LISTEN##*:}[[:space:]]" | sed 's/^/  /' >&2 || true
    fi
    die "  chạy chồng lên nó thì client nối vào cái nào là chuyện may rủi.
  dừng cái đang chạy, hoặc đổi cổng:  LISTEN=127.0.0.1:8801 $0"
}

wait_for_port() {
    local hostport=$1 host port i
    host=${hostport%:*}; port=${hostport##*:}
    [[ $host == 0.0.0.0 || $host == "::" ]] && host=127.0.0.1
    for ((i = 0; i < 100; i++)); do
        if (exec 3<>"/dev/tcp/$host/$port") 2>/dev/null; then
            exec 3<&- 3>&-
            return 0
        fi
        # Chết ngay lúc khởi động thì đừng đợi hết 20 giây rồi mới báo.
        if [[ -f $PID_FILE ]] && ! kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
            return 1
        fi
        sleep 0.2
    done
    return 1
}

# -------------------------------------------------------------------- lệnh

cmd_env() {
    echo "Qt        : $QT_ROOT"
    echo "build     : $BUILD_DIR"
    echo "DISPLAY   : $DISPLAY   (chỉ client dùng)"
    echo "log       : mode=$S2T_LOG_MODE level=$S2T_LOG_LEVEL"
    echo "server    : $(find_binary s2t-qt-server || echo '(chưa build)')"
    echo "client    : $(find_binary s2t-qt-client || echo '(chưa build)')"
    echo
    echo "tham số server:"
    # server_args in mỗi phần tử một dòng để chỗ khác đọc bằng mapfile; ở đây
    # thì ghép lại thành từng cặp cờ-giá trị cho dễ đọc.
    server_args | paste -d ' ' - - | sed 's/^/  /'
    echo
    echo "cấu hình client: $CLIENT_CONFIG"
    echo "  server/target = $LISTEN"
    echo "  server/token  = ${TOKEN:-(rỗng)}"
}

cmd_build() {
    echo "==> build (QMAKE6=${QMAKE6:-tự dò}, OUT=$BUILD_DIR)"
    OUT=$BUILD_DIR "$root/tools/build_rhel9.sh" "$@"
}

# Client chỉ đọc địa chỉ và token từ QSettings, không có tham số dòng lệnh cho
# hai thứ đó - nên muốn cặp này chạy được thì tệp cấu hình phải khớp.  Tệp đã
# có sẵn thì KHÔNG ghi đè lặng lẽ: nó là cấu hình thật của người dùng (micro,
# mức nhật ký, mã điều hành viên).  Chỉ lệnh `config` mới ghi, và có sao lưu.
cmd_config() {
    local force=${1:-} backup
    mkdir -p "$(dirname "$CLIENT_CONFIG")"

    if [[ ! -f $CLIENT_CONFIG ]]; then
        printf '[server]\ntarget=%s\ntoken=%s\n' "$LISTEN" "$TOKEN" > "$CLIENT_CONFIG"
        chmod 600 "$CLIENT_CONFIG"
        echo "==> đã tạo $CLIENT_CONFIG (target=$LISTEN)"
        return 0
    fi

    if [[ $force != force ]]; then
        echo "==> $CLIENT_CONFIG đã có, giữ nguyên:"
        awk '/^\[server\]/ { s = 1; next } /^\[/ { s = 0 }
             s && /^(target|token)=/ { print "    " $0 }' "$CLIENT_CONFIG"
        echo "    ghi đè cho khớp với server ở đây:  $0 config"
        return 0
    fi

    backup="$CLIENT_CONFIG.bak-$(date +%Y%m%d-%H%M%S)"
    cp -p "$CLIENT_CONFIG" "$backup"
    awk -v target="$LISTEN" -v tok="$TOKEN" '
        BEGIN { insec = 0; seen = 0; t = 0; k = 0 }
        /^\[/ {
            if (insec) { if (!t) print "target=" target; if (!k) print "token=" tok }
            insec = ($0 == "[server]"); if (insec) seen = 1
            print; next
        }
        insec && /^[ \t]*target[ \t]*=/ { print "target=" target; t = 1; next }
        insec && /^[ \t]*token[ \t]*=/  { print "token=" tok;     k = 1; next }
        { print }
        END {
            if (insec) { if (!t) print "target=" target; if (!k) print "token=" tok }
            else if (!seen) { print ""; print "[server]"
                              print "target=" target; print "token=" tok }
        }
    ' "$backup" > "$CLIENT_CONFIG"
    echo "==> đã ghi $CLIENT_CONFIG (bản cũ: $backup)"
}

prepare_dirs() {
    mkdir -p "$STATE_DIR" "$LOG_DIR"
    [[ -n $JOURNAL_DIR ]] && mkdir -p "$JOURNAL_DIR"
    [[ -n $DB_DIR ]] && mkdir -p "$DB_DIR"
    return 0
}

cmd_server() {
    local bin args
    bin=$(require_binary s2t-qt-server)
    check_token
    check_port_free
    prepare_dirs
    mapfile -t args < <(server_args)
    echo "==> s2t-qt-server nghe ở $LISTEN, tầng suy luận $BACKEND $UPSTREAM"
    exec "$bin" "${args[@]}"
}

start_server_background() {
    local bin args
    bin=$(require_binary s2t-qt-server)
    check_token

    if [[ -f $PID_FILE ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        die "server đã chạy sẵn (pid $(cat "$PID_FILE")).
  một thư mục nhật ký chỉ một tiến trình - dừng nó trước:  $0 stop"
    fi
    check_port_free

    prepare_dirs
    mapfile -t args < <(server_args)
    # < /dev/null có lý do: chạy qua ssh, một tiến trình nền thừa hưởng stdin
    # sẽ giữ kênh ssh mở kể cả khi stdout đã được chuyển hướng.
    "$bin" "${args[@]}" </dev/null >>"$SERVER_LOG" 2>&1 &
    echo $! > "$PID_FILE"
    echo "==> s2t-qt-server pid $(cat "$PID_FILE"), nhật ký: $SERVER_LOG"
}

stop_server() {
    [[ -f $PID_FILE ]] || return 0
    local pid i
    pid=$(cat "$PID_FILE")
    if kill -0 "$pid" 2>/dev/null; then
        # SIGTERM là đường dừng sạch: server đẩy nốt hàng đợi lên tầng suy luận
        # trước khi đóng, nên đừng vội SIGKILL.
        kill -TERM "$pid" 2>/dev/null || true
        for ((i = 0; i < 90; i++)); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 1
        done
        kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    fi
    rm -f "$PID_FILE"
    return 0
}

cmd_stop() {
    if [[ ! -f $PID_FILE ]]; then
        echo "không có server nào do script này khởi động."
        return 0
    fi
    echo "==> dừng pid $(cat "$PID_FILE")"
    stop_server
    echo "xong."
}

cmd_client() {
    local bin
    bin=$(require_binary s2t-qt-client)
    cmd_config
    echo "==> s2t-qt-client (DISPLAY=$DISPLAY) -> $LISTEN"
    exec "$bin" "$@"
}

cmd_selftest() {
    local server client rc=0
    server=$(require_binary s2t-qt-server)
    echo "==> s2t-qt-server --selftest"
    "$server" --selftest || rc=$?
    if client=$(find_binary s2t-qt-client); then
        echo
        echo "==> s2t-qt-client --selftest"
        "$client" --selftest || rc=$?
    fi
    echo
    if [[ $rc -eq 0 ]]; then echo "self-test: đạt"; else echo "self-test: HỎNG (mã $rc)"; fi
    return $rc
}

cmd_all() {
    local bin
    start_server_background
    trap stop_server EXIT INT TERM
    if ! wait_for_port "$LISTEN"; then
        echo "server không mở được $LISTEN - 20 dòng cuối của nhật ký:" >&2
        tail -n 20 "$SERVER_LOG" >&2 || true
        exit 1
    fi
    echo "==> server sẵn sàng ở $LISTEN"
    bin=$(require_binary s2t-qt-client)
    cmd_config
    echo "==> s2t-qt-client (DISPLAY=$DISPLAY); đóng cửa sổ là dừng cả hai"
    local rc=0
    "$bin" || rc=$?
    if [[ $rc -eq 0 ]]; then
        echo "==> client đã đóng, dừng server"
    else
        # Đóng cửa sổ và chết lúc khởi động đều dẫn tới đây; nói ra mã thoát để
        # hai chuyện đó không trông giống nhau.
        echo "==> client thoát với mã $rc, dừng server"
    fi
}

case "${1:-all}" in
    all)      cmd_all ;;
    server)   cmd_server ;;
    client)   shift; cmd_client "$@" ;;
    selftest) cmd_selftest ;;
    build)    shift; cmd_build "$@" ;;
    config)   cmd_config force ;;
    env)      cmd_env ;;
    stop)     cmd_stop ;;
    -h|--help|help) usage ;;
    *)
        die "tham số không hiểu: $1 (dùng: all | server | client | selftest | build | config | env | stop)"
        ;;
esac
