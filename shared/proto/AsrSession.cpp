#include "AsrSession.h"

namespace asr {

using pw::Reader;
using pw::Writer;
using pw::WireType;

// ---------------------------------------------------------------- requests --

QByteArray StartSessionRequest::serialize() const
{
    Writer w;
    w.putString(1, configJson);
    return w.take();
}

QByteArray PushAudioRequest::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putBytes(2, pcm);
    w.putUInt32(3, sampleRate);
    w.putUInt32(4, channels);
    w.putString(5, audioFormat);
    w.putBool(6, reset);
    w.putUInt32(7, vadChunkMs);
    w.putUInt64(8, seq);
    return w.take();
}

QByteArray SessionRequest::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    return w.take();
}

QByteArray ReviewRequest::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putBool(2, hasViewStartSec);
    w.putDouble(3, viewStartSec);
    w.putBool(4, hasViewEndSec);
    w.putDouble(5, viewEndSec);
    return w.take();
}

QByteArray AudioRangeRequest::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putDouble(2, startSec);
    w.putDouble(3, endSec);
    return w.take();
}

QByteArray Word::serialize() const
{
    Writer out;
    out.putString(1, w);
    out.putFloat(2, c);
    out.putDouble(3, startSec);
    out.putDouble(4, endSec);
    out.putString(5, speaker);
    return out.take();
}

QByteArray TextEditRequest::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putUInt64(2, baseRevision);
    w.putDouble(3, startSec);
    w.putDouble(4, endSec);
    w.putRepeatedMessage(5, replacementWords);
    w.putString(6, editorId);
    w.putString(7, note);
    return w.take();
}

QByteArray ListSessionsRequest::serialize() const
{
    Writer w;
    w.putUInt32(1, limit);
    w.putString(2, cursor);
    return w.take();
}

QByteArray RenameSpeakerRequest::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putString(2, fromSpeaker);
    w.putString(3, toSpeaker);
    w.putString(4, verifiedName);
    w.putString(5, editorId);
    w.putString(6, note);
    return w.take();
}

QByteArray PipelineTraceRequest::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putUInt64(2, afterSeq);
    w.putUInt32(3, limit);
    w.putRepeatedString(4, stages);
    return w.take();
}

QByteArray AuditHistoryRequest::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putUInt32(2, limit);
    return w.take();
}

// --------------------------------------------------------------- responses --

void AudioRangeResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: pcm = r.readBytes(); break;
        case 3: sampleRate = r.readUInt32(); break;
        case 4: channels = r.readUInt32(); break;
        case 5: audioFormat = r.readString(); break;
        case 6: startSec = r.readDouble(); break;
        case 7: endSec = r.readDouble(); break;
        case 8: totalSec = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

void Word::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: w = r.readString(); break;
        case 2: c = r.readFloat(); break;
        case 3: startSec = r.readDouble(); break;
        case 4: endSec = r.readDouble(); break;
        case 5: speaker = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void Phrase::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: text = r.readString(); break;
        case 2: avgConf = r.readFloat(); break;
        case 3: isLowConf = r.readBool(); break;
        case 4: startSec = r.readDouble(); break;
        case 5: endSec = r.readDouble(); break;
        case 6: r.appendMessage(&words); break;
        default: r.skip(type); break;
        }
    }
}

void DisplayRow::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: rowId = r.readString(); break;
        case 2: speaker = r.readString(); break;
        case 3: speakerProb = r.readFloat(); break;
        case 4: verifiedName = r.readString(); break;
        case 5: startSec = r.readDouble(); break;
        case 6: endSec = r.readDouble(); break;
        case 8: mergedText = r.readString(); break;
        case 10: updatingText = r.readString(); break;
        case 11: stableTokenCount = r.readUInt32(); break;
        case 12: isProvisional = r.readBool(); break;
        case 13: r.appendMessage(&phrases); break;
        case 14: r.appendMessage(&displayTokens); break;
        case 15: r.appendMessage(&updatingTokens); break;
        // 7 and 9 are reserved (itn_text/stable_text); an old server that
        // still sends them must be skipped, not treated as corruption.
        default: r.skip(type); break;
        }
    }
}

void Highlight::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: startSec = r.readDouble(); break;
        case 2: speaker = r.readString(); break;
        case 3: text = r.readString(); break;
        case 4: confPct = r.readUInt32(); break;
        default: r.skip(type); break;
        }
    }
}

