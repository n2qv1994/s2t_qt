#include "SpeakerRegistry.h"

namespace reg {

using pw::Reader;
using pw::Writer;
using pw::WireType;

QByteArray EnrollSpeakerRequest::serialize() const
{
    Writer out;
    out.putString(1, displayName);
    out.putBytes(2, wav);
    out.putString(3, editorId);
    out.putString(4, note);
    out.putBool(5, allowBelowPolicy);
    return out.take();
}

QByteArray ListSessionSpeakersRequest::serialize() const
{
    Writer out;
    out.putString(1, sessionId);
    return out.take();
}

QByteArray SpeakerSelection::serialize() const
{
    Writer out;
    out.putString(1, sessionSpeakerId);
    out.putEnum(2, destination);
    out.putString(3, globalName);
    return out.take();
}

QByteArray SaveSessionSpeakersRequest::serialize() const
{
    Writer out;
    out.putString(1, sessionId);
    out.putRepeatedMessage(2, selections);
    out.putString(3, editorId);
    return out.take();
}

QByteArray GetSpeakerRegistryStatusRequest::serialize() const
{
    Writer out;
    out.putString(1, sessionId);
    return out.take();
}

void GetEnrollmentScriptResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: scriptText = r.readString(); break;
        case 2: sampleRate = r.readUInt32(); break;
        case 3: recommendedDurationSec = r.readDouble(); break;
        case 4: targetSegments = r.readUInt32(); break;
        default: r.skip(type); break;
        }
    }
}

void EnrollSpeakerResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: ok = r.readBool(); break;
        case 2: error = r.readString(); break;
        case 3: speakerId = r.readString(); break;
        case 4: rawSeconds = r.readDouble(); break;
        case 5: speechSecondsAfterVad = r.readDouble(); break;
        case 6: segmentsEnrolled = r.readUInt32(); break;
        case 7: targetSegments = r.readUInt32(); break;
        case 8: warning = r.readString(); break;
        case 9: dbMtime = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

void SessionSpeakerEvidence::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: totalSpeechSec = r.readDouble(); break;
        case 2: spanCount = r.readUInt32(); break;
        case 3: stagedAt = r.readDouble(); break;
        case 4: sourceVerifiedName = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void SessionSpeakerEntry::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionSpeakerId = r.readString(); break;
        case 2: diarSlots.append(r.readString()); break;
        case 3: verifiedName = r.readString(); break;
        case 4: score = r.readDouble(); break;
        case 5: windows = r.readUInt32(); break;
        case 6: createdAt = r.readDouble(); break;
        case 7: updatedAt = r.readDouble(); break;
        case 8: status = r.readString(); break;
        case 9: hasEvidence = r.readBool(); break;
        case 10: r.readMessageInto(&evidence); break;
        case 11: publishedName = r.readString(); break;
        case 12: publishedAt = r.readDouble(); break;
        case 13: publishError = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void ListSessionSpeakersResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: r.appendMessage(&speakers); break;
        default: r.skip(type); break;
        }
    }
}

void SaveSpeakerResult::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionSpeakerId = r.readString(); break;
        case 2: ok = r.readBool(); break;
        case 3: status = r.readString(); break;
        case 4: error = r.readString(); break;
        case 5: segmentsEnrolled = r.readUInt32(); break;
        default: r.skip(type); break;
        }
    }
}

void SaveSessionSpeakersResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: r.appendMessage(&results); break;
        default: r.skip(type); break;
        }
    }
}

void SpeakerBelowPolicy::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: spkId = r.readString(); break;
        case 2: spkName = r.readString(); break;
        case 3: sampleCount = r.readUInt32(); break;
        case 4: longestSampleSec = r.readDouble(); break;
        case 5: reason = r.readString(); break;
        case 6: kind = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void GetSpeakerRegistryStatusResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: globalDbMtime = r.readDouble(); break;
        case 2: globalDbRevision = r.readString(); break;
        case 3: globalSpeakerCount = r.readUInt32(); break;
        case 4: sidecarReachable = r.readBool(); break;
        case 5: sessionId = r.readString(); break;
        case 6: sessionPendingCount = r.readUInt32(); break;
        case 7: sessionPublishedCount = r.readUInt32(); break;
        case 8: sessionFailedCount = r.readUInt32(); break;
        case 9: globalSpeakerNames.append(r.readString()); break;
        case 10: r.appendMessage(&speakersBelowPolicy); break;
        default: r.skip(type); break;
        }
    }
}


