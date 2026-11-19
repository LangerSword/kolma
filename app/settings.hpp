// kolma desktop -- persisted preferences (QSettings), with sane defaults.
#pragma once

#include <QSettings>
#include <QString>

namespace kolma::ui {

inline QSettings prefs() { return QSettings("kolma", "kolma-desktop"); }

inline int prefThreads() { return prefs().value("threads", 0).toInt(); }
inline int prefPriority() { return prefs().value("priority", 1).toInt(); }
inline int prefLevel() { return prefs().value("level", 0).toInt(); }
inline QString prefAlgo() { return prefs().value("algo", "auto").toString(); }

}  // namespace kolma::ui
