// kolma desktop -- the Tools view: everyday utilities that need no compression.
// Inspect any file (what it is, how compressible, its checksums) and peek inside
// any archive without extracting. Built for people who just want answers.
#pragma once

#include <QWidget>
#include <QString>
#include <QVariantMap>
#include <QVariantList>

class QLineEdit;
class QLabel;
class QPushButton;
class QComboBox;
class QProgressBar;
class QTreeWidget;
class QGroupBox;

namespace kolma::ui {

class ToolsView : public QWidget {
  Q_OBJECT
 public:
  explicit ToolsView(QWidget* parent = nullptr);
  void inspectFile(const QString& path);  // select + inspect (headless QA / drop)

 signals:
  void status(const QString& msg, bool isError);

 private:
  void chooseFile();
  void inspect();
  void onInspectProgress(int pct, const QString& line);
  void onInspected(bool ok, const QString& err, const QVariantMap& m);
  void chooseArchive();
  void onListed(const QVariantList& entries);
  void testArchive();
  void onTested(bool ok, const QString& err, const QVariantMap& m);
  void chooseSplitFile();
  void split();
  void onSplit(bool ok, const QString& err, const QVariantMap& m);

  QString m_file;
  QString m_archive;
  QLineEdit* m_path;
  QPushButton* m_inspectBtn;
  QProgressBar* m_bar;
  QGroupBox* m_result;
  QLabel* m_verdict;
  QLabel* m_details;
  QLabel* m_hashes;
  QLabel* m_archName;
  QTreeWidget* m_tree;
  QPushButton* m_testBtn;
  QLabel* m_testResult;
  QString m_splitFile;
  QLineEdit* m_splitPath;
  QComboBox* m_partSize;
  QPushButton* m_splitBtn;
  QLabel* m_splitResult;
  bool m_busy = false;
};

}  // namespace kolma::ui
