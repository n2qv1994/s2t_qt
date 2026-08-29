#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QHash>
#include <QStyle>
#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QScreen>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTranslator>
#include <QWidget>

#include <array>

namespace theme {
namespace {

bool g_dark = false;

struct Tokens
{
    QColor window;
    QColor surface;
    QColor surfaceAlt;
    QColor surfaceSunken;
    QColor border;
    QColor borderStrong;
    QColor text;
    QColor textMuted;
    QColor textFaint;
    QColor accent;
    QColor accentSoft;
    QColor ok;
    QColor okSoft;
    QColor warn;
    QColor warnSoft;
    QColor danger;
    QColor dangerSoft;
    std::array<QColor, 5> lanes;
};

const Tokens &tokens()
{
    // Light first, because that is what the operator workstations run.  The
    // dark table exists so a dark desktop does not paint dark text into the
    // white boxes this application used to hard-code.
    static const Tokens light{
        // The window is a step darker than the cards on purpose: at #f1f3f5 a
        // white card on it had no visible edge, so the panels read as one flat
        // sheet and the grouping did no work.
        QColor("#e7eaee"), QColor("#ffffff"), QColor("#f4f6f8"), QColor("#dde1e7"),
        QColor("#ccd2da"), QColor("#aab3be"),
        QColor("#181c22"), QColor("#5b6572"), QColor("#8b95a1"),
        QColor("#1f6feb"), QColor("#e4edfd"),
        QColor("#17803d"), QColor("#e2f4e9"),
        QColor("#9a6700"), QColor("#fbf2da"),
        QColor("#b42318"), QColor("#fdeceb"),
        {QColor("#1b6ed8"), QColor("#1f8f47"), QColor("#c9483f"), QColor("#7d52cf"),
         QColor("#a9691a")}};
    static const Tokens dark{
        QColor("#14171b"), QColor("#1d2126"), QColor("#23282f"), QColor("#101317"),
        QColor("#333a43"), QColor("#4a535e"),
        QColor("#e6e9ee"), QColor("#9ba5b2"), QColor("#6e7885"),
        QColor("#4d9dff"), QColor("#16304f"),
        QColor("#4ac97e"), QColor("#123021"),
        QColor("#e3b341"), QColor("#3a2f10"),
        QColor("#ff7a6e"), QColor("#3d1a17"),
        {QColor("#69adff"), QColor("#5fc87a"), QColor("#ff8b80"), QColor("#b394f0"),
         QColor("#e0a44a")}};
    return g_dark ? dark : light;
}

// Decides the scheme once.  Qt only grew a real answer in 6.5; below that,
// read the lightness the desktop already handed us, which is what the
// platform theme plugin resolved from the system settings anyway.
bool detectDark()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown)
        return scheme == Qt::ColorScheme::Dark;
#endif
    const QPalette base = QGuiApplication::palette();
    return base.color(QPalette::Window).lightness() < 128;
}

QString hex(const QColor &color)
{
    return color.name(QColor::HexRgb);
}

// A translucent version, expressed as rgba() so it works as a stylesheet fill
// over whatever happens to be behind it.
QString rgba(const QColor &color, double alpha)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(alpha, 0, 'f', 3);
}

// ---------------------------------------------------------------------------
// Icons
//
// Painted rather than shipped: this project has no .qrc and deliberately no
// third-party dependencies, and QStyle's standard icons are a different visual
// language on every platform - the whole point of the restyle is that the two
// kits look the same.  Sixteen logical units square; every glyph is drawn in
// that box and scaled to whatever size is asked for.
// ---------------------------------------------------------------------------

void strokePolyline(QPainter &painter, std::initializer_list<QPointF> points)
{
    QPainterPath path;
    bool first = true;
    for (const QPointF &point : points) {
        if (first) {
            path.moveTo(point);
            first = false;
        } else {
            path.lineTo(point);
        }
    }
    painter.drawPath(path);
}

