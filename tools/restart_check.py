#!/usr/bin/env python3
"""Kiểm tra ở mức tiến trình: giết s2t-qt-server bằng SIGKILL giữa cuộc họp,
bật lại, và xem cuộc họp có đi tiếp không.

Bài --selftest trong C++ đã kiểm toàn bộ logic khôi phục, nhưng nó tháo đối
tượng chứ không giết tiến trình - nên nó KHÔNG chứng minh được điều quan trọng
nhất: rằng bản ghi đã nằm ngoài tiến trình trước khi client được ACK.  SIGKILL
không chạy destructor, không flush gì cả.  Nếu Journal còn giữ dữ liệu trong bộ
đệm của Qt thì chỉ bài này mới phát hiện ra.

Nó cũng dùng grpc/protobuf THẬT ở cả hai đầu: client là grpcio, tầng suy luận
giả cũng là grpcio.  Xem thêm tools/interop_check.py.

Cách chạy (cần python3 + grpcio-tools; máy RHEL đã có).  Sinh stub trước, đúng
như interop_check.py mô tả, rồi:

    python3 restart_check.py /đường/dẫn/s2t-qt-server [tham số thêm cho server...]

Mọi tham số sau đường dẫn được chuyển thẳng cho s2t-qt-server, nên chạy được cả
các chế độ độ bền khác:

    python3 restart_check.py ./s2t-qt-server --durability fsync
    python3 restart_check.py ./s2t-qt-server --journal-keep session
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from concurrent import futures

sys.path.insert(0, "out")

import grpc
import asr_session_pb2 as asr
import asr_session_pb2_grpc as asrrpc

SERVER = sys.argv[1] if len(sys.argv) > 1 else "./s2t-qt-server"
EXTRA = sys.argv[2:]
TOKEN = "restart-token"
PACKET = b"\x11" * 4096
BEFORE_STALL = 4
DURING_STALL = 6

failures = 0


def check(ok, what):
    global failures
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        failures += 1


class FakePipeline(asrrpc.ProductASRServiceServicer):
    """Stands in for grpc_session_adapter.py.  Outlives the restart."""

    def __init__(self):
        self.seqs = []
        self.events = []
        self.refuse = False
        self.source_seen = 0.0

    def start_session(self, request, context):
        self.events.append("start")
        response = asr.StartSessionResponse(session_id="sess-restart", stream_id=7,
                                            state_version=1)
        response.state.title = "Khởi động lại"
        return response

    def push_audio(self, request, context):
        if self.refuse:
            # A transport status: the buffer holds the packet and retries it,
            # which is how the backlog to survive the restart is built.
            context.abort(grpc.StatusCode.UNAVAILABLE, "đang từ chối (thử nghiệm)")
        self.events.append("push")
        self.seqs.append(request.seq)
        self.source_seen += len(request.pcm) / (48000.0 * 2.0)
        return asr.PushAudioResponse(session_id=request.session_id, stream_id=7,
                                     state_version=1 + len(self.seqs),
                                     source_seen_sec=self.source_seen)

    def get_live_state(self, request, context):
        response = asr.StateResponse(session_id=request.session_id, state_version=100)
        response.state.title = "Khởi động lại"
        return response

    def stop_session(self, request, context):
        self.events.append("stop")
        response = asr.StopSessionResponse(session_id=request.session_id, stream_id=7)
        response.state.done = True
        return response


def push(stub, session_id, seq, meta):
    request = asr.PushAudioRequest(session_id=session_id, pcm=PACKET, sample_rate=48000,
                                   channels=1, audio_format="s16le", reset=(seq == 1),
                                   vad_chunk_ms=160, seq=seq)
    return stub.push_audio(request, timeout=10, metadata=meta)


def wait_for(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


def start_server(journal, upstream_port, port, log):
    process = subprocess.Popen(
        [SERVER, "--listen", "127.0.0.1:%d" % port, "--token", TOKEN,
         "--upstream", "127.0.0.1:%d" % upstream_port, "--journal-dir", journal,
         "--log-level", "info"] + EXTRA,
        stdout=log, stderr=subprocess.STDOUT)
    # Wait for the port rather than sleeping a guessed amount.
    channel = grpc.insecure_channel("127.0.0.1:%d" % port)
    try:
        grpc.channel_ready_future(channel).result(timeout=15)
    finally:
        channel.close()
    return process


def main():
    global failures
    journal = tempfile.mkdtemp(prefix="s2t-restart-")
    meta = (("authorization", "Bearer " + TOKEN),)
    pipeline = FakePipeline()
    upstream = grpc.server(futures.ThreadPoolExecutor(max_workers=8))
    asrrpc.add_ProductASRServiceServicer_to_server(pipeline, upstream)
    upstream_port = upstream.add_insecure_port("127.0.0.1:0")
    upstream.start()
    port = 18877
    log_path = os.path.join(journal, "server.log")

    try:
        with open(log_path, "wb") as log:
            # ---- phase 1: a meeting, then SIGKILL -------------------------
            server = start_server(journal, upstream_port, port, log)
            channel = grpc.insecure_channel("127.0.0.1:%d" % port)
            stub = asrrpc.ProductASRServiceStub(channel)

            started = stub.start_session(
                asr.StartSessionRequest(config_json='{"title":"Khởi động lại"}'),
                timeout=15, metadata=meta)
            session_id = started.session_id
            check(bool(session_id), "start_session through the buffer (%s)" % session_id)

            for seq in range(1, BEFORE_STALL + 1):
                push(stub, session_id, seq, meta)
            check(wait_for(lambda: len(pipeline.seqs) >= BEFORE_STALL, 10),
                  "the pipeline received the first %d packets" % BEFORE_STALL)

            pipeline.refuse = True
            for seq in range(BEFORE_STALL + 1, BEFORE_STALL + DURING_STALL + 1):
                push(stub, session_id, seq, meta)
            check(True, "%d more packets accepted while the pipeline refuses" % DURING_STALL)
            time.sleep(0.4)
            check(len(pipeline.seqs) == BEFORE_STALL,
                  "the pipeline still has only %d (%d)" % (BEFORE_STALL, len(pipeline.seqs)))
            channel.close()

            # No destructors, no flush, no clean shutdown of any kind.  Whatever
            # is on disk at this instant is all the recovery gets.
            server.send_signal(signal.SIGKILL)
            server.wait(timeout=15)
            check(True, "server killed with SIGKILL (no clean shutdown)")

            # ---- phase 2: the pipeline recovers, and so does the server ---
            pipeline.refuse = False
            server = start_server(journal, upstream_port, port, log)
            channel = grpc.insecure_channel("127.0.0.1:%d" % port)
            stub = asrrpc.ProductASRServiceStub(channel)

            total = BEFORE_STALL + DURING_STALL
            check(wait_for(lambda: len(pipeline.seqs) >= total, 20),
                  "the backlog reached the pipeline after the restart (%d of %d)"
                  % (len(pipeline.seqs), total))
            check(pipeline.seqs == list(range(1, total + 1)),
                  "every packet arrived exactly once, in order, across a SIGKILL: %s"
                  % pipeline.seqs)

            # A client retrying its last seq - what its transport-retry loop
            # does - must not have its audio counted twice.
            push(stub, session_id, total, meta)
            push(stub, session_id, total + 1, meta)
            check(wait_for(lambda: len(pipeline.seqs) >= total + 1, 10),
                  "the meeting continues at seq %d" % (total + 1))
            time.sleep(0.4)
            check(pipeline.seqs == list(range(1, total + 2)),
                  "and the resent seq produced no duplicate: %s" % pipeline.seqs)

            stub.stop_session(asr.SessionRequest(session_id=session_id), timeout=30,
                              metadata=meta)
            check(pipeline.events[-1] == "stop", "stop_session still reached the pipeline last")
            channel.close()
            server.send_signal(signal.SIGTERM)
            server.wait(timeout=30)

        print("ALL PASS" if failures == 0 else "%d FAILURE(S)" % failures)
        if failures:
            with open(log_path, "r", errors="replace") as log:
                print("---- server log ----")
                print(log.read()[-4000:])
    finally:
        upstream.stop(0)
        shutil.rmtree(journal, ignore_errors=True)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
