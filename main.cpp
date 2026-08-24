#include "mainwindow.h"

#include "audio/AudioCapture.h"
#include "audio/MicDenoise.h"
#include "core/Logger.h"
#include "core/SelfTest.h"
#include "core/SessionTypes.h"
#include "proto/AsrSession.h"

#include <QApplication>
#include <QMetaType>
#include <QStringList>
#include <QTextStream>

// The GUI binary is linked for the windows subsystem, so it has no console of
// its own and stdout goes nowhere.  applog::ensureConsole() borrows the
// launching shell's one, which is what makes the headless modes below usable
// without turning every normal start into a window with a black box behind
// it; the logger needs exactly the same thing before it can write a line in
// debug mode, which is why it owns that code rather than main.cpp.

#ifdef Q_OS_UNIX
// A RHEL box is normally reached over plain ssh, with no X11 forwarding.
// Constructing QApplication there aborts with a Qt platform-plugin message
// that says nothing about what to do next, so refuse first and say it
// plainly.  The headless modes below have already returned by the time this
// runs, so it only ever rejects the GUI.
static bool displayAvailable()
{
    // An explicit QT_QPA_PLATFORM (offscreen, vnc, eglfs, wayland) is a
    // deliberate choice; never second-guess it.
    if (!qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        return true;
    return !qEnvironmentVariableIsEmpty("DISPLAY")
        || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
}
#endif

int main(int argc, char *argv[])
{
    // Headless modes run before any GUI object exists, so they work over a
    // remote shell and in CI.  QCoreApplication is enough for both.
    QStringList args;
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    // Set before anything else: QStandardPaths derives the app-data directory
    // the log file lives in from these two names, and they are static, so
    // they do not need an application object to exist yet.
    QCoreApplication::setApplicationName(QStringLiteral("s2t_qt"));
    QCoreApplication::setOrganizationName(QStringLiteral("s2t"));
    // First thing after that, so a failure in any of the startup paths below
    // is already on the record.
    applog::initFromArguments(args);
    LOG_INFO(applog::cat::App) << "s2t_qt starting - Qt" << QT_VERSION_STR
                               << "- arguments:" << (args.isEmpty() ? QStringLiteral("(none)")
                                                                    : args.join(QLatin1Char(' ')));

    if (args.contains(QStringLiteral("--selftest"))) {
        applog::ensureConsole();
        QCoreApplication headless(argc, argv);
        LOG_INFO(applog::cat::App) << "headless mode: --selftest";
        const int code = selftest::runCodecTests();
        LOG_INFO(applog::cat::App) << "--selftest finished with code" << code;
        applog::shutdown();
        return code;
    }
    const int netIndex = args.indexOf(QStringLiteral("--selftest-net"));
    if (netIndex >= 0 && netIndex + 1 < args.size()) {
        applog::ensureConsole();
        QCoreApplication headless(argc, argv);
        const int tokenIndex = args.indexOf(QStringLiteral("--token"));
        const QString token = (tokenIndex >= 0 && tokenIndex + 1 < args.size())
            ? args.at(tokenIndex + 1)
            : QStringLiteral("test-token");
        LOG_INFO(applog::cat::App) << "headless mode: --selftest-net" << args.at(netIndex + 1);
        const int code = selftest::runNetworkTests(args.at(netIndex + 1), token);
        LOG_INFO(applog::cat::App) << "--selftest-net finished with code" << code;
        applog::shutdown();
        return code;
    }
    const int probeIndex = args.indexOf(QStringLiteral("--probe"));
    if (probeIndex >= 0 && probeIndex + 1 < args.size()) {
        applog::ensureConsole();
        QCoreApplication headless(argc, argv);
        const int tokenIndex = args.indexOf(QStringLiteral("--token"));
        const QString token =
            (tokenIndex >= 0 && tokenIndex + 1 < args.size()) ? args.at(tokenIndex + 1) : QString();
        LOG_INFO(applog::cat::App) << "headless mode: --probe" << args.at(probeIndex + 1);
        const int code = selftest::runProbe(args.at(probeIndex + 1), token);
        LOG_INFO(applog::cat::App) << "--probe finished with code" << code;
        applog::shutdown();
        return code;
    }

#ifdef Q_OS_UNIX
    if (!displayAvailable()) {
        QTextStream(stderr)
            << "s2t_qt cần một màn hình đồ hoạ.\n"
            << "Phiên ssh này không có DISPLAY/WAYLAND_DISPLAY.\n"
            << "  - chạy lại bằng: ssh -X (hoặc -Y) rồi mở s2t_qt\n"
            << "  - hoặc chạy trên console/VNC của máy\n"
            << "  - chế độ không cần màn hình: --selftest, --selftest-net <host:port>, "
               "--probe <host:port> --token <token>\n";
        LOG_ERROR(applog::cat::App) << "no DISPLAY/WAYLAND_DISPLAY - refusing to open the GUI";
        applog::shutdown();
        return 2;
    }
#endif

    QApplication app(argc, argv);

    // Everything that crosses a thread boundary through a queued signal or a
    // queued invokeMethod has to be a registered metatype, or Qt drops the
    // call at runtime with a warning rather than failing to build.
    qRegisterMetaType<AudioDeviceChoice>("AudioDeviceChoice");
    qRegisterMetaType<MicStatus>("MicStatus");
    qRegisterMetaType<SessionTelemetry>("SessionTelemetry");
    qRegisterMetaType<LatencySample>("LatencySample");
    qRegisterMetaType<SessionMeta>("SessionMeta");
    qRegisterMetaType<FinishedSession>("FinishedSession");
    qRegisterMetaType<asr::StateResponse>("asr::StateResponse");
    qRegisterMetaType<mic::DenoiseAbResult>("mic::DenoiseAbResult");
    LOG_DEBUG(applog::cat::App) << "metatypes registered for cross-thread signals";

    // Scoped so the window - and with it the controller, which spends seconds
    // stopping the worker, the poller and three RPC lanes - is fully torn down
    // before the logger is.  Otherwise shutdown() closes the sink and every
    // teardown line after it has to reopen the file it just closed, under a
    // "logger stopping" marker that is not yet true.
    int code = 0;
    {
        MainWindow window;
        window.show();
        LOG_INFO(applog::cat::App) << "main window shown - entering the event loop";
        code = QApplication::exec();
        LOG_INFO(applog::cat::App) << "event loop finished with code" << code;
    }
    applog::shutdown();
    return code;
}
