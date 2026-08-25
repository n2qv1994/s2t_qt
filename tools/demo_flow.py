#!/usr/bin/env python3
"""Drives the demo flow end to end against a running s2t-qt-server.

    s2t-qt-client  ->  s2t-qt-server  ->  Riva / Triton  ->  text  ->  client

This stands in for the client half so the server half can be proved on its own,
with a real gRPC implementation on the far side rather than our own.  That is
the point: our client and our server can agree with each other and both be
wrong about the wire format, and only a third implementation catches it.

It also answers the question the demo asks - "is 2 s chunking right?" - by
measuring it.  Audio goes in at the client's real cadence (160 ms) and the
transcript is polled while it does, so the delay between a word being spoken
and appearing is what the numbers at the end describe.

Needs the generated stubs; see tools/interop_check.py for the recipe, or reuse
the ones already sitting next to grpc_session_adapter.py.

    python3 tools/demo_flow.py --target 127.0.0.1:18800 --token demo \\
                               --wav ~/s2t-dgpu/ui_sample_60s.wav
"""

import argparse
import json
import os
import statistics
import sys
import time
import wave

import grpc

import asr_session_pb2 as pb
import asr_session_pb2_grpc as pb_grpc


def read_wav(path):
    """PCM plus its real format, rather than trusting a 44-byte header guess."""
    with wave.open(path, "rb") as handle:
        if handle.getsampwidth() != 2:
            raise SystemExit(f"{path}: cần PCM 16-bit, tệp này {handle.getsampwidth() * 8}-bit")
        return (
            handle.readframes(handle.getnframes()),
            handle.getframerate(),
            handle.getnchannels(),
        )


def rows_text(state):
    """The settled transcript, plus whatever edge is still moving."""
    settled = " ".join(row.merged_text for row in state.rows if row.merged_text)
    moving = " ".join(
        row.updating_text or row.merged_text for row in state.provisional_rows
    )
    return settled, moving


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", default="127.0.0.1:18800")
    parser.add_argument("--token", default="")
    parser.add_argument("--wav", required=True)
    parser.add_argument(
        "--chunk-ms",
        type=int,
        default=160,
        help="gửi mỗi gói bấy nhiêu mili-giây (mặc định 160, bằng nhịp của client thật)",
    )
    parser.add_argument(
        "--realtime",
        action="store_true",
        help="đẩy đúng tốc độ thời gian thực thay vì nhanh nhất có thể",
    )
    parser.add_argument("--poll-ms", type=int, default=200)
    args = parser.parse_args()

    pcm, rate, channels = read_wav(os.path.expanduser(args.wav))
    bytes_per_sec = rate * channels * 2
    chunk = int(bytes_per_sec * args.chunk_ms / 1000)
    total_sec = len(pcm) / bytes_per_sec
    print(
        f"audio: {total_sec:.1f}s  {rate} Hz  {channels} kênh  "
        f"-> {len(pcm) // chunk + 1} gói {args.chunk_ms} ms"
    )

    metadata = [("authorization", f"Bearer {args.token}")] if args.token else []
    channel = grpc.insecure_channel(args.target)
    stub = pb_grpc.ProductASRServiceStub(channel)

    config = {
        "title": "Demo luồng s2t",
        "sample_rate": rate,
        "channels": channels,
        "vad_chunk_ms": args.chunk_ms,
    }
    started = stub.start_session(
        pb.StartSessionRequest(config_json=json.dumps(config)), metadata=metadata, timeout=30
    )
    session_id = started.session_id
    print(f"phiên: {session_id}")

    push_ms = []
    first_text_at = None
    began = time.monotonic()
    last_poll = 0.0
    seq = 0
    offset = 0

    while offset < len(pcm):
        piece = pcm[offset : offset + chunk]
        offset += chunk
        seq += 1
        request = pb.PushAudioRequest(
            session_id=session_id,
            pcm=piece,
            sample_rate=rate,
            channels=channels,
            audio_format="s16le",
            reset=(seq == 1),
            vad_chunk_ms=args.chunk_ms,
            seq=seq,
        )
        sent = time.monotonic()
        stub.push_audio(request, metadata=metadata, timeout=30)
        push_ms.append((time.monotonic() - sent) * 1000.0)

        now = time.monotonic()
        if (now - last_poll) * 1000.0 >= args.poll_ms:
            last_poll = now
            state = stub.get_live_state(
                pb.SessionRequest(session_id=session_id), metadata=metadata, timeout=30
            )
            settled, moving = rows_text(state.state)
            if (settled or moving) and first_text_at is None:
                first_text_at = now - began
                print(f"  chữ đầu tiên sau {first_text_at:.2f}s")
            if settled or moving:
                line = (settled + " ⟨" + moving + "⟩") if moving else settled
                print(f"  [{state.state.source_seen_sec:6.2f}s] {line[-100:]}")

        if args.realtime:
            target = began + (offset / bytes_per_sec)
            slack = target - time.monotonic()
            if slack > 0:
                time.sleep(slack)

    wall = time.monotonic() - began
    stopped = stub.stop_session(
        pb.SessionRequest(session_id=session_id), metadata=metadata, timeout=120
    )

    settled, _ = rows_text(stopped.state)
    print()
    print("--- bản chép cuối ---")
    print(settled or "(rỗng)")
    print()
    print(f"gói đã gửi        : {seq}")
    print(f"thời gian đẩy hết : {wall:.1f}s cho {total_sec:.1f}s audio "
          f"({total_sec / wall:.1f}x thời gian thực)")
    if push_ms:
        print(f"push_audio p50/p95: {statistics.median(push_ms):.1f} / "
              f"{statistics.quantiles(push_ms, n=20)[18]:.1f} ms")
    if first_text_at is not None:
        print(f"chữ đầu tiên sau  : {first_text_at:.2f}s")
    print(f"số dòng           : {len(stopped.state.rows)}")
    print(f"số người nói      : {len(stopped.state.speaker_ids)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