void paintGlyph(QPainter &painter, Glyph glyph, const QColor &color)
{
    QPen pen(color);
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (glyph) {
    case Glyph::Record:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(8, 8), 4.6, 4.6);
        break;
    case Glyph::Pause:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(4.2, 3.4, 2.6, 9.2), 1.1, 1.1);
        painter.drawRoundedRect(QRectF(9.2, 3.4, 2.6, 9.2), 1.1, 1.1);
        break;
    case Glyph::Stop:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(4, 4, 8, 8), 1.6, 1.6);
        break;
    case Glyph::File:
        strokePolyline(painter, {{4, 2.4}, {9.4, 2.4}, {12, 5.2}, {12, 13.6}, {4, 13.6}, {4, 2.4}});
        strokePolyline(painter, {{9.2, 2.6}, {9.2, 5.4}, {11.8, 5.4}});
        painter.drawLine(QPointF(6, 9), QPointF(10, 9));
        painter.drawLine(QPointF(6, 11.2), QPointF(10, 11.2));
        break;
    case Glyph::Live:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(8, 8), 2.1, 2.1);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(3.4, 3.4, 9.2, 9.2), -60 * 16, 120 * 16);
        painter.drawArc(QRectF(3.4, 3.4, 9.2, 9.2), 120 * 16, 120 * 16);
        painter.drawArc(QRectF(1, 1, 14, 14), -50 * 16, 100 * 16);
        painter.drawArc(QRectF(1, 1, 14, 14), 130 * 16, 100 * 16);
        break;
    case Glyph::JumpText:
        painter.drawLine(QPointF(2.6, 3.6), QPointF(13.4, 3.6));
        painter.drawLine(QPointF(2.6, 6.6), QPointF(10.4, 6.6));
        painter.drawLine(QPointF(2.6, 9.6), QPointF(13.4, 9.6));
        strokePolyline(painter, {{5.4, 12}, {8, 14.2}, {10.6, 12}});
        break;
    case Glyph::Ticker:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(1.6, 4.2, 8.4, 2.0), 1.0, 1.0);
        painter.drawRoundedRect(QRectF(4.4, 7.4, 10.0, 2.0), 1.0, 1.0);
        painter.drawRoundedRect(QRectF(2.6, 10.6, 6.4, 2.0), 1.0, 1.0);
        break;
    case Glyph::LowConf:
        painter.drawLine(QPointF(3.2, 4.4), QPointF(12.8, 4.4));
        painter.drawLine(QPointF(3.2, 7.4), QPointF(10.2, 7.4));
        {
            QPen dotted = pen;
            dotted.setStyle(Qt::DotLine);
            dotted.setWidthF(1.6);
            painter.setPen(dotted);
            painter.drawLine(QPointF(3.2, 11.4), QPointF(12.8, 11.4));
        }
        break;
    case Glyph::Review:
        painter.drawLine(QPointF(2.6, 4.0), QPointF(8.6, 4.0));
        painter.drawLine(QPointF(2.6, 7.4), QPointF(7.0, 7.4));
        painter.drawLine(QPointF(2.6, 10.8), QPointF(6.2, 10.8));
        strokePolyline(painter, {{13.6, 4.2}, {8.4, 9.4}, {7.8, 12.0}, {10.4, 11.4}, {13.6, 8.2}});
        break;
    case Glyph::Subtitle:
        painter.drawRoundedRect(QRectF(1.8, 3.0, 12.4, 10.0), 1.8, 1.8);
        painter.drawLine(QPointF(4.0, 9.6), QPointF(8.2, 9.6));
        painter.drawLine(QPointF(9.6, 9.6), QPointF(12.0, 9.6));
        painter.drawLine(QPointF(4.0, 11.5), QPointF(6.4, 11.5));
        painter.drawLine(QPointF(7.8, 11.5), QPointF(12.0, 11.5));
        break;
    case Glyph::Trace:
        painter.drawLine(QPointF(4.6, 4.2), QPointF(11.2, 8.0));
        painter.drawLine(QPointF(4.6, 11.8), QPointF(11.2, 8.0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(3.6, 4.0), 2.0, 2.0);
        painter.drawEllipse(QPointF(3.6, 12.0), 2.0, 2.0);
        painter.drawEllipse(QPointF(12.2, 8.0), 2.0, 2.0);
        break;
    case Glyph::Evidence:
        painter.drawRoundedRect(QRectF(3.0, 2.8, 10.0, 11.4), 1.6, 1.6);
        painter.drawRoundedRect(QRectF(5.8, 1.4, 4.4, 2.6), 0.9, 0.9);
        strokePolyline(painter, {{5.6, 9.4}, {7.4, 11.2}, {10.8, 6.8}});
        break;
    case Glyph::Enroll:
        painter.drawEllipse(QPointF(8, 5.4), 2.9, 2.9);
        painter.drawArc(QRectF(2.6, 8.6, 10.8, 10.4), 0, 180 * 16);
        break;
    case Glyph::Log:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int row = 0; row < 4; ++row) {
            const double y = 3.0 + row * 3.2;
            painter.drawEllipse(QPointF(3.0, y + 0.9), 0.95, 0.95);
            painter.drawRoundedRect(QRectF(5.6, y, 8.0 - row * 0.9, 1.8), 0.9, 0.9);
        }
        break;
    case Glyph::History:
        painter.drawEllipse(QPointF(8, 8), 5.8, 5.8);
        strokePolyline(painter, {{8, 4.6}, {8, 8.3}, {10.8, 9.6}});
        break;
    case Glyph::Settings:
        painter.setPen(pen);
        for (int row = 0; row < 3; ++row) {
            const double y = 4.0 + row * 4.0;
            painter.drawLine(QPointF(2.4, y), QPointF(13.6, y));
        }
        painter.setBrush(color);
        painter.drawEllipse(QPointF(5.4, 4.0), 1.9, 1.9);
        painter.drawEllipse(QPointF(10.6, 8.0), 1.9, 1.9);
        painter.drawEllipse(QPointF(6.6, 12.0), 1.9, 1.9);
        break;
    case Glyph::Denoise: {
        QPen faint = pen;
        faint.setWidthF(1.2);
        faint.setColor(QColor(color.red(), color.green(), color.blue(), 110));
        painter.setPen(faint);
        strokePolyline(painter, {{1.6, 8}, {3.2, 3.2}, {4.8, 12.4}, {6.4, 4.6}, {8, 8}});
        painter.setPen(pen);
        strokePolyline(painter, {{8, 8}, {10, 6.6}, {12, 9.4}, {14.4, 8}});
        break;
    }
    }
}

