#ifndef UI_THEME_H
#define UI_THEME_H

// One place that decides what this application looks like.
//
// Before this file the look was an accident: nine UI files each wrote their
// own `setStyleSheet("font-family:monospace; font-size:11px; ...")` with hex
// colours picked one at a time, all of them assuming a light desktop.  That
// is why the same widget had three different borders depending on which
// window it was in, and why a dark RHEL theme rendered dark-grey text on the
// hard-coded white boxes.
//
// Everything visual now comes from here:
//
//   theme::apply()      - once, from main(), before any widget exists
//   theme::color(Role)  - semantic colours, resolved for the active scheme
//   theme::icon(Glyph)  - the toolbar icons, painted rather than shipped
//   theme::mono()       - the real monospace face for this platform
//
// Two rules keep it honest.  Never write a hex literal in a widget file: add
// a Role here instead, so the dark scheme gets it too.  Never write a pixel
// font size: use the Text roles, which are relative to the desktop font, or
// Vietnamese labels overflow on the kit you did not test on.

#include <QColor>
#include <QFont>
#include <QIcon>
#include <QString>

class QApplication;
class QLabel;
class QWidget;

namespace theme {

enum class Role {
    Window,         // the desktop behind everything
    Surface,        // cards, list backgrounds, editors
    SurfaceAlt,     // alternating rows, toolbar, headers
    SurfaceSunken,  // wells the eye should read as "below" the surface
    Border,         // ordinary 1px separations
    BorderStrong,   // the edge of something interactive
    Text,           // primary reading text
    TextMuted,      // labels, units, secondary information
    TextFaint,      // placeholders, disabled, empty-state hints
    Accent,         // selection, focus, the "live" state
    AccentSoft,     // accent used as a fill behind text
    Ok,
    OkSoft,
    Warn,
    WarnSoft,
    Danger,
    DangerSoft,
};

// Status tones for pills, metric cards and inline messages.  Neutral is the
// "nothing to say" tone and deliberately does not colour anything.
enum class Tone { Neutral, Ok, Warn, Danger, Info };

enum class Glyph {
    Record,     // filled dot - start capturing
    Pause,      // two bars
    Stop,       // square
    File,       // a page with a corner fold
    Live,       // broadcast arcs
    JumpText,   // arrow into a line of text
    Ticker,     // three sliding rules
    LowConf,    // a dotted underline under a mark
    Review,     // pencil over a list
    Subtitle,   // frame with caption lines
    Trace,      // connected nodes
    Evidence,   // clipboard with a tick
    Enroll,     // a person
    Log,        // stacked lines
    History,    // a clock hand
    Settings,   // sliders
    Denoise,    // a wave being flattened
};

// Applies the style, palette and global stylesheet.  Call once, from main(),
// before the first widget is constructed - a QSS installed later is applied
// to existing widgets but not to the polish-time metrics some of them cached.
void apply(QApplication &app);

// True when apply() selected the dark token table.  Painting code that has to
// pick a blend (TimelineView) needs to know; nothing else should care.
bool isDark();

QColor color(Role role);

// The speaker lane colours, in a fixed order, tuned for the active scheme so
// they stay distinguishable on both.  index is taken modulo the count.
QColor laneColor(int index);
int laneColorCount();

// The platform's real monospace face.  `font-family:monospace` in a Qt style
// sheet is a CSS generic and resolves to whatever the font database happens
// to offer first, which on Windows is not a fixed-pitch face at all.
QFont mono(qreal pointSizeDelta = -0.5);
QString monoFamily();

// A section heading: small, letter-spaced, muted.  Used instead of yet
// another bold QLabel with a hand-written stylesheet.
void styleHeading(QLabel *label);

// A status pill - the connection indicator, a metric verdict.  Applies the
// tone as a rounded tinted badge rather than as coloured text on nothing.
void stylePill(QLabel *label, Tone tone);

// Marks a button as the primary action of its dialog.  Reads by the global
// stylesheet, so it survives a restyle.
void markPrimary(QWidget *button);

// A card: surface colour, hairline border, rounded, ready to hold a layout.
void styleCard(QWidget *frame);

QIcon icon(Glyph glyph, Tone tone = Tone::Neutral);

// Opens a window at whichever is larger: the size its content actually needs,
// or `floor` - then clamps to the screen.
//
// Call it as the LAST line of the constructor, never the first.  A window that
// resizes before its labels are filled in has chosen a size for content that
// does not exist yet; and a constant chosen on one kit is the wrong number on
// the other, because the same Vietnamese label measures about 90 px wider
// under the RHEL font stack than under MinGW.  Keep the old constant as the
// floor so the kit that already fitted does not move.
void sizeToContent(QWidget *window, const QSize &floor);

// How much room a window may take, or an **invalid** QSize when the platform
// will not say.
//
// Never read QScreen::availableGeometry() directly.  The RHEL 9 host this is
// deployed to runs an Xorg session where the XCB plugin reports the screen as
// 0x0 - primary screen, geometry and availableGeometry, at any time, not only
// before the window is mapped.  Code that clamps with `qMin(want, room)` then
// asks for a zero-sized window and gets whatever each widget's minimum happens
// to be.  This returns QSize() there, which every caller must read as "no
// limit known - do not clamp".
QSize screenRoom(const QWidget *window);

// Spacing scale.  Four steps is enough for this application and having them
// named stops the 6/8/9/12 drift that made panels look untidy next to
// each other.
constexpr int kGapTight = 4;
constexpr int kGap = 8;
constexpr int kGapWide = 14;
constexpr int kPad = 12;

} // namespace theme

#endif // UI_THEME_H
