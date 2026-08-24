// Test double for grpc_session_adapter.py, in plain Node (no dependencies).
//
// Its job is to exercise the hand-written HTTP/2 + HPACK + protobuf stack in
// grpc/ against a real, independent HTTP/2 implementation: Node's http2 is
// nghttp2, which Huffman-encodes response headers and uses its own HPACK
// dynamic table, so a client that only ever talked to itself would not prove
// much.  It also returns a deliberately large payload to force flow-control
// WINDOW_UPDATE handling, and a deliberately failing RPC with a Vietnamese
// grpc-message to check percent-decoding of the status.
//
//   node tools/mock_adapter.js [port]
//
// This is a test fixture, not a second implementation of the pipeline: the
// bodies it returns are hand-built protobuf, not real inference output.

'use strict';

const http2 = require('http2');

const PORT = Number(process.argv[2] || 18700);
const TOKEN = 'test-token';

// ---------------------------------------------------------------- protobuf --

function varint(value) {
  const bytes = [];
  let v = BigInt(value);
  do {
    let byte = Number(v & 0x7fn);
    v >>= 7n;
    if (v > 0n) byte |= 0x80;
    bytes.push(byte);
  } while (v > 0n);
  return Buffer.from(bytes);
}

function tag(field, wire) {
  return varint((field << 3) | wire);
}

function str(field, value) {
  const payload = Buffer.from(value, 'utf8');
  return Buffer.concat([tag(field, 2), varint(payload.length), payload]);
}

function bytes(field, buffer) {
  return Buffer.concat([tag(field, 2), varint(buffer.length), buffer]);
}

function uint(field, value) {
  if (!value) return Buffer.alloc(0);
  return Buffer.concat([tag(field, 0), varint(value)]);
}

function bool(field, value) {
  return value ? Buffer.concat([tag(field, 0), varint(1)]) : Buffer.alloc(0);
}

function dbl(field, value) {
  if (!value) return Buffer.alloc(0);
  const payload = Buffer.alloc(8);
  payload.writeDoubleLE(value);
  return Buffer.concat([tag(field, 1), payload]);
}

function flt(field, value) {
  if (!value) return Buffer.alloc(0);
  const payload = Buffer.alloc(4);
  payload.writeFloatLE(value);
  return Buffer.concat([tag(field, 5), payload]);
}

function msg(field, buffer) {
  return Buffer.concat([tag(field, 2), varint(buffer.length), buffer]);
}

function packedFloat(field, values) {
  if (!values.length) return Buffer.alloc(0);
  const payload = Buffer.alloc(values.length * 4);
  values.forEach((value, index) => payload.writeFloatLE(value, index * 4));
  return bytes(field, payload);
}

// Minimal reader, only enough to read back what the client sent.
function readFields(buffer) {
  const out = {};
  let offset = 0;
  const readVarint = () => {
    let value = 0n;
    let shift = 0n;
    while (offset < buffer.length) {
      const byte = buffer[offset++];
      value |= BigInt(byte & 0x7f) << shift;
      if (!(byte & 0x80)) break;
      shift += 7n;
    }
    return value;
  };
  while (offset < buffer.length) {
    const key = Number(readVarint());
    const field = key >> 3;
    const wire = key & 7;
    if (wire === 0) out[field] = readVarint();
    else if (wire === 1) { out[field] = buffer.subarray(offset, offset + 8); offset += 8; }
    else if (wire === 5) { out[field] = buffer.subarray(offset, offset + 4); offset += 4; }
    else if (wire === 2) {
      const length = Number(readVarint());
      out[field] = buffer.subarray(offset, offset + length);
      offset += length;
    } else break;
  }
  return out;
}

// ------------------------------------------------------------- responses ----

function word(text, conf, start, end) {
  return Buffer.concat([str(1, text), flt(2, conf), dbl(3, start), dbl(4, end)]);
}

