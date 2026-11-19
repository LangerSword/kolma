// kolma desktop -- the Shrink view: make a video or image smaller while keeping
// it directly playable/viewable. This is perceptual re-encoding, not archiving.
#pragma once

#include <QWidget>
#include <QString>

class QLineEdit;
class QComboBox;
class QProgressBar;
class QLabel;
class QPushButton;
class QGroupBox;

namespace kolma::ui {

class ShrinkView : public QWidget {
  Q_OBJECT
 public:
  explicit ShrinkView(QWidget* parent = nullptr);
  void setInput(const QString& path);

 signals:
  void status(const QString& msg, bool isError);

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  void refreshInput();
  void defaultOutput();
  QString preset() const;
  void start();
  void onProgress(int pct, const QString& line);
  void onFinished(bool ok, const QString& error, const QVariantMap& result);
  void setRunning(bool running);

  QString m_input;
  QLabel* m_fileLabel;
  QComboBox* m_quality;
  QLineEdit* m_output;
  QPushButton* m_go;
  QProgressBar* m_bar;
  QLabel* m_progressLine;
  QGroupBox* m_result;
  QLabel* m_resultTitle;
  QLabel* m_resultBody;
  QPushButton* m_openFolder;
  QLabel* m_ffmpegNote;
  QString m_lastOutput;
  bool m_running = false;
};

}  // namespace kolma::ui
