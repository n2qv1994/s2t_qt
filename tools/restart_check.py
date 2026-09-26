#!/usr/bin/env python3
"""Kiểm tra ở mức tiến trình: giết s2t-qt-server bằng SIGKILL giữa cuộc họp,
bật lại, và xem cuộc họp có đi tiếp không.

Bài --selftest trong C++ đã kiểm toàn bộ logic khôi phục, nhưng nó tháo đối
tượng chứ không giết tiến trình - nên nó KHÔNG chứng minh được điều quan trọng
nhất: rằng bản ghi đã nằm ngoài tiến trình trước khi client được ACK.  SIGKILL
không chạy destructor, không flush gì cả.  Nếu Journal còn giữ dữ liệu trong bộ
đệm của Qt thì chỉ bài này mới phát hiện ra.

Tầng suy luận giả là một Triton giả, nói đúng giao thức gRPC của Triton
(inference.GRPCInferenceService) - vì từ khi bỏ adapter, server nói thẳng với
Triton.  Bản trước của tệp này giả lập adapter cũ (ProductASRService); server
không còn gọi dịch vụ đó nữa, nên nó chỉ nhận về "Method not found" và mọi
khẳng định đều FAIL mà không nói gì về server.

Stub Triton sinh từ tools/triton_grpc.desc: FileDescriptorSet của
grpc_service.proto + model_config.proto CHÍNH THỨC, trích từ tritonclient
2.49.0 (giấy phép BSD) trong container triton_ui_debug trên máy RHEL:

    from google.protobuf import descriptor_pb2
    from tritonclient.grpc import service_pb2, model_config_pb2
    s = descriptor_pb2.FileDescriptorSet()
    for m in (model_config_pb2, service_pb2):
        m.DESCRIPTOR.CopyToProto(s.file.add())
    open("triton_grpc.desc", "wb").write(s.SerializeToString())

Dùng schema của Triton chứ không tự viết lại: bộ mã protobuf của server là bản
tự viết (shared/proto/TritonInfer.cpp), và một Triton giả dựng từ cùng hiểu
biết đó có thể đồng ý với server trong khi cả hai cùng sai.  Tệp này tự sinh
stub Triton vào out/ nếu chưa có.

Mỗi gói audio mang MỘT giá trị mẫu hằng số (seq * 64), gửi ở 48 kHz như client
thật.  Server chuẩn hoá về 16 kHz từng gói một (Pcm16k.cpp, không mang trạng
thái qua gói), nên giá trị vẫn là hằng số và Triton giả đọc ngược ra seq từ
tensor audio_chunk.  Request gửi Triton không mang seq, nên đây là cách duy
nhất để biết gói nào tới, bao nhiêu lần, theo thứ tự nào.

Triton giả trả về cho mỗi gói một từ "s<seq>" trong itn_merged_words_json,
tính giờ TỪ ĐẦU STREAM CỦA NÓ, và chốt tới cuối từ đó - đúng hợp đồng mà
LiveTranscript đọc.  Sau khi khôi phục, stream mới đếm lại từ 0; nếu server dời
mốc thời gian đúng (bản sửa N10, 2026-09-24) thì từ đầu tiên sau khi khôi phục
nằm đúng chỗ của nó trong cuộc họp, không phải ở 0 s.

Cách chạy (cần python3 + grpcio-tools; máy RHEL đã có).  Sinh stub
asr_session từ docs/danh-sach-api.md vào out/ trước, đúng như interop_check.py
mô tả, rồi:

    python3 restart_check.py /đường/dẫn/s2t-qt-server [tham số thêm cho server...]

Mọi tham số sau đường dẫn được chuyển thẳng cho s2t-qt-server, nên chạy được cả
các chế độ độ bền khác:

    python3 restart_check.py ./s2t-qt-server --durability fsync
    python3 restart_check.py ./s2t-qt-server --journal-keep session

Không đụng tới server đang chạy thật: server thử nghiệm nghe ở 127.0.0.1:18877,
Triton giả ở một cổng ngẫu nhiên, journal và kho phiên trong một thư mục tạm.
"""
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
from concurrent import futures

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = "out"
sys.path.insert(0, OUT)


def ensure_triton_stubs():
    if os.path.exists(os.path.join(OUT, "grpc_service_pb2_grpc.py")):
        return
    from grpc_tools import protoc
    os.makedirs(OUT, exist_ok=True)
    desc = os.path.join(HERE, "triton_grpc.desc")
    code = protoc.main(["protoc", "--descriptor_set_in=" + desc, "--python_out=" + OUT,
                        "--grpc_python_out=" + OUT, "model_config.proto", "grpc_service.proto"])
    if code != 0:
        sys.exit("không sinh được stub Triton từ %s" % desc)


