// kolma desktop -- the Benchmark view: race all seven methods on a real file.
#pragma once

#include <QWidget>
#include <QString>

class QTableWidget;
class QLabel;
class QPushButton;
class QProgressBar;

namespace kolma::ui {

class BenchView : public QWidget {
  Q_OBJECT
 public:
  explicit BenchView(QWidget* parent = nullptr);

 signals:
  void status(const QString& msg, bool isError);

 private:
  void choose();
  void start();
  void onProgress(int pct, const QString& line);
  void onRows(const QVariantList& rows);
  void onFinished(bool ok, const QString& error, const QVariantMap& result);

  QString m_file;
  QLabel* m_fileLabel;
  QPushButton* m_go;
  QProgressBar* m_bar;
  QLabel* m_progressLine;
  QTableWidget* m_table;
};

}  // namespace kolma::ui
