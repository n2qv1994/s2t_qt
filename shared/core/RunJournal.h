// The one file to ask a tester for.
//
// The debug log (Logger.h) is a firehose: eight thousand lines for a twenty
// minute meeting, most of them one HTTP/2 frame each.  It is the right thing
// to have when you already know what you are looking for, and the wrong thing
// to hand somebody who has to work out what happened from scratch.
//
// This is the other half: one line per thing that actually happened, in order,
// with the environment it happened in written at the top.  A run of the
// program produces exactly one of these files, named after the moment it
// started, so "send me the newest file in that folder" is an unambiguous
// instruction to give somebody testing on a machine you cannot reach.
//
// Three rules it exists to keep:
//
//   1. It is ALWAYS on.  A log that has to be switched on is a log nobody has
//      when it turns out to be needed - and the person who would have to
//      switch it on is usually not the person who needs it.
//   2. It is written for reading, not for grepping a symptom you already
//      suspect: the environment header first, then the steps, then a summary
//      that says how the run ended.
//   3. A file with no summary at the end means the process did not get to say
//      goodbye - it was killed, or it crashed.  That absence is information,
//      so nothing else is allowed to write the closing section.
//
// Every line also goes to the ordinary log at Info level, so the two can be
// read side by side when the detail matters.
#ifndef RUNJOURNAL_H
#define RUNJOURNAL_H

#include <QString>
#include <QStringList>

namespace runjournal {

// Opens the journal for this run and writes the header.  `program` is what the
// file will call itself - "s2t-qt-client" or "s2t-qt-server".  Safe to call
// once, from main(), after applog::initFromArguments().
//
// `instance` tells apart several copies of one program sharing a log folder -
// the server passes its listen port.  It goes into the file name, and the
// kept-files limit counts per instance: before it, the acceptance harness's
// server on :8801 and restart_check's on :18877 wrote into the same folder as
// the production server on :8800, and one test run pushed every production
// journal past the limit - the very file an operator is asked to send.
void start(const QString &program, const QString &version, const QString &instance = QString());

// A titled block in the header.  Used for the parts of the environment that
// only one half knows about: audio devices on the client, the inference tier
// and the stores on the server.
void section(const QString &title);
void field(const QString &name, const QString &value);

// One thing that happened.  `event` is a short stable tag - `session.start`,
// `mic.lost`, `stop.retry` - and `detail` is free text for a person.  The tag
// is what makes a run diffable against another run; the text is what makes it
// readable without the source in front of you.
void step(const QString &event, const QString &detail);

// The closing section.  Written once, by whoever is shutting the program down;
// a second call is ignored, because the first one is the truth and the rest is
// teardown noise.
void finish(const QString &reason);

// Where the file is.  Empty when the journal could not be opened at all -
// which is itself logged, once, to the ordinary log.
QString path();

// The directory the journals live in, whether or not one is open.
QString directory();

// Everything this run has recorded, for the diagnostics window to show
// without the operator having to find the file first.
QStringList recent();

} // namespace runjournal

// The call sites.  Deliberately a macro rather than a function so that adding
// a step reads like adding a log line, which is what it is.
#define LOG_STEP(event, detail) runjournal::step(QStringLiteral(event), detail)

#endif // RUNJOURNAL_H
