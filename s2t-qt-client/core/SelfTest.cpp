#include "SelfTest.h"

#include "TranscriptModel.h"
#include "grpc/AsrClient.h"
#include "grpc/Hpack.h"

#include <QElapsedTimer>
#include <QTextStream>

#include <memory>

namespace selftest {
namespace {

// Set while a GUI run is in progress; every out() below then lands in the
// caller's QString instead of on a stdout the GUI does not have.
std::unique_ptr<QTextStream> g_capture;

QTextStream &out()
{
    if (g_capture)
        return *g_capture;
    static QTextStream stream(stdout);
    return stream;
}

int g_failures = 0;

void check(bool condition, const QString &what)
{
    out() << (condition ? "  ok    " : "  FAIL  ") << what << Qt::endl;
    if (!condition)
        ++g_failures;
}

void testProtoRoundTrip()
{
    out() << "proto3 wire format" << Qt::endl;

    asr::PushAudioRequest push;
    push.sessionId = QStringLiteral("phiên-ăn-Ăn_01");
    push.pcm = QByteArray(15360, '\x7f');
    push.sampleRate = 48000;
    push.channels = 1;
    push.audioFormat = QStringLiteral("s16le");
    push.reset = true;
    push.vadChunkMs = 160;
    push.seq = 300000;

    // Nothing decodes a request in this client, so parse it through the same
    // reader the responses use: a field-number or wire-type mistake in the
    // writer shows up here rather than as a silent INVALID_ARGUMENT.
    const QByteArray encoded = push.serialize();
    pw::Reader reader(encoded);
    QString sessionId;
    QByteArray pcm;
    quint32 sampleRate = 0;
    quint64 seq = 0;
    bool reset = false;
    int field = 0;
    pw::WireType type = pw::VarintType;
    while (reader.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = reader.readString(); break;
        case 2: pcm = reader.readBytes(); break;
        case 3: sampleRate = reader.readUInt32(); break;
        case 6: reset = reader.readBool(); break;
        case 8: seq = reader.readUInt64(); break;
        default: reader.skip(type); break;
        }
    }
    check(reader.ok(), QStringLiteral("PushAudioRequest parses back cleanly"));
    check(sessionId == push.sessionId, QStringLiteral("utf-8 session id survives"));
    check(pcm == push.pcm, QStringLiteral("15360-byte pcm survives"));
    check(sampleRate == 48000, QStringLiteral("varint sample rate survives"));
    check(reset, QStringLiteral("bool reset survives"));
    check(seq == 300000, QStringLiteral("multi-byte varint seq survives"));

    // Doubles and floats are the fields the timeline positions words with, so
    // an endianness slip here would misplace every word on the axis.
    pw::Writer writer;
    writer.putDouble(3, 1234.5678);
    writer.putFloat(2, 0.8125f);
    pw::Reader numbers(writer.data());
    double readDouble = 0.0;
    float readFloat = 0.0f;
    while (numbers.nextField(&field, &type)) {
        if (field == 3)
            readDouble = numbers.readDouble();
        else if (field == 2)
            readFloat = numbers.readFloat();
        else
            numbers.skip(type);
    }
    check(qFuzzyCompare(readDouble, 1234.5678), QStringLiteral("double round trip"));
    check(qFuzzyCompare(readFloat, 0.8125f), QStringLiteral("float round trip"));

    // An unknown field from a newer server must be skipped, not treated as
    // corruption - that is what keeps this client working across upgrades.
    pw::Writer forward;
    forward.putString(1, QStringLiteral("abc"));
    forward.putSubMessage(999, QByteArray("\x08\x01", 2));
    forward.putUInt64(5, 42);
    asr::StateResponse state;
    pw::Reader forwardReader(forward.data());
    state.parse(forwardReader);
    check(forwardReader.ok() && state.sessionId == QStringLiteral("abc")
              && state.transcriptRevision == 42,
          QStringLiteral("unknown high field number is skipped"));

