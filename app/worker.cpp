#include "worker.hpp"

#include <QThread>

#include <filesystem>
#include <fstream>

#include "kolma/analyzer.hpp"
#include "kolma/archive.hpp"
#include "kolma/bundle.hpp"
#include "kolma/engine.hpp"
#include "kolma/file.hpp"
#include "kolma/stdarchive.hpp"
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
    case JobSpec::CompressStd: {
      kolma::StdPackOptions opts;
      opts.format = m_spec.format.toStdString();
      opts.level = m_spec.level > 0 ? m_spec.level : 6;
      opts.force = m_spec.force;
      std::vector<std::string> inputs;
      for (const QString& s : m_spec.inputs) inputs.push_back(s.toStdString());
      std::string err;
      bool ok = kolma::std_pack(inputs, output, opts, err, onProgress);
      r.ok = ok;
      r.error = err;
      r.stats.output_path = output;
      r.stats.algorithm = m_spec.format.toStdString();
      r.stats.algorithm_name = r.stats.algorithm + " (standard)";
      r.stats.verified = ok;
      r.stats.bundle = inputs.size() > 1;
      if (ok) {
        std::error_code ec;
        r.stats.comp_size = std::filesystem::file_size(output, ec);
        uint64_t orig = 0;
        for (const auto& p : inputs) {
          std::error_code ec2;
          if (std::filesystem::is_directory(p, ec2)) {
            for (auto it = std::filesystem::recursive_directory_iterator(p, ec2);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec2)) {
              if (it->is_regular_file(ec2)) orig += it->file_size(ec2);
            }
          } else if (std::filesystem::is_regular_file(p, ec2)) {
            orig += std::filesystem::file_size(p, ec2);
          }
        }
        r.stats.orig_size = orig;
        r.stats.ratio = orig ? double(r.stats.comp_size) / double(orig) : 1.0;
      }
      break;
    }
    case JobSpec::Extract: {
      if (kolma::std_is_standard(archive)) {
        std::string err;
        bool ok = kolma::std_extract(archive, output, m_spec.force, err, onProgress);
        r.ok = ok;
        r.error = err;
        if (ok) {
          r.stats.output_path = output;
          r.stats.verified = true;
          r.stats.algorithm_name = "standard archive";
        }
      } else {
        r = kolma::extract_archive(archive, output, password, m_spec.force, onProgress);
      }
      break;
    }
    case JobSpec::List: {
      QVariantList list;
      std::string err;
      bool ok = false;
      if (kolma::std_is_standard(archive)) {
        std::vector<kolma::StdEntry> entries;
        ok = kolma::std_list(archive, entries, err);
        for (const auto& e : entries) {
          QVariantMap m;
          m["path"] = qs(e.path);
          m["size"] = qulonglong(e.size);
          m["is_dir"] = e.is_dir;
          list.append(m);
        }
      } else {
        std::vector<kolma::BundleEntry> entries;
        ok = kolma::list_bundle(archive, password, entries, err);
        for (const auto& e : entries) {
          QVariantMap m;
          m["path"] = qs(e.path);
          m["size"] = qulonglong(e.size);
          m["is_dir"] = e.is_dir;
          list.append(m);
        }
      }
      emit listed(list);
      emit finished(ok, qs(err), QVariantMap{});
      return;
    }
    case JobSpec::Inspect: {
      const std::string path = m_spec.inputs.value(0).toStdString();
      kolma::FileInfo fi = kolma::inspect_file(path);
      QVariantMap m;
      if (!fi.exists) {
        emit finished(false, qs(fi.error.empty() ? "file not found" : fi.error), m);
        return;
      }
      m["name"] = qs(fi.name);
      m["path"] = qs(fi.path);
      m["size"] = qulonglong(fi.size);
      m["type"] = qs(kolma::file_type_name(fi.type));
      m["type_detail"] = qs(fi.type_detail);
      m["entropy"] = fi.entropy;
      m["mtime"] = qlonglong(fi.mtime);
      // Full-file checksums, streamed so huge files never balloon memory.
      uint32_t crc = 0;
      kolma::Sha256 sha;
      std::ifstream in(path, std::ios::binary);
      char buf[65536];
      uint64_t hashed = 0;
      while (in) {
        in.read(buf, sizeof buf);
        std::streamsize n = in.gcount();
        if (n <= 0) break;
        kolma::View chunk(reinterpret_cast<const uint8_t*>(buf), size_t(n));
        crc = kolma::crc32(chunk, crc);
        sha.update(chunk);
        hashed += uint64_t(n);
        int pct = fi.size ? int(hashed * 100 / fi.size) : 100;
        emit progress(pct, QString("Hashing… %1%").arg(pct));
      }
      m["crc32"] = qs(kolma::crc32_hex(crc));
      m["sha256"] = qs(kolma::to_hex(sha.final()));
      kolma::Analysis an = kolma::analyze_file(path, kolma::Priority::Balanced);
      m["compressibility"] = an.compressibility;
      m["store_raw"] = an.store_raw;
      m["recommended"] = qs(an.recommended_algo);
      m["rationale"] = qs(an.rationale);
      emit finished(true, QString(), m);
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
