// Persisted client configuration.
//
// These are the same knobs run_windows_ui.ps1 passed on the command line
// (server, input device, rate/channels, expected device name, xvf_host path),
// minus everything that only existed because the UI was a browser talking to
// a local HTTP bridge - there is no UI port, upload dir or token file to
// point at any more.
#ifndef APPCONFIG_H
#define APPCONFIG_H

// The client's released version, reported by --version, by the About box and
// - the reason it has to exist at all - in the header of every run journal.
// Kept in step by hand with S2T_SERVER_VERSION in s2t-qt-server/ServerConfig.h:
// the two halves ship together and a mismatch between them is itself the sort
// of thing a journal is read to find.
#define S2T_CLIENT_VERSION "1.0"

#include "core/Logger.h"

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

class AppConfig
{
public:
    void load();
    void save() const;

    // s2t-qt-server, "host:port".  Not the adapter: since the split this
    // client only ever talks to the Server buffer, which is what holds the
    // audio queue and relays everything else on to the inference tier.  Point
    // it at :8700 by mistake and start_session still works, but push_audio
    // then bypasses the buffer entirely - which is why the buffer's own
    // BufferAdminService/ping is what the connection check uses.
    QString serverTarget = QStringLiteral("192.168.1.47:8800");
    // Bearer token for all three services on that port.  It is the *buffer's*
    // token; the upstream token is the server's business and never leaves it.
    QString apiToken;

    QByteArray inputDeviceId;
    // The name a device must contain before it will be opened.  EMPTY by
    // default, which means "any input", and the settings dialog fills it in
    // from whatever microphone the operator picks.
    //
    // It used to default to "Speaker" - the name of the xvf3800 board this
    // started on - and that is a default that matches almost nothing else.
    // On the RHEL machine the only microphone is "Built-in Audio Analog
    // Stereo", so a fresh install refused to record at all, with a message
    // naming a string the operator had never typed.  A guard is worth having
    // (see AudioCapture.h) but it has to describe the device in front of the
    // person, not the device this project was first written against.
    QString expectedDeviceName;
    int sampleRate = 48000;
    int channels = 1;
    // How much un-ACKed capture may pile up on this machine before the
    // session stops loudly instead of silently deleting audio.
    double bufferSec = 60.0;

    QString micControlApp;

    // Written into every audit record the client produces; deliberately not
    // defaulted to a placeholder, because a blank one must block a save.
    QString operatorId;

    bool pipelineTrace = true;
    // Replay a file at its own clock instead of as fast as the pipeline will
    // take it.  The deployed bridge ran with --source-realtime, so this
    // defaults to matching it.
    bool paceFileReplay = true;

    // Where the debug log goes: Debug -> console, Develop -> file.  Persisted
    // so a deployed workstation keeps writing to its file across restarts
    // without anyone having to remember a command-line switch.  --log-mode and
    // S2T_LOG_MODE still outrank this at startup; see core/Logger.h.
    applog::Mode logMode = applog::Mode::Debug;
    applog::Level logLevel = applog::Level::Debug;

    static QString tokenFromFile(const QString &path, QString *error);

    // Every setting as (label, value) for a person to read - the run journal's
    // header prints it, and the settings dialog diffs two of these to journal
    // what an operator changed.  One list, so the two cannot drift apart.
    //
    // The token is reported as set / not set and never by value: the journal
    // is the file a tester is told to send to somebody else.
    QList<QPair<QString, QString>> describe() const;
};

#endif // APPCONFIG_H
