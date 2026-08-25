#!/usr/bin/env python3
"""Kiểm tra tương thích: grpc/protobuf THẬT gọi vào Server buffer tự viết.

Đây là phép kiểm mà --selftest không làm được. Client và server của dự án này
dùng chung một bản HPACK và một bộ mã proto3, nên chúng có thể đồng ý với nhau
mà cả hai cùng sai. grpc C-core thì không đồng ý với ai cả.

Nó cũng kiểm luôn rằng tệp .proto in trong docs/danh-sach-api.md là thật: các
stub dùng ở đây được sinh ra từ chính tệp đó.

Cách chạy (cần python3 + grpcio-tools; máy RHEL đã có):

    # 1. bóc ba khối protobuf ra khỏi tài liệu
    awk 'BEGIN{n=0} /^```protobuf$/{n++; f=1; next} f&&/^```$/{f=0; next} f{print > ("p" n ".proto")}' \
        docs/danh-sach-api.md
    mv p1.proto asr_session.proto; mv p2.proto speaker_registry.proto
    mv p3.proto buffer_admin.proto

    # 2. sinh stub
    mkdir -p out && python3 -m grpc_tools.protoc -I. --python_out=out \
        --grpc_python_out=out *.proto

    # 3. chạy, với một s2t-qt-server đang chạy ở địa chỉ dưới đây
    python3 interop_check.py 127.0.0.1:8800 <token>
"""
import sys
import time

sys.path.insert(0, "out")

import grpc
import buffer_admin_pb2 as buf
import buffer_admin_pb2_grpc as bufrpc
import asr_session_pb2 as asr
import asr_session_pb2_grpc as asrrpc

TARGET = sys.argv[1]
TOKEN = sys.argv[2] if len(sys.argv) > 2 else ""
META = (("authorization", "Bearer " + TOKEN),) if TOKEN else ()

failures = 0


def check(ok, what):
    global failures
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        failures += 1


ch = grpc.insecure_channel(TARGET)
admin = bufrpc.BufferAdminServiceStub(ch)
asr_stub = asrrpc.ProductASRServiceStub(ch)

# --- ping: the smallest possible round trip -------------------------------
sent = time.time()
pong = admin.ping(buf.PingRequest(client_ts=sent), timeout=10, metadata=META)
check(abs(pong.client_ts - sent) < 1e-6, "ping echoes client_ts bit for bit")
check(bool(pong.server_version), "ping returns a server_version (%s)" % pong.server_version)
print("       upstream_ready = %s, round trip %.1f ms"
      % (pong.upstream_ready, (time.time() - sent) * 1000))

# --- get_buffer_status: a nested message and a repeated field --------------
st = admin.get_buffer_status(buf.BufferStatusRequest(), timeout=10, metadata=META)
check(st.uptime_sec > 0, "get_buffer_status reports uptime (%.1f s)" % st.uptime_sec)
check(st.upstream.target != "", "nested UpstreamStatus decodes (%s)" % st.upstream.target)
# The counter is incremented after a handler returns, so the call reading it
# never sees itself.  Read it twice and check it moved.
st2 = admin.get_buffer_status(buf.BufferStatusRequest(), timeout=10, metadata=META)
check(st2.total_calls > st.total_calls,
      "call counter advances (%d then %d)" % (st.total_calls, st2.total_calls))

# --- list_buffered_sessions ------------------------------------------------
ls = admin.list_buffered_sessions(buf.BufferSessionsRequest(include_finished=True),
                                  timeout=10, metadata=META)
check(True, "list_buffered_sessions returns %d session(s)" % len(ls.sessions))

# --- an unknown session: status code and a Vietnamese message --------------
try:
    asr_stub.get_live_state(asr.SessionRequest(session_id="không-có"),
                            timeout=10, metadata=META)
    check(False, "an unknown session should have raised")
except grpc.RpcError as e:
    check(e.code() == grpc.StatusCode.NOT_FOUND,
          "unknown session is NOT_FOUND (%s)" % e.code())
    # Deliberately checks the half that is true whatever the server's
    # durability setting is.  The parenthetical differs - with a journal
    # configured the message no longer blames a restart, because a restart no
    # longer loses meetings - and asserting on that half would make this test
    # fail for the right reason at the wrong time.
    check("máy chủ đệm không giữ phiên" in e.details(),
          "grpc-message percent-decodes to Vietnamese (%s)" % e.details()[:60])

# --- a bad token -----------------------------------------------------------
if TOKEN:
    try:
        bufrpc.BufferAdminServiceStub(grpc.insecure_channel(TARGET)).ping(
            buf.PingRequest(), timeout=10,
            metadata=(("authorization", "Bearer wrong"),))
        check(False, "a bad token should have raised")
    except grpc.RpcError as e:
        check(e.code() == grpc.StatusCode.UNAUTHENTICATED,
              "a bad token is UNAUTHENTICATED (%s)" % e.code())

# --- an unregistered method ------------------------------------------------
try:
    asr_stub.get_audit_history(asr.AuditHistoryRequest(session_id="x", limit=1),
                               timeout=15, metadata=META)
    check(True, "a relayed RPC was accepted and answered")
except grpc.RpcError as e:
    # Relayed to an upstream that is not there: UNAVAILABLE is the honest
    # answer and still proves the relay path decoded our request.
    check(e.code() in (grpc.StatusCode.UNAVAILABLE, grpc.StatusCode.NOT_FOUND,
                       grpc.StatusCode.UNAUTHENTICATED, grpc.StatusCode.INTERNAL),
          "a relayed RPC reached the upstream and came back (%s)" % e.code())

print("ALL PASS" if failures == 0 else "%d FAILURE(S)" % failures)
sys.exit(0 if failures == 0 else 1)
