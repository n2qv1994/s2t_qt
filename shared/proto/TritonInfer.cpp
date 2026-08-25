#include "TritonInfer.h"

#include <cstring>

namespace trt {

using pw::Reader;
using pw::WireType;
using pw::Writer;

namespace {

// Triton's raw tensor buffers are little-endian and untagged: an FP32 tensor is
// just its floats end to end.  BYTES is the one exception - each element is a
// 4-byte little-endian length followed by that many bytes - which is why a
// string output cannot simply be handed to QString::fromUtf8.
template <typename T>
QList<T> rawToList(const QByteArray &raw)
{
    QList<T> out;
    const int count = raw.size() / int(sizeof(T));
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        T value = T();
        std::memcpy(&value, raw.constData() + i * int(sizeof(T)), sizeof(T));
        out.append(value);
    }
    return out;
}

QList<QString> rawToStrings(const QByteArray &raw)
{
    QList<QString> out;
    int offset = 0;
    while (offset + 4 <= raw.size()) {
        quint32 length = 0;
        std::memcpy(&length, raw.constData() + offset, 4);
        offset += 4;
        if (offset + int(length) > raw.size())
            break; // truncated element: return what was whole rather than guess
        out.append(QString::fromUtf8(raw.constData() + offset, int(length)));
        offset += int(length);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------- health ----

QByteArray ServerLiveResponse::serialize() const
{
    Writer w;
    w.putBool(1, live);
    return w.take();
}

void ServerLiveResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: live = r.readBool(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray ServerReadyResponse::serialize() const
{
    Writer w;
    w.putBool(1, ready);
    return w.take();
}

void ServerReadyResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: ready = r.readBool(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray ModelReadyRequest::serialize() const
{
    Writer w;
    w.putString(1, name);
    w.putString(2, version);
    return w.take();
}

void ModelReadyRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: name = r.readString(); break;
        case 2: version = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray ModelReadyResponse::serialize() const
{
    Writer w;
    w.putBool(1, ready);
    return w.take();
}

void ModelReadyResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: ready = r.readBool(); break;
        default: r.skip(type); break;
        }
    }
}

// ------------------------------------------------------------ repository ----

QByteArray RepositoryIndexRequest::serialize() const
{
    Writer w;
    w.putString(1, repositoryName);
    w.putBool(2, ready);
    return w.take();
}

void RepositoryIndexRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: repositoryName = r.readString(); break;
        case 2: ready = r.readBool(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray ModelIndex::serialize() const
{
    Writer w;
    w.putString(1, name);
    w.putString(2, version);
    w.putString(3, state);
    w.putString(4, reason);
    return w.take();
}

void ModelIndex::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: name = r.readString(); break;
        case 2: version = r.readString(); break;
        case 3: state = r.readString(); break;
        case 4: reason = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray RepositoryIndexResponse::serialize() const
{
    Writer w;
    w.putRepeatedMessage(1, models);
    return w.take();
}

void RepositoryIndexResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.appendMessage(&models); break;
        default: r.skip(type); break;
        }
    }
}

// ----------------------------------------------------------------- infer ----

QByteArray InferTensorContents::serialize() const
{
    Writer w;
    w.putPackedInt32(2, intContents);
    w.putPackedInt64(3, int64Contents);
    w.putPackedFloat(6, fp32Contents);
    w.putRepeatedBytes(8, bytesContents);
    return w.take();
}

void InferTensorContents::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 2: r.readPackedInt32(type, &intContents); break;
        case 3: r.readPackedInt64(type, &int64Contents); break;
        case 6: r.readPackedFloat(type, &fp32Contents); break;
        case 8: bytesContents.append(r.readBytes()); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray InferInputTensor::serialize() const
{
    Writer w;
    w.putString(1, name);
    w.putString(2, datatype);
    w.putPackedInt64(3, shape);
    if (!contents.isEmpty())
        w.putSubMessage(5, contents.serialize());
    return w.take();
}

void InferInputTensor::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: name = r.readString(); break;
        case 2: datatype = r.readString(); break;
        case 3: r.readPackedInt64(type, &shape); break;
        case 5: r.readMessageInto(&contents); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray InferRequestedOutputTensor::serialize() const
{
    Writer w;
    w.putString(1, name);
    return w.take();
}

void InferRequestedOutputTensor::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: name = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray ModelInferRequest::serialize() const
{
    Writer w;
    w.putString(1, modelName);
    w.putString(2, modelVersion);
    w.putString(3, id);
    w.putRepeatedMessage(5, inputs);
    w.putRepeatedMessage(6, outputs);
    w.putRepeatedBytes(7, rawInputContents);
    return w.take();
}

void ModelInferRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: modelName = r.readString(); break;
        case 2: modelVersion = r.readString(); break;
        case 3: id = r.readString(); break;
        case 5: r.appendMessage(&inputs); break;
        case 6: r.appendMessage(&outputs); break;
        case 7: rawInputContents.append(r.readBytes()); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray InferOutputTensor::serialize() const
{
    Writer w;
    w.putString(1, name);
    w.putString(2, datatype);
    w.putPackedInt64(3, shape);
    if (!contents.isEmpty())
        w.putSubMessage(5, contents.serialize());
    return w.take();
}

void InferOutputTensor::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: name = r.readString(); break;
        case 2: datatype = r.readString(); break;
        case 3: r.readPackedInt64(type, &shape); break;
        case 5: r.readMessageInto(&contents); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray ModelInferResponse::serialize() const
{
    Writer w;
    w.putString(1, modelName);
    w.putString(2, modelVersion);
    w.putString(3, id);
    w.putRepeatedMessage(5, outputs);
    w.putRepeatedBytes(6, rawOutputContents);
    return w.take();
}

void ModelInferResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: modelName = r.readString(); break;
        case 2: modelVersion = r.readString(); break;
        case 3: id = r.readString(); break;
        case 5: r.appendMessage(&outputs); break;
        case 6: rawOutputContents.append(r.readBytes()); break;
        default: r.skip(type); break;
        }
    }
}

// ---- reading a named output ------------------------------------------------

bool ModelInferResponse::rawFor(const QString &name, QByteArray *out) const
{
    for (int i = 0; i < outputs.size(); ++i) {
        if (outputs.at(i).name != name)
            continue;
        // A model may answer either way for the same tensor, so both shapes
        // have to work: raw buffers are positional, typed contents are inline.
        if (i < rawOutputContents.size()) {
            *out = rawOutputContents.at(i);
            return true;
        }
        return false;
    }
    return false;
}

bool ModelInferResponse::stringAt(const QString &name, QString *out) const
{
    QList<QString> all;
    if (!stringsAt(name, &all) || all.isEmpty())
        return false;
    *out = all.first();
    return true;
}

bool ModelInferResponse::stringsAt(const QString &name, QList<QString> *out) const
{
    QByteArray raw;
    if (rawFor(name, &raw)) {
        *out = rawToStrings(raw);
        return true;
    }
    for (const InferOutputTensor &tensor : outputs) {
        if (tensor.name != name)
            continue;
        out->clear();
        for (const QByteArray &item : tensor.contents.bytesContents)
            out->append(QString::fromUtf8(item));
        return true;
    }
    return false;
}

bool ModelInferResponse::floatsAt(const QString &name, QList<float> *out) const
{
    QByteArray raw;
    if (rawFor(name, &raw)) {
        *out = rawToList<float>(raw);
        return true;
    }
    for (const InferOutputTensor &tensor : outputs) {
        if (tensor.name != name)
            continue;
        *out = tensor.contents.fp32Contents;
        return true;
    }
    return false;
}

bool ModelInferResponse::int32sAt(const QString &name, QList<qint32> *out) const
{
    QByteArray raw;
    if (rawFor(name, &raw)) {
        *out = rawToList<qint32>(raw);
        return true;
    }
    for (const InferOutputTensor &tensor : outputs) {
        if (tensor.name != name)
            continue;
        *out = tensor.contents.intContents;
        return true;
    }
    return false;
}

bool ModelInferResponse::int64sAt(const QString &name, QList<qint64> *out) const
{
    QByteArray raw;
    if (rawFor(name, &raw)) {
        *out = rawToList<qint64>(raw);
        return true;
    }
    for (const InferOutputTensor &tensor : outputs) {
        if (tensor.name != name)
            continue;
        *out = tensor.contents.int64Contents;
        return true;
    }
    return false;
}

bool ModelInferResponse::floatAt(const QString &name, float *out) const
{
    QList<float> all;
    if (!floatsAt(name, &all) || all.isEmpty())
        return false;
    *out = all.first();
    return true;
}

bool ModelInferResponse::int32At(const QString &name, qint32 *out) const
{
    QList<qint32> all;
    if (!int32sAt(name, &all) || all.isEmpty())
        return false;
    *out = all.first();
    return true;
}

} // namespace trt