QColor toneColor(Tone tone)
{
    switch (tone) {
    case Tone::Ok:
        return color(Role::Ok);
    case Tone::Warn:
        return color(Role::Warn);
    case Tone::Danger:
        return color(Role::Danger);
    case Tone::Info:
        return color(Role::Accent);
    case Tone::Neutral:
        break;
    }
    return color(Role::TextMuted);
}

QColor toneFill(Tone tone)
{
    switch (tone) {
    case Tone::Ok:
        return color(Role::OkSoft);
    case Tone::Warn:
        return color(Role::WarnSoft);
    case Tone::Danger:
        return color(Role::DangerSoft);
    case Tone::Info:
        return color(Role::AccentSoft);
    case Tone::Neutral:
        break;
    }
    return color(Role::SurfaceSunken);
}

// ---------------------------------------------------------------------------
// Arrow assets
//
// A Qt style sheet cannot draw a triangle.  The moment any rule targets
// QComboBox, Qt switches that widget to QStyleSheetStyle, which draws a
// sub-control only if the sheet hands it an `image:` - and this project ships
// no .qrc on purpose, so there is no `:/arrow.png` to point at.  Leaving it
// unstyled is not an option either: a combo box with no arrow does not read as
// a combo box.
//
// So the arrows are painted at startup and written next to the application's
// settings, and the sheet points at those files by absolute path.  Two sizes
// each: Qt picks up the "@2x" variant by itself on a scaled desktop.
//
// If the directory is not writable the caller drops the arrow rules and the
// platform style keeps drawing its own - degraded, never broken.
// ---------------------------------------------------------------------------

bool writeArrow(const QString &path, bool pointsDown, int size, const QColor &ink)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(size / 12.0, size / 12.0);
    QPen pen(ink);
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    if (pointsDown)
        strokePolyline(painter, {{3.0, 4.8}, {6.0, 7.8}, {9.0, 4.8}});
    else
        strokePolyline(painter, {{3.0, 7.2}, {6.0, 4.2}, {9.0, 7.2}});
    painter.end();
    return pixmap.save(path, "PNG");
}

