# s2t_qt — native Qt/C++ client for the S2T pipeline

A Qt 6 Widgets desktop application that replaces the two client-side pieces of
`s2t-dgpu`:

| Replaced | Was | Now |
|---|---|---|
| `ui_client/debug_ui_bridge.py` | Python HTTP bridge: mic capture, packetisation, retry, local JSON API | in-process C++ workers |
| `ui_client/live_ui.html` + `enroll_ui.html` + `pipeline_trace_ui.html` + `pipeline_evidence_ui.html` | browser polling that bridge | native widgets |

The browser and the local HTTP hop are gone. This app speaks **gRPC directly
to the Linux adapter** (`grpc_session_adapter.py`, default `:8700`), which is
one fewer process, one fewer port and one fewer serialisation boundary on the
live audio path.

Everything server-side is unchanged: the Triton GPU pipeline, the adapter, the
CAM++ sidecar and both `.proto` contracts are exactly as they were.

```
s2t_qt (this app)
  │ unary gRPC/protobuf + Bearer token
  ▼
grpc_session_adapter.py (:8700)
  │ Triton gRPC
  ▼
Triton (:8011)
```

## Why the protocol stack is hand written

The Qt kit on the target machine ships **no Qt::Grpc, no Qt::Protobuf, no
protoc, no vcpkg**. Rather than add a build-time dependency chain to a
deployed workstation, `proto/` and `grpc/` implement what is needed directly:

| Directory | Contents |
|---|---|
| `proto/` | proto3 wire codec, plus C++ mirrors of `asr_session.proto` and `speaker_registry.proto` |
| `grpc/` | HPACK (RFC 7541) with Huffman + dynamic table, a blocking HTTP/2 client, gRPC unary calls, and a typed façade over every RPC |

Total external dependencies: **none beyond Qt itself.**

That answer survives the move to RHEL unchanged: `qt6-qtgrpc` and
`qt6-qtprotobuf` are not in RHEL 9 AppStream either, so the alternative there
would have been building a gRPC C++ stack from source on a production host.
`dnf install qt6-qtbase-devel qt6-qtmultimedia-devel` is the whole dependency
list.

## Threading

One concern per thread, each with its own gRPC channel — the same separation
the Python bridge used, for the same reason: a multi-megabyte state
serialisation must never queue in front of a 160 ms audio packet.

| Thread | Owns | Job |
|---|---|---|
| GUI | `SessionController`, all widgets | render, dispatch |
| `audio-capture` | `QAudioSource` | 20 ms buffers → bounded `AudioQueue` |
| session worker | one channel | `start_session` → `push_audio` loop → `stop_session` |
| state poller | one channel | `get_live_state` every 200 ms |
| `rpc-lane-0..2` | one channel each | review, audio, edit, enrolment, trace |

## Behaviour carried over deliberately

These are not incidental; each exists because of a specific failure it
prevents, and the reasoning is in the comment at each site.

- **The device is opened before the session is created.** A driver failure
  reads as "cannot record", instead of leaving an empty meeting open on the
  server with nothing ever arriving for it.
- **`seq` idempotency + transport-only retry.** Only `UNAVAILABLE`,
  `DEADLINE_EXCEEDED` and `CANCELLED` are retried. `INTERNAL` is not: the
  adapter returns it precisely when the server may already have consumed the
  audio, so a blind retry duplicates words.
- **Two queues reported separately.** `ACK` means *durably spooled*, never
  *inferred*. "Hàng đợi máy này" and "Hàng đợi server AI" are different
  numbers and are shown as such.
- **Pause discards at capture time**, so speech spoken while paused is never
  delivered later as if it had been live.
- **Device identity is re-checked on a timer.** An OS can re-use an audio
  endpoint after an unplug — a WASAPI endpoint id on Windows, a PipeWire or
  PulseAudio node name on Linux — and a stale endpoint can keep a stream
  "running" while delivering silence, so name, presence *and* byte progress
  are all checked. A lost device pauses the session and waits; it never ends
  it and never inserts silence.
