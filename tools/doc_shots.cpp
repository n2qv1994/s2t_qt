// Chụp ảnh minh hoạ cho docs/huong-dan-su-dung.md.
//
// A screenshot in a handbook goes stale the moment a label changes, and the
// only cheap way to keep one honest is to be able to retake the whole set in
// one command.  That is what this is: it links against a normal client build
// (every object except main.o), opens the **real** MainWindow, drives it the
// way the operator would - the same QActions the menu triggers - and grabs
// each window into docs/images/.
//
// It is not part of the .pro: a diagnostic, like tools/subtitle_driver.cpp.
// Build it beside an existing build tree, on the RHEL host, because the point
// of the pictures is to show what the operator there actually sees:
//
//   export PKG_CONFIG_PATH=$HOME/Qt/6.11.2/gcc_64/lib/pkgconfig
//   g++ -fPIC -std=c++17 tools/doc_shots.cpp \
//       $(ls build-rhel/s2t-qt-client/*.o | grep -v '/main\.o$') \
//       -I s2t-qt-client -I shared \
//       $(pkg-config --cflags --libs Qt6Widgets Qt6Multimedia Qt6Network) \
//       -o /tmp/doc_shots
//   DISPLAY=:1 timeout 240 /tmp/doc_shots docs/images sample/"Ngọc Trinh.mp4"
//
// Run it on the real X11 session, never `-platform offscreen`: the offscreen
// plugin cannot find a font directory, so every caption in the pictures would
// come out as boxes.
//
// Two things the recipe above hides, both learned the hard way:
//
//   - A modal dialog blocks inside exec(), so the capture has to be *armed
//     before* the action that opens it - QTimer::singleShot into the nested
//     event loop, grab QApplication::activeModalWidget(), then reject() it.
//   - A grab re-renders the widget tree, so it is blind to how the window
//     manager stacks anything with a surface of its own.  That is exactly how
//     the subtitle caption came to be invisible on the deployed host while
//     these pictures showed it - so the subtitle window is also captured with
//     `gnome-screenshot -w` (see the recipe in luong-hoat-dong.md §13), and
//     the two are compared.  Since the caption moved into SubtitleStage there
//     is no second surface left, and the two captures should now agree.
//
// It talks to whatever Server buffer the saved config points at and it starts
// a real session - the transcript in the pictures is real text from the real
// inference tier, and the session stays in the server's database afterwards.
#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/SessionController.h"
#include "mainwindow.h"
#include "ui/DiagnosticsWindow.h"
#include "ui/EvidenceWindow.h"
#include "ui/SubtitleWindow.h"
#include "ui/TraceWindow.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>

#include <cstdio>
#include <functional>

