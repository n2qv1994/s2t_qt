#!/usr/bin/env python3
"""Checks SpeakerRegistryService end to end against a running s2t-qt-server.

    client --gRPC--> s2t-qt-server --HTTP--> campp_native/enroll_service.py

Same reason as tools/demo_flow.py: our client and our server can agree with
each other and both be wrong about the wire, and only a third implementation
built from the .proto catches it.

By default it does the two read-only calls.  `--enroll <file>` also performs a
real enrolment, which writes to the shared CAM++ database and reruns rebuild_db
over every speaker on file - so it is opt-in, not part of the default run.

    PYTHONPATH=<dir with the generated stubs> \\
    python3 tools/enroll_check.py --target 127.0.0.1:18800 --token demo
"""

import argparse
import sys
import wave

import grpc

import speaker_registry_pb2 as pb
import speaker_registry_pb2_grpc as pb_grpc


def to_wav_16k_mono(path):
    """CAM++ takes a complete WAV at 16 kHz mono and infers nothing from the
    name, so anything else is rejected on the far side rather than resampled."""
    with wave.open(path, "rb") as handle:
        if handle.getsampwidth() != 2:
            raise SystemExit(f"{path}: cần PCM 16-bit")
        if handle.getframerate() != 16000 or handle.getnchannels() != 1:
            raise SystemExit(
                f"{path}: cần 16000 Hz mono, tệp này {handle.getframerate()} Hz "
                f"/ {handle.getnchannels()} kênh"
            )
    with open(path, "rb") as handle:
        return handle.read()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", default="127.0.0.1:18800")
    parser.add_argument("--token", default="")
    parser.add_argument("--enroll", help="tệp WAV 16 kHz mono để đăng ký thật")
    parser.add_argument("--name", default="Kiểm thử s2t-qt")
    parser.add_argument("--editor", default="selftest")
    args = parser.parse_args()

    metadata = [("authorization", f"Bearer {args.token}")] if args.token else []
    stub = pb_grpc.SpeakerRegistryServiceStub(grpc.insecure_channel(args.target))

    script = stub.GetEnrollmentScript(
        pb.GetEnrollmentScriptRequest(), metadata=metadata, timeout=30
    )
    print("GetEnrollmentScript:")
    print(f"  sample_rate              = {script.sample_rate}")
    print(f"  recommended_duration_sec = {script.recommended_duration_sec}")
    print(f"  target_segments          = {script.target_segments}")
    print(f"  script_text[:70]         = {script.script_text[:70]!r}")
    assert script.sample_rate == 16000, "kịch bản phải yêu cầu 16 kHz"
    assert script.script_text, "kịch bản rỗng"

    status = stub.GetSpeakerRegistryStatus(
        pb.GetSpeakerRegistryStatusRequest(session_id=""), metadata=metadata, timeout=30
    )
    print("GetSpeakerRegistryStatus:")
    print(f"  global_speaker_count = {status.global_speaker_count}")
    print(f"  sidecar_reachable    = {status.sidecar_reachable}")
    print(f"  global_db_mtime      = {status.global_db_mtime}")
    print(f"  names[:5]            = {list(status.global_speaker_names)[:5]}")
    print(f"  below_policy         = {len(status.speakers_below_policy)}")

    if args.enroll:
        wav_bytes = to_wav_16k_mono(args.enroll)
        print(f"EnrollSpeaker: {args.name!r}, {len(wav_bytes)} byte WAV...")
        reply = stub.EnrollSpeaker(
            pb.EnrollSpeakerRequest(
                display_name=args.name,
                wav=wav_bytes,
                editor_id=args.editor,
                note="tools/enroll_check.py",
                allow_below_policy=True,
            ),
            metadata=metadata,
            timeout=300,
        )
        print(f"  ok                       = {reply.ok}")
        print(f"  error                    = {reply.error!r}")
        print(f"  speaker_id               = {reply.speaker_id!r}")
        print(f"  raw_seconds              = {reply.raw_seconds}")
        print(f"  speech_seconds_after_vad = {reply.speech_seconds_after_vad}")
        print(f"  segments_enrolled        = {reply.segments_enrolled}"
              f" / {reply.target_segments}")
        print(f"  warning                  = {reply.warning!r}")

    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
