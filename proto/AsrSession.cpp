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

} // namespace asr