ensure_triton_stubs()

import grpc  # noqa: E402
import asr_session_pb2 as asr  # noqa: E402
import asr_session_pb2_grpc as asrrpc  # noqa: E402
import grpc_service_pb2 as trt  # noqa: E402
import grpc_service_pb2_grpc as trtrpc  # noqa: E402

SERVER = sys.argv[1] if len(sys.argv) > 1 else "./s2t-qt-server"
EXTRA = sys.argv[2:]
TOKEN = "restart-token"
MODEL = "asr_diar_session"
PORT = 18877

CLIENT_RATE = 48000                 # what the real client sends
PIPELINE_RATE = 16000               # what the tier is always handed
PACKET_SEC = 0.1
PACKET_FRAMES = int(CLIENT_RATE * PACKET_SEC)            # 4800
PIPELINE_FRAMES = int(PIPELINE_RATE * PACKET_SEC)        # 1600
VALUE_STEP = 64                     # sample value of packet n is n * 64

BEFORE_STALL = 4
DURING_STALL = 6
# The server writes its transcript snapshot to the store every ~2 s (it
# stretches with the cost of a write, see SessionBuffer::saveState).  Waiting
# past that before the SIGKILL is what makes "the words before the crash
# survive it" a fair thing to assert.
SNAPSHOT_WAIT_SEC = 3.0
STOP_FLUSH_TICKS = 6                # kFlushTicks in TritonBackend.cpp

failures = 0


def check(ok, what):
    global failures
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        failures += 1


def packet_pcm(seq):
    return struct.pack("<h", seq * VALUE_STEP) * PACKET_FRAMES


# ---- the fake tier ----------------------------------------------------------

def tensor_bytes(value):
    raw = value.encode("utf-8")
    return struct.pack("<I", len(raw)) + raw


class FakeTriton(trtrpc.GRPCInferenceServiceServicer):
    """Stands in for Triton serving asr_diar_session.  Outlives the restart."""

    def __init__(self):
        self.lock = threading.Lock()
        self.calls = []          # every accepted ModelInfer, decoded
        self.refuse = False
        self.stream_pos = {}     # stream_id -> samples seen in that stream
        self.part = 0

    # The server's upstream health check.
    def ServerLive(self, request, context):
        return trt.ServerLiveResponse(live=True)

    def ServerReady(self, request, context):
        return trt.ServerReadyResponse(ready=True)

    def ModelReady(self, request, context):
        return trt.ModelReadyResponse(ready=True)

    def RepositoryIndex(self, request, context):
        response = trt.RepositoryIndexResponse()
        response.models.add(name=MODEL, version="1", state="READY")
        return response

    def ModelInfer(self, request, context):
        if self.refuse:
            # A transport status: the buffer keeps the packet and retries it,
            # which is how the backlog that has to survive the SIGKILL is built.
            context.abort(grpc.StatusCode.UNAVAILABLE, "tầng suy luận đang từ chối (thử nghiệm)")

        inputs = {}
        for index, tensor in enumerate(request.inputs):
            raw = request.raw_input_contents[index] if index < len(request.raw_input_contents) else b""
            inputs[tensor.name] = (tensor, raw)

        audio_raw = inputs.get("audio_chunk", (None, b""))[1]
        count = len(audio_raw) // 4
        samples = struct.unpack("<%df" % count, audio_raw) if count else ()
        values = {round(s * 32768.0) for s in samples}
        seq = None
        if len(values) == 1:
            value = values.pop()
            seq = value // VALUE_STEP if value % VALUE_STEP == 0 else None

        def int_input(name, fmt):
            raw = inputs.get(name, (None, b""))[1]
            return struct.unpack(fmt, raw[:struct.calcsize(fmt)])[0] if raw else None

        call = {
            "model": request.model_name,
            "stream": int_input("stream_id", "<q"),
            "reset": int_input("reset", "<i"),
            "final": int_input("is_final", "<i"),
            "valid": int_input("valid_samples", "<i"),
            "samples": count,
            "seq": seq,                       # 0 = silence (the stop flush)
            "mixed": count > 0 and seq is None,
        }

        response = trt.ModelInferResponse(model_name=request.model_name, id=request.id)
        with self.lock:
            stream = call["stream"]
            if call["reset"] or stream not in self.stream_pos:
                self.stream_pos[stream] = 0
            start = self.stream_pos[stream] / PIPELINE_RATE
            self.stream_pos[stream] += count
            end = self.stream_pos[stream] / PIPELINE_RATE
            self.calls.append(call)

            if seq:   # a data packet: one word, placed on THIS stream's clock
                self.part += 1
                word = {"w": "s%d" % seq, "c": 0.99, "start_sec": start, "end_sec": end,
                        "speaker": "0", "itn_part_text": "s%d" % seq,
                        "itn_source_part_idx": self.part}
                outputs = [
                    ("itn_merged_words_json", "BYTES", tensor_bytes(json.dumps([word]))),
                    ("itn_correction_text", "BYTES", tensor_bytes(word["w"])),
                    ("itn_commit_boundary_sec", "FP32", struct.pack("<f", end)),
                ]
                for name, datatype, raw in outputs:
                    response.outputs.add(name=name, datatype=datatype, shape=[1])
                    response.raw_output_contents.append(raw)
        return response

    # -- what the assertions read --
    def snapshot(self):
        with self.lock:
            return list(self.calls)