void LatencyClient::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: prepareP50 = r.readDouble(); break;
        case 2: prepareP95 = r.readDouble(); break;
        case 3: waitP50 = r.readDouble(); break;
        case 4: waitP95 = r.readDouble(); break;
        case 5: parseP50 = r.readDouble(); break;
        case 6: parseP95 = r.readDouble(); break;
        case 7: e2eP50 = r.readDouble(); break;
        case 8: e2eP95 = r.readDouble(); break;
        case 9: overheadVsServerSumP50 = r.readDouble(); break;
        case 10: overheadVsServerSumP95 = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

void LatencyServer::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: asrP50 = r.readDouble(); break;
        case 2: asrP95 = r.readDouble(); break;
        case 3: diarP50 = r.readDouble(); break;
        case 4: diarP95 = r.readDouble(); break;
        case 5: verifyP50 = r.readDouble(); break;
        case 6: verifyP95 = r.readDouble(); break;
        case 7: itnP50 = r.readDouble(); break;
        case 8: itnP95 = r.readDouble(); break;
        case 9: vadP50 = r.readDouble(); break;
        case 10: vadP95 = r.readDouble(); break;
        case 11: denoiseP50 = r.readDouble(); break;
        case 12: denoiseP95 = r.readDouble(); break;
        case 13: sumP50 = r.readDouble(); break;
        case 14: sumP95 = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

void LatencyUi::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: onChunkTotalP50 = r.readDouble(); break;
        case 2: onChunkTotalP95 = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

void LatencySummary::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.readMessageInto(&client); break;
        case 2: r.readMessageInto(&server); break;
        case 3: r.readMessageInto(&ui); break;
        default: r.skip(type); break;
        }
    }
}

void SessionState::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: title = r.readString(); break;
        case 2: confThresholdPct = r.readUInt32(); break;
        case 3: r.appendMessage(&rows); break;
        case 4: r.appendMessage(&provisionalRows); break;
        case 5: speakerIds.append(r.readString()); break;
        case 6: r.appendMessage(&highlights); break;
        case 7: nPhrases = r.readUInt32(); break;
        case 8: nLow = r.readUInt32(); break;
        case 9: r.readPackedFloat(type, &ampTrace); break;
        case 10: ampTraceStepSec = r.readDouble(); break;
        case 11: sourceTotalSec = r.readDouble(); break;
        case 12: sourceSeenSec = r.readDouble(); break;
        case 13: speechSeenSec = r.readDouble(); break;
        case 14: wallElapsedSec = r.readDouble(); break;
        case 15: playheadRatio = r.readDouble(); break;
        case 16: done = r.readBool(); break;
        case 17: ts = r.readDouble(); break;
        case 18: lastAsrChunkMs = r.readUInt32(); break;
        case 19: inferP50Ms = r.readDouble(); break;
        case 20: inferP95Ms = r.readDouble(); break;
        case 21: r.readMessageInto(&latency); break;
        default: r.skip(type); break;
        }
    }
}

void EventFlags::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: streaming = r.readBool(); break;
        case 2: correction = r.readBool(); break;
        case 3: final = r.readBool(); break;
        default: r.skip(type); break;
        }
    }
}

void Diarization::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.readPackedFloat(type, &flatScores); break;
        case 2: r.readPackedInt32(type, &shape); break;
        case 3: r.readPackedInt64(type, &subframeStartMs); break;
        case 4: r.readPackedInt64(type, &subframeEndMs); break;
        default: r.skip(type); break;
        }
    }
}

void CorrectionUpdate::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: text = r.readString(); break;
        case 2: fullText = r.readString(); break;
        case 3: committedText = r.readString(); break;
        case 4: tailText = r.readString(); break;
        case 5: r.appendMessage(&mergedWords); break;
        case 6: r.readPackedInt32(type, &updatedIndices); break;
        case 7: updateStartSec = r.readDouble(); break;
        case 8: updateEndSec = r.readDouble(); break;
        case 9: commitBoundarySec = r.readDouble(); break;
        case 10: numCommitted = r.readUInt32(); break;
        case 11: numTail = r.readUInt32(); break;
        case 12: itnMs = r.readDouble(); break;
        case 13: mergeMs = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

