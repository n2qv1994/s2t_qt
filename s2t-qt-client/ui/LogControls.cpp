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

} // namespace

void fillModes(QComboBox *box, applog::Mode selected)
{
    for (applog::Mode mode : {applog::Mode::Debug, applog::Mode::Develop})
        box->addItem(modeLabel(mode), applog::modeName(mode));
    box->setCurrentIndex(box->findData(applog::modeName(selected)));
}

void fillLevels(QComboBox *box, applog::Level selected)
{
    for (applog::Level level : applog::selectableLevels())
        box->addItem(levelLabel(level), applog::levelName(level));
    box->setCurrentIndex(box->findData(applog::levelName(selected)));
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