// Returns the directory the arrows were written to, or an empty string.
QString writeArrowAssets()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        return QString();
    const QString dir = base + QStringLiteral("/ui");
    if (!QDir().mkpath(dir))
        return QString();

    const QColor ink = tokens().textMuted;
    struct Asset
    {
        const char *name;
        bool down;
    };
    for (const Asset &asset : {Asset{"arrow-down", true}, Asset{"arrow-up", false}}) {
        const QString stem = dir + QLatin1Char('/') + QLatin1String(asset.name);
        if (!writeArrow(stem + QStringLiteral(".png"), asset.down, 12, ink))
            return QString();
        if (!writeArrow(stem + QStringLiteral("@2x.png"), asset.down, 24, ink))
            return QString();
    }
    return dir;
}

// ---------------------------------------------------------------------------
// Standard button text
//
// Every dialog in this application is written in Vietnamese and then ends in a
// row of English: "Save", "Cancel", "Close".  Those strings do not come from
// our code - Qt builds them in QPlatformTheme and there is no Vietnamese .qm
// shipped with Qt to load.
//
// A QTranslator does not have to read a file.  Installing one that answers for
// the "QPlatformTheme" context fixes the buttons of every QDialogButtonBox and
// QMessageBox at once, including the ones this project has not written yet.
// ---------------------------------------------------------------------------

class StandardButtonTranslator : public QTranslator
{
public:
    using QTranslator::QTranslator;

    bool isEmpty() const override { return false; }

    QString translate(const char *context, const char *sourceText, const char * = nullptr,
                      int = -1) const override
    {
        if (qstrcmp(context, "QPlatformTheme") != 0)
            return QString();

        // Keys are Qt's own source strings, "&" and all: QPlatformTheme asks
        // for "&Yes" and "&No" but for a bare "Save" and "Cancel".
        static const QHash<QByteArray, QString> table{
            {"OK", QStringLiteral("Đồng ý")},
            {"&OK", QStringLiteral("Đồng ý")},
            {"Cancel", QStringLiteral("Huỷ")},
            {"&Cancel", QStringLiteral("Huỷ")},
            {"Save", QStringLiteral("Lưu")},
            {"&Save", QStringLiteral("Lưu")},
            {"Save All", QStringLiteral("Lưu tất cả")},
            {"Open", QStringLiteral("Mở")},
            {"&Yes", QStringLiteral("Có")},
            {"Yes to &All", QStringLiteral("Có cho tất cả")},
            {"&No", QStringLiteral("Không")},
            {"N&o to All", QStringLiteral("Không cho tất cả")},
            {"Abort", QStringLiteral("Bỏ dở")},
            {"Retry", QStringLiteral("Thử lại")},
            {"Ignore", QStringLiteral("Bỏ qua")},
            {"Close", QStringLiteral("Đóng")},
            {"Discard", QStringLiteral("Không lưu")},
            {"Help", QStringLiteral("Trợ giúp")},
            {"Apply", QStringLiteral("Áp dụng")},
            {"Reset", QStringLiteral("Đặt lại")},
            {"Restore Defaults", QStringLiteral("Khôi phục mặc định")},
        };
        // An empty return means "no translation", which is what Qt needs to
        // hear for anything not in the table.
        return table.value(QByteArray(sourceText));
    }
};

QPalette buildPalette()
{
    const Tokens &t = tokens();
    QPalette palette;
    palette.setColor(QPalette::Window, t.window);
    palette.setColor(QPalette::WindowText, t.text);
    palette.setColor(QPalette::Base, t.surface);
    palette.setColor(QPalette::AlternateBase, t.surfaceAlt);
    palette.setColor(QPalette::Text, t.text);
    palette.setColor(QPalette::PlaceholderText, t.textFaint);
    palette.setColor(QPalette::Button, t.surfaceAlt);
    palette.setColor(QPalette::ButtonText, t.text);
    palette.setColor(QPalette::BrightText, t.danger);
    palette.setColor(QPalette::Highlight, t.accent);
    palette.setColor(QPalette::HighlightedText, g_dark ? t.window : QColor("#ffffff"));
    palette.setColor(QPalette::Link, t.accent);
    palette.setColor(QPalette::LinkVisited, t.accent.darker(120));
    palette.setColor(QPalette::ToolTipBase, t.surface);
    palette.setColor(QPalette::ToolTipText, t.text);
    palette.setColor(QPalette::Mid, t.border);
    palette.setColor(QPalette::Dark, t.borderStrong);
    palette.setColor(QPalette::Shadow, g_dark ? QColor("#000000") : QColor("#c3c9d1"));

    palette.setColor(QPalette::Disabled, QPalette::Text, t.textFaint);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, t.textFaint);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, t.textFaint);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, t.surfaceSunken);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, t.textFaint);
    return palette;
}

