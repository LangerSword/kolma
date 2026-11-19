#include "worker.hpp"

#include <QThread>

#include "kolma/analyzer.hpp"
#include "kolma/archive.hpp"
#include "kolma/bundle.hpp"
#include "kolma/engine.hpp"
#include "kolma/util.hpp"

namespace kolma::ui {

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }

kolma::Priority toPriority(int p) {
  switch (p) {
    case 0: return kolma::Priority::MaxRatio;
    case 2: return kolma::Priority::MaxSpeed;
    default: return kolma::Priority::Balanced;
  }
}

QVariantMap statsToMap(const kolma::Stats& s) {
  QVariantMap m;
  m["algorithm"] = qs(s.algorithm);
  m["algorithm_name"] = qs(s.algorithm_name);
  m["level"] = s.level;
  m["orig_size"] = qulonglong(s.orig_size);
  m["comp_size"] = qulonglong(s.comp_size);
  m["ratio"] = s.ratio;
  m["elapsed_ms"] = s.elapsed_ms;
  m["throughput"] = s.throughput_mbps;
  m["chunks"] = s.chunks;
  m["threads"] = qulonglong(s.threads);
  m["encrypted"] = s.encrypted;
  m["authenticated"] = s.authenticated;
  m["bundle"] = s.bundle;
  m["verified"] = s.verified;
  m["output"] = qs(s.output_path);
  m["orig_crc"] = qs(kolma::crc32_hex(s.orig_crc));
  return m;
}
}  // namespace

EngineWorker::EngineWorker(JobSpec spec, QObject* parent)
    : QObject(parent), m_spec(std::move(spec)) {}

void EngineWorker::run() {
  auto onProgress = [this](const kolma::Progress& p) {
    int pct = p.chunks_total > 0 ? int(p.chunks_done * 100 / p.chunks_total) : 0;
    QString line = QString("%1%   ·   %2   ·   ratio %3")
                       .arg(pct)
                       .arg(qs(kolma::human_rate(p.throughput_mbps)))
                       .arg(p.ratio, 0, 'f', 4);
    emit progress(pct, line);
  };

  const std::string password = m_spec.password.toStdString();
  const std::string output = m_spec.output.toStdString();
  const std::string archive = m_spec.archive.toStdString();

  kolma::Result r;
  switch (m_spec.kind) {
    case JobSpec::CompressFile: {
      kolma::Job job;
      job.mode = kolma::Mode::Compress;
      job.input = m_spec.inputs.value(0).toStdString();
      job.output = output;
      job.algorithm = m_spec.algorithm.toStdString();
      job.level = m_spec.level;
      job.priority = toPriority(m_spec.priority);
      job.password = password;
      job.threads = size_t(m_spec.threads);
      job.force = m_spec.force;
      r = kolma::run_job(job, onProgress);
      break;
    }
    case JobSpec::CompressBundle: {
      kolma::BundleOptions opts;
      opts.algorithm = m_spec.algorithm.toStdString();
      opts.level = m_spec.level;
      opts.priority = toPriority(m_spec.priority);
      opts.password = password;
      opts.threads = size_t(m_spec.threads);
      std::vector<std::string> inputs;
      for (const QString& s : m_spec.inputs) inputs.push_back(s.toStdString());
      r = kolma::compress_bundle(inputs, output, opts, onProgress);
      break;
    }
    case JobSpec::Extract: {
      r = kolma::extract_archive(archive, output, password, m_spec.force, onProgress);
      break;
    }
    case JobSpec::List: {
      std::vector<kolma::BundleEntry> entries;
      std::string err;
      bool ok = kolma::list_bundle(archive, password, entries, err);
      QVariantList list;
      for (const auto& e : entries) {
        QVariantMap m;
        m["path"] = qs(e.path);
        m["size"] = qulonglong(e.size);
        m["is_dir"] = e.is_dir;
        list.append(m);
      }
      emit listed(list);
      emit finished(ok, qs(err), QVariantMap{});
      return;
    }
    case JobSpec::Benchmark: {
      r = kolma::benchmark_file(archive, m_spec.level, size_t(m_spec.threads), 8u << 20,
                                onProgress);
      QVariantList rows;
      for (const auto& b : r.benchmark) {
        QVariantMap m;
        m["algo"] = qs(b.algo_id);
        m["name"] = qs(b.algo_name);
        m["ratio"] = b.ratio;
        m["output_bytes"] = qulonglong(b.output_bytes);
        m["throughput"] = b.throughput_mbps;
        m["best_ratio"] = b.best_ratio;
        m["best_speed"] = b.best_speed;
        rows.append(m);
      }
      emit benchmarked(rows);
      break;
    }
  }
  emit finished(r.ok, qs(r.error), statsToMap(r.stats));
}

PendingJob prepareJob(const JobSpec& spec) {
  auto* thread = new QThread;
  auto* worker = new EngineWorker(spec);
  worker->moveToThread(thread);
  QObject::connect(thread, &QThread::started, worker, &EngineWorker::run);
  QObject::connect(worker, &EngineWorker::finished, thread, &QThread::quit);
  QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
  QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  return {thread, worker};
}

}  // namespace kolma::ui
