// kolma desktop -- the Extract view: open an archive, see what's inside and
// whether it's intact, then extract it where you want.
#pragma once

#include <QWidget>
#include <QString>

class QTreeWidget;
class QLineEdit;
class QCheckBox;
class QProgressBar;
class QLabel;
class QPushButton;
class QGroupBox;

namespace kolma::ui {

class ExtractView : public QWidget {
  Q_OBJECT
 public:
  explicit ExtractView(QWidget* parent = nullptr);
  void openArchive(const QString& path);

 signals:
  void status(const QString& msg, bool isError);

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  void loadContents();
  void start();
  void onProgress(int pct, const QString& line);
  void onListed(const QVariantList& entries);
  void onFinished(bool ok, const QString& error, const QVariantMap& result);
  void setRunning(bool running);

  QString m_archive;
  bool m_encrypted = false;
  bool m_bundle = false;
  QLabel* m_infoTitle;
  QLabel* m_infoBody;
  QTreeWidget* m_tree;
  QLineEdit* m_dest;
  QLineEdit* m_password;
  QCheckBox* m_overwrite;
  QPushButton* m_go;
  QProgressBar* m_bar;
  QLabel* m_progressLine;
  QGroupBox* m_result;
  QLabel* m_resultTitle;
  QLabel* m_resultBody;
  QPushButton* m_openFolder;
  QString m_lastDest;
  bool m_running = false;
};

}  // namespace kolma::ui
