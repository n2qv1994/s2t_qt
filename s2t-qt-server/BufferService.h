// The gRPC surface s2t-qt-client talks to.
//
// Three services on one port, which is what lets the client keep one typed
// façade and one token:
//
//   asr.ui.v1.ProductASRService      the twelve RPCs the pipeline defines
//   asr.ui.v1.SpeakerRegistryService the five enrolment RPCs
//   s2t.buffer.v1.BufferAdminService this server's own three
//
// Four of the first twelve are answered by the buffer itself - start_session,
// push_audio, get_live_state and stop_session, the ones that touch the audio
// path or its state.  Everything else is a straight relay: the Server buffer
// has no opinion about a review query, and inventing one would mean two places
// that could disagree about what a transcript says.
#ifndef BUFFERSERVICE_H
#define BUFFERSERVICE_H

#include "BufferHub.h"
#include "grpc/GrpcServer.h"

class BufferService
{
public:
    BufferService(BufferHub *hub, grpc::Server *server);

    // Registers every method.  Called once, before the server starts
    // listening, because the method table is deliberately not locked.
    void registerMethods();

private:
    BufferHub *m_hub = nullptr;
    grpc::Server *m_server = nullptr;
};

#endif // BUFFERSERVICE_H