- **Tri-state `expected_speakers`.** Key absent = match the whole registry;
  explicit empty list = assign no registered name. The start dialog makes the
  difference explicit rather than implying it.
- **Commit boundary is honoured client-side.** A word past it is shown
  read-only, mirroring the server's own `edit_range_not_committed` rule,
  instead of letting the operator type into a rejection.
- **Optimistic concurrency on every edit** via `base_revision`.
- **Operator name is never remembered between runs.** It is recorded as the
  person answerable for a change; a field that refills itself would file one
  person's work under another's.
- **Denoise A/B restore is reported honestly.** The host-control tool cannot
  read the current state back, so on a fresh start the prior state is genuinely
  unknown and the dashboard says so instead of claiming a restore.
- **Span stitching refuses rather than truncates.** Over 200 spans or 300 s is
  a hard refusal, and a `..._truncated` payload flag is surfaced as a warning,
  because playing back a third less audio than the model received while
  labelling it "the real input" is worse than not playing it.

## One bug fixed rather than ported

`live_ui.html` deleted a superseded `sid:N` lane once verification produced a
real identity, then immediately re-created it from `state.speaker_ids` on the
next loop — so an identified speaker appeared twice, once under their name and
once as an empty `speaker_N` row. `TranscriptModel::rebuild()` skips
re-creating a slot that verification has superseded. This is what the
`two speaker lanes built` assertion in `--selftest-net` covers.

## Build

Two supported toolchains. The sources are the same on both; the only
platform-specific code is the console shim in `main.cpp`, the display check
beside it, and the file filter for the reSpeaker host-control tool.

**RHEL 9** (target: 9.8, glibc 2.34, gcc 11.5, x86_64):

```
sudo dnf install gcc-c++ make qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qtbase-gui
tools/build_rhel9.sh                 # → ../build-rhel/s2t_qt
```

The script exists for one reason: RHEL 9 carries Qt 5 and Qt 6 side by side
and plain `qmake` is the Qt 5 one, which cannot build this app. It locates a
Qt 6 `qmake` (`qmake6`, `qmake-qt6`, `/usr/lib64/qt6/bin/qmake`, or `$QMAKE6`)
and verifies the version before using it. By hand it is:

```
mkdir ../build-rhel && cd ../build-rhel
qmake6 ../s2t_qt/s2t_qt.pro
make -j"$(nproc)"
```

`-std=c++17` is all this needs, so gcc 11.5 is comfortably enough. Optional
at runtime: `nodejs` (the mock adapter used by `--selftest-net`),
`ffmpeg-free` (`.m4a` replay), `valgrind` and `gdb`.

**Windows** (the deployed workstation kit):

```
qmake s2t_qt.pro
mingw32-make -j8
```

Qt 6.11.2 MinGW 64-bit, GCC 13.1. Qt 6.2 is the floor on either platform —
`s2t_qt.pro` checks the version and the presence of Qt Multimedia up front and
stops with a named reason rather than a page of missing headers. `-Wall
-Wextra` is set in the `.pro` rather than left to the kit, and the tree builds
clean under it.

## Self-tests

```
s2t_qt --selftest                         # proto3 + HPACK, no network
s2t_qt --selftest-net 127.0.0.1:18700     # end-to-end against the mock
s2t_qt --probe 192.168.1.47:8700 --token <token>   # real adapter
```

`--selftest` checks the proto3 codec and HPACK against the **RFC 7541 C.4.1
and C.6.1 reference vectors**, including Huffman decoding and dynamic-table
indexing.

`--selftest-net` runs against `tools/mock_adapter.js`, a dependency-free Node
HTTP/2 server. Node's http2 is nghttp2, so it Huffman-encodes responses and
uses its own HPACK dynamic table — which is the point: it proves the client
interoperates with an independent implementation, not just with itself. It
also returns a ~2 MB payload (forcing real `WINDOW_UPDATE` flow control),
trailers-only error responses, and a percent-encoded Vietnamese
`grpc-message`.

```
node tools/mock_adapter.js 18700
s2t_qt --selftest-net 127.0.0.1:18700
```

`--probe` is also a field diagnostic: it answers "is the adapter reachable and
is this token accepted" without starting a session.

