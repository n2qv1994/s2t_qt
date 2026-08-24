#include "AsrClient.h"

#include "Methods.h"


AsrClient::AsrClient(const QString &target, const QString &token) : m_channel(target, token) {}

void AsrClient::setTarget(const QString &target, const QString &token)
{
    m_channel.setTarget(target, token);
}

grpc::Status AsrClient::startSession(const asr::StartSessionRequest &req,
                                     asr::StartSessionResponse *out, int timeoutMs)
{
    return call(rpcpath::StartSession, req, out, timeoutMs);
}

grpc::Status AsrClient::pushAudio(const asr::PushAudioRequest &req, asr::PushAudioResponse *out,
                                  int timeoutMs)
{
    return call(rpcpath::PushAudio, req, out, timeoutMs);
}

grpc::Status AsrClient::getLiveState(const asr::SessionRequest &req, asr::StateResponse *out,
                                     int timeoutMs)
{
    return call(rpcpath::GetLiveState, req, out, timeoutMs);
}

grpc::Status AsrClient::getReviewState(const asr::ReviewRequest &req, asr::StateResponse *out,
                                       int timeoutMs)
{
    return call(rpcpath::GetReviewState, req, out, timeoutMs);
}

grpc::Status AsrClient::getAudioRange(const asr::AudioRangeRequest &req,
                                      asr::AudioRangeResponse *out, int timeoutMs)
{
    return call(rpcpath::GetAudioRange, req, out, timeoutMs);
}

grpc::Status AsrClient::applyTextEdit(const asr::TextEditRequest &req, asr::ReviewEditResponse *out,
                                      int timeoutMs)
{
    return call(rpcpath::ApplyTextEdit, req, out, timeoutMs);
}

grpc::Status AsrClient::stopSession(const asr::SessionRequest &req, asr::StopSessionResponse *out,
                                    int timeoutMs)
{
    return call(rpcpath::StopSession, req, out, timeoutMs);
}

grpc::Status AsrClient::listSessions(const asr::ListSessionsRequest &req,
                                     asr::ListSessionsResponse *out, int timeoutMs)
{
    return call(rpcpath::ListSessions, req, out, timeoutMs);
}

grpc::Status AsrClient::renameSpeaker(const asr::RenameSpeakerRequest &req,
                                      asr::ReviewEditResponse *out, int timeoutMs)
{
    return call(rpcpath::RenameSpeaker, req, out, timeoutMs);
}

grpc::Status AsrClient::getPipelineTrace(const asr::PipelineTraceRequest &req,
                                         asr::PipelineTraceResponse *out, int timeoutMs)
{
    return call(rpcpath::GetPipelineTrace, req, out, timeoutMs);
}

grpc::Status AsrClient::getAuditHistory(const asr::AuditHistoryRequest &req,
                                        asr::AuditHistoryResponse *out, int timeoutMs)
{
    return call(rpcpath::GetAuditHistory, req, out, timeoutMs);
}

grpc::Status AsrClient::getModelStatus(asr::ModelStatusResponse *out, int timeoutMs)
{
    return call(rpcpath::GetModelStatus, asr::ModelStatusRequest(), out, timeoutMs);
}

grpc::Status AsrClient::getEnrollmentScript(reg::GetEnrollmentScriptResponse *out, int timeoutMs)
{
    return call(rpcpath::GetEnrollmentScript, reg::GetEnrollmentScriptRequest(), out, timeoutMs);
}

grpc::Status AsrClient::enrollSpeaker(const reg::EnrollSpeakerRequest &req,
                                      reg::EnrollSpeakerResponse *out, int timeoutMs)
{
    return call(rpcpath::EnrollSpeaker, req, out, timeoutMs);
}

grpc::Status AsrClient::listSessionSpeakers(const reg::ListSessionSpeakersRequest &req,
                                            reg::ListSessionSpeakersResponse *out, int timeoutMs)
{
    return call(rpcpath::ListSessionSpeakers, req, out, timeoutMs);
}

grpc::Status AsrClient::saveSessionSpeakers(const reg::SaveSessionSpeakersRequest &req,
                                            reg::SaveSessionSpeakersResponse *out, int timeoutMs)
{
    return call(rpcpath::SaveSessionSpeakers, req, out, timeoutMs);
}

grpc::Status AsrClient::getSpeakerRegistryStatus(const reg::GetSpeakerRegistryStatusRequest &req,
                                                 reg::GetSpeakerRegistryStatusResponse *out,
                                                 int timeoutMs)
{
    return call(rpcpath::GetSpeakerRegistryStatus, req, out, timeoutMs);
}

grpc::Status AsrClient::bufferPing(const buf::PingRequest &req, buf::PingResponse *out,
                                   int timeoutMs)
{
    return call(rpcpath::BufferPing, req, out, timeoutMs);
}

grpc::Status AsrClient::getBufferStatus(const buf::BufferStatusRequest &req,
                                        buf::BufferStatusResponse *out, int timeoutMs)
{
    return call(rpcpath::GetBufferStatus, req, out, timeoutMs);
}

grpc::Status AsrClient::listBufferedSessions(const buf::BufferSessionsRequest &req,
                                             buf::BufferSessionsResponse *out, int timeoutMs)
{
    return call(rpcpath::ListBufferedSessions, req, out, timeoutMs);
}
