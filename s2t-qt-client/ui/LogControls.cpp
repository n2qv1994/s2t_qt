#include "LogControls.h"

#include <QComboBox>

namespace logcontrols {
namespace {

// Only the two modes need a sentence of their own; the levels read well enough
// from their name plus a short note.
QString modeLabel(applog::Mode mode)
{
    return mode == applog::Mode::Develop ? QStringLiteral("Develop — ghi ra tệp")
                                         : QStringLiteral("Debug — in ra console");
}

QString levelLabel(applog::Level level)
{
    switch (level) {
    case applog::Level::Trace: return QStringLiteral("trace — từng gói audio, rất nhiều");
    case applog::Level::Debug: return QStringLiteral("debug — đủ để lần theo luồng");
    case applog::Level::Info: return QStringLiteral("info — chỉ các mốc chính");
    case applog::Level::Warn: return QStringLiteral("warn — chỉ cảnh báo");
    case applog::Level::Error: return QStringLiteral("error — chỉ lỗi");
    case applog::Level::Off: break;
    }
    return applog::levelName(level);
}

// Every label here is a sentence, and they differ in length by a lot.  Qt's
// default is AdjustToContentsOnFirstShow, which means the box reports the
// width of the *current* item while the window is being laid out and then
// grows to the longest one when it is first shown - after any code that sized
// the window from sizeHint() has already run.  On RHEL that pushed the last
// item of the filter row ("Tạm giữ") off the right edge of a window that
// believed it was wide enough.
void sizeToLongestItem(QComboBox *box)
{
    box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
}

} // namespace

void fillModes(QComboBox *box, applog::Mode selected)
{
    for (applog::Mode mode : {applog::Mode::Debug, applog::Mode::Develop})
        box->addItem(modeLabel(mode), applog::modeName(mode));
    box->setCurrentIndex(box->findData(applog::modeName(selected)));
    sizeToLongestItem(box);
}

void fillLevels(QComboBox *box, applog::Level selected)
{
    for (applog::Level level : applog::selectableLevels())
        box->addItem(levelLabel(level), applog::levelName(level));
    box->setCurrentIndex(box->findData(applog::levelName(selected)));
    sizeToLongestItem(box);
}

applog::Mode selectedMode(const QComboBox *box)
{
    return applog::modeFromString(box->currentData().toString());
}

applog::Level selectedLevel(const QComboBox *box)
{
    return applog::levelFromString(box->currentData().toString());
}

} // namespace logcontrols