namespace {

QString g_outDir;
int g_saved = 0;

void save(QWidget *widget, const QString &name)
{
    if (!widget) {
        std::printf("MISSING  %s - no widget\n", qPrintable(name));
        return;
    }
    const QString path = g_outDir + QLatin1Char('/') + name;
    const QPixmap pixmap = widget->grab();
    if (pixmap.save(path)) {
        ++g_saved;
        std::printf("saved    %s (%dx%d)\n", qPrintable(name), pixmap.width(), pixmap.height());
    } else {
        std::printf("FAILED   %s\n", qPrintable(name));
    }
    std::fflush(stdout);
}

// The menu and the toolbar share one QAction per job, so the label is the
// address of a feature.  `&&` in a label is a literal ampersand for Qt.
QAction *action(MainWindow *window, const QString &text)
{
    for (QAction *candidate : window->findChildren<QAction *>()) {
        if (candidate->text() == text)
            return candidate;
    }
    std::printf("MISSING  action \"%s\"\n", qPrintable(text));
    return nullptr;
}

void trigger(MainWindow *window, const QString &text)
{
    if (QAction *found = action(window, text))
        found->trigger();
}

void at(int ms, const std::function<void()> &step)
{
    QTimer::singleShot(ms, qApp, step);
}

// Arms the capture first, then opens the dialog: exec() does not return until
// the dialog closes, and closing it is what this capture has to do.
void shootModal(MainWindow *window, const QString &actionText, const QString &name, int settleMs)
{
    at(settleMs, [name] {
        QWidget *modal = QApplication::activeModalWidget();
        save(modal, name);
        if (auto *dialog = qobject_cast<QDialog *>(modal))
            dialog->reject();
    });
    trigger(window, actionText);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    applog::setMode(applog::Mode::Debug);
    applog::setLevel(applog::Level::Warn);

    g_outDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("docs/images");
    const QString media = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    QDir().mkpath(g_outDir);

    MainWindow window;
    // Wider than the constructor's own guess.  MainWindow sizes itself from
    // the toolbar's size hint before the status pill is added to it, so at
    // 1440 the pill lands in the toolbar's ">>" overflow menu - and a manual
    // showing a toolbar with its most-looked-at item hidden is worse than no
    // picture.  The deployed desktop is 1920x1080, so this is a real size.
    window.resize(1680, 950);
    window.show();

    auto *controller = window.findChild<SessionController *>();
    if (!controller) {
        std::printf("no SessionController under MainWindow\n");
        return 2;
    }
    // The audit trail wants a name, and the enrolment dialog refuses to record
    // without one - an empty box would make that picture show an error state.
    for (QLineEdit *edit : window.findChildren<QLineEdit *>()) {
        if (edit->placeholderText() == QStringLiteral("tên của bạn"))
            edit->setText(QStringLiteral("Điều hành viên"));
    }

    // A file replay rather than the microphone: the pictures have to show real
    // transcript rows, and this host has no microphone worth recording.
    if (!media.isEmpty()) {
        at(1200, [controller, media] {
            SessionMeta meta;
            meta.title = QStringLiteral("Phiên minh hoạ tài liệu");
            meta.participants = QStringList{QStringLiteral("Ngọc Trinh")};
            controller->startFile(media, false, QStringList(), meta);
        });
    }

    // --- cửa sổ chính, có bản chép thật đang chạy ---
    at(24000, [&window] { save(&window, QStringLiteral("01-cua-so-chinh.png")); });

    // --- chế độ chữ chạy ---
    at(25000, [&window] { trigger(&window, QStringLiteral("Chữ chạy")); });
    at(26500, [&window] {
        save(&window, QStringLiteral("02-chu-chay.png"));
        trigger(&window, QStringLiteral("Chữ chạy"));
    });

    // --- bảng soát & sửa, mở trên chính phiên đang chạy ---
    at(27500, [&window] { trigger(&window, QStringLiteral("Soát && sửa")); });
    at(33000, [&window] {
        save(&window, QStringLiteral("03-soat-va-sua.png"));
        trigger(&window, QStringLiteral("Soát && sửa"));
    });

    // --- dừng phiên, rồi tới các cửa sổ tra cứu ---
    at(34000, [&window] { trigger(&window, QStringLiteral("Dừng phiên")); });

    at(40000, [&window] {
        trigger(&window, QStringLiteral("Pipeline trace..."));
    });
    at(43000, [&window] {
        save(window.findChild<TraceWindow *>(), QStringLiteral("04-pipeline-trace.png"));
        if (auto *trace = window.findChild<TraceWindow *>())
            trace->hide();
    });

    at(44000, [&window] { trigger(&window, QStringLiteral("Nghiệm thu pipeline...")); });
    at(48000, [&window] {
        save(window.findChild<EvidenceWindow *>(), QStringLiteral("05-nghiem-thu.png"));
        if (auto *evidence = window.findChild<EvidenceWindow *>())
            evidence->hide();
    });

    at(49000, [&window] { trigger(&window, QStringLiteral("Nhật ký && chẩn đoán")); });
    at(52000, [&window] {
        save(window.findChild<DiagnosticsWindow *>(), QStringLiteral("06-nhat-ky-chan-doan.png"));
        if (auto *diagnostics = window.findChild<DiagnosticsWindow *>())
            diagnostics->hide();
    });

    // --- các hộp thoại modal ---
    at(54000, [&window] {
        shootModal(&window, QStringLiteral("Ghi âm từ micro"),
                   QStringLiteral("07-bat-dau-phien.png"), 1800);
    });
    at(58000, [&window] {
        shootModal(&window, QStringLiteral("Đăng ký giọng nói..."),
                   QStringLiteral("08-dang-ky-giong.png"), 2500);
    });
    at(63000, [&window] {
        shootModal(&window, QStringLiteral("Lịch sử hiệu chỉnh..."),
                   QStringLiteral("09-lich-su-hieu-chinh.png"), 2200);
    });
    at(68000, [&window] {
        shootModal(&window, QStringLiteral("Cấu hình..."), QStringLiteral("10-cau-hinh.png"), 1800);
    });

    // --- cửa sổ phụ đề, phát chính tệp mẫu ---
    at(72000, [&window, media] {
        trigger(&window, QStringLiteral("Phụ đề"));
        if (auto *subtitles = window.findChild<SubtitleWindow *>()) {
            subtitles->resize(1280, 720);
            // raise() alone leaves the keyboard focus on the main window, and
            // `gnome-screenshot -w` shoots whatever has the focus - that is
            // how the companion capture of the video frames finds this window.
            subtitles->raise();
            subtitles->activateWindow();
            if (!media.isEmpty())
                subtitles->loadFile(media);
            std::printf("subtitle window is up - external capture window opens now\n");
            std::fflush(stdout);
        }
    });
    at(92000, [&window] {
        auto *subtitles = window.findChild<SubtitleWindow *>();
        save(subtitles, QStringLiteral("11-phu-de-truc-tiep.png"));
        if (subtitles) {
            for (QPushButton *button : subtitles->findChildren<QPushButton *>()) {
                if (button->text() == QStringLiteral("Dừng"))
                    button->click();
            }
        }
    });

    at(95000, [] {
        std::printf("%d ảnh đã lưu vào %s\n", g_saved, qPrintable(g_outDir));
        qApp->quit();
    });

    const int code = app.exec();
    applog::shutdown();
    return code;
}
