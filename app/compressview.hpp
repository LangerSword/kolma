// kolma desktop -- the Compress view: drop files/folders, pick a few friendly
// options, watch it run. No flags, no jargon required.
#pragma once

#include <QWidget>
#include <QStringList>

class QListWidget;
class QLineEdit;
class QComboBox;
class QSlider;
class QCheckBox;
class QProgressBar;
class QLabel;
class QPushButton;
class QGroupBox;

namespace kolma::ui {

class CompressView : public QWidget {
  Q_OBJECT
 public:
  explicit CompressView(QWidget* parent = nullptr);
  void addPaths(const QStringList& paths);

 signals:
  void status(const QString& msg, bool isError);

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  void refreshInputs();
  void defaultOutput();
  void start();
  void onProgress(int pct, const QString& line);
  void onFinished(bool ok, const QString& error, const QVariantMap& result);
  void setRunning(bool running);

  QStringList m_inputs;
  QListWidget* m_list;
  QLineEdit* m_output;
  QComboBox* m_algo;
  QComboBox* m_priority;
  QSlider* m_level;
  QLabel* m_levelLabel;
  QCheckBox* m_encrypt;
  QLineEdit* m_password;
  QPushButton* m_go;
  QProgressBar* m_bar;
  QLabel* m_progressLine;
  QGroupBox* m_result;
  QLabel* m_resultTitle;
  QLabel* m_resultBody;
  QPushButton* m_openFolder;
  QString m_lastOutput;
  bool m_running = false;
};

}  // namespace kolma::ui