    // A truncated buffer must fail loudly rather than yielding half a message.
    QByteArray truncated = push.serialize();
    truncated.chop(40);
    pw::Reader truncatedReader(truncated);
    asr::PushAudioResponse response;
    response.parse(truncatedReader);
    check(!truncatedReader.ok(), QStringLiteral("truncated payload is rejected"));
}

void testHpack()
{
    out() << "HPACK" << Qt::endl;

    // RFC 7541 C.4.1: "www.example.com" Huffman encoded.
    const QByteArray huffman = QByteArray::fromHex("f1e3c2e5f23a6ba0ab90f4ff");
    bool ok = false;
    check(hpack::huffmanDecode(huffman, &ok) == QByteArray("www.example.com") && ok,
          QStringLiteral("huffman decodes the RFC 7541 C.4.1 vector"));

    // RFC 7541 C.6.1: a full response header set with Huffman literals and
    // incremental indexing - exercises the dynamic table too.
    const QByteArray block = QByteArray::fromHex(
        "488264025885aec3771a4b6196d07abe941054d444a8200595040b8166e082a62d1bff6e919d29ad171863c78f0b"
        "97c8e9ae82ae43d3");
    hpack::Decoder decoder;
    QList<hpack::Header> headers;
    QString error;
    const bool decoded = decoder.decode(block, &headers, &error);
    check(decoded, QStringLiteral("C.6.1 response header block decodes (%1)").arg(error));
    if (decoded) {
        check(headers.size() == 4, QStringLiteral("C.6.1 yields 4 headers"));
        check(!headers.isEmpty() && headers.at(0).name == QByteArray(":status")
                  && headers.at(0).value == QByteArray("302"),
              QStringLiteral("C.6.1 :status is 302"));
        check(headers.size() > 3 && headers.at(3).name == QByteArray("location")
                  && headers.at(3).value == QByteArray("https://www.example.com"),
              QStringLiteral("C.6.1 location decodes through the huffman path"));
    }

    // Our own encoder must survive our own decoder, including the
    // never-indexed representation reserved for the bearer token.
    QList<hpack::Header> request;
    request.append({QByteArrayLiteral(":method"), QByteArrayLiteral("POST")});
    request.append({QByteArrayLiteral(":path"),
                    QByteArrayLiteral("/asr.ui.v1.ProductASRService/push_audio")});
    request.append({QByteArrayLiteral("authorization"), QByteArrayLiteral("Bearer secret-token")});
    request.append({QByteArrayLiteral("x-custom"), QByteArrayLiteral("giá trị tiếng Việt")});
    hpack::Decoder roundTrip;
    QList<hpack::Header> back;
    const bool encodedOk =
        roundTrip.decode(hpack::Encoder::encode(request), &back, &error);
    check(encodedOk && back.size() == request.size(),
          QStringLiteral("encoder output decodes back (%1)").arg(error));
    bool identical = encodedOk && back.size() == request.size();
    for (int i = 0; identical && i < back.size(); ++i) {
        identical = back.at(i).name == request.at(i).name
            && back.at(i).value == request.at(i).value;
    }
    check(identical, QStringLiteral("every header survives the round trip"));

    // A dynamic-table index the peer never defined must be an error, not a
    // silently empty header - HPACK state cannot be resynchronised after one.
    hpack::Decoder fresh;
    QList<hpack::Header> bogus;
    check(!fresh.decode(QByteArray::fromHex("be"), &bogus, &error),
          QStringLiteral("out-of-range dynamic index is rejected"));
}

} // namespace

void captureReportInto(QString *sink)
{
    if (sink)
        g_capture = std::make_unique<QTextStream>(sink);
    else
        g_capture.reset();
}

int runCodecTests()
{
    g_failures = 0;
    testProtoRoundTrip();
    testHpack();
    out() << (g_failures == 0 ? "ALL PASS" : QStringLiteral("%1 FAILURE(S)").arg(g_failures))
          << Qt::endl;
    out().flush();
    return g_failures == 0 ? 0 : 1;
}