void Timing::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: clientPrepareMs = r.readDouble(); break;
        case 2: clientWaitMs = r.readDouble(); break;
        case 3: asrMs = r.readDouble(); break;
        case 4: diarMs = r.readDouble(); break;
        case 5: verifyMs = r.readDouble(); break;
        case 6: itnMs = r.readDouble(); break;
        case 7: vadMs = r.readDouble(); break;
        case 8: denoiseMs = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

void StartSessionResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: streamId = r.readInt64(); break;
        case 3: stateVersion = r.readUInt64(); break;
        case 4: r.readMessageInto(&state); break;
        default: r.skip(type); break;
        }
    }
}

void PushAudioResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: streamId = r.readInt64(); break;
        case 3: stateVersion = r.readUInt64(); break;
        case 4: r.readMessageInto(&events); break;
        case 5: sourceSeenSec = r.readDouble(); break;
        case 6: speechSeenSec = r.readDouble(); break;
        case 7: streamingText = r.readString(); break;
        case 8: text = r.readString(); break;
        case 9: itnText = r.readString(); break;
        case 10: itnFullText = r.readString(); break;
        case 11: itnCorrectionText = r.readString(); break;
        case 12: r.appendMessage(&asrWords); break;
        case 13: asrConfidence = r.readFloat(); break;
        case 14: r.readPackedFloat(type, &asrWordConfidence); break;
        case 15: speaker = r.readString(); break;
        case 16: speakerProb = r.readFloat(); break;
        case 17: verifiedName = r.readString(); break;
        case 18: verifyScore = r.readFloat(); break;
        case 19: chunkStartMs = r.readInt64(); break;
        case 20: chunkStartSec = r.readDouble(); break;
        case 21: chunkEndSec = r.readDouble(); break;
        case 22: r.readMessageInto(&diarization); break;
        case 23: r.readMessageInto(&correction); break;
        case 24: r.readMessageInto(&timing); break;
        default: r.skip(type); break;
        }
    }
}

void StateResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: streamId = r.readInt64(); break;
        case 3: stateVersion = r.readUInt64(); break;
        case 4: r.readMessageInto(&state); break;
        case 5: transcriptRevision = r.readUInt64(); break;
        case 6: transcriptFinal = r.readBool(); break;
        case 7: commitBoundarySec = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

void StopSessionResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: streamId = r.readInt64(); break;
        case 3: stateVersion = r.readUInt64(); break;
        case 4: r.readMessageInto(&events); break;
        case 5: r.readMessageInto(&result); break;
        case 6: r.readMessageInto(&state); break;
        default: r.skip(type); break;
        }
    }
}

void CanonicalTranscript::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: revision = r.readUInt64(); break;
        case 2: final = r.readBool(); break;
        case 3: commitBoundarySec = r.readDouble(); break;
        case 4: text = r.readString(); break;
        case 5: r.appendMessage(&words); break;
        default: r.skip(type); break;
        }
    }
}

void ReviewEditResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: r.readMessageInto(&transcript); break;
        case 3: r.readMessageInto(&state); break;
        default: r.skip(type); break;
        }
    }
}

void SessionSummary::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: title = r.readString(); break;
        case 3: createdAt = r.readDouble(); break;
        case 4: updatedAt = r.readDouble(); break;
        case 5: durationSec = r.readDouble(); break;
        case 6: final = r.readBool(); break;
        case 7: running = r.readBool(); break;
        case 8: participants.append(r.readString()); break;
        case 9: securityLevel = r.readString(); break;
        case 10: mode = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void ListSessionsResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.appendMessage(&sessions); break;
        case 2: nextCursor = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void PipelineTraceEvent::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: seq = r.readUInt64(); break;
        case 2: ts = r.readDouble(); break;
        case 3: stage = r.readString(); break;
        case 4: event = r.readString(); break;
        case 5: audioStartSec = r.readDouble(); break;
        case 6: audioEndSec = r.readDouble(); break;
        case 7: payloadJson = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void PipelineTraceResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: r.appendMessage(&events); break;
        case 3: nextSeq = r.readUInt64(); break;
        case 4: hasMore = r.readBool(); break;
        case 5: enabled = r.readBool(); break;
        case 6: truncated = r.readBool(); break;
        case 7: maxBytes = r.readUInt64(); break;
        default: r.skip(type); break;
        }
    }
}

