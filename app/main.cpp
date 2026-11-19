// kolma desktop -- a free, friendly compression app. Links the C++ engine
// directly; no subprocess, no flags required.
#include <QApplication>
#include <QFileInfo>
#include <QTimer>

#include "mainwindow.hpp"
#include "theme.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("kolma");
  QApplication::setOrganizationName("kolma");
  QApplication::setApplicationDisplayName("kolma");
  app.setStyleSheet(kolma::ui::stylesheet());

  // Headless QA / CI mode: --screenshot <path> [--view name] [files...] renders
  // the window, saves a PNG, and exits. Works with -platform offscreen.
  QString shotPath;
  QString viewName;
  QStringList files;
  for (int i = 1; i < argc; ++i) {
    QString a = QString::fromLocal8Bit(argv[i]);
    if (a == "--screenshot" && i + 1 < argc) {
      shotPath = QString::fromLocal8Bit(argv[++i]);
    } else if (a == "--view" && i + 1 < argc) {
      viewName = QString::fromLocal8Bit(argv[++i]);
    } else if (QFileInfo::exists(a)) {
      files.append(a);
    }
  }

  kolma::ui::MainWindow w;
  w.resize(1060, 760);
  w.show();
  if (!files.isEmpty()) w.handleFiles(files);
  if (!viewName.isEmpty()) {
    w.showView(viewName);
    if (viewName == "tools" && !files.isEmpty()) w.inspectInTools(files.first());
  }

  if (!shotPath.isEmpty()) {
    // Give async workers (inspect/list) a moment to populate before grabbing.
    QTimer::singleShot(1500, [&] {
      w.grab().save(shotPath);
      app.quit();
    });
  }
  return app.exec();
}
