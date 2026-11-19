// kolma desktop -- the shell: a calm sidebar, one view at a time, and drag-drop
// that routes archives to Extract and everything else to Compress.
#pragma once

#include <QMainWindow>
#include <QList>

class QStackedWidget;
class QPushButton;
class QLabel;
class QVBoxLayout;

namespace kolma::ui {

class CompressView;
class ExtractView;
class BenchView;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  void handleFiles(const QStringList& files);

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  void showStatus(const QString& msg, bool isError);
  void addNav(const QString& text, int index);

  QStackedWidget* m_stack;
  CompressView* m_compress;
  ExtractView* m_extract;
  BenchView* m_bench;
  QLabel* m_statusLabel;
  QVBoxLayout* m_sidebarLay = nullptr;
  QList<QPushButton*> m_nav;
};

}  // namespace kolma::ui
