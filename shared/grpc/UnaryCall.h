// One unary gRPC call: serialize, invoke, parse.
//
// AsrClient has had this as a private template since the client was written,
// because back then asr.ui.v1 was the only service anyone here spoke.  The
// server now also speaks inference.GRPCInferenceService and
// nvidia.riva.asr.RivaSpeechRecognition, neither of which has - or wants - a
// typed façade of its own, so the three lines that turn a Channel into a typed
// call live here where all of them can reach them.
//
// The decode failure deliberately reports Internal rather than the transport's
// own code: reaching this point means the far side answered OK and the bytes it
// sent do not match the .proto we mirrored, which is a contract mismatch and
// wants to read like one.
#ifndef UNARYCALL_H
#define UNARYCALL_H

#include "GrpcChannel.h"
#include "proto/ProtoWire.h"

namespace grpc {

template <typename Req, typename Resp>
Status unaryCall(Channel &channel, const char *method, const Req &request, Resp *out, int timeoutMs)
{
    QByteArray payload;
    Status status =
        channel.invoke(QString::fromLatin1(method), request.serialize(), timeoutMs, &payload);
    if (!status.ok())
        return status;
    pw::Reader reader(payload);
    out->parse(reader);
    if (!reader.ok()) {
        status.code = Internal;
        status.message =
            QStringLiteral("không giải mã được phản hồi %1").arg(QString::fromLatin1(method));
    }
    return status;
}

} // namespace grpc

#endif // UNARYCALL_H