// ---------------------------------------------------------------------------
// The other direction: what s2t-qt-server needs to answer this service.  Same
// field numbers as above, written out again on purpose - see the note in
// AsrSession.cpp.
// ---------------------------------------------------------------------------

void EnrollSpeakerRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: displayName = r.readString(); break;
        case 2: wav = r.readBytes(); break;
        case 3: editorId = r.readString(); break;
        case 4: note = r.readString(); break;
        case 5: allowBelowPolicy = r.readBool(); break;
        default: r.skip(type); break;
        }
    }
}

void ListSessionSpeakersRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void SpeakerSelection::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionSpeakerId = r.readString(); break;
        case 2: destination = r.readEnum(); break;
        case 3: globalName = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void SaveSessionSpeakersRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: r.appendMessage(&selections); break;
        case 3: editorId = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void GetSpeakerRegistryStatusRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray GetEnrollmentScriptResponse::serialize() const
{
    Writer out;
    out.putString(1, scriptText);
    out.putUInt32(2, sampleRate);
    out.putDouble(3, recommendedDurationSec);
    out.putUInt32(4, targetSegments);
    return out.take();
}

QByteArray EnrollSpeakerResponse::serialize() const
{
    Writer out;
    out.putBool(1, ok);
    out.putString(2, error);
    out.putString(3, speakerId);
    out.putDouble(4, rawSeconds);
    out.putDouble(5, speechSecondsAfterVad);
    out.putUInt32(6, segmentsEnrolled);
    out.putUInt32(7, targetSegments);
    out.putString(8, warning);
    out.putDouble(9, dbMtime);
    return out.take();
}

QByteArray SessionSpeakerEvidence::serialize() const
{
    Writer out;
    out.putDouble(1, totalSpeechSec);
    out.putUInt32(2, spanCount);
    out.putDouble(3, stagedAt);
    out.putString(4, sourceVerifiedName);
    return out.take();
}

QByteArray SessionSpeakerEntry::serialize() const
{
    Writer out;
    out.putString(1, sessionSpeakerId);
    out.putRepeatedString(2, diarSlots);
    out.putString(3, verifiedName);
    out.putDouble(4, score);
    out.putUInt32(5, windows);
    out.putDouble(6, createdAt);
    out.putDouble(7, updatedAt);
    out.putString(8, status);
    out.putBool(9, hasEvidence);
    out.putSubMessage(10, evidence.serialize());
    out.putString(11, publishedName);
    out.putDouble(12, publishedAt);
    out.putString(13, publishError);
    return out.take();
}

QByteArray ListSessionSpeakersResponse::serialize() const
{
    Writer out;
    out.putString(1, sessionId);
    out.putRepeatedMessage(2, speakers);
    return out.take();
}

QByteArray SaveSpeakerResult::serialize() const
{
    Writer out;
    out.putString(1, sessionSpeakerId);
    out.putBool(2, ok);
    out.putString(3, status);
    out.putString(4, error);
    out.putUInt32(5, segmentsEnrolled);
    return out.take();
}

QByteArray SaveSessionSpeakersResponse::serialize() const
{
    Writer out;
    out.putString(1, sessionId);
    out.putRepeatedMessage(2, results);
    return out.take();
}

QByteArray SpeakerBelowPolicy::serialize() const
{
    Writer out;
    out.putString(1, spkId);
    out.putString(2, spkName);
    out.putUInt32(3, sampleCount);
    out.putDouble(4, longestSampleSec);
    out.putString(5, reason);
    out.putString(6, kind);
    return out.take();
}

QByteArray GetSpeakerRegistryStatusResponse::serialize() const
{
    Writer out;
    out.putDouble(1, globalDbMtime);
    out.putString(2, globalDbRevision);
    out.putUInt32(3, globalSpeakerCount);
    out.putBool(4, sidecarReachable);
    out.putString(5, sessionId);
    out.putUInt32(6, sessionPendingCount);
    out.putUInt32(7, sessionPublishedCount);
    out.putUInt32(8, sessionFailedCount);
    out.putRepeatedString(9, globalSpeakerNames);
    out.putRepeatedMessage(10, speakersBelowPolicy);
    return out.take();
}

} // namespace reg