QString buildStyleSheet(const QFont &base)
{
    const Tokens &t = tokens();

    // Sizes are derived from whatever font the desktop chose rather than
    // written in pixels.  A Vietnamese label is about 90 px wider on the RHEL
    // font stack than on the MinGW one, so a sheet that pins px sizes fits on
    // exactly one of the two kits.
    const qreal pt = base.pointSizeF() > 0 ? base.pointSizeF() : 9.0;
    const QString small = QString::number(qMax(6.5, pt - 0.5), 'f', 1);
    const QString tiny = QString::number(qMax(6.0, pt - 1.0), 'f', 1);

    QString sheet = QStringLiteral(R"QSS(
/* ---- base ------------------------------------------------------------- */
QWidget            { color: @text; }
QMainWindow, QDialog, QWidget#s2tPage { background: @window; }
QToolTip {
    background: @surface; color: @text;
    border: 1px solid @border; border-radius: 4px; padding: 4px 7px;
}

/* ---- menu bar --------------------------------------------------------- */
QMenuBar { background: @surfaceAlt; border-bottom: 1px solid @border; padding: 2px 4px; }
QMenuBar::item { padding: 4px 10px; border-radius: 5px; background: transparent; }
QMenuBar::item:selected  { background: @accentSoft; color: @accent; }
QMenuBar::item:pressed   { background: @accentSoft; color: @accent; }
QMenu { background: @surface; border: 1px solid @border; border-radius: 7px; padding: 5px; }
QMenu::item { padding: 5px 26px 5px 22px; border-radius: 5px; }
QMenu::item:selected { background: @accentSoft; color: @accent; }
QMenu::item:disabled { color: @textFaint; }
QMenu::separator { height: 1px; background: @border; margin: 5px 8px; }
QMenu::icon { left: 6px; }

/* ---- tool bar --------------------------------------------------------- */
QToolBar {
    background: @surfaceAlt; border: 0px; border-bottom: 1px solid @border;
    padding: 5px 8px; spacing: 3px;
}
QToolBar::separator {
    background: @border; width: 1px; margin: 5px 7px;
}
QToolButton {
    background: transparent; border: 1px solid transparent; border-radius: 6px;
    padding: 4px 9px; color: @text;
}
QToolButton:hover   { background: @surfaceSunken; border-color: @border; }
QToolButton:pressed { background: @accentSoft; border-color: @accent; }
QToolButton:checked {
    background: @accentSoft; border-color: @accent; color: @accent; font-weight: 600;
}
QToolButton:disabled { color: @textFaint; }
QToolButton#s2tRecord:enabled { color: @danger; font-weight: 600; }
QToolButton#s2tRecord:enabled:hover { background: @dangerSoft; border-color: @danger; }

/* ---- buttons ---------------------------------------------------------- */
QPushButton {
    background: @surface; border: 1px solid @borderStrong; border-radius: 6px;
    padding: 5px 14px; min-height: 20px;
}
QPushButton:hover    { background: @surfaceAlt; border-color: @accent; }
QPushButton:pressed  { background: @accentSoft; }
QPushButton:disabled { background: @surfaceSunken; color: @textFaint; border-color: @border; }
/* The accept button of a QDialogButtonBox is Qt's `default` button, so every
   dialog in the application gets one filled call-to-action without any of them
   having to ask for it. */
QPushButton:default, QPushButton[s2tPrimary="true"] {
    background: @accent; border-color: @accent; color: #ffffff; font-weight: 600;
}
QPushButton:default:hover, QPushButton[s2tPrimary="true"]:hover {
    background: @accentHover; border-color: @accentHover;
}
QPushButton:default:disabled, QPushButton[s2tPrimary="true"]:disabled {
    background: @surfaceSunken; color: @textFaint; border-color: @border;
}

/* ---- inputs ----------------------------------------------------------- */
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: @surface; border: 1px solid @borderStrong; border-radius: 6px;
    padding: 4px 8px; selection-background-color: @accent; selection-color: #ffffff;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus,
QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: @accent; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
    background: @surfaceSunken; color: @textFaint;
}
QComboBox::drop-down { border: 0px; width: 20px; }
QSpinBox::up-button, QDoubleSpinBox::up-button {
    subcontrol-origin: border; subcontrol-position: top right;
    width: 18px; border-left: 1px solid @border; border-top-right-radius: 6px;
}
QSpinBox::down-button, QDoubleSpinBox::down-button {
    subcontrol-origin: border; subcontrol-position: bottom right;
    width: 18px; border-left: 1px solid @border; border-bottom-right-radius: 6px;
}
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover { background: @surfaceSunken; }
QComboBox QAbstractItemView {
    background: @surface; border: 1px solid @border; border-radius: 6px;
    selection-background-color: @accentSoft; selection-color: @accent; padding: 3px;
}

/* ---- containers ------------------------------------------------------- */
QGroupBox {
    background: @surface; border: 1px solid @border; border-radius: 8px;
    margin-top: 12px; padding: 10px 10px 8px 10px; font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin; subcontrol-position: top left; left: 10px; padding: 0px 5px;
    color: @textMuted; font-size: @smallpt;
}
#s2tCard {
    background: @surface; border: 1px solid @border; border-radius: 8px;
}
QSplitter::handle { background: @border; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical   { height: 1px; }
QSplitter::handle:hover      { background: @accent; }

/* ---- tabs ------------------------------------------------------------- */
QTabWidget::pane { border: 1px solid @border; border-radius: 8px; top: -1px; background: @surface; }
QTabBar::tab {
    background: transparent; color: @textMuted; padding: 6px 14px; margin-right: 2px;
    border: 1px solid transparent; border-top-left-radius: 7px; border-top-right-radius: 7px;
}
QTabBar::tab:hover    { color: @text; background: @surfaceSunken; }
QTabBar::tab:selected {
    background: @surface; color: @accent; font-weight: 600;
    border-color: @border; border-bottom-color: @surface;
}

/* ---- item views ------------------------------------------------------- */
QListWidget, QListView, QTreeView, QTableView, QTableWidget {
    background: @surface; border: 1px solid @border; border-radius: 8px;
    alternate-background-color: @surfaceAlt; outline: 0;
}
QListWidget::item, QTreeView::item { padding: 4px 6px; border-radius: 5px; }
QListWidget::item:hover, QTreeView::item:hover, QTableView::item:hover { background: @surfaceSunken; }
QListWidget::item:selected, QTreeView::item:selected, QTableView::item:selected {
    background: @accentSoft; color: @accent;
}
QHeaderView::section {
    background: @surfaceAlt; color: @textMuted; border: 0px;
    border-bottom: 1px solid @border; border-right: 1px solid @border;
    padding: 5px 8px; font-size: @smallpt; font-weight: 600;
}
QTableView { gridline-color: @border; }
QTableCornerButton::section { background: @surfaceAlt; border: 0px; }

/* ---- scrollbars ------------------------------------------------------- */
QScrollBar:vertical   { background: transparent; width: 11px; margin: 0px; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 0px; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
    background: @scrollHandle; border-radius: 5px; min-height: 28px; min-width: 28px;
}
QScrollBar::handle:hover { background: @scrollHandleHover; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0px; width: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ---- chrome ----------------------------------------------------------- */
QStatusBar { background: @surfaceAlt; border-top: 1px solid @border; color: @textMuted; }
QStatusBar::item { border: 0px; }
/* Do NOT set titlebar-close-icon here.  Blanking it removes the dock's close
   button altogether, and the review panel is then only closable from the menu
   - which is exactly the sort of thing a restyle is not allowed to break. */
QDockWidget { font-weight: 600; color: @textMuted; }
QDockWidget::title {
    background: @surfaceAlt; border-bottom: 1px solid @border;
    padding: 6px 10px; text-align: left;
}
/* Spacing only.  Giving the indicator a `background` makes Qt stop drawing the
   native mark inside it, so a ticked box became an unmarked blue square that
   an operator has to learn rather than read.  Fusion draws a correct tick from
   the palette, which is already ours. */
QCheckBox, QRadioButton { spacing: 7px; padding: 2px 0px; }
QProgressBar {
    background: @surfaceSunken; border: 0px; border-radius: 5px; height: 8px; text-align: center;
}
QProgressBar::chunk { background: @accent; border-radius: 5px; }

/* ---- named roles used across the windows ------------------------------ */
QLabel#s2tHeading {
    color: @textMuted; font-size: @tinypt; font-weight: 700;
}
QLabel[s2tMuted="true"] { color: @textMuted; }
QLabel[s2tMono="true"]  { font-family: "@mono"; font-size: @smallpt; }
)QSS");

    // Only now, once the tokens exist, can the arrows be painted in the right
    // ink.  No directory means no rules: the platform style then keeps drawing
    // its own arrows, which is worse-looking but never blank.
    const QString arrows = writeArrowAssets();
    if (!arrows.isEmpty()) {
        sheet += QStringLiteral(R"QSS(
QComboBox::down-arrow    { image: url("%1/arrow-down.png"); width: 12px; height: 12px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    image: url("%1/arrow-down.png"); width: 10px; height: 10px;
}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    image: url("%1/arrow-up.png"); width: 10px; height: 10px;
}
)QSS")
                     .arg(arrows);
    }

    struct Sub
    {
        const char *key;
        QString value;
    };
    const QColor accentHover = g_dark ? t.accent.lighter(115) : t.accent.darker(112);
    const Sub subs[] = {
        {"@window", hex(t.window)},
        {"@surfaceAlt", hex(t.surfaceAlt)},
        {"@surfaceSunken", hex(t.surfaceSunken)},
        {"@surface", hex(t.surface)},
        {"@borderStrong", hex(t.borderStrong)},
        {"@border", hex(t.border)},
        {"@textMuted", hex(t.textMuted)},
        {"@textFaint", hex(t.textFaint)},
        {"@text", hex(t.text)},
        {"@accentSoft", hex(t.accentSoft)},
        {"@accentHover", hex(accentHover)},
        {"@accent", hex(t.accent)},
        {"@dangerSoft", hex(t.dangerSoft)},
        {"@danger", hex(t.danger)},
        {"@scrollHandleHover", rgba(t.textMuted, 0.55)},
        {"@scrollHandle", rgba(t.textMuted, 0.32)},
        {"@smallpt", small + QStringLiteral("pt")},
        {"@tinypt", tiny + QStringLiteral("pt")},
        {"@mono", monoFamily()},
    };
    // Longest-first above, because "@surface" is a prefix of "@surfaceAlt" and
    // a naive pass would rewrite it to "#ffffffAlt".
    for (const Sub &sub : subs)
        sheet.replace(QLatin1String(sub.key), sub.value);
    return sheet;
}

} // namespace

