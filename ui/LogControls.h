// The two combo boxes that pick where the log goes and how much of it there
// is.  Both the settings dialog and the diagnostics window offer them, and a
// list that exists twice is a list that drifts - a level added to
// core/Logger.h would otherwise have to be remembered in two more places.
#ifndef LOGCONTROLS_H
#define LOGCONTROLS_H

#include "../core/Logger.h"

QT_BEGIN_NAMESPACE
class QComboBox;
QT_END_NAMESPACE

namespace logcontrols {

// Items carry the canonical name as user data, so reading a selection back is
// applog::modeFromString / levelFromString on currentData().
void fillModes(QComboBox *box, applog::Mode selected);
void fillLevels(QComboBox *box, applog::Level selected);

applog::Mode selectedMode(const QComboBox *box);
applog::Level selectedLevel(const QComboBox *box);

} // namespace logcontrols

#endif // LOGCONTROLS_H