function displayRow(id, speaker, name, start, end, text, tokens) {
  return Buffer.concat([
    str(1, id), str(2, speaker), str(4, name), dbl(5, start), dbl(6, end), str(8, text),
    ...tokens.map((t) => msg(14, t)),
  ]);
}

function buildStateResponse(sessionId) {
  const tokens = [];
  const rows = [];
  // Two speakers, several rows, enough words to make the lane/slot logic do
  // real work rather than trivially pass.
  for (let r = 0; r < 12; r += 1) {
    const start = r * 2.0;
    const words = [];
    for (let i = 0; i < 6; i += 1) {
      const ws = start + i * 0.3;
      words.push(word(`từ${r}_${i}`, 0.5 + (i % 5) * 0.1, ws, ws + 0.28));
    }
    rows.push(displayRow(`row-${r}`, String(r % 2), r % 2 ? 'Nguyễn Văn A' : '',
                         start, start + 1.8, `câu số ${r} có dấu tiếng Việt`, words));
  }
  const amp = [];
  for (let i = 0; i < 1200; i += 1) amp.push(Math.abs(Math.sin(i / 20)) * 0.8);

  const state = Buffer.concat([
    str(1, 'Mock meeting'),
    uint(2, 75),
    ...rows.map((row) => msg(3, row)),
    str(5, '0'), str(5, '1'),
    msg(6, Buffer.concat([dbl(1, 4.2), str(2, '1'), str(3, 'đoạn kém tin cậy'), uint(4, 61)])),
    uint(7, 12), uint(8, 1),
    packedFloat(9, amp),
    dbl(10, 0.02), dbl(11, 24.0), dbl(12, 24.0), dbl(13, 21.5), dbl(14, 25.4),
    bool(16, false),
    msg(21, msg(2, Buffer.concat([dbl(1, 41), dbl(13, 120), dbl(14, 180)]))),
  ]);
  return Buffer.concat([
    str(1, sessionId), uint(2, 7), uint(3, 99), msg(4, state), uint(5, 42), bool(6, false),
    dbl(7, 18.5),
  ]);
}

function buildModelStatus() {
  const entry = (name, version, state) =>
    msg(1, Buffer.concat([str(1, name), str(2, version), str(3, state)]));
  return Buffer.concat([
    entry('asr_vi', '1', 'READY'),
    entry('asr_diar_session', '1', 'READY'),
    entry('itn_phobert', '1', 'UNAVAILABLE'),
  ]);
}

function buildListSessions() {
  const summary = (id, title, duration, isFinal, running) =>
    msg(1, Buffer.concat([
      str(1, id), str(2, title), dbl(3, 1755800000), dbl(4, 1755801000), dbl(5, duration),
      bool(6, isFinal), bool(7, running), str(8, 'Trần Thị B'), str(9, 'mat'),
      str(10, 'record_and_s2t'),
    ]));
  return Buffer.concat([
    summary('sess-1', 'Họp điều hành', 3600, true, false),
    summary('sess-2', 'Ca trực 01', 120.5, false, true),
    str(2, ''),
  ]);
}

function buildRegistryStatus() {
  return Buffer.concat([
    dbl(1, 1755800000), str(2, '1755800000.0'), uint(3, 3), bool(4, true),
    str(9, 'p1'), str(9, 'p2'), str(9, 'Nguyễn Văn A'),
    msg(10, Buffer.concat([str(1, 'p2'), str(2, 'p2'), uint(3, 1), dbl(4, 3.2),
                           str(5, 'ghi ngắn'), str(6, 'urgent')])),
  ]);
}

// About 2 MB of PCM: comfortably past HTTP/2's default 64 KiB window, so the
// client must actually send WINDOW_UPDATE frames to receive all of it.
function buildAudioRange() {
  const pcm = Buffer.alloc(2 * 1024 * 1024);
  for (let i = 0; i < pcm.length; i += 2) pcm.writeInt16LE(((i / 2) % 3000) - 1500, i);
  return Buffer.concat([
    str(1, 'sess-1'), bytes(2, pcm), uint(3, 16000), uint(4, 1), str(5, 's16le'),
    dbl(6, 0), dbl(7, 65.5), dbl(8, 3600),
  ]);
}