void AuditEvent::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: ts = r.readDouble(); break;
        case 2: event = r.readString(); break;
        case 3: payloadJson = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void AuditHistoryResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: r.appendMessage(&events); break;
        default: r.skip(type); break;
        }
    }
}

void ModelStatusEntry::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: name = r.readString(); break;
        case 2: version = r.readString(); break;
        case 3: state = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void ModelStatusResponse::parse(Reader &r)
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


// ---------------------------------------------------------------------------
// The other direction.
//
// s2t-qt-client only ever writes requests and reads responses; s2t-qt-server
// does exactly the opposite, and the Server buffer has to do both, because it
// answers one contract while calling the same one upstream.  The field numbers
// below are the numbers above, deliberately written out again rather than
// shared through a macro: they are the external contract, and a contract you
// have to expand in your head is one nobody checks.
// ---------------------------------------------------------------------------

// ------------------------------------------------------- requests, reading --

void StartSessionRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: configJson = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void PushAudioRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: pcm = r.readBytes(); break;
        case 3: sampleRate = r.readUInt32(); break;
        case 4: channels = r.readUInt32(); break;
        case 5: audioFormat = r.readString(); break;
        case 6: reset = r.readBool(); break;
        case 7: vadChunkMs = r.readUInt32(); break;
        case 8: seq = r.readUInt64(); break;
        default: r.skip(type); break;
        }
    }
}

void SessionRequest::parse(Reader &r)
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

void ReviewRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: hasViewStartSec = r.readBool(); break;
        case 3: viewStartSec = r.readDouble(); break;
        case 4: hasViewEndSec = r.readBool(); break;
        case 5: viewEndSec = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

void AudioRangeRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: startSec = r.readDouble(); break;
        case 3: endSec = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

void TextEditRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: baseRevision = r.readUInt64(); break;
        case 3: startSec = r.readDouble(); break;
        case 4: endSec = r.readDouble(); break;
        case 5: r.appendMessage(&replacementWords); break;
        case 6: editorId = r.readString(); break;
        case 7: note = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void ListSessionsRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: limit = r.readUInt32(); break;
        case 2: cursor = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void RenameSpeakerRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: fromSpeaker = r.readString(); break;
        case 3: toSpeaker = r.readString(); break;
        case 4: verifiedName = r.readString(); break;
        case 5: editorId = r.readString(); break;
        case 6: note = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

void PipelineTraceRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: afterSeq = r.readUInt64(); break;
        case 3: limit = r.readUInt32(); break;
        case 4: stages.append(r.readString()); break;
        default: r.skip(type); break;
        }
    }
}

void AuditHistoryRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: limit = r.readUInt32(); break;
        default: r.skip(type); break;
        }
    }
}

// ------------------------------------------------------ responses, writing --

QByteArray AudioRangeResponse::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putBytes(2, pcm);
    w.putUInt32(3, sampleRate);
    w.putUInt32(4, channels);
    w.putString(5, audioFormat);
    w.putDouble(6, startSec);
    w.putDouble(7, endSec);
    w.putDouble(8, totalSec);
    return w.take();
}

QByteArray Phrase::serialize() const
{
    Writer w;
    w.putString(1, text);
    w.putFloat(2, avgConf);
    w.putBool(3, isLowConf);
    w.putDouble(4, startSec);
    w.putDouble(5, endSec);
    w.putRepeatedMessage(6, words);
    return w.take();
}

QByteArray DisplayRow::serialize() const
{
    Writer w;
    w.putString(1, rowId);
    w.putString(2, speaker);
    w.putFloat(3, speakerProb);
    w.putString(4, verifiedName);
    w.putDouble(5, startSec);
    w.putDouble(6, endSec);
    // 7 and 9 are reserved (the retired itn_text/stable_text) and are never
    // written, in either direction.
    w.putString(8, mergedText);
    w.putString(10, updatingText);
    w.putUInt32(11, stableTokenCount);
    w.putBool(12, isProvisional);
    w.putRepeatedMessage(13, phrases);
    w.putRepeatedMessage(14, displayTokens);
    w.putRepeatedMessage(15, updatingTokens);
    return w.take();
}

