// Headless diagnostics, reachable from the command line.
//
// The protocol stack under grpc/ is hand written, so it needs a way to be
// exercised without a GUI and without the real GPU pipeline:
//
//   s2t_qt --selftest              proto3 + HPACK round trips, no network
//   s2t_qt --probe host:port [--token T]   real RPCs against an adapter
//
// The probe half doubles as a field diagnostic: it answers "is the adapter
// reachable and is this token accepted" without starting a session.
#ifndef SELFTEST_H
#define SELFTEST_H

#include <QString>

namespace selftest {

// Diagnostics normally print to stdout, which is where the command-line modes
// want them.  A GUI has no stdout worth reading, so it points the report at a
// QString instead, runs one of the three below, and displays what came back.
// One capture at a time: pass nullptr to go back to stdout when done.
void captureReportInto(QString *sink);

int runCodecTests();
int runProbe(const QString &target, const QString &token);
// End-to-end assertions against tools/mock_adapter.js: a real, independent
// HTTP/2 peer, so HPACK decoding, flow control on a multi-megabyte response,
// trailers-only error statuses and percent-decoded messages all get exercised
// against something other than our own encoder.
int runNetworkTests(const QString &target, const QString &token);

} // namespace selftest

#endif // SELFTEST_H