int runNetworkTests(const QString &target, const QString &token)
{
    g_failures = 0;
    out() << "gRPC over HTTP/2 against " << target << Qt::endl;
    AsrClient client(target, token);

    asr::ModelStatusResponse models;
    grpc::Status status = client.getModelStatus(&models, 10000);
    check(status.ok(), QStringLiteral("get_model_status: %1").arg(status.toString()));
    check(models.models.size() == 3, QStringLiteral("three models returned"));
    check(models.models.size() > 2 && models.models.at(2).state == QStringLiteral("UNAVAILABLE"),
          QStringLiteral("a non-READY model is reported as such, not hidden"));

    asr::ListSessionsRequest listRequest;
    listRequest.limit = 5;
    asr::ListSessionsResponse sessions;
    status = client.listSessions(listRequest, &sessions, 10000);
    check(status.ok() && sessions.sessions.size() == 2, QStringLiteral("list_sessions"));
    check(sessions.sessions.size() > 0
              && sessions.sessions.at(0).title == QString::fromUtf8("Họp điều hành"),
          QStringLiteral("utf-8 title survives the wire"));
    check(sessions.sessions.size() > 1 && sessions.sessions.at(1).running,
          QStringLiteral("running flag survives"));
    check(sessions.sessions.size() > 0 && sessions.sessions.at(0).securityLevel == QStringLiteral("mat"),
          QStringLiteral("security_level passthrough survives"));

    // Reuses the same connection as the calls above: a second stream on one
    // channel is where an HPACK dynamic-table mistake would first show up.
    asr::SessionRequest liveRequest;
    liveRequest.sessionId = QStringLiteral("sess-live");
    asr::StateResponse live;
    status = client.getLiveState(liveRequest, &live, 10000);
    check(status.ok(), QStringLiteral("get_live_state: %1").arg(status.toString()));
    check(live.state.rows.size() == 12, QStringLiteral("12 rows parsed"));
    check(live.transcriptRevision == 42, QStringLiteral("transcript_revision parsed"));
    check(qFuzzyCompare(live.commitBoundarySec, 18.5),
          QStringLiteral("commit_boundary_sec parsed"));
    check(live.state.ampTrace.size() == 1200,
          QStringLiteral("packed float amp_trace parsed (%1 values)").arg(live.state.ampTrace.size()));
    check(!live.state.rows.isEmpty() && live.state.rows.at(0).displayTokens.size() == 6,
          QStringLiteral("nested repeated Word messages parsed"));
    check(!live.state.rows.isEmpty()
              && live.state.rows.at(1).verifiedName == QString::fromUtf8("Nguyễn Văn A"),
          QStringLiteral("verified_name with diacritics parsed"));
    check(live.state.highlights.size() == 1 && live.state.highlights.at(0).confPct == 61,
          QStringLiteral("highlights parsed"));
    check(qFuzzyCompare(live.state.latency.server.sumP50, 120.0),
          QStringLiteral("nested LatencySummary parsed"));

    // The lane/slot layout is the part with real logic in it, so run the
    // parsed state through it rather than only checking the wire decode.
    TranscriptModel model;
    model.resetForSession(live.sessionId);
    model.applyLiveState(live);
    check(model.lanes().size() == 2, QStringLiteral("two speaker lanes built"));
    int laneWords = 0;
    for (const Lane &lane : model.lanes())
        laneWords += lane.words.size();
    check(laneWords > 0, QStringLiteral("lanes carry words after the drop threshold"));
    check(model.latestTextEndSec() > 20.0, QStringLiteral("latest text end computed"));

    // ~2 MB, well past HTTP/2's 64 KiB default window: this only completes if
    // the client really is sending WINDOW_UPDATE frames as it consumes DATA.
    asr::AudioRangeRequest audioRequest;
    audioRequest.sessionId = QStringLiteral("sess-1");
    audioRequest.startSec = 0;
    audioRequest.endSec = 65.5;
    asr::AudioRangeResponse audio;
    QElapsedTimer clock;
    clock.start();
    status = client.getAudioRange(audioRequest, &audio, 30000);
    check(status.ok(), QStringLiteral("get_audio_range: %1").arg(status.toString()));
    check(audio.pcm.size() == 2 * 1024 * 1024,
          QStringLiteral("2 MiB payload received intact (%1 bytes, %2 ms)")
              .arg(audio.pcm.size())
              .arg(clock.elapsed()));
    check(audio.sampleRate == 16000, QStringLiteral("audio sample_rate parsed"));

    // Idempotency field and a body large enough to span several DATA frames.
    asr::PushAudioRequest push;
    push.sessionId = QStringLiteral("sess-live");
    push.pcm = QByteArray(15360, '\x20');
    push.sampleRate = 48000;
    push.channels = 1;
    push.audioFormat = QStringLiteral("s16le");
    push.reset = true;
    push.vadChunkMs = 160;
    push.seq = 5;
    asr::PushAudioResponse pushed;
    status = client.pushAudio(push, &pushed, 10000);
    check(status.ok(), QStringLiteral("push_audio: %1").arg(status.toString()));
    check(pushed.streamingText == QStringLiteral("gói 5, 15360 byte"),
          QStringLiteral("server saw the whole 15360-byte packet and its seq"));
    check(pushed.events.streaming, QStringLiteral("event flags parsed"));
    check(qFuzzyCompare(pushed.timing.asrMs, 30.0), QStringLiteral("Timing submessage parsed"));

    // Trailers-only error response with a percent-encoded Vietnamese message.
    asr::SessionRequest missing;
    missing.sessionId = QStringLiteral("missing");
    asr::StateResponse ignored;
    status = client.getLiveState(missing, &ignored, 10000);
    check(status.code == grpc::NotFound, QStringLiteral("NOT_FOUND surfaces as NOT_FOUND"));
    check(status.message == QString::fromUtf8("không tìm thấy phiên: đã bị dọn dẹp"),
          QStringLiteral("grpc-message percent-decodes to utf-8 (%1)").arg(status.message));

    reg::GetSpeakerRegistryStatusResponse registry;
    status = client.getSpeakerRegistryStatus(reg::GetSpeakerRegistryStatusRequest(), &registry,
                                             10000);
    check(status.ok() && registry.globalSpeakerNames.size() == 3,
          QStringLiteral("SpeakerRegistryService reachable on the same channel"));
    check(registry.speakersBelowPolicy.size() == 1
              && registry.speakersBelowPolicy.at(0).kind == QStringLiteral("urgent"),
          QStringLiteral("below-policy speaker and its kind survive"));

    // A rejected token must land as UNAUTHENTICATED with a readable reason,
    // not as a generic transport failure - that distinction is what tells an
    // operator to fix the token rather than the network.
    AsrClient unauthorized(target, QStringLiteral("wrong-token"));
    asr::ModelStatusResponse rejected;
    status = unauthorized.getModelStatus(&rejected, 10000);
    check(status.code == grpc::Unauthenticated,
          QStringLiteral("bad token gives UNAUTHENTICATED (%1)").arg(status.codeName()));
    check(status.message.contains(QString::fromUtf8("token")),
          QStringLiteral("auth failure carries a readable message"));

    out() << (g_failures == 0 ? "ALL PASS" : QStringLiteral("%1 FAILURE(S)").arg(g_failures))
          << Qt::endl;
    out().flush();
    return g_failures == 0 ? 0 : 1;
}

