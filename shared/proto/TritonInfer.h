// C++ mirror of the part of Triton's grpc_service.proto (package `inference`,
// the KServe v2 predict protocol) that s2t-qt-server calls.
//
// Same rule as every other file under shared/proto: the field numbers are the
// contract, copied verbatim from the .proto, and there is no protoc here.
//
// Only five of the twenty RPCs are mirrored, because only five are used:
//
//   ServerLive       the reachability probe behind "upstream_ready"
//   ServerReady      the same question one level deeper
//   ModelReady       whether the session model in particular is loaded
//   RepositoryIndex  what get_model_status reports
//   ModelInfer       the audio path
//
// The `parameters` maps on the infer messages are deliberately absent.  They
// are map<string, InferParameter>, and the model this server drives
// (asr_diar_session) declares no sequence batching and takes its whole state
// through the stream_id tensor - so there is nothing to put in them.  An
// unknown field on the wire is skipped, which is what protobuf would do too.
#ifndef TRITONINFER_H
#define TRITONINFER_H

#include "ProtoWire.h"

#include <QByteArray>
#include <QList>
#include <QString>

namespace trt {

// Triton's tensor datatype strings, spelled as the v2 protocol spells them.
// They travel as plain strings, so a typo is a runtime INVALID_ARGUMENT from
// the server rather than a build error - hence constants rather than literals
// at the call sites.
namespace dtype {
constexpr const char *Bool = "BOOL";
constexpr const char *Int32 = "INT32";
constexpr const char *Int64 = "INT64";
constexpr const char *Fp32 = "FP32";
constexpr const char *Bytes = "BYTES";
} // namespace dtype

struct ServerLiveRequest
{
    QByteArray serialize() const { return QByteArray(); }
    void parse(pw::Reader &reader) { reader.skipRemaining(); }
};

struct ServerLiveResponse
{
    bool live = false; // 1
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct ServerReadyRequest
{
    QByteArray serialize() const { return QByteArray(); }
    void parse(pw::Reader &reader) { reader.skipRemaining(); }
};

struct ServerReadyResponse
{
    bool ready = false; // 1
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct ModelReadyRequest
{
    QString name;    // 1
    QString version; // 2
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct ModelReadyResponse
{
    bool ready = false; // 1
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct RepositoryIndexRequest
{
    QString repositoryName; // 1
    bool ready = false;     // 2
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct ModelIndex
{
    QString name;    // 1
    QString version; // 2
    QString state;   // 3
    QString reason;  // 4
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct RepositoryIndexResponse
{
    QList<ModelIndex> models; // 1
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

// Contents travel in raw_input_contents / raw_output_contents rather than in
// this message.  Triton allows either, and raw is both smaller and the only
// one that can carry a 160 ms float32 window without 40 000 varints.  It is
// mirrored anyway because a model may answer with it, and silently reading a
// tensor as empty would look like the model produced nothing.
struct InferTensorContents
{
    QList<qint32> intContents;   // 2
    QList<qint64> int64Contents; // 3
    QList<float> fp32Contents;   // 6
    QList<QByteArray> bytesContents; // 8
    bool isEmpty() const
    {
        return intContents.isEmpty() && int64Contents.isEmpty() && fp32Contents.isEmpty()
            && bytesContents.isEmpty();
    }
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct InferInputTensor
{
    QString name;         // 1
    QString datatype;     // 2
    QList<qint64> shape;  // 3
    InferTensorContents contents; // 5
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct InferRequestedOutputTensor
{
    QString name; // 1
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct ModelInferRequest
{
    QString modelName;    // 1
    QString modelVersion; // 2
    QString id;           // 3
    QList<InferInputTensor> inputs;             // 5
    QList<InferRequestedOutputTensor> outputs;  // 6
    // One entry per input tensor, in the same order as `inputs`, for every
    // input whose contents were left empty.  Triton pairs them positionally.
    QList<QByteArray> rawInputContents;         // 7
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct InferOutputTensor
{
    QString name;                 // 1
    QString datatype;             // 2
    QList<qint64> shape;          // 3
    InferTensorContents contents; // 5
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct ModelInferResponse
{
    QString modelName;    // 1
    QString modelVersion; // 2
    QString id;           // 3
    QList<InferOutputTensor> outputs;   // 5
    QList<QByteArray> rawOutputContents; // 6
    QByteArray serialize() const;
    void parse(pw::Reader &reader);

    // ---- reading a named output -------------------------------------------
    //
    // Triton answers with the tensors in whatever order the model declared
    // them and puts the payloads in a parallel list, so every read is "find the
    // index, then take that raw buffer".  Doing that by hand at 40 call sites
    // is where an off-by-one would hide, so it is done once here.
    //
    // Each returns false when the model did not produce that output at all,
    // which is different from producing an empty one.
    bool rawFor(const QString &name, QByteArray *out) const;
    bool stringAt(const QString &name, QString *out) const;
    bool floatAt(const QString &name, float *out) const;
    bool int32At(const QString &name, qint32 *out) const;
    bool floatsAt(const QString &name, QList<float> *out) const;
    bool int32sAt(const QString &name, QList<qint32> *out) const;
    bool int64sAt(const QString &name, QList<qint64> *out) const;
    bool stringsAt(const QString &name, QList<QString> *out) const;
};

} // namespace trt

#endif // TRITONINFER_H