QByteArray Highlight::serialize() const
{
    Writer w;
    w.putDouble(1, startSec);
    w.putString(2, speaker);
    w.putString(3, text);
    w.putUInt32(4, confPct);
    return w.take();
}

QByteArray LatencyClient::serialize() const
{
    Writer w;
    w.putDouble(1, prepareP50);
    w.putDouble(2, prepareP95);
    w.putDouble(3, waitP50);
    w.putDouble(4, waitP95);
    w.putDouble(5, parseP50);
    w.putDouble(6, parseP95);
    w.putDouble(7, e2eP50);
    w.putDouble(8, e2eP95);
    w.putDouble(9, overheadVsServerSumP50);
    w.putDouble(10, overheadVsServerSumP95);
    return w.take();
}

QByteArray LatencyServer::serialize() const
{
    Writer w;
    w.putDouble(1, asrP50);
    w.putDouble(2, asrP95);
    w.putDouble(3, diarP50);
    w.putDouble(4, diarP95);
    w.putDouble(5, verifyP50);
    w.putDouble(6, verifyP95);
    w.putDouble(7, itnP50);
    w.putDouble(8, itnP95);
    w.putDouble(9, vadP50);
    w.putDouble(10, vadP95);
    w.putDouble(11, denoiseP50);
    w.putDouble(12, denoiseP95);
    w.putDouble(13, sumP50);
    w.putDouble(14, sumP95);
    return w.take();
}

QByteArray LatencyUi::serialize() const
{
    Writer w;
    w.putDouble(1, onChunkTotalP50);
    w.putDouble(2, onChunkTotalP95);
    return w.take();
}

QByteArray LatencySummary::serialize() const
{
    Writer w;
    w.putSubMessage(1, client.serialize());
    w.putSubMessage(2, server.serialize());
    w.putSubMessage(3, ui.serialize());
    return w.take();
}

QByteArray SessionState::serialize() const
{
    Writer w;
    w.putString(1, title);
    w.putUInt32(2, confThresholdPct);
    w.putRepeatedMessage(3, rows);
    w.putRepeatedMessage(4, provisionalRows);
    w.putRepeatedString(5, speakerIds);
    w.putRepeatedMessage(6, highlights);
    w.putUInt32(7, nPhrases);
    w.putUInt32(8, nLow);
    w.putPackedFloat(9, ampTrace);
    w.putDouble(10, ampTraceStepSec);
    w.putDouble(11, sourceTotalSec);
    w.putDouble(12, sourceSeenSec);
    w.putDouble(13, speechSeenSec);
    w.putDouble(14, wallElapsedSec);
    w.putDouble(15, playheadRatio);
    w.putBool(16, done);
    w.putDouble(17, ts);
    w.putUInt32(18, lastAsrChunkMs);
    w.putDouble(19, inferP50Ms);
    w.putDouble(20, inferP95Ms);
    w.putSubMessage(21, latency.serialize());
    return w.take();
}

QByteArray EventFlags::serialize() const
{
    Writer w;
    w.putBool(1, streaming);
    w.putBool(2, correction);
    w.putBool(3, final);
    return w.take();
}

QByteArray Diarization::serialize() const
{
    Writer w;
    w.putPackedFloat(1, flatScores);
    w.putPackedInt32(2, shape);
    w.putPackedInt64(3, subframeStartMs);
    w.putPackedInt64(4, subframeEndMs);
    return w.take();
}

QByteArray CorrectionUpdate::serialize() const
{
    Writer w;
    w.putString(1, text);
    w.putString(2, fullText);
    w.putString(3, committedText);
    w.putString(4, tailText);
    w.putRepeatedMessage(5, mergedWords);
    w.putPackedInt32(6, updatedIndices);
    w.putDouble(7, updateStartSec);
    w.putDouble(8, updateEndSec);
    w.putDouble(9, commitBoundarySec);
    w.putUInt32(10, numCommitted);
    w.putUInt32(11, numTail);
    w.putDouble(12, itnMs);
    w.putDouble(13, mergeMs);
    return w.take();
}

QByteArray Timing::serialize() const
{
    Writer w;
    w.putDouble(1, clientPrepareMs);
    w.putDouble(2, clientWaitMs);
    w.putDouble(3, asrMs);
    w.putDouble(4, diarMs);
    w.putDouble(5, verifyMs);
    w.putDouble(6, itnMs);
    w.putDouble(7, vadMs);
    w.putDouble(8, denoiseMs);
    return w.take();
}