int runProbe(const QString &target, const QString &token)
{
    out() << "probing " << target << Qt::endl;
    AsrClient client(target, token);

    QElapsedTimer clock;
    clock.start();
    asr::ModelStatusResponse models;
    grpc::Status status = client.getModelStatus(&models, 10000);
    out() << "  get_model_status: " << status.toString() << "  ("
          << QString::number(clock.elapsed()) << " ms)" << Qt::endl;
    if (!status.ok()) {
        out().flush();
        return 1;
    }
    for (const asr::ModelStatusEntry &entry : models.models)
        out() << "    " << entry.name << " v" << entry.version << " " << entry.state << Qt::endl;

    asr::ListSessionsRequest listRequest;
    listRequest.limit = 5;
    asr::ListSessionsResponse sessions;
    status = client.listSessions(listRequest, &sessions, 15000);
    out() << "  list_sessions: " << status.toString() << Qt::endl;
    for (const asr::SessionSummary &summary : sessions.sessions) {
        out() << "    " << summary.sessionId << "  " << summary.title << "  "
              << QString::number(summary.durationSec, 'f', 1) << "s  "
              << (summary.final ? "final" : "open") << Qt::endl;
    }

    reg::GetSpeakerRegistryStatusRequest registryRequest;
    reg::GetSpeakerRegistryStatusResponse registry;
    status = client.getSpeakerRegistryStatus(registryRequest, &registry, 15000);
    out() << "  GetSpeakerRegistryStatus: " << status.toString() << Qt::endl;
    if (status.ok()) {
        out() << "    " << QString::number(registry.globalSpeakerCount) << " speaker, sidecar "
              << (registry.sidecarReachable ? "reachable" : "UNREACHABLE") << Qt::endl;
        out() << "    roster: " << registry.globalSpeakerNames.join(QStringLiteral(", "))
              << Qt::endl;
    }
    out().flush();
    return status.ok() ? 0 : 1;
}

} // namespace selftest