function buildPushAudioResponse(request) {
  const fields = readFields(request);
  const seq = fields[8] ? Number(fields[8]) : 0;
  const pcmLength = fields[2] ? fields[2].length : 0;
  return Buffer.concat([
    str(1, 'sess-live'), uint(2, 7), uint(3, 100 + seq),
    msg(4, bool(1, true)),
    dbl(5, seq * 0.16), dbl(6, seq * 0.15),
    str(7, `gói ${seq}, ${pcmLength} byte`),
    msg(24, Buffer.concat([dbl(1, 1.5), dbl(2, 12.0), dbl(3, 30.0)])),
  ]);
}

// ------------------------------------------------------------------ server --

function frame(body) {
  const header = Buffer.alloc(5);
  header.writeUInt8(0, 0);
  header.writeUInt32BE(body.length, 1);
  return Buffer.concat([header, body]);
}

function reply(stream, body) {
  stream.respond({
    ':status': 200,
    'content-type': 'application/grpc+proto',
    'grpc-accept-encoding': 'identity',
  }, { waitForTrailers: true });
  stream.on('wantTrailers', () => stream.sendTrailers({ 'grpc-status': '0' }));
  stream.end(frame(body));
}

function fail(stream, code, message) {
  // Trailers-only response, which is the shape a real gRPC server uses for an
  // error, and the message is percent-encoded per the gRPC spec.
  stream.respond({
    ':status': 200,
    'content-type': 'application/grpc+proto',
    'grpc-status': String(code),
    'grpc-message': encodeURIComponent(message),
  }, { endStream: true });
}

const server = http2.createServer();

server.on('stream', (stream, headers) => {
  const path = headers[':path'] || '';
  const chunks = [];
  stream.on('data', (chunk) => chunks.push(chunk));
  stream.on('end', () => {
    const authorization = headers.authorization || '';
    if (authorization !== `Bearer ${TOKEN}`) {
      fail(stream, 16, 'thiếu hoặc sai API token');
      return;
    }
    const raw = Buffer.concat(chunks);
    // Strip the 5-byte gRPC length prefix.
    const body = raw.length >= 5 ? raw.subarray(5) : Buffer.alloc(0);

    switch (path) {
      case '/asr.ui.v1.ProductASRService/get_model_status':
        reply(stream, buildModelStatus());
        return;
      case '/asr.ui.v1.ProductASRService/list_sessions':
        reply(stream, buildListSessions());
        return;
      case '/asr.ui.v1.SpeakerRegistryService/GetSpeakerRegistryStatus':
        reply(stream, buildRegistryStatus());
        return;
      case '/asr.ui.v1.ProductASRService/get_live_state': {
        const fields = readFields(body);
        const sessionId = fields[1] ? fields[1].toString('utf8') : '';
        if (sessionId === 'missing') {
          fail(stream, 5, 'không tìm thấy phiên: đã bị dọn dẹp');
          return;
        }
        reply(stream, buildStateResponse(sessionId || 'sess-live'));
        return;
      }
      case '/asr.ui.v1.ProductASRService/get_audio_range':
        reply(stream, buildAudioRange());
        return;
      case '/asr.ui.v1.ProductASRService/push_audio':
        reply(stream, buildPushAudioResponse(body));
        return;
      case '/asr.ui.v1.ProductASRService/start_session':
        reply(stream, Buffer.concat([str(1, 'sess-live'), uint(2, 7), uint(3, 1)]));
        return;
      default:
        fail(stream, 12, `chưa cài đặt: ${path}`);
    }
  });
  stream.on('error', () => {});
});

server.on('error', (error) => {
  process.stderr.write(`mock adapter error: ${error}\n`);
  process.exit(1);
});

server.listen(PORT, '127.0.0.1', () => {
  process.stdout.write(`mock adapter listening on 127.0.0.1:${PORT}\n`);
});