bool isDark()
{
    return g_dark;
}

QColor color(Role role)
{
    const Tokens &t = tokens();
    switch (role) {
    case Role::Window:        return t.window;
    case Role::Surface:       return t.surface;
    case Role::SurfaceAlt:    return t.surfaceAlt;
    case Role::SurfaceSunken: return t.surfaceSunken;
    case Role::Border:        return t.border;
    case Role::BorderStrong:  return t.borderStrong;
    case Role::Text:          return t.text;
    case Role::TextMuted:     return t.textMuted;
    case Role::TextFaint:     return t.textFaint;
    case Role::Accent:        return t.accent;
    case Role::AccentSoft:    return t.accentSoft;
    case Role::Ok:            return t.ok;
    case Role::OkSoft:        return t.okSoft;
    case Role::Warn:          return t.warn;
    case Role::WarnSoft:      return t.warnSoft;
    case Role::Danger:        return t.danger;
    case Role::DangerSoft:    return t.dangerSoft;
    }
    return t.text;
}

int laneColorCount()
{
    return int(tokens().lanes.size());
}

QColor laneColor(int index)
{
    const auto &lanes = tokens().lanes;
    const int count = int(lanes.size());
    return lanes[std::size_t(((index % count) + count) % count)];
}

QString monoFamily()
{
    // Resolved once against the font database.  A style sheet cannot do this:
    // `font-family: monospace` is a CSS generic that Qt matches against real
    // family names, and on Windows nothing is called that - the log views
    // ended up in the proportional fallback, which is why their columns never
    // lined up.
    static const QString family = []() -> QString {
        const QStringList wanted{QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                                 QStringLiteral("DejaVu Sans Mono"),
                                 QStringLiteral("Liberation Mono"),
                                 QStringLiteral("Noto Sans Mono"), QStringLiteral("Menlo"),
                                 QStringLiteral("Courier New")};
        const QStringList available = QFontDatabase::families();
        for (const QString &candidate : wanted) {
            if (available.contains(candidate, Qt::CaseInsensitive))
                return candidate;
        }
        return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    }();
    return family;
}

