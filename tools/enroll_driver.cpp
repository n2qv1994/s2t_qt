// Throwaway driver for the speaker-enrolment dialog.
//
// Same reasoning as tools/subtitle_driver.cpp: a widget is not verified by a
// build.  This one exists for the "DB giọng chung" tab, which is the tab that
// can take a voice out of a database shared by every meeting - so what its
// buttons say, and which one reads as the dangerous one, is worth looking at
// rather than assuming.
//
// It opens the dialog non-modally (the product opens it with exec(), which
// would block a driver), loads the real global speaker list over the real
// transport, then grabs the pixels.
//
// Not part of the .pro on purpose - a diagnostic, not a product.  Build it
// next to an existing client build tree:
//
//   g++ -fPIC -std=c++17 tools/enroll_driver.cpp \
//       $(ls build-rhel/s2t-qt-client/*.o | grep -v '/main\.o$') \
//       -I s2t-qt-client -I shared -I build-rhel/s2t-qt-client \
//       $(pkg-config --cflags --libs Qt6Widgets Qt6Multimedia Qt6Network) \
//       -o /tmp/enroll_driver
//
// Run it on the real X11 session (DISPLAY=:1), not offscreen: Vietnamese
// labels measure about 90 px wider here than on the Windows kit, and a
// clipped column is exactly what this is looking for.
#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/SessionController.h"
#include "ui/EnrollDialog.h"

#include <QApplication>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    applog::setMode(applog::Mode::Debug);
    applog::setLevel(applog::Level::Info);

    const QString target = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral("127.0.0.1:8801");
    const QString token = argc > 2 ? QString::fromLocal8Bit(argv[2])
                                   : QStringLiteral("s2t-local");
    const int tabIndex = argc > 3 ? QString::fromLocal8Bit(argv[3]).toInt() : 2;
    const QString shot = argc > 4 ? QString::fromLocal8Bit(argv[4])
                                  : QStringLiteral("/tmp/enroll.png");

    AppConfig config;
    config.serverTarget = target;
    config.apiToken = token;

    SessionController controller(&config);
    EnrollDialog dialog(&controller, QStringLiteral("kiemthu-anh"));
    dialog.show();

    // The tabs have no accessible names, so they are picked by index - 0
    // enrol, 1 session speakers, 2 the global database.
    QTimer::singleShot(300, &dialog, [&] {
        const QList<QTabWidget *> tabs = dialog.findChildren<QTabWidget *>();
        if (tabs.isEmpty()) {
            std::fprintf(stderr, "no tab widget found\n");
            return;
        }
        tabs.first()->setCurrentIndex(tabIndex);
        // The global tab does not load itself - nothing should hit a shared
        // database just because a dialog opened - so the driver presses the
        // same button an operator would.
        for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Tải lại") && button->isVisible()) {
                button->click();
                break;
            }
        }
    });

    QTimer::singleShot(4000, &dialog, [&] {
        if (dialog.grab().save(shot))
            std::fprintf(stderr, "saved %s\n", qPrintable(shot));
        else
            std::fprintf(stderr, "could not save %s\n", qPrintable(shot));
        QApplication::quit();
    });

    return app.exec();
}
