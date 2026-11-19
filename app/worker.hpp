// kolma desktop -- bridges the blocking C++ engine to Qt's event loop.
// Each job runs in its own QThread; progress and results are delivered to the
// GUI thread via queued signal connections. The engine is linked directly, so
// there is no subprocess and no JSON parsing here.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>

namespace kolma::ui {

struct JobSpec {
  enum Kind { CompressFile, CompressBundle, CompressStd, Extract, List, Inspect, Benchmark };
  Kind kind = CompressFile;
  QStringList inputs;   // files/folders to compress
  QString archive;      // extract/list source
  QString output;       // compress destination file / extract destination dir
  QString password;
  QString algorithm = "auto";
  QString format = "zip";  // CompressStd: zip / tar / tar.gz / tar.xz / tar.bz2 / 7z
  int level = 0;        // 0 = auto
  int priority = 1;     // 0 maxratio, 1 balanced, 2 maxspeed
  int threads = 0;      // 0 = hardware concurrency
  bool force = false;
};

class EngineWorker : public QObject {
  Q_OBJECT
 public:
  explicit EngineWorker(JobSpec spec, QObject* parent = nullptr);

 public slots:
  void run();

 signals:
  void progress(int percent, QString line);
  void finished(bool ok, QString error, QVariantMap result);
  void listed(QVariantList entries);
  void benchmarked(QVariantList rows);

 private:
  JobSpec m_spec;
};

// A worker wired to a fresh thread, not yet started. Connect its signals, then
// call thread->start(). The thread and worker delete themselves when done.
struct PendingJob {
  QObject* thread;   // QThread*
  EngineWorker* worker;
};
PendingJob prepareJob(const JobSpec& spec);

}  // namespace kolma::ui