QFont mono(qreal pointSizeDelta)
{
    QFont font(monoFamily());
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    const qreal base = QApplication::font().pointSizeF();
    if (base > 0)
        font.setPointSizeF(qMax(6.5, base + pointSizeDelta));
    return font;
}

void styleHeading(QLabel *label)
{
    if (!label)
        return;
    label->setObjectName(QStringLiteral("s2tHeading"));
    // Letter-spacing is not a Qt style-sheet property, so it goes on the font.
    QFont font = label->font();
    font.setBold(true);
    font.setLetterSpacing(QFont::PercentageSpacing, 106);
    label->setFont(font);
}

void stylePill(QLabel *label, Tone tone)
{
    if (!label)
        return;
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color:%1; background:%2; border:1px solid %3;"
                                        "border-radius:9px; padding:2px 10px; font-weight:700;")
                             .arg(hex(toneColor(tone)), hex(toneFill(tone)),
                                  rgba(toneColor(tone), 0.35)));
}

void markPrimary(QWidget *button)
{
    if (!button)
        return;
    button->setProperty("s2tPrimary", true);
    // A property the sheet selects on only takes effect after a repolish.
    button->style()->unpolish(button);
    button->style()->polish(button);
}

void styleCard(QWidget *frame)
{
    if (!frame)
        return;
    frame->setObjectName(QStringLiteral("s2tCard"));
    // A plain QWidget does not draw the background a style sheet gives it -
    // only subclasses that paint themselves do - so the card came out
    // invisible until this flag was set.
    frame->setAttribute(Qt::WA_StyledBackground, true);
}

