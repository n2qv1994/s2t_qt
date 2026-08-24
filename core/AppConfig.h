// Persisted client configuration.
//
// These are the same knobs run_windows_ui.ps1 passed on the command line
// (server, input device, rate/channels, expected device name, xvf_host path),
// minus everything that only existed because the UI was a browser talking to
// a local HTTP bridge - there is no UI port, upload dir or token file to
// point at any more.
#ifndef APPCONFIG_H
#define APPCONFIG_H

#include "Logger.h"

#include <QByteArray>
#include <QString>

class AppConfig
{
public:
    void load();
    void save() const;

    // Linux gRPC adapter, "host:port".  The deployed pipeline host is .47;
    // see run_windows_ui.ps1's note about .49 being a stale default.
    QString serverTarget = QStringLiteral("192.168.1.47:8700");
    // Bearer token for both ProductASRService and SpeakerRegistryService.
    QString apiToken;

    QByteArray inputDeviceId;
    QString expectedDeviceName = QStringLiteral("Speaker");
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
};

#endif // APPCONFIG_H