QByteArray StartSessionResponse::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putInt64(2, streamId);
    w.putUInt64(3, stateVersion);
    w.putSubMessage(4, state.serialize());
    return w.take();
}

QByteArray PushAudioResponse::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putInt64(2, streamId);
    w.putUInt64(3, stateVersion);
    w.putSubMessage(4, events.serialize());
    w.putDouble(5, sourceSeenSec);
    w.putDouble(6, speechSeenSec);
    w.putString(7, streamingText);
    w.putString(8, text);
    w.putString(9, itnText);
    w.putString(10, itnFullText);
    w.putString(11, itnCorrectionText);
    w.putRepeatedMessage(12, asrWords);
    w.putFloat(13, asrConfidence);
    w.putPackedFloat(14, asrWordConfidence);
    w.putString(15, speaker);
    w.putFloat(16, speakerProb);
    w.putString(17, verifiedName);
    w.putFloat(18, verifyScore);
    w.putInt64(19, chunkStartMs);
    w.putDouble(20, chunkStartSec);
    w.putDouble(21, chunkEndSec);
    w.putSubMessage(22, diarization.serialize());
    w.putSubMessage(23, correction.serialize());
    w.putSubMessage(24, timing.serialize());
    return w.take();
}

QByteArray StateResponse::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putInt64(2, streamId);
    w.putUInt64(3, stateVersion);
    w.putSubMessage(4, state.serialize());
    w.putUInt64(5, transcriptRevision);
    w.putBool(6, transcriptFinal);
    w.putDouble(7, commitBoundarySec);
    return w.take();
}

QByteArray StopSessionResponse::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putInt64(2, streamId);
    w.putUInt64(3, stateVersion);
    w.putSubMessage(4, events.serialize());
    w.putSubMessage(5, result.serialize());
    w.putSubMessage(6, state.serialize());
    return w.take();
}

QByteArray CanonicalTranscript::serialize() const
{
    Writer w;
    w.putUInt64(1, revision);
    w.putBool(2, final);
    w.putDouble(3, commitBoundarySec);
    w.putString(4, text);
    w.putRepeatedMessage(5, words);
    return w.take();
}

QByteArray ReviewEditResponse::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putSubMessage(2, transcript.serialize());
    w.putSubMessage(3, state.serialize());
    return w.take();
}

QByteArray SessionSummary::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putString(2, title);
    w.putDouble(3, createdAt);
    w.putDouble(4, updatedAt);
    w.putDouble(5, durationSec);
    w.putBool(6, final);
    w.putBool(7, running);
    w.putRepeatedString(8, participants);
    w.putString(9, securityLevel);
    w.putString(10, mode);
    return w.take();
}

QByteArray ListSessionsResponse::serialize() const
{
    Writer w;
    w.putRepeatedMessage(1, sessions);
    w.putString(2, nextCursor);
    return w.take();
}

QByteArray PipelineTraceEvent::serialize() const
{
    Writer w;
    w.putUInt64(1, seq);
    w.putDouble(2, ts);
    w.putString(3, stage);
    w.putString(4, event);
    w.putDouble(5, audioStartSec);
    w.putDouble(6, audioEndSec);
    w.putString(7, payloadJson);
    return w.take();
}

QByteArray PipelineTraceResponse::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putRepeatedMessage(2, events);
    w.putUInt64(3, nextSeq);
    w.putBool(4, hasMore);
    w.putBool(5, enabled);
    w.putBool(6, truncated);
    w.putUInt64(7, maxBytes);
    return w.take();
}

QByteArray AuditEvent::serialize() const
{
    Writer w;
    w.putDouble(1, ts);
    w.putString(2, event);
    w.putString(3, payloadJson);
    return w.take();
}

QByteArray AuditHistoryResponse::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putRepeatedMessage(2, events);
    return w.take();
}

QByteArray ModelStatusEntry::serialize() const
{
    Writer w;
    w.putString(1, name);
    w.putString(2, version);
    w.putString(3, state);
    return w.take();
}

QByteArray ModelStatusResponse::serialize() const
{
    Writer w;
    w.putRepeatedMessage(1, models);
    return w.take();
}

} // namespace asr