def data_seqs(calls):
    return [c["seq"] for c in calls if c["seq"]]


# ---- the client side --------------------------------------------------------

def push(stub, session_id, seq, meta):
    request = asr.PushAudioRequest(session_id=session_id, pcm=packet_pcm(seq),
                                   sample_rate=CLIENT_RATE, channels=1, audio_format="s16le",
                                   reset=(seq == 1), vad_chunk_ms=160, seq=seq)
    return stub.push_audio(request, timeout=10, metadata=meta)


def wait_for(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


def start_server(workdir, upstream_port, log):
    process = subprocess.Popen(
        [SERVER, "--listen", "127.0.0.1:%d" % PORT, "--token", TOKEN,
         "--backend", "triton", "--model", MODEL,
         "--upstream", "127.0.0.1:%d" % upstream_port,
         "--journal-dir", os.path.join(workdir, "journal"),
         "--database-dir", os.path.join(workdir, "database"),
         "--log-level", "info"] + EXTRA,
        stdout=log, stderr=subprocess.STDOUT)
    # Wait for the port rather than sleeping a guessed amount.
    channel = grpc.insecure_channel("127.0.0.1:%d" % PORT)
    try:
        grpc.channel_ready_future(channel).result(timeout=15)
    finally:
        channel.close()
    return process


def main():
    global failures
    workdir = tempfile.mkdtemp(prefix="s2t-restart-")
    meta = (("authorization", "Bearer " + TOKEN),)
    tier = FakeTriton()
    upstream = grpc.server(futures.ThreadPoolExecutor(max_workers=8))
    trtrpc.add_GRPCInferenceServiceServicer_to_server(tier, upstream)
    upstream_port = upstream.add_insecure_port("127.0.0.1:0")
    upstream.start()
    log_path = os.path.join(workdir, "server.log")
    total = BEFORE_STALL + DURING_STALL
    server = None

    try:
        with open(log_path, "wb") as log:
            # ---- phase 1: a meeting, then SIGKILL -------------------------
            server = start_server(workdir, upstream_port, log)
            channel = grpc.insecure_channel("127.0.0.1:%d" % PORT)
            stub = asrrpc.ProductASRServiceStub(channel)

            started = stub.start_session(
                asr.StartSessionRequest(config_json='{"session_title":"Khởi động lại"}'),
                timeout=15, metadata=meta)
            session_id = started.session_id
            check(bool(session_id), "start_session through the buffer (%s)" % session_id)

            for seq in range(1, BEFORE_STALL + 1):
                push(stub, session_id, seq, meta)
            check(wait_for(lambda: len(data_seqs(tier.snapshot())) >= BEFORE_STALL, 10),
                  "Triton received the first %d packets" % BEFORE_STALL)
            first = tier.snapshot()
            check(first and first[0]["reset"] == 1 and first[0]["model"] == MODEL,
                  "the stream opened with reset=1 on model %s" % MODEL)
            check(all(c["samples"] == PIPELINE_FRAMES for c in first if c["seq"]),
                  "each 48 kHz packet reached Triton as %d samples at 16 kHz (%s)"
                  % (PIPELINE_FRAMES, sorted({c["samples"] for c in first})))

            tier.refuse = True
            for seq in range(BEFORE_STALL + 1, total + 1):
                push(stub, session_id, seq, meta)
            check(True, "%d more packets ACKed while Triton refuses" % DURING_STALL)
            time.sleep(SNAPSHOT_WAIT_SEC)
            check(len(data_seqs(tier.snapshot())) == BEFORE_STALL,
                  "Triton still has only %d (%d)"
                  % (BEFORE_STALL, len(data_seqs(tier.snapshot()))))
            channel.close()
            before_kill = len(tier.snapshot())

            # No destructors, no flush, no clean shutdown of any kind.  Whatever
            # is on disk at this instant is all the recovery gets.
            server.send_signal(signal.SIGKILL)
            server.wait(timeout=15)
            check(True, "server killed with SIGKILL (no clean shutdown)")

            # ---- phase 2: the tier recovers, and so does the server -------
            tier.refuse = False
            server = start_server(workdir, upstream_port, log)
            channel = grpc.insecure_channel("127.0.0.1:%d" % PORT)
            stub = asrrpc.ProductASRServiceStub(channel)

            check(wait_for(lambda: len(data_seqs(tier.snapshot())) >= total, 20),
                  "the backlog reached Triton after the restart (%d of %d)"
                  % (len(data_seqs(tier.snapshot())), total))
            calls = tier.snapshot()
            check(data_seqs(calls) == list(range(1, total + 1)),
                  "every packet arrived exactly once, in order, across a SIGKILL: %s"
                  % data_seqs(calls))
            after = calls[before_kill:]
            check(bool(after) and after[0]["reset"] == 1,
                  "the recovered session reopened its Triton stream with reset=1")
            check(not any(c["mixed"] for c in calls),
                  "no request mixed two packets' audio")

            # A client retrying its last seq - what its transport-retry loop
            # does - must not have its audio counted twice.
            push(stub, session_id, total, meta)
            push(stub, session_id, total + 1, meta)
            check(wait_for(lambda: len(data_seqs(tier.snapshot())) >= total + 1, 10),
                  "the meeting continues at seq %d" % (total + 1))
            time.sleep(0.4)
            check(data_seqs(tier.snapshot()) == list(range(1, total + 2)),
                  "and the resent seq produced no duplicate: %s" % data_seqs(tier.snapshot()))

            stub.stop_session(asr.SessionRequest(session_id=session_id), timeout=60,
                              metadata=meta)
            tail = tier.snapshot()[-(STOP_FLUSH_TICKS + 1):]
            check(len(tail) == STOP_FLUSH_TICKS + 1
                  and all(c["seq"] == 0 and not c["final"] for c in tail[:-1])
                  and tail[-1]["final"] == 1 and tail[-1]["samples"] == 0,
                  "stop reached Triton last: %d silence ticks, then an empty is_final"
                  % STOP_FLUSH_TICKS)

            # ---- the transcript, across the crash -------------------------
            state = stub.get_review_state(asr.ReviewRequest(session_id=session_id),
                                          timeout=15, metadata=meta).state
            words = [(w.w, w.start_sec) for row in state.rows for w in row.display_tokens]
            got = [w for w, _ in words]
            want = ["s%d" % n for n in range(1, total + 2)]
            check(got == want,
                  "the transcript has every word once, the ones before the SIGKILL "
                  "included: %s" % got)
            placed = {w: start for w, start in words}
            misplaced = ["%s@%.2f" % (w, placed[w]) for w in want
                         if w in placed
                         and abs(placed[w] - (int(w[1:]) - 1) * PACKET_SEC) > 0.02]
            check(not misplaced,
                  "every word sits at its own time in the MEETING, not in the new "
                  "stream (s%d at %.2f s): %s"
                  % (BEFORE_STALL + 1, placed.get("s%d" % (BEFORE_STALL + 1), -1.0),
                     misplaced or "ok"))

            channel.close()
            server.send_signal(signal.SIGTERM)
            check(server.wait(timeout=30) == 0, "SIGTERM shuts the server down cleanly")
            server = None

        print("ALL PASS" if failures == 0 else "%d FAILURE(S)" % failures)
    except Exception:
        failures += 1
        raise
    finally:
        # The server's own log is the first thing to read when a check fails,
        # and it has to be printed before the work directory goes - including
        # when the run died on an exception halfway through.
        if failures and os.path.exists(log_path):
            with open(log_path, "r", errors="replace") as log:
                print("---- server log (tail) ----")
                print(log.read()[-6000:])
        if server is not None and server.poll() is None:
            server.kill()
            server.wait()
        upstream.stop(0)
        shutil.rmtree(workdir, ignore_errors=True)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
