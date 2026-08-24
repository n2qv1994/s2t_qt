#include "AsrClient.h"

namespace {
// Method paths come straight from the two .proto files.  ProductASRService
// uses snake_case rpc names and SpeakerRegistryService uses PascalCase - that
// asymmetry is in the contract, not a mistake here.
const char kStartSession[] = "/asr.ui.v1.ProductASRService/start_session";
const char kPushAudio[] = "/asr.ui.v1.ProductASRService/push_audio";
const char kGetLiveState[] = "/asr.ui.v1.ProductASRService/get_live_state";
const char kGetReviewState[] = "/asr.ui.v1.ProductASRService/get_review_state";
const char kGetAudioRange[] = "/asr.ui.v1.ProductASRService/get_audio_range";
const char kApplyTextEdit[] = "/asr.ui.v1.ProductASRService/apply_text_edit";
const char kStopSession[] = "/asr.ui.v1.ProductASRService/stop_session";
const char kListSessions[] = "/asr.ui.v1.ProductASRService/list_sessions";
const char kRenameSpeaker[] = "/asr.ui.v1.ProductASRService/rename_speaker";
const char kGetPipelineTrace[] = "/asr.ui.v1.ProductASRService/get_pipeline_trace";
const char kGetAuditHistory[] = "/asr.ui.v1.ProductASRService/get_audit_history";
const char kGetModelStatus[] = "/asr.ui.v1.ProductASRService/get_model_status";

const char kGetEnrollmentScript[] = "/asr.ui.v1.SpeakerRegistryService/GetEnrollmentScript";
const char kEnrollSpeaker[] = "/asr.ui.v1.SpeakerRegistryService/EnrollSpeaker";
const char kListSessionSpeakers[] = "/asr.ui.v1.SpeakerRegistryService/ListSessionSpeakers";
const char kSaveSessionSpeakers[] = "/asr.ui.v1.SpeakerRegistryService/SaveSessionSpeakers";
const char kGetSpeakerRegistryStatus[] =
    "/asr.ui.v1.SpeakerRegistryService/GetSpeakerRegistryStatus";
} // namespace

AsrClient::AsrClient(const QString &target, const QString &token) : m_channel(target, token) {}

void AsrClient::setTarget(const QString &target, const QString &token)
{
    m_channel.setTarget(target, token);
}

grpc::Status AsrClient::startSession(const asr::StartSessionRequest &req,
                                     asr::StartSessionResponse *out, int timeoutMs)
{
    return call(kStartSession, req, out, timeoutMs);
}

grpc::Status AsrClient::pushAudio(const asr::PushAudioRequest &req, asr::PushAudioResponse *out,
                                  int timeoutMs)
{
    return call(kPushAudio, req, out, timeoutMs);
}

grpc::Status AsrClient::getLiveState(const asr::SessionRequest &req, asr::StateResponse *out,
                                     int timeoutMs)
{
    return call(kGetLiveState, req, out, timeoutMs);
}

grpc::Status AsrClient::getReviewState(const asr::ReviewRequest &req, asr::StateResponse *out,
                                       int timeoutMs)
{
    return call(kGetReviewState, req, out, timeoutMs);
}

grpc::Status AsrClient::getAudioRange(const asr::AudioRangeRequest &req,
                                      asr::AudioRangeResponse *out, int timeoutMs)
{
    return call(kGetAudioRange, req, out, timeoutMs);
}

grpc::Status AsrClient::applyTextEdit(const asr::TextEditRequest &req, asr::ReviewEditResponse *out,
                                      int timeoutMs)
{
    return call(kApplyTextEdit, req, out, timeoutMs);
}

grpc::Status AsrClient::stopSession(const asr::SessionRequest &req, asr::StopSessionResponse *out,
                                    int timeoutMs)
{
    return call(kStopSession, req, out, timeoutMs);
}

grpc::Status AsrClient::listSessions(const asr::ListSessionsRequest &req,
                                     asr::ListSessionsResponse *out, int timeoutMs)
{
    return call(kListSessions, req, out, timeoutMs);
}

grpc::Status AsrClient::renameSpeaker(const asr::RenameSpeakerRequest &req,
                                      asr::ReviewEditResponse *out, int timeoutMs)
{
    return call(kRenameSpeaker, req, out, timeoutMs);
}

grpc::Status AsrClient::getPipelineTrace(const asr::PipelineTraceRequest &req,
                                         asr::PipelineTraceResponse *out, int timeoutMs)
{
    return call(kGetPipelineTrace, req, out, timeoutMs);
}

grpc::Status AsrClient::getAuditHistory(const asr::AuditHistoryRequest &req,
                                        asr::AuditHistoryResponse *out, int timeoutMs)
{
    return call(kGetAuditHistory, req, out, timeoutMs);
}

grpc::Status AsrClient::getModelStatus(asr::ModelStatusResponse *out, int timeoutMs)
{
    return call(kGetModelStatus, asr::ModelStatusRequest(), out, timeoutMs);
}

grpc::Status AsrClient::getEnrollmentScript(reg::GetEnrollmentScriptResponse *out, int timeoutMs)
{
    return call(kGetEnrollmentScript, reg::GetEnrollmentScriptRequest(), out, timeoutMs);
}

grpc::Status AsrClient::enrollSpeaker(const reg::EnrollSpeakerRequest &req,
                                      reg::EnrollSpeakerResponse *out, int timeoutMs)
{
    return call(kEnrollSpeaker, req, out, timeoutMs);
}

grpc::Status AsrClient::listSessionSpeakers(const reg::ListSessionSpeakersRequest &req,
                                            reg::ListSessionSpeakersResponse *out, int timeoutMs)
{
    return call(kListSessionSpeakers, req, out, timeoutMs);
}

grpc::Status AsrClient::saveSessionSpeakers(const reg::SaveSessionSpeakersRequest &req,
                                            reg::SaveSessionSpeakersResponse *out, int timeoutMs)
{
    return call(kSaveSessionSpeakers, req, out, timeoutMs);
}

grpc::Status AsrClient::getSpeakerRegistryStatus(const reg::GetSpeakerRegistryStatusRequest &req,
                                                 reg::GetSpeakerRegistryStatusResponse *out,
                                                 int timeoutMs)
{
    return call(kGetSpeakerRegistryStatus, req, out, timeoutMs);
}