## Memory checking and debugging on RHEL

The RHEL host has gdb 16.3 and valgrind 3.26, and neither is much use against
an `-O2` build with no frame pointers, so the build carries a mode for them:

```
tools/build_rhel9.sh memcheck        # -O1 -g3 -fno-omit-frame-pointer
cd ../build-rhel && ../s2t_qt/tools/run_valgrind.sh
```

`run_valgrind.sh` defaults to `--selftest` on purpose. That mode drives
exactly the code with no library standing behind it — the proto3 codec, HPACK
with its dynamic table, HTTP/2 framing — with no GUI, no device and no
network, so anything memcheck reports there is this project's own bug.
`tools/run_valgrind.sh --selftest-net 127.0.0.1:18700` extends the same run
over a real socket against the mock adapter.

`tools/valgrind.supp` suppresses only third-party noise: the dynamic loader,
Qt's plugin loader, glib/GStreamer/FFmpeg registries, PulseAudio/PipeWire,
fontconfig and Mesa. Nothing under `proto/`, `grpc/`, `core/`, `audio/` or
`ui/` is suppressed, which is the point of keeping the list short.

Under gdb, the worker threads are named (`audio-capture`, `session-worker`,
`state-poller`, `rpc-lane-0..2`), so `thread apply all bt` reads as the
threading table above rather than as a list of numbers.

## Configuration

Settings are edited in **Cấu hình** and stored by `QSettings` in whatever the
platform's own place is — the registry under `HKCU\Software\s2t\s2t_qt` on
Windows, `~/.config/s2t/s2t_qt.conf` on RHEL. Same keys either way: server
`host:port`, Bearer token, microphone, the device-name substring the bound
device must match, sample rate/channels, the bounded queue length, the
`xvf_host` path, pipeline trace, and file-replay pacing.

The token is stored in that file in plain text, so on the Linux host the file
should be `chmod 600` — `QSettings` creates it `0600` already, but a config
directory copied between machines will not keep that.

These are the same knobs `run_windows_ui.ps1` passed on the command line,
minus everything that only existed because the UI was a browser talking to a
local HTTP bridge — there is no UI port, upload directory or token file to
point at any more.

## Low-confidence words

The browser UI dropped every token below **0.75** confidence from the timeline
entirely (`token_low_threshold` in the bridge's snapshot), which also made its
own "low confidence" styling unreachable. That default is preserved so the
timeline reads the same, but the **HIỆN TỪ YẾU** toggle now lets an operator
see everything, drawn in the low-confidence style, without a rebuild. The
Highlights panel is unaffected either way — it comes from the server.

## Known gaps

- `.m4a` decoding needs `ffmpeg` on `PATH`. Without it the app says so plainly
  rather than reporting a corrupt file; plain 16-bit PCM WAV needs nothing.
  On RHEL 9 that is `dnf install ffmpeg-free` (or the RPM Fusion build).
- Ticker mode is a flowing coloured transcript rather than the browser's
  horizontal teleprompter blocks.
- The evidence dashboard's model **architecture** labels are compiled in (they
  were `ASR_MODEL_LABEL` &c. environment overrides on the bridge). The model
  **status** table next to them is read live from Triton and is the actual
  evidence.
- **Qt 6 only.** Capture is `QAudioSource`/`QMediaDevices` and playback is
  `QMediaPlayer::setAudioOutput`; there is no Qt 5 spelling of those to fall
  back to, so `s2t_qt.pro` fails fast with a named reason rather than letting
  RHEL's default Qt 5 `qmake` produce a wall of missing-header errors.
- **The GUI needs a display.** Over a bare `ssh` session the app now exits
  with an explanation and the list of headless modes, instead of aborting
  inside the Qt platform plugin. Use `ssh -X`, the machine's own console, or
  a VNC session.
- **The reSpeaker host-control tool is optional on Linux.** Left blank, the
  denoise toggle and the A/B evidence recorder report that no tool is
  configured; everything else works. Point it at the ELF `xvf_host` binary
  (no `.exe`) and make sure the execute bit is set — the app now says so by
  name when it is not.
