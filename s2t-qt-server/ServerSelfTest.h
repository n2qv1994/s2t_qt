// Headless diagnostics for s2t-qt-server, reachable from the command line.
//
//   s2t-qt-server --selftest              codec + gRPC framing + a real
//                                         loopback server driven by the real
//                                         client stack, no network needed
//   s2t-qt-server --probe host:port [--token T]   is the inference tier up
//
// The loopback test is the one that earns its keep.  Http2Server, GrpcServer
// and the reverse half of the proto codec are all new code with no library
// standing behind them, and the only honest way to check them is to make the
// existing Http2Client talk to them over a real socket.
#ifndef SERVERSELFTEST_H
#define SERVERSELFTEST_H

#include <QString>

namespace serverselftest {

// Proto3 round trips in both directions, plus gRPC message framing and
// grpc-message percent encoding.  No sockets.
int runCodecTests();

// Starts a real gRPC server on a loopback port and drives it with grpc::Channel.
int runLoopbackTests();

// The whole chain in one process: a stand-in inference tier, the real
// BufferHub and BufferService in front of it, and a real client driving them.
// Checks the things only the buffer can get wrong - packet order, the drain
// barrier before stop_session, idempotent replay, and the state cache fanning
// one upstream poll out to many readers.
int runBufferTests();

// All three.
int runAll();

int runProbe(const QString &target, const QString &token);

} // namespace serverselftest

#endif // SERVERSELFTEST_H
