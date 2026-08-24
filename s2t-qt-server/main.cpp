#include "BufferHub.h"
#include "BufferService.h"
#include "ServerConfig.h"
#include "ServerSelfTest.h"
#include "core/Logger.h"
#include "grpc/GrpcServer.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QTextStream>
#include <QTimer>

#include <csignal>

namespace {

// Set from a signal handler, read from a timer on the main thread.
//
// Almost nothing is legal inside a signal handler - not QCoreApplication::quit,
// not logging, not allocating - so the handler does the one thing that is
// (write a flag) and a 200 ms timer does the rest.  It costs one wakeup every
// fifth of a second and removes a whole class of shutdown crash.
volatile std::sig_atomic_t g_signalled = 0;

extern "C" void onSignal(int)
{
    g_signalled = 1;
}

void usage()
{
    QTextStream stream(stdout);
    stream
        << "s2t-qt-server " S2T_SERVER_VERSION " - Server buffer cho hệ thống ASR + Diarization\n"
        << "\n"
        << "Nhận audio từ s2t-qt-client qua gRPC, đệm lại, rồi đẩy lên tầng suy luận\n"
        << "(adapter -> Triton).  Không có giao diện; chạy như một dịch vụ.\n"
        << "\n"
        << "Cách dùng:\n"
        << "  s2t-qt-server [--config TỆP] [tuỳ chọn...]\n"
        << "\n"
        << "Tuỳ chọn:\n"
        << "  --config TỆP            tệp cấu hình INI (mặc định: "
        << ServerConfig::defaultPath() << ")\n"
        << "  --listen ĐỊA_CHỈ:CỔNG   nơi lắng nghe client (mặc định 0.0.0.0:8800)\n"
        << "  --token TOKEN           token mà client phải gửi kèm\n"
        << "  --upstream HOST:CỔNG    địa chỉ tầng suy luận (mặc định 192.168.1.47:8700)\n"
        << "  --upstream-token TOKEN  token gửi lên tầng suy luận\n"
        << "  --upstream-lanes N      số kênh dùng lại cho các RPC chuyển tiếp\n"
        << "  --buffer-seconds N      dung lượng đệm mỗi phiên, tính bằng giây audio\n"
        << "  --spool-dir THƯ_MỤC     lưu bản sao audio đã nhận vào đây\n"
        << "  --state-poll-ms N       tuổi tối đa của bộ nhớ đệm get_live_state\n"
        << "  --max-connections N     số kết nối tối đa\n"
        << "  --log-mode debug|develop, --log-level trace|debug|info|warn|error\n"
        << "\n"
        << "Chế độ không cần mạng:\n"
        << "  --selftest              kiểm tra bộ mã proto3, đóng khung gRPC và một\n"
        << "                          máy chủ loopback thật do chính client stack gọi\n"
        << "  --selftest-codec        chỉ phần bộ mã, không mở socket\n"
        << "  --probe HOST:CỔNG [--token T]   thử với tầng suy luận\n"
        << "  --show-config           in cấu hình đã nạp rồi thoát\n"
        << "  --write-config TỆP      ghi ra một tệp cấu hình mẫu rồi thoát\n"
        << "  --version, --help\n";
    stream.flush();
}

} // namespace

