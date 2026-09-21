// Every gRPC method path in one place.
//
// It used to be enough for these to live in AsrClient.cpp, because only one
// side of the wire existed here.  Now s2t-qt-server registers handlers under
// the same strings that s2t-qt-client calls, and a typo in either would show
// up as UNIMPLEMENTED at run time on a deployed host rather than as a build
// error.  Sharing the constants makes that mistake impossible to make.
//
// The paths come straight from the .proto files.  ProductASRService uses
// snake_case rpc names and SpeakerRegistryService uses PascalCase - that
// asymmetry is in the contract, not a mistake here.
#ifndef GRPCMETHODS_H
#define GRPCMETHODS_H

namespace rpcpath {

// ---- asr.ui.v1.ProductASRService ------------------------------------------
constexpr const char *StartSession = "/asr.ui.v1.ProductASRService/start_session";
constexpr const char *PushAudio = "/asr.ui.v1.ProductASRService/push_audio";
constexpr const char *GetLiveState = "/asr.ui.v1.ProductASRService/get_live_state";
constexpr const char *GetReviewState = "/asr.ui.v1.ProductASRService/get_review_state";
constexpr const char *GetAudioRange = "/asr.ui.v1.ProductASRService/get_audio_range";
constexpr const char *ApplyTextEdit = "/asr.ui.v1.ProductASRService/apply_text_edit";
constexpr const char *StopSession = "/asr.ui.v1.ProductASRService/stop_session";
constexpr const char *ListSessions = "/asr.ui.v1.ProductASRService/list_sessions";
constexpr const char *RenameSpeaker = "/asr.ui.v1.ProductASRService/rename_speaker";
constexpr const char *GetPipelineTrace = "/asr.ui.v1.ProductASRService/get_pipeline_trace";
constexpr const char *GetAuditHistory = "/asr.ui.v1.ProductASRService/get_audit_history";
constexpr const char *GetModelStatus = "/asr.ui.v1.ProductASRService/get_model_status";

// ---- asr.ui.v1.SpeakerRegistryService -------------------------------------
constexpr const char *GetEnrollmentScript =
    "/asr.ui.v1.SpeakerRegistryService/GetEnrollmentScript";
constexpr const char *EnrollSpeaker = "/asr.ui.v1.SpeakerRegistryService/EnrollSpeaker";
constexpr const char *ListSessionSpeakers =
    "/asr.ui.v1.SpeakerRegistryService/ListSessionSpeakers";
constexpr const char *SaveSessionSpeakers =
    "/asr.ui.v1.SpeakerRegistryService/SaveSessionSpeakers";
constexpr const char *GetSpeakerRegistryStatus =
    "/asr.ui.v1.SpeakerRegistryService/GetSpeakerRegistryStatus";
// The global registry's own lifecycle.  These five were in the .proto and in
// the reference adapter from the start and missing here until 2026-09-21,
// which left no way at all to see what is in the shared speaker database, let
// alone take anything back out of it.
constexpr const char *PreviewEnrollment = "/asr.ui.v1.SpeakerRegistryService/PreviewEnrollment";
constexpr const char *ListGlobalSpeakers = "/asr.ui.v1.SpeakerRegistryService/ListGlobalSpeakers";
constexpr const char *DeactivateGlobalSpeaker =
    "/asr.ui.v1.SpeakerRegistryService/DeactivateGlobalSpeaker";
constexpr const char *ActivateGlobalSpeaker =
    "/asr.ui.v1.SpeakerRegistryService/ActivateGlobalSpeaker";
constexpr const char *DeleteGlobalSpeaker =
    "/asr.ui.v1.SpeakerRegistryService/DeleteGlobalSpeaker";

// ---- s2t.buffer.v1.BufferAdminService -------------------------------------
// The Server buffer's own surface.  The adapter does not implement these, and
// is never asked to: they describe this process, not the inference tier.
constexpr const char *BufferPing = "/s2t.buffer.v1.BufferAdminService/ping";
constexpr const char *GetBufferStatus = "/s2t.buffer.v1.BufferAdminService/get_buffer_status";
constexpr const char *ListBufferedSessions =
    "/s2t.buffer.v1.BufferAdminService/list_buffered_sessions";

// ---- nvidia.riva.asr.RivaSpeechRecognition --------------------------------
// The inference tier, when s2t-qt-server is configured for Riva.  Only the
// server ever calls these; a client never learns the backend even exists.
constexpr const char *RivaRecognize = "/nvidia.riva.asr.RivaSpeechRecognition/Recognize";
constexpr const char *RivaStreamingRecognize =
    "/nvidia.riva.asr.RivaSpeechRecognition/StreamingRecognize";
constexpr const char *RivaGetConfig =
    "/nvidia.riva.asr.RivaSpeechRecognition/GetRivaSpeechRecognitionConfig";

// ---- inference.GRPCInferenceService ---------------------------------------
// The same tier when configured for Triton: the KServe v2 predict protocol,
// spoken straight to the model repository rather than through a Python adapter.
constexpr const char *TritonServerLive = "/inference.GRPCInferenceService/ServerLive";
constexpr const char *TritonServerReady = "/inference.GRPCInferenceService/ServerReady";
constexpr const char *TritonModelReady = "/inference.GRPCInferenceService/ModelReady";
constexpr const char *TritonRepositoryIndex = "/inference.GRPCInferenceService/RepositoryIndex";
constexpr const char *TritonModelInfer = "/inference.GRPCInferenceService/ModelInfer";

} // namespace rpcpath

#endif // GRPCMETHODS_H