QSize screenRoom(const QWidget *window)
{
    const QScreen *display = window ? window->screen() : QGuiApplication::primaryScreen();
    if (!display)
        return QSize();
    QSize room = display->availableGeometry().size();
    if (room.isEmpty())
        room = display->geometry().size();
    // A screen this small is a platform plugin that does not know, not a real
    // display; treating it as a limit is how a window ends up 132x123.
    if (room.width() < 320 || room.height() < 240)
        return QSize();
    // Leave room for the desktop's own chrome; a window sized to the exact
    // screen has its buttons under the panel on some setups.
    return QSize(room.width() - 48, room.height() - 72);
}

void sizeToContent(QWidget *window, const QSize &floor)
{
    if (!window)
        return;
    QSize wanted = window->sizeHint().expandedTo(floor);
    const QSize room = screenRoom(window);
    if (room.isValid())
        wanted = wanted.boundedTo(room);
    window->resize(wanted);
}

QIcon icon(Glyph glyph, Tone tone)
{
    const QColor ink = tone == Tone::Neutral ? color(Role::Text) : toneColor(tone);
    QIcon result;
    for (int size : {16, 20, 24, 32, 48}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.scale(size / 16.0, size / 16.0);
        paintGlyph(painter, glyph, ink);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

void apply(QApplication &app)
{
    // Fusion, deliberately, on both kits.  The native Windows 11 style ignores
    // most of the sheet below and brings its own metrics, so keeping it would
    // mean this application looked like two different products depending on
    // where it ran - and every layout check done on one kit would say nothing
    // about the other.
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        QApplication::setStyle(fusion);

    g_dark = detectDark();
    QApplication::setPalette(buildPalette());
    app.setStyleSheet(buildStyleSheet(app.font()));
    // Parented to the application so it lives as long as the dialogs do.
    QCoreApplication::installTranslator(new StandardButtonTranslator(&app));
}

} // namespace theme