int main(int argc, char *argv[])
{
    QStringList args;
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    QCoreApplication::setApplicationName(QStringLiteral("s2t-qt-server"));
    QCoreApplication::setOrganizationName(QStringLiteral("s2t"));
    QCoreApplication::setApplicationVersion(QStringLiteral(S2T_SERVER_VERSION));

    if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
        usage();
        return 0;
    }
    if (args.contains(QStringLiteral("--version"))) {
        QTextStream(stdout) << "s2t-qt-server " S2T_SERVER_VERSION " (Qt " QT_VERSION_STR ")\n";
        return 0;
    }

    applog::initFromArguments(args);
    applog::ensureConsole();
    QCoreApplication app(argc, argv);

    LOG_INFO(applog::cat::App) << "s2t-qt-server" << S2T_SERVER_VERSION << "starting - Qt"
                               << QT_VERSION_STR << "- arguments:"
                               << (args.isEmpty() ? QStringLiteral("(none)")
                                                  : args.join(QLatin1Char(' ')));

    // ---- headless modes, before any socket is opened -----------------------
    if (args.contains(QStringLiteral("--selftest-codec"))) {
        const int code = serverselftest::runCodecTests();
        applog::shutdown();
        return code;
    }
    if (args.contains(QStringLiteral("--selftest"))) {
        const int code = serverselftest::runAll();
        applog::shutdown();
        return code;
    }
    const int probeIndex = args.indexOf(QStringLiteral("--probe"));
    if (probeIndex >= 0 && probeIndex + 1 < args.size()) {
        const int tokenIndex = args.indexOf(QStringLiteral("--token"));
        const QString token =
            (tokenIndex >= 0 && tokenIndex + 1 < args.size()) ? args.at(tokenIndex + 1) : QString();
        const int code = serverselftest::runProbe(args.at(probeIndex + 1), token);
        applog::shutdown();
        return code;
    }

    ServerConfig config;
    QString error;
    if (!config.load(args, &error)) {
        QTextStream(stderr) << "Lỗi cấu hình: " << error << "\n";
        LOG_ERROR(applog::cat::Config) << "configuration rejected:" << error;
        applog::shutdown();
        return 2;
    }
    applog::applyStoredPreference(config.logMode, config.logLevel);

    const int writeIndex = args.indexOf(QStringLiteral("--write-config"));
    if (writeIndex >= 0 && writeIndex + 1 < args.size()) {
        QString writeError;
        config.save(args.at(writeIndex + 1), &writeError);
        QTextStream stream(writeError.isEmpty() ? stdout : stderr);
        stream << (writeError.isEmpty()
                       ? QStringLiteral("Đã ghi %1\n").arg(args.at(writeIndex + 1))
                       : QStringLiteral("Lỗi: %1\n").arg(writeError));
        stream.flush();
        applog::shutdown();
        return writeError.isEmpty() ? 0 : 3;
    }

    {
        QTextStream stream(stdout);
        for (const QString &line : config.describe()) {
            stream << "  " << line << "\n";
            LOG_INFO(applog::cat::Config) << line;
        }
        stream.flush();
    }
    if (args.contains(QStringLiteral("--show-config"))) {
        applog::shutdown();
        return 0;
    }

    QHostAddress address;
    if (!address.setAddress(config.listenAddress)) {
        if (config.listenAddress == QLatin1String("*")
            || config.listenAddress.compare(QLatin1String("any"), Qt::CaseInsensitive) == 0) {
            address = QHostAddress::Any;
        } else {
            QTextStream(stderr) << "Địa chỉ lắng nghe không hợp lệ: " << config.listenAddress
                                << "\n";
            applog::shutdown();
            return 2;
        }
    }

    int code = 0;
    {
        BufferHub hub(config);
        grpc::Server server;
        server.setToken(config.listenToken);
        server.setMaxConnections(config.maxConnections);
        server.setIdleTimeoutMs(config.idleTimeoutMs);

        BufferService service(&hub, &server);
        service.registerMethods();

        if (!server.start(address, config.listenPort, &error)) {
            QTextStream(stderr) << "Không mở được cổng " << config.listenPort << ": " << error
                                << "\n";
            LOG_ERROR(applog::cat::App) << "cannot listen on port" << config.listenPort << ":"
                                        << error;
            // Deliberately not an early return.  `hub` already owns a probe
            // thread and a relay pool, and both log while they stop; returning
            // from here would run applog::shutdown() first and file seconds of
            // teardown *after* the line that says the logger stopped.  Fall
            // through instead and let the scope end in the normal order.
            code = 2;
        } else {
            QTextStream(stdout) << "s2t-qt-server sẵn sàng trên " << config.listenAddress << ":"
                                << server.port() << "\n";
            QTextStream(stdout).flush();
            LOG_INFO(applog::cat::App) << "ready on" << config.listenAddress << ":"
                                       << server.port();

            std::signal(SIGINT, onSignal);
            std::signal(SIGTERM, onSignal);

            QTimer signalPoll;
            QObject::connect(&signalPoll, &QTimer::timeout, &app, [&app]() {
                if (g_signalled) {
                    LOG_INFO(applog::cat::App) << "signal received - shutting down";
                    QTextStream(stdout) << "\nĐang tắt...\n";
                    QTextStream(stdout).flush();
                    app.quit();
                }
            });
            signalPoll.start(200);

            code = QCoreApplication::exec();
            LOG_INFO(applog::cat::App) << "event loop finished with code" << code;
        }

        // Order matters, on both paths.  Stopping the forwarders first wakes
        // every handler that is blocked waiting for a drain, so the connection
        // threads can finish their replies instead of being cut off
        // mid-response.
        hub.shutdown();
        server.stop();
    }
    applog::shutdown();
    return code;
}
