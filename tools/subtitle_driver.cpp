// Throwaway driver for the subtitle window.
//
// A widget is not verified by a build.  This links against every object of a
// normal client build except main.o, so it gets the real SessionController, the
// real transport and the real SubtitleWindow, then drives them the only way a
// headless check can: by calling the public entry point the file dialog would
// have called, waiting, and grabbing the pixels.
//
// Not part of the .pro on purpose - it is a diagnostic, not a product.  Build
// it next to an existing build tree:
//
//   g++ -fPIC -std=c++17 tools/subtitle_driver.cpp \
//       $(ls build-client/*.o | grep -v '/main\.o$') \
//       -I s2t-qt-client -I shared -I build-client \
//       $(pkg-config --cflags --libs Qt6Widgets Qt6MultimediaWidgets Qt6Network) \
//       -o /tmp/subtitle_driver
//
// Run it on the real X11 session (DISPLAY=:1), not offscreen: the offscreen
// plugin cannot find a font directory, and a caption that does not render is
// exactly the defect this is looking for.
#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/SessionController.h"
#include "ui/SubtitleWindow.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    applog::setMode(applog::Mode::Debug);
    applog::setLevel(applog::Level::Info);

    const QString target = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral("127.0.0.1:18800");
    const QString token = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("demo");
    const QString media = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    const int holdMs = argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt() : 25000;
    const QString shot = argc > 5 ? QString::fromLocal8Bit(argv[5])
                                  : QStringLiteral("/tmp/subtitle.png");

    AppConfig config;
    config.serverTarget = target;
    config.apiToken = token;

    SessionController controller(&config);
    SubtitleWindow window(&controller, &config);
    window.resize(1280, 720);
    window.show();

    if (!media.isEmpty()) {
        // After show(), so the video surface has a real size before the first
        // caption is painted onto it.
        QTimer::singleShot(500, &window, [&] { window.loadFile(media); });
    }

    QTimer::singleShot(holdMs, &app, [&] {
        const QPixmap pixmap = window.grab();
        if (pixmap.save(shot))
            std::printf("saved %s (%dx%d)\n", qPrintable(shot), pixmap.width(), pixmap.height());
        else
            std::printf("FAILED to save %s\n", qPrintable(shot));
        app.quit();
    });

    const int code = app.exec();
    applog::shutdown();
    return code;
}
