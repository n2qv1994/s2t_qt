#include "ServerSelfTest.h"

#include "BufferHub.h"
#include "BufferService.h"
#include "ServerConfig.h"
#include "core/Logger.h"
#include "grpc/AsrClient.h"
#include "grpc/GrpcServer.h"
#include "grpc/Methods.h"
#include "proto/TritonInfer.h"
#include "proto/BufferAdmin.h"

#include <QDir>
#include <QElapsedTimer>
#include <QObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

namespace serverselftest {
namespace {

int g_failures = 0;

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

void check(bool condition, const QString &what)
{
    if (condition) {
        out() << "  ok   " << what << "\n";
    } else {
        out() << "  FAIL " << what << "\n";
        ++g_failures;
        LOG_ERROR(applog::cat::App) << "selftest failed:" << what;
    }
    out().flush();
}

bool nearly(double a, double b)
{
    return qAbs(a - b) < 1e-9;
}

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

void testRequestRoundTrip()
{
    out() << "proto3, request direction (client writes, server reads)\n";

    asr::PushAudioRequest push;
    push.sessionId = QStringLiteral("phiên-01");
    // Deliberately not text: pcm is the one field where a bytes/string mix-up
    // would survive every ASCII test and corrupt real audio.
    push.pcm = QByteArray("\x00\x01\xff\xfe\x80\x7f", 6);
    push.sampleRate = 48000;
    push.channels = 1;
    push.audioFormat = QStringLiteral("s16le");
    push.reset = true;
    push.vadChunkMs = 320;
    push.seq = 4294967297ull; // past 32 bits, where a varint width bug shows up

    asr::PushAudioRequest decoded;
    const QByteArray encoded = push.serialize();
    pw::Reader reader(encoded);
    decoded.parse(reader);
    check(reader.ok(), "push_audio request decodes");
    check(decoded.sessionId == push.sessionId, "push_audio session_id survives (Vietnamese)");
    check(decoded.pcm == push.pcm, "push_audio pcm survives byte for byte");
    check(decoded.sampleRate == push.sampleRate && decoded.channels == push.channels,
          "push_audio rate/channels survive");
    check(decoded.reset == push.reset && decoded.vadChunkMs == push.vadChunkMs,
          "push_audio reset/vad_chunk_ms survive");
    check(decoded.seq == push.seq, "push_audio seq survives past 2^32");

    asr::TextEditRequest edit;
    edit.sessionId = QStringLiteral("s2");
    edit.baseRevision = 12;
    edit.startSec = 1.5;
    edit.endSec = 2.25;
    asr::Word word;
    word.w = QStringLiteral("một");
    word.c = 0.5f;
    word.startSec = 1.5;
    word.endSec = 1.75;
    word.speaker = QStringLiteral("speaker_1");
    edit.replacementWords.append(word);
    edit.editorId = QStringLiteral("nguyen");
    edit.note = QStringLiteral("sửa lỗi");

    asr::TextEditRequest editBack;
    const QByteArray editBytes = edit.serialize();
    pw::Reader editReader(editBytes);
    editBack.parse(editReader);
    check(editReader.ok(), "apply_text_edit request decodes");
    check(editBack.replacementWords.size() == 1
              && editBack.replacementWords.at(0).w == word.w
              && nearly(editBack.replacementWords.at(0).endSec, word.endSec),
          "apply_text_edit repeated submessage survives");
    check(editBack.editorId == edit.editorId && editBack.note == edit.note,
          "apply_text_edit editor/note survive");

    reg::EnrollSpeakerRequest enroll;
    enroll.displayName = QStringLiteral("Trần Văn A");
    enroll.wav = QByteArray(1024, '\x42');
    enroll.allowBelowPolicy = true;
    reg::EnrollSpeakerRequest enrollBack;
    const QByteArray enrollBytes = enroll.serialize();
    pw::Reader enrollReader(enrollBytes);
    enrollBack.parse(enrollReader);
    check(enrollReader.ok() && enrollBack.displayName == enroll.displayName
              && enrollBack.wav == enroll.wav && enrollBack.allowBelowPolicy,
          "EnrollSpeaker request survives");
}

void testResponseRoundTrip()
{
    out() << "proto3, response direction (server writes, client reads)\n";

    asr::StateResponse state;
    state.sessionId = QStringLiteral("phiên-02");
    state.streamId = 7;
    state.stateVersion = 99;
    state.transcriptRevision = 5;
    state.transcriptFinal = true;
    state.commitBoundarySec = 12.5;
    state.state.title = QStringLiteral("Cuộc họp thử");
    state.state.confThresholdPct = 75;
    state.state.speakerIds << QStringLiteral("speaker_1") << QStringLiteral("speaker_2");
    state.state.ampTrace << 0.1f << 0.2f << 0.3f;
    state.state.ampTraceStepSec = 0.32;
    state.state.done = true;
    state.state.latency.server.asrP50 = 42.0;

    asr::DisplayRow row;
    row.rowId = QStringLiteral("r1");
    row.speaker = QStringLiteral("speaker_1");
    row.speakerProb = 0.9f;
    row.verifiedName = QStringLiteral("Trần Văn A");
    row.startSec = 0.0;
    row.endSec = 2.0;
    row.mergedText = QStringLiteral("xin chào");
    row.stableTokenCount = 2;
    asr::Word w;
    w.w = QStringLiteral("chào");
    w.c = 0.81f;
    row.displayTokens.append(w);
    state.state.rows.append(row);

    asr::StateResponse back;
    const QByteArray bytes = state.serialize();
    pw::Reader reader(bytes);
    back.parse(reader);
    check(reader.ok(), "get_live_state response decodes");
    check(back.sessionId == state.sessionId && back.streamId == state.streamId
              && back.stateVersion == state.stateVersion,
          "state envelope survives");
    check(back.transcriptFinal && back.transcriptRevision == 5
              && nearly(back.commitBoundarySec, 12.5),
          "transcript revision/final/boundary survive");
    check(back.state.speakerIds == state.state.speakerIds, "repeated string survives");
    check(back.state.ampTrace == state.state.ampTrace, "packed float survives");
    check(back.state.rows.size() == 1 && back.state.rows.at(0).mergedText == row.mergedText
              && back.state.rows.at(0).verifiedName == row.verifiedName,
          "display row survives, including a Vietnamese verified name");
    check(back.state.rows.at(0).displayTokens.size() == 1
              && qAbs(back.state.rows.at(0).displayTokens.at(0).c - w.c) < 1e-6f,
          "nested repeated tokens survive");
    check(nearly(back.state.latency.server.asrP50, 42.0), "nested latency submessage survives");

    // Field 7 and 9 on DisplayRow are reserved.  Writing them would be a
    // contract violation that only shows up against a real adapter, so check
    // the encoder never does.
    const QByteArray rowBytes = row.serialize();
    pw::Reader rowReader(rowBytes);
    int field = 0;
    pw::WireType type = pw::VarintType;
    bool sawReserved = false;
    while (rowReader.nextField(&field, &type)) {
        if (field == 7 || field == 9)
            sawReserved = true;
        rowReader.skip(type);
    }
    check(!sawReserved, "DisplayRow never writes the reserved fields 7 and 9");

    buf::BufferedSession session;
    session.sessionId = QStringLiteral("s3");
    session.client = QStringLiteral("192.168.1.9:51000");
    session.acceptedPackets = 1000;
    session.forwardedPackets = 998;
    session.pendingBytes = 30720;
    session.lagSec = 0.32;
    session.lastError = QStringLiteral("mất kết nối");
    buf::BufferedSession sessionBack;
    const QByteArray sessionBytes = session.serialize();
    pw::Reader sessionReader(sessionBytes);
    sessionBack.parse(sessionReader);
    check(sessionReader.ok() && sessionBack.sessionId == session.sessionId
              && sessionBack.acceptedPackets == session.acceptedPackets
              && nearly(sessionBack.lagSec, session.lagSec)
              && sessionBack.lastError == session.lastError,
          "BufferedSession survives");
}

void testFraming()
{
    out() << "gRPC message framing and status encoding\n";

    const QByteArray message("\x01\x02\x03", 3);
    const QByteArray framed = grpc::Server::frameMessage(message);
    check(framed.size() == message.size() + 5, "framed message is 5 bytes longer");
    check(quint8(framed.at(0)) == 0, "compression flag is zero");
    QByteArray back;
    QString error;
    check(grpc::Server::unframeMessage(framed, &back, &error) && back == message,
          "framed message unframes to the same bytes");

    check(!grpc::Server::unframeMessage(QByteArray(), &back, &error),
          "an empty body is rejected, not treated as an empty message");
    check(!grpc::Server::unframeMessage(QByteArray("\x00\x00\x00\x00\x10zzz", 9), &back, &error),
          "a frame shorter than its declared length is rejected");
    check(!grpc::Server::unframeMessage(framed + framed, &back, &error),
          "two messages in one unary request are rejected");

    const QString vietnamese = QStringLiteral("token không hợp lệ");
    const QByteArray encoded = grpc::Server::encodeGrpcMessage(vietnamese);
    check(!encoded.contains('\xc3'), "grpc-message is percent-encoded, not raw UTF-8");
    check(encoded.contains("%C3%B4"), "a Vietnamese codepoint encodes as %XX pairs");
    check(grpc::Server::encodeGrpcMessage(QStringLiteral("50%")) == QByteArray("50%25"),
          "a literal percent sign is escaped");
}

// ---------------------------------------------------------------------------
// Loopback: the real server, driven by the real client
// ---------------------------------------------------------------------------

const char kToken[] = "selftest-token";
// Big enough to force several rounds of HTTP/2 flow control on the way out,
// which is the part of Http2Server that no smaller payload exercises at all.
const int kBigRows = 6000;

void registerTestMethods(grpc::Server *server)
{
    server->registerMethod(
        QString::fromLatin1(rpcpath::PushAudio),
        [](const grpc::ServerCall &call, QByteArray *out) {
            asr::PushAudioRequest request;
            pw::Reader reader(call.message);
            request.parse(reader);
            grpc::Status status;
            if (!reader.ok()) {
                status.code = grpc::InvalidArgument;
                status.message = QStringLiteral("hỏng gói");
                return status;
            }
            asr::PushAudioResponse response;
            response.sessionId = request.sessionId;
            // Echoes back things a wire bug would disturb: the exact byte
            // count, the seq, and the deadline the client advertised.
            response.sourceSeenSec = double(request.pcm.size());
            response.speechSeenSec = double(request.seq);
            response.timing.clientWaitMs = double(call.deadlineMs);
            *out = response.serialize();
            return status;
        });

    server->registerMethod(
        QString::fromLatin1(rpcpath::GetLiveState),
        [](const grpc::ServerCall &call, QByteArray *out) {
            asr::SessionRequest request;
            pw::Reader reader(call.message);
            request.parse(reader);
            asr::StateResponse response;
            response.sessionId = request.sessionId;
            response.state.title = QStringLiteral("Cuộc họp thử");
            for (int i = 0; i < kBigRows; ++i) {
                asr::DisplayRow row;
                row.rowId = QStringLiteral("r%1").arg(i);
                row.speaker = QStringLiteral("speaker_%1").arg(i % 4);
                row.mergedText =
                    QStringLiteral("dòng %1 với vài từ tiếng Việt có dấu để chuỗi đủ dài").arg(i);
                row.startSec = double(i);
                row.endSec = double(i) + 1.0;
                response.state.rows.append(row);
            }
            *out = response.serialize();
            return grpc::Status();
        });

    server->registerMethod(
        QString::fromLatin1(rpcpath::GetModelStatus),
        [](const grpc::ServerCall &call, QByteArray *out) {
            // The empty-request case: the client sends a zero-length message,
            // which has its own framing path on both sides.
            grpc::Status status;
            if (!call.message.isEmpty()) {
                status.code = grpc::InvalidArgument;
                status.message = QStringLiteral("get_model_status không nhận tham số");
                return status;
            }
            asr::ModelStatusResponse response;
            asr::ModelStatusEntry entry;
            entry.name = QStringLiteral("asr");
            entry.version = QStringLiteral("1");
            entry.state = QStringLiteral("READY");
            response.models.append(entry);
            *out = response.serialize();
            return status;
        });

    server->registerMethod(
        QString::fromLatin1(rpcpath::StopSession),
        [](const grpc::ServerCall &, QByteArray *) {
            // The error path, with a message that only survives if
            // percent-encoding works in both directions.
            grpc::Status status;
            status.code = grpc::FailedPrecondition;
            status.message = QStringLiteral("phiên đã dừng rồi — không thể dừng lại lần nữa");
            return status;
        });
}

int loopback()
{
    // QTcpServer needs an event loop to accept, and the checks below block
    // this thread for the length of each call - so the server gets a thread of
    // its own, exactly as it has one in main().
    QThread serverThread;
    serverThread.setObjectName(QStringLiteral("selftest-server"));
    serverThread.start();

    QObject anchor;
    anchor.moveToThread(&serverThread);

    grpc::Server *server = nullptr;
    QString error;
    bool started = false;
    quint16 port = 0;
    QMetaObject::invokeMethod(
        &anchor,
        [&]() {
            server = new grpc::Server();
            server->setToken(QString::fromLatin1(kToken));
            registerTestMethods(server);
            started = server->start(QHostAddress::LocalHost, 0, &error);
            port = server->port();
        },
        Qt::BlockingQueuedConnection);

    check(started, QStringLiteral("loopback server listens (%1)")
                       .arg(started ? QStringLiteral("port %1").arg(port) : error));
    if (!started) {
        QMetaObject::invokeMethod(
            &anchor, [&]() { delete server; }, Qt::BlockingQueuedConnection);
        serverThread.quit();
        serverThread.wait();
        return 1;
    }

    const QString target = QStringLiteral("127.0.0.1:%1").arg(port);
    {
        AsrClient client(target, QString::fromLatin1(kToken));

        double latencyMs = 0.0;
        check(client.ping(3000, &latencyMs).ok(), "a channel can be opened");

        asr::PushAudioRequest push;
        push.sessionId = QStringLiteral("phiên-loopback");
        push.pcm = QByteArray(15360, '\x05'); // one 160 ms packet at 48 kHz mono
        push.sampleRate = 48000;
        push.channels = 1;
        push.audioFormat = QStringLiteral("s16le");
        push.seq = 1;
        asr::PushAudioResponse pushBack;
        grpc::Status status = client.pushAudio(push, &pushBack, 5000);
        check(status.ok(), QStringLiteral("push_audio round trip (%1)").arg(status.toString()));
        check(pushBack.sessionId == push.sessionId, "the session id crossed both directions");
        check(nearly(pushBack.sourceSeenSec, 15360.0), "the pcm arrived whole");
        check(nearly(pushBack.speechSeenSec, 1.0), "seq crossed the wire");
        check(pushBack.timing.clientWaitMs >= 4000.0 && pushBack.timing.clientWaitMs <= 5000.0,
              "grpc-timeout reached the handler as a deadline");

        // The flow-control case.  Several megabytes cannot fit in one DATA
        // frame or in the initial 64 KiB connection window, so this only
        // passes if WINDOW_UPDATE handling works on both sides.
        QElapsedTimer clock;
        clock.start();
        asr::SessionRequest sessionRequest;
        sessionRequest.sessionId = QStringLiteral("phiên-lớn");
        asr::StateResponse state;
        status = client.getLiveState(sessionRequest, &state, 30000);
        check(status.ok(), QStringLiteral("a multi-megabyte reply arrives (%1)")
                               .arg(status.toString()));
        check(state.state.rows.size() == kBigRows,
              QStringLiteral("all %1 rows arrived, not a truncated prefix").arg(kBigRows));
        check(state.state.rows.last().mergedText.contains(QStringLiteral("tiếng Việt")),
              "the last row is intact, so nothing was lost mid-stream");
        out() << "       (" << state.state.rows.size() << " rows in " << clock.elapsed()
              << " ms)\n";

        asr::ModelStatusResponse models;
        status = client.getModelStatus(&models, 5000);
        check(status.ok() && models.models.size() == 1,
              "an empty request message is framed and answered");

        asr::StopSessionResponse stop;
        status = client.stopSession(sessionRequest, &stop, 5000);
        check(status.code == grpc::FailedPrecondition, "a handler error becomes a gRPC status");
        check(status.message.contains(QStringLiteral("không thể dừng lại")),
              QStringLiteral("a Vietnamese error message survives percent encoding (%1)")
                  .arg(status.message));

        // Unknown method.
        asr::ListSessionsRequest list;
        asr::ListSessionsResponse listBack;
        status = client.listSessions(list, &listBack, 5000);
        check(status.code == grpc::Unimplemented, "an unregistered method is UNIMPLEMENTED");
    }

    {
        // Wrong token, on a fresh channel.
        AsrClient client(target, QStringLiteral("wrong-token"));
        asr::ModelStatusResponse models;
        const grpc::Status status = client.getModelStatus(&models, 5000);
        check(status.code == grpc::Unauthenticated, "a bad token is UNAUTHENTICATED");
    }
    {
        // No token at all.
        AsrClient client(target, QString());
        asr::ModelStatusResponse models;
        const grpc::Status status = client.getModelStatus(&models, 5000);
        check(status.code == grpc::Unauthenticated, "a missing token is UNAUTHENTICATED");
    }

    quint64 calls = 0;
    QMetaObject::invokeMethod(
        &anchor,
        [&]() {
            calls = server->totalRequests();
            delete server;
            server = nullptr;
        },
        Qt::BlockingQueuedConnection);
    // Exactly the seven RPCs above, and not one more.  ping() only opens a
    // channel - it makes no call - so a count of eight here would mean
    // something was retried behind our back.
    check(calls == 7, QStringLiteral("the server counted exactly 7 calls (%1)").arg(calls));

    serverThread.quit();
    serverThread.wait(5000);
    return 0;
}

} // namespace


// ---------------------------------------------------------------------------
// The chain: a stand-in inference tier, the real buffer, and a real client
// ---------------------------------------------------------------------------

namespace {

// What the stand-in pipeline saw, in the order it saw it.  The order is the
// point: the drain barrier is only real if every push_audio is recorded before
// stop_session is.
struct UpstreamRecord
{
    QMutex mutex;
    QStringList events;
    QList<quint64> seqs;
    QList<int> sizes;
    int livePolls = 0;
    double sourceSeenSec = 0.0;
    // When set, push_audio answers UNAVAILABLE.  That is a *transport* status,
    // so the buffer holds the packet and keeps retrying it - which is how the
    // restart test gets a backlog to survive a restart with.
    bool refuse = false;
};

// Finds one named input's raw buffer.  Triton pairs raw_input_contents
// positionally with inputs, which is exactly the pairing that would go wrong
// silently, so the stand-in checks it the same way the real server does.
bool rawInput(const trt::ModelInferRequest &request, const char *name, QByteArray *out)
{
    for (int i = 0; i < request.inputs.size(); ++i) {
        if (request.inputs.at(i).name != QLatin1String(name))
            continue;
        if (i >= request.rawInputContents.size())
            return false;
        *out = request.rawInputContents.at(i);
        return true;
    }
    return false;
}

qint32 firstInt32(const QByteArray &raw)
{
    if (raw.size() < 4)
        return 0;
    qint32 value = 0;
    std::memcpy(&value, raw.constData(), 4);
    return value;
}

// The test fills every packet with one repeated byte whose value *is* the seq
// (`QByteArray(kPacketBytes, char(seq))`).  TritonBackend turns that into
// float32, so the seq comes back out by undoing the conversion: two identical
// bytes b make the int16 (b<<8)|b, and the low byte of that is b again.
//
// This is what keeps "every packet arrived exactly once, in order" a real
// assertion.  A stand-in that just counted its own calls would make the same
// check pass no matter what the buffer did with the ordering.
quint64 seqFromAudio(const QByteArray &rawFloats)
{
    if (rawFloats.size() < 4)
        return 0;
    float sample = 0.0f;
    std::memcpy(&sample, rawFloats.constData(), 4);
    const int scaled = int(qRound(double(sample) * 32768.0));
    return quint64(scaled & 0xff);
}

void registerFakeUpstream(grpc::Server *server, UpstreamRecord *record)
{
    // The buffer speaks KServe v2 to the tier now, so the stand-in has to as
    // well.  There is no start/stop RPC any more: a meeting begins with the
    // first infer and ends with one carrying is_final=1.
    server->registerMethod(
        QString::fromLatin1(rpcpath::TritonServerLive),
        [](const grpc::ServerCall &, QByteArray *out) {
            trt::ServerLiveResponse response;
            response.live = true;
            *out = response.serialize();
            return grpc::Status();
        });

    server->registerMethod(
        QString::fromLatin1(rpcpath::TritonRepositoryIndex),
        [](const grpc::ServerCall &, QByteArray *out) {
            trt::RepositoryIndexResponse response;
            trt::ModelIndex model;
            model.name = QStringLiteral("asr_diar_session");
            model.version = QStringLiteral("1");
            model.state = QStringLiteral("READY");
            response.models.append(model);
            *out = response.serialize();
            return grpc::Status();
        });

    server->registerMethod(
        QString::fromLatin1(rpcpath::TritonModelInfer),
        [record](const grpc::ServerCall &call, QByteArray *out) {
            trt::ModelInferRequest request;
            pw::Reader reader(call.message);
            request.parse(reader);
            if (!reader.ok()) {
                grpc::Status status;
                status.code = grpc::InvalidArgument;
                status.message = QStringLiteral("ModelInferRequest hỏng");
                return status;
            }

            QByteArray audio;
            QByteArray isFinal;
            rawInput(request, "audio_chunk", &audio);
            rawInput(request, "is_final", &isFinal);
            const bool final = firstInt32(isFinal) != 0;

            QMutexLocker lock(&record->mutex);
            if (record->refuse && !final) {
                grpc::Status status;
                status.code = grpc::Unavailable;
                status.message = QStringLiteral("tầng suy luận đang từ chối (thử nghiệm)");
                return status;
            }

            if (final) {
                // The far end of the drain barrier.  Recorded as "stop" so the
                // ordering assertion reads the same as it did when there was a
                // stop_session RPC to record.
                record->events << QStringLiteral("stop");
            } else {
                record->events << QStringLiteral("push");
                record->seqs << seqFromAudio(audio);
                // Back to bytes: two bytes of PCM per float32 sample.
                record->sizes << (audio.size() / 2);
                record->sourceSeenSec += double(audio.size() / 4) / 48000.0;
            }

            trt::ModelInferResponse response;
            response.modelName = request.modelName;
            response.id = request.id;
            const auto addOutput = [&response](const char *name, const char *datatype,
                                               const QByteArray &raw) {
                trt::InferOutputTensor tensor;
                tensor.name = QString::fromLatin1(name);
                tensor.datatype = QString::fromLatin1(datatype);
                tensor.shape = {1};
                response.outputs.append(tensor);
                response.rawOutputContents.append(raw);
            };
            // One BYTES element: 4-byte little-endian length then the utf-8.
            const auto bytesTensor = [](const QString &text) {
                const QByteArray utf8 = text.toUtf8();
                QByteArray raw;
                const quint32 length = quint32(utf8.size());
                raw.resize(4);
                std::memcpy(raw.data(), &length, 4);
                raw.append(utf8);
                return raw;
            };
            addOutput("text", trt::dtype::Bytes,
                      bytesTensor(final ? QStringLiteral("kết thúc")
                                        : QStringLiteral("gói %1").arg(record->seqs.size())));
            addOutput("speaker", trt::dtype::Int32, QByteArray(4, '\0'));
            *out = response.serialize();
            return grpc::Status();
        });
}

int chain()
{
    const int kPackets = 10;
    // 160 ms at 48 kHz mono s16 - the client's live packet size.
    const int kPacketBytes = 15360;

    // ---- the stand-in inference tier --------------------------------------
    UpstreamRecord record;
    QThread upstreamThread;
    upstreamThread.setObjectName(QStringLiteral("selftest-upstream"));
    upstreamThread.start();
    QObject upstreamAnchor;
    upstreamAnchor.moveToThread(&upstreamThread);

    grpc::Server *upstream = nullptr;
    quint16 upstreamPort = 0;
    bool upstreamUp = false;
    QString error;
    QMetaObject::invokeMethod(
        &upstreamAnchor,
        [&]() {
            upstream = new grpc::Server();
            registerFakeUpstream(upstream, &record);
            upstreamUp = upstream->start(QHostAddress::LocalHost, 0, &error);
            upstreamPort = upstream->port();
        },
        Qt::BlockingQueuedConnection);
    check(upstreamUp, QStringLiteral("stand-in inference tier listens (%1)")
                          .arg(upstreamUp ? QStringLiteral("port %1").arg(upstreamPort) : error));
    if (!upstreamUp) {
        QMetaObject::invokeMethod(
            &upstreamAnchor, [&]() { delete upstream; }, Qt::BlockingQueuedConnection);
        upstreamThread.quit();
        upstreamThread.wait();
        return 1;
    }

    // ---- the real Server buffer, in front of it ---------------------------
    ServerConfig config;
    config.listenAddress = QStringLiteral("127.0.0.1");
    config.listenPort = 0;
    config.listenToken = QString::fromLatin1(kToken);
    config.upstreamTarget = QStringLiteral("127.0.0.1:%1").arg(upstreamPort);
    config.upstreamLanes = 2;
    config.upstreamTimeoutMs = 10000;
    // Far enough out that the probe never runs during the test and cannot be
    // mistaken for one of the polls counted below.
    config.upstreamProbeMs = 300000;
    config.bufferSeconds = 30.0;
    config.statePollMs = 5000;

    QThread bufferThread;
    bufferThread.setObjectName(QStringLiteral("selftest-buffer"));
    bufferThread.start();
    QObject bufferAnchor;
    bufferAnchor.moveToThread(&bufferThread);

    BufferHub *hub = nullptr;
    grpc::Server *buffer = nullptr;
    BufferService *service = nullptr;
    quint16 bufferPort = 0;
    bool bufferUp = false;
    QMetaObject::invokeMethod(
        &bufferAnchor,
        [&]() {
            // Created on the buffer thread: BufferHub owns a QTimer, and a
            // timer only ticks on the thread that made it.
            hub = new BufferHub(config);
            buffer = new grpc::Server();
            buffer->setToken(config.listenToken);
            service = new BufferService(hub, buffer);
            service->registerMethods();
            bufferUp = buffer->start(QHostAddress::LocalHost, config.listenPort, &error);
            bufferPort = buffer->port();
        },
        Qt::BlockingQueuedConnection);
    check(bufferUp, QStringLiteral("Server buffer listens (%1)")
                        .arg(bufferUp ? QStringLiteral("port %1").arg(bufferPort) : error));

    if (bufferUp) {
        const QString target = QStringLiteral("127.0.0.1:%1").arg(bufferPort);
        AsrClient client(target, QString::fromLatin1(kToken));

        // ---- start ---------------------------------------------------------
        asr::StartSessionRequest startRequest;
        startRequest.configJson = QStringLiteral("{\"title\":\"Cuộc họp e2e\"}");
        asr::StartSessionResponse started;
        grpc::Status status = client.startSession(startRequest, &started, 10000);
        check(status.ok(), QStringLiteral("start_session through the buffer (%1)")
                               .arg(status.toString()));
        // The id is the buffer's own since the adapter went: there is no
        // session registry below it to borrow one from.  What matters is that
        // it is non-empty and that every later RPC accepts it.
        check(!started.sessionId.isEmpty() && started.sessionId.size() >= 8,
              QStringLiteral("the buffer mints its own session id (%1)").arg(started.sessionId));
        check(started.state.title == QStringLiteral("Cuộc họp e2e"),
              "the title from config_json reaches the client");

        // ---- push ----------------------------------------------------------
        double lastSourceSeen = -1.0;
        for (int i = 1; i <= kPackets && status.ok(); ++i) {
            asr::PushAudioRequest push;
            push.sessionId = started.sessionId;
            push.pcm = QByteArray(kPacketBytes, char(i));
            push.sampleRate = 48000;
            push.channels = 1;
            push.audioFormat = QStringLiteral("s16le");
            push.reset = (i == 1);
            push.vadChunkMs = 160;
            push.seq = quint64(i);
            asr::PushAudioResponse ack;
            status = client.pushAudio(push, &ack, 10000);
            lastSourceSeen = ack.sourceSeenSec;
        }
        check(status.ok(), QStringLiteral("%1 packets accepted (%2)")
                               .arg(kPackets)
                               .arg(status.toString()));

        // ---- idempotent replay ---------------------------------------------
        asr::PushAudioRequest replay;
        replay.sessionId = started.sessionId;
        replay.pcm = QByteArray(kPacketBytes, '\x7f');
        replay.sampleRate = 48000;
        replay.channels = 1;
        replay.audioFormat = QStringLiteral("s16le");
        replay.seq = quint64(kPackets); // the seq the client would resend after a timeout
        asr::PushAudioResponse replayAck;
        status = client.pushAudio(replay, &replayAck, 10000);
        check(status.ok(), QStringLiteral("a resent seq is accepted (%1)").arg(status.toString()));
        check(qAbs(replayAck.sourceSeenSec - lastSourceSeen) < 1e-9,
              "a resent seq replays the stored ACK instead of queueing the audio again");

        // ---- state cache ----------------------------------------------------
        int pollsBefore = 0;
        {
            QMutexLocker lock(&record.mutex);
            pollsBefore = record.livePolls;
        }
        asr::SessionRequest sessionRequest;
        sessionRequest.sessionId = started.sessionId;
        asr::StateResponse first;
        asr::StateResponse second;
        asr::StateResponse third;
        check(client.getLiveState(sessionRequest, &first, 10000).ok(), "get_live_state works");
        check(client.getLiveState(sessionRequest, &second, 10000).ok(),
              "get_live_state works twice");
        check(client.getLiveState(sessionRequest, &third, 10000).ok(),
              "get_live_state works three times");
        int pollsAfter = 0;
        {
            QMutexLocker lock(&record.mutex);
            pollsAfter = record.livePolls;
        }
        // The transcript is built here now, so a read costs the tier nothing at
        // all - not "one poll for many watchers", but zero.
        check(pollsAfter == pollsBefore,
              QStringLiteral("client reads cost the inference tier nothing (%1 calls)")
                  .arg(pollsAfter - pollsBefore));
        // Not equality: the forwarder may still be draining the queue while
        // these three reads happen, and every packet it delivers legitimately
        // advances the version.  What a client actually depends on is that the
        // number never goes backwards - that is what makes it usable as "has
        // anything changed since I last looked".
        check(first.stateVersion <= second.stateVersion
                  && second.stateVersion <= third.stateVersion,
              QStringLiteral("the state version never goes backwards (%1 <= %2 <= %3)")
                  .arg(first.stateVersion)
                  .arg(second.stateVersion)
                  .arg(third.stateVersion));

        // ---- the buffer's own admin surface ---------------------------------
        buf::BufferStatusRequest statusRequest;
        buf::BufferStatusResponse bufferStatus;
        status = client.getBufferStatus(statusRequest, &bufferStatus, 10000);
        check(status.ok(), QStringLiteral("get_buffer_status (%1)").arg(status.toString()));
        check(bufferStatus.sessions.size() == 1, "the admin view shows exactly one session");
        if (bufferStatus.sessions.size() == 1) {
            const buf::BufferedSession &session = bufferStatus.sessions.at(0);
            check(session.acceptedPackets == quint64(kPackets),
                  QStringLiteral("accepted packets counts the replay only once (%1)")
                      .arg(session.acceptedPackets));
            check(session.running, "the session reads as running before it is stopped");
            check(session.client.startsWith(QStringLiteral("127.0.0.1")),
                  QStringLiteral("the session records which client opened it (%1)")
                      .arg(session.client));
        }
        check(bufferStatus.upstream.target == config.upstreamTarget,
              "the admin view names the inference tier it is in front of");

        buf::PingRequest pingRequest;
        pingRequest.clientTs = 1234.5;
        buf::PingResponse pingResponse;
        status = client.bufferPing(pingRequest, &pingResponse, 10000);
        check(status.ok() && qAbs(pingResponse.clientTs - 1234.5) < 1e-9,
              "buffer ping echoes the caller's own timestamp");
        check(pingResponse.serverVersion == QStringLiteral(S2T_SERVER_VERSION),
              "buffer ping reports the server version");

        // ---- a relayed method ------------------------------------------------
        asr::ModelStatusResponse models;
        status = client.getModelStatus(&models, 10000);
        check(status.ok() && models.models.size() == 1,
              QStringLiteral("a relayed RPC reaches the pipeline unchanged (%1)")
                  .arg(status.toString()));

        // ---- stop, and the drain barrier ---------------------------------------
        asr::StopSessionResponse stopped;
        status = client.stopSession(sessionRequest, &stopped, 30000);
        check(status.ok(), QStringLiteral("stop_session (%1)").arg(status.toString()));
        check(stopped.state.done, "the pipeline's final state comes back through the buffer");

        {
            QMutexLocker lock(&record.mutex);
            check(record.seqs.size() == kPackets,
                  QStringLiteral("the pipeline received exactly %1 packets, no duplicates (%2)")
                      .arg(kPackets)
                      .arg(record.seqs.size()));
            bool ordered = true;
            for (int i = 0; i < record.seqs.size(); ++i) {
                if (record.seqs.at(i) != quint64(i + 1))
                    ordered = false;
            }
            check(ordered, "the pipeline received them in order");
            bool wholePackets = true;
            for (int size : record.sizes) {
                if (size != kPacketBytes)
                    wholePackets = false;
            }
            check(wholePackets, "every packet arrived whole");
            // The barrier itself: stop is last, and everything before it is a
            // push.  If stop_session could overtake the queue this fails.
            check(!record.events.isEmpty() && record.events.last() == QStringLiteral("stop"),
                  "stop_session reached the pipeline last");
            check(record.events.count(QStringLiteral("push")) == kPackets,
                  "no push_audio arrived after stop_session");
        }

        // ---- after the end ------------------------------------------------------
        asr::PushAudioRequest late;
        late.sessionId = started.sessionId;
        late.pcm = QByteArray(kPacketBytes, '\x01');
        late.seq = quint64(kPackets + 1);
        asr::PushAudioResponse lateAck;
        status = client.pushAudio(late, &lateAck, 10000);
        check(status.code == grpc::FailedPrecondition,
              QStringLiteral("audio sent after stop is refused (%1)").arg(status.codeName()));

        asr::SessionRequest unknown;
        unknown.sessionId = QStringLiteral("không-có-phiên-này");
        asr::StateResponse ignored;
        status = client.getLiveState(unknown, &ignored, 10000);
        check(status.code == grpc::NotFound,
              QStringLiteral("an unknown session is NOT_FOUND (%1)").arg(status.codeName()));
        check(status.message.contains(QStringLiteral("khởi động lại")),
              "the NOT_FOUND message explains that a restart loses buffered sessions");
    }

    QMetaObject::invokeMethod(
        &bufferAnchor,
        [&]() {
            if (hub)
                hub->shutdown();
            if (buffer)
                buffer->stop();
            delete service;
            delete buffer;
            delete hub;
            service = nullptr;
            buffer = nullptr;
            hub = nullptr;
        },
        Qt::BlockingQueuedConnection);
    bufferThread.quit();
    bufferThread.wait(15000);

    QMetaObject::invokeMethod(
        &upstreamAnchor,
        [&]() {
            delete upstream;
            upstream = nullptr;
        },
        Qt::BlockingQueuedConnection);
    upstreamThread.quit();
    upstreamThread.wait(15000);
    return 0;
}

} // namespace

int runBufferTests()
{
    g_failures = 0;
    out() << "== s2t-qt-server: client -> buffer -> pipeline ==\n";
    chain();
    out() << (g_failures == 0 ? "chain: OK\n"
                              : QStringLiteral("chain: %1 lỗi\n").arg(g_failures));
    out().flush();
    return g_failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Restart: does a meeting survive the Server buffer going down under it?
// ---------------------------------------------------------------------------

namespace {

// One Server buffer, on its own thread, over a given journal directory.  Built
// twice by the test below - once before the "restart" and once after - so the
// second instance sees only what the first left on disk.
class BufferInstance
{
public:
    BufferInstance(const ServerConfig &config)
    {
        m_thread.setObjectName(QStringLiteral("selftest-buffer"));
        m_thread.start();
        m_anchor.moveToThread(&m_thread);
        QMetaObject::invokeMethod(
            &m_anchor,
            [this, config]() {
                m_hub = new BufferHub(config);
                m_server = new grpc::Server();
                m_server->setToken(config.listenToken);
                m_service = new BufferService(m_hub, m_server);
                m_service->registerMethods();
                m_up = m_server->start(QHostAddress::LocalHost, config.listenPort, &m_error);
                m_port = m_server->port();
                m_recovered = m_hub->recoveredCount();
            },
            Qt::BlockingQueuedConnection);
    }

    ~BufferInstance()
    {
        QMetaObject::invokeMethod(
            &m_anchor,
            [this]() {
                if (m_hub)
                    m_hub->shutdown();
                if (m_server)
                    m_server->stop();
                delete m_service;
                delete m_server;
                delete m_hub;
                m_service = nullptr;
                m_server = nullptr;
                m_hub = nullptr;
            },
            Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait(15000);
    }

    BufferInstance(const BufferInstance &) = delete;
    BufferInstance &operator=(const BufferInstance &) = delete;

    bool ok() const { return m_up; }
    QString error() const { return m_error; }
    quint16 port() const { return m_port; }
    int recovered() const { return m_recovered; }
    QString target() const { return QStringLiteral("127.0.0.1:%1").arg(m_port); }

private:
    QThread m_thread;
    QObject m_anchor;
    BufferHub *m_hub = nullptr;
    grpc::Server *m_server = nullptr;
    BufferService *m_service = nullptr;
    bool m_up = false;
    int m_recovered = 0;
    quint16 m_port = 0;
    QString m_error;
};

// Waits for the stand-in pipeline to have received `count` packets.
bool waitForSeqs(UpstreamRecord *record, int count, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    for (;;) {
        {
            QMutexLocker lock(&record->mutex);
            if (record->seqs.size() >= count)
                return true;
        }
        if (clock.elapsed() > timeoutMs)
            return false;
        QThread::msleep(25);
    }
}

grpc::Status pushOne(AsrClient &client, const QString &sessionId, quint64 seq, int bytes)
{
    asr::PushAudioRequest push;
    push.sessionId = sessionId;
    push.pcm = QByteArray(bytes, char(seq & 0x7f));
    push.sampleRate = 48000;
    push.channels = 1;
    push.audioFormat = QStringLiteral("s16le");
    push.reset = (seq == 1);
    push.vadChunkMs = 160;
    push.seq = seq;
    asr::PushAudioResponse ack;
    return client.pushAudio(push, &ack, 10000);
}

int restart()
{
    const int kBeforeStall = 4;   // forwarded normally
    const int kDuringStall = 6;   // accepted, journalled, never forwarded
    const int kPacketBytes = 4096;

    QTemporaryDir journalDir;
    check(journalDir.isValid(), "a journal directory can be created");
    if (!journalDir.isValid())
        return 1;

    // ---- the stand-in inference tier, which outlives the "restart" ---------
    UpstreamRecord record;
    QThread upstreamThread;
    upstreamThread.setObjectName(QStringLiteral("selftest-upstream"));
    upstreamThread.start();
    QObject upstreamAnchor;
    upstreamAnchor.moveToThread(&upstreamThread);

    grpc::Server *upstream = nullptr;
    quint16 upstreamPort = 0;
    bool upstreamUp = false;
    QString error;
    QMetaObject::invokeMethod(
        &upstreamAnchor,
        [&]() {
            upstream = new grpc::Server();
            registerFakeUpstream(upstream, &record);
            upstreamUp = upstream->start(QHostAddress::LocalHost, 0, &error);
            upstreamPort = upstream->port();
        },
        Qt::BlockingQueuedConnection);
    check(upstreamUp, QStringLiteral("stand-in inference tier listens (%1)")
                          .arg(upstreamUp ? QStringLiteral("port %1").arg(upstreamPort) : error));
    if (!upstreamUp) {
        QMetaObject::invokeMethod(
            &upstreamAnchor, [&]() { delete upstream; }, Qt::BlockingQueuedConnection);
        upstreamThread.quit();
        upstreamThread.wait();
        return 1;
    }

    ServerConfig config;
    config.listenAddress = QStringLiteral("127.0.0.1");
    config.listenPort = 0;
    config.listenToken = QString::fromLatin1(kToken);
    config.upstreamTarget = QStringLiteral("127.0.0.1:%1").arg(upstreamPort);
    config.upstreamLanes = 2;
    config.upstreamTimeoutMs = 5000;
    config.upstreamProbeMs = 300000;
    config.bufferSeconds = 60.0;
    config.statePollMs = 5000;
    config.journalDir = journalDir.path();
    // Small enough that this handful of packets rolls a segment, so segment
    // rolling is on the tested path rather than only on the deployed one.
    config.segmentBytes = 256 * 1024;

    QString sessionId;

    // ---- phase 1: a meeting, then the server dies mid-backlog -------------
    {
        BufferInstance first(config);
        check(first.ok(), QStringLiteral("Server buffer listens (%1)")
                              .arg(first.ok() ? QStringLiteral("port %1").arg(first.port())
                                              : first.error()));
        check(first.recovered() == 0, "an empty journal directory recovers nothing");
        if (!first.ok())
            return 1;

        AsrClient client(first.target(), QString::fromLatin1(kToken));
        asr::StartSessionRequest startRequest;
        startRequest.configJson = QStringLiteral("{\"title\":\"Khởi động lại\"}");
        asr::StartSessionResponse started;
        grpc::Status status = client.startSession(startRequest, &started, 10000);
        check(status.ok(), QStringLiteral("start_session (%1)").arg(status.toString()));
        sessionId = started.sessionId;

        for (int i = 1; i <= kBeforeStall && status.ok(); ++i)
            status = pushOne(client, sessionId, quint64(i), kPacketBytes);
        check(status.ok(), QStringLiteral("%1 packets accepted before the stall").arg(kBeforeStall));
        check(waitForSeqs(&record, kBeforeStall, 5000),
              QStringLiteral("the pipeline received the first %1").arg(kBeforeStall));

        // From here the pipeline refuses.  The buffer must keep accepting from
        // the client and keep the packets - that is the whole point of it.
        {
            QMutexLocker lock(&record.mutex);
            record.refuse = true;
        }
        for (int i = kBeforeStall + 1; i <= kBeforeStall + kDuringStall && status.ok(); ++i)
            status = pushOne(client, sessionId, quint64(i), kPacketBytes);
        check(status.ok(),
              QStringLiteral("%1 more packets accepted while the pipeline refuses (%2)")
                  .arg(kDuringStall)
                  .arg(status.toString()));

        // Give the forwarder a moment to be sure it is stuck retrying rather
        // than quietly succeeding.
        QThread::msleep(300);
        {
            QMutexLocker lock(&record.mutex);
            check(record.seqs.size() == kBeforeStall,
                  QStringLiteral("the pipeline still has only %1 packets (%2)")
                      .arg(kBeforeStall)
                      .arg(record.seqs.size()));
        }
        // `first` goes out of scope here: hub shutdown, server stop, threads
        // joined.  Exactly what a restart does, minus the process exiting.
    }

    const QStringList handles = jrn::store::handles(journalDir.path());
    check(handles.size() == 1,
          QStringLiteral("one journal survived the shutdown (%1)").arg(handles.size()));

    // ---- phase 2: the pipeline recovers, and so does the server -----------
    {
        QMutexLocker lock(&record.mutex);
        record.refuse = false;
    }

    {
        BufferInstance second(config);
        check(second.ok(), QStringLiteral("Server buffer restarts (%1)")
                               .arg(second.ok() ? QStringLiteral("port %1").arg(second.port())
                                                : second.error()));
        check(second.recovered() == 1,
              QStringLiteral("the session was read back from disk (%1 recovered)")
                  .arg(second.recovered()));
        if (!second.ok())
            return 1;

        // The backlog goes upstream on its own, with no client involved at all.
        const bool drained = waitForSeqs(&record, kBeforeStall + kDuringStall, 15000);
        int delivered = 0;
        {
            QMutexLocker lock(&record.mutex);
            delivered = record.seqs.size();
        }
        check(drained, QStringLiteral("the backlog reached the pipeline after the restart "
                                      "(%1 of %2, with no client attached)")
                           .arg(delivered)
                           .arg(kBeforeStall + kDuringStall));

        {
            QMutexLocker lock(&record.mutex);
            bool ordered = true;
            for (int i = 0; i < record.seqs.size(); ++i) {
                if (record.seqs.at(i) != quint64(i + 1))
                    ordered = false;
            }
            check(ordered, "every packet arrived exactly once, in order, across the restart");
            bool whole = true;
            for (int size : record.sizes) {
                if (size != kPacketBytes)
                    whole = false;
            }
            check(whole, "every packet arrived whole");
        }

        AsrClient client(second.target(), QString::fromLatin1(kToken));

        // A client that retries the last seq it sent - which is exactly what
        // its transport-retry loop does after the connection came back - must
        // get the stored ACK, not a second copy of the audio.
        grpc::Status status =
            pushOne(client, sessionId, quint64(kBeforeStall + kDuringStall), kPacketBytes);
        check(status.ok(), QStringLiteral("a resent seq is accepted after the restart (%1)")
                               .arg(status.toString()));

        // And the meeting simply carries on from the next seq.
        const quint64 next = quint64(kBeforeStall + kDuringStall + 1);
        status = pushOne(client, sessionId, next, kPacketBytes);
        check(status.ok(), QStringLiteral("the meeting continues at seq %1 (%2)")
                               .arg(next)
                               .arg(status.toString()));
        check(waitForSeqs(&record, kBeforeStall + kDuringStall + 1, 5000),
              "the continued packet reached the pipeline");
        {
            QMutexLocker lock(&record.mutex);
            check(record.seqs.size() == kBeforeStall + kDuringStall + 1,
                  QStringLiteral("no duplicate was delivered by the replay (%1 packets total)")
                      .arg(record.seqs.size()));
        }

        buf::BufferStatusResponse bufferStatus;
        status = client.getBufferStatus(buf::BufferStatusRequest(), &bufferStatus, 10000);
        check(status.ok() && bufferStatus.sessions.size() == 1,
              "the recovered session shows in the admin view");
        if (bufferStatus.sessions.size() == 1) {
            check(bufferStatus.sessions.at(0).sessionId == sessionId,
                  "under its original session id");
        }

        asr::SessionRequest sessionRequest;
        sessionRequest.sessionId = sessionId;
        asr::StopSessionResponse stopped;
        status = client.stopSession(sessionRequest, &stopped, 30000);
        check(status.ok(), QStringLiteral("stop_session after recovery (%1)")
                               .arg(status.toString()));
        {
            QMutexLocker lock(&record.mutex);
            check(!record.events.isEmpty() && record.events.last() == QStringLiteral("stop"),
                  "stop_session still reached the pipeline last");
        }
    }

    // ---- phase 3: a finished meeting is not resumed again -----------------
    {
        BufferInstance third(config);
        check(third.recovered() == 0,
              QStringLiteral("a session that ended cleanly is not resumed (%1 recovered)")
                  .arg(third.recovered()));
    }
    check(jrn::store::handles(journalDir.path()).isEmpty(),
          "and its journal is gone from disk");

    QMetaObject::invokeMethod(
        &upstreamAnchor,
        [&]() {
            delete upstream;
            upstream = nullptr;
        },
        Qt::BlockingQueuedConnection);
    upstreamThread.quit();
    upstreamThread.wait(15000);
    return 0;
}

// A crash leaves the last record half written.  That is the normal shape of a
// journal after the thing this feature exists for, so it has to read back as
// "everything before the tear", not as a failure.
void tornTail()
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        check(false, "a journal directory can be created");
        return;
    }
    const QString handle = jrn::store::handleFor(QStringLiteral("phiên-rách"));

    {
        jrn::Journal journal;
        jrn::Meta meta;
        meta.sessionId = QStringLiteral("phiên-rách");
        meta.client = QStringLiteral("127.0.0.1:1");
        QString error;
        check(journal.create(dir.path(), handle, meta, jrn::Durability::Os, jrn::Keep::Session,
                             1024 * 1024, &error),
              QStringLiteral("a journal can be created (%1)").arg(error));
        for (quint64 seq = 1; seq <= 5; ++seq) {
            jrn::Packet packet;
            packet.seq = seq;
            packet.pcm = QByteArray(512, char(seq));
            packet.sampleRate = 48000;
            packet.channels = 1;
            packet.audioFormat = QStringLiteral("s16le");
            journal.appendPacket(packet, &error);
        }
        jrn::Progress progress;
        progress.seq = 2;
        progress.sourceSeenSec = 1.5;
        journal.appendProgress(progress, &error);
    }

    // Half a record, as a power cut or a SIGKILL mid-write would leave.
    {
        const QDir target(dir.path());
        QStringList names =
            target.entryList(QStringList{handle + QStringLiteral(".*.jrn")}, QDir::Files);
        names.sort();
        QFile last(target.filePath(names.last()));
        check(last.open(QIODevice::WriteOnly | QIODevice::Append), "the last segment can be opened");
        last.write(QByteArray("\x02\x00\x00\x10\x00partial", 12));
        last.close();
    }

    jrn::Recovered recovered;
    QString error;
    check(jrn::store::recover(dir.path(), handle, &recovered, &error),
          QStringLiteral("a journal with a torn tail still recovers (%1)").arg(error));
    check(recovered.truncated, "and reports that it was torn");
    check(recovered.meta.sessionId == QStringLiteral("phiên-rách"),
          "the session id survives (Vietnamese)");
    check(recovered.lastAcceptedSeq == 5,
          QStringLiteral("all five complete packets were read (last seq %1)")
              .arg(recovered.lastAcceptedSeq));
    check(recovered.progress.seq == 2,
          QStringLiteral("the progress watermark survives (%1)").arg(recovered.progress.seq));
    check(recovered.backlog.size() == 3,
          QStringLiteral("the backlog is what the pipeline had not acknowledged (%1)")
              .arg(recovered.backlog.size()));
    check(!recovered.backlog.isEmpty() && recovered.backlog.first().seq == 3,
          "and it starts at the packet after the watermark");
    check(!recovered.backlog.isEmpty() && recovered.backlog.first().pcm.size() == 512,
          "with its audio intact");
}

} // namespace

int runRestartTests()
{
    g_failures = 0;
    out() << "== s2t-qt-server: a meeting survives a restart ==\n";
    tornTail();
    restart();
    out() << (g_failures == 0 ? "restart: OK\n"
                              : QStringLiteral("restart: %1 lỗi\n").arg(g_failures));
    out().flush();
    return g_failures == 0 ? 0 : 1;
}
int runCodecTests()
{
    g_failures = 0;
    out() << "== s2t-qt-server: codec ==\n";
    testRequestRoundTrip();
    testResponseRoundTrip();
    testFraming();
    out() << (g_failures == 0 ? "codec: OK\n"
                              : QStringLiteral("codec: %1 lỗi\n").arg(g_failures));
    out().flush();
    return g_failures == 0 ? 0 : 1;
}

int runLoopbackTests()
{
    g_failures = 0;
    out() << "== s2t-qt-server: loopback ==\n";
    loopback();
    out() << (g_failures == 0 ? "loopback: OK\n"
                              : QStringLiteral("loopback: %1 lỗi\n").arg(g_failures));
    out().flush();
    return g_failures == 0 ? 0 : 1;
}

int runAll()
{
    const int codec = runCodecTests();
    const int loop = runLoopbackTests();
    const int chainCode = runBufferTests();
    const int restartCode = runRestartTests();
    return codec != 0 || loop != 0 || chainCode != 0 || restartCode != 0 ? 1 : 0;
}

int runProbe(const QString &target, const QString &token)
{
    out() << "== s2t-qt-server: probe " << target << " ==\n";
    AsrClient client(target, token);
    double latencyMs = 0.0;
    grpc::Status status = client.ping(5000, &latencyMs);
    if (!status.ok()) {
        out() << "  không mở được kênh tới " << target << ": " << status.toString() << "\n";
        out().flush();
        return 2;
    }
    out() << "  kênh mở sau " << QString::number(latencyMs, 'f', 1) << " ms\n";

    asr::ModelStatusResponse models;
    status = client.getModelStatus(&models, 10000);
    if (!status.ok()) {
        // A reachable adapter that rejects the token is a different problem
        // from an unreachable one, and the two need different fixes.
        out() << "  get_model_status: " << status.toString() << "\n";
        out().flush();
        return status.code == grpc::Unauthenticated ? 3 : 4;
    }
    out() << "  get_model_status: " << models.models.size() << " mô hình\n";
    for (const asr::ModelStatusEntry &entry : std::as_const(models.models))
        out() << "    " << entry.name << " v" << entry.version << " " << entry.state << "\n";
    out().flush();
    return 0;
}

} // namespace serverselftest
