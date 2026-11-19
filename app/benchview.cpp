#include "benchview.hpp"

#include <algorithm>

#include <QColor>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

#include "kolma/util.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "worker.hpp"

namespace kolma::ui {

namespace {
QString hs(uint64_t b) { return QString::fromStdString(kolma::human_size(b)); }
QString hr(double mbps) { return QString::fromStdString(kolma::human_rate(mbps)); }
}  // namespace

BenchView::BenchView(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(16);

  auto* title = new QLabel("Benchmark");
  title->setObjectName("h1");
  auto* sub = new QLabel("Race all seven methods on one of your files and see the real trade-off.");
  sub->setObjectName("sub");
  root->addWidget(title);
  root->addWidget(sub);

  auto* row = new QHBoxLayout;
  auto* choose = new QPushButton("Choose a file…");
  m_fileLabel = new QLabel("No file selected");
  m_fileLabel->setObjectName("dim");
  m_go = new QPushButton("Run benchmark");
  m_go->setObjectName("primary");
  m_go->setEnabled(false);
  row->addWidget(choose);
  row->addWidget(m_fileLabel, 1);
  row->addWidget(m_go);
  root->addLayout(row);
  connect(choose, &QPushButton::clicked, this, &BenchView::choose);
  connect(m_go, &QPushButton::clicked, this, &BenchView::start);

  m_bar = new QProgressBar;
  m_bar->setRange(0, 100);
  m_bar->setValue(0);
  m_progressLine = new QLabel(" ");
  m_progressLine->setObjectName("dim");
  root->addWidget(m_bar);
  root->addWidget(m_progressLine);

  m_table = new QTableWidget(0, 6);
  m_table->setHorizontalHeaderLabels({"Method", "Ratio", "", "Output", "Throughput", "Note"});
  m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
  m_table->verticalHeader()->setVisible(false);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setAlternatingRowColors(true);
  root->addWidget(m_table, 1);

  auto* legend = new QLabel("Ratio = compressed ÷ original. Lower is better. Bars are scaled to the worst method.");
  legend->setObjectName("dim");
  legend->setWordWrap(true);
  root->addWidget(legend);
}

void BenchView::choose() {
  QString f = QFileDialog::getOpenFileName(this, "Choose a file to benchmark");
  if (f.isEmpty()) return;
  m_file = f;
  m_fileLabel->setText(QFileInfo(f).fileName() + "  ·  " + hs(uint64_t(QFileInfo(f).size())));
  m_go->setEnabled(true);
}

void BenchView::start() {
  if (m_file.isEmpty()) return;
  JobSpec spec;
  spec.kind = JobSpec::Benchmark;
  spec.archive = m_file;
  spec.threads = prefThreads();
  m_table->setRowCount(0);
  m_bar->setValue(0);
  m_progressLine->setText("Running…");
  m_go->setEnabled(false);
  emit status("Benchmarking…", false);
  PendingJob job = prepareJob(spec);
  connect(job.worker, &EngineWorker::progress, this, &BenchView::onProgress);
  connect(job.worker, &EngineWorker::benchmarked, this, &BenchView::onRows);
  connect(job.worker, &EngineWorker::finished, this, &BenchView::onFinished);
  static_cast<QThread*>(job.thread)->start();
}

void BenchView::onProgress(int pct, const QString& line) {
  m_bar->setValue(pct);
  m_progressLine->setText(line);
}

void BenchView::onRows(const QVariantList& rows) {
  double maxRatio = 0.0;
  for (const QVariant& v : rows) maxRatio = std::max(maxRatio, v.toMap().value("ratio").toDouble());
  m_table->setRowCount(int(rows.size()));
  int r = 0;
  for (const QVariant& v : rows) {
    QVariantMap m = v.toMap();
    double ratio = m.value("ratio").toDouble();
    bool bestRatio = m.value("best_ratio").toBool();
    bool bestSpeed = m.value("best_speed").toBool();
    QString note = bestRatio ? "best ratio" : (bestSpeed ? "fastest" : "");

    auto* name = new QTableWidgetItem(m.value("algo").toString());
    if (bestRatio) name->setForeground(QColor(Palette::accent));
    m_table->setItem(r, 0, name);
    m_table->setItem(r, 1, new QTableWidgetItem(QString::number(ratio, 'f', 4)));

    auto* bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setValue(maxRatio > 0 ? int(ratio / maxRatio * 1000) : 0);
    bar->setTextVisible(false);
    bar->setFixedHeight(14);
    m_table->setCellWidget(r, 2, bar);

    m_table->setItem(r, 3, new QTableWidgetItem(hs(m.value("output_bytes").toULongLong())));
    m_table->setItem(r, 4, new QTableWidgetItem(hr(m.value("throughput").toDouble())));
    auto* noteItem = new QTableWidgetItem(note);
    if (bestRatio) noteItem->setForeground(QColor(Palette::accent));
    if (bestSpeed) noteItem->setForeground(QColor(Palette::info));
    m_table->setItem(r, 5, noteItem);
    m_table->setRowHeight(r, 34);
    ++r;
  }
}

void BenchView::onFinished(bool ok, const QString& error, const QVariantMap&) {
  m_go->setEnabled(true);
  if (!ok) {
    m_progressLine->setText(" ");
    emit status(error, true);
    return;
  }
  m_bar->setValue(100);
  m_progressLine->setText("Done.");
  emit status("Benchmark complete.", false);
}

}  // namespace kolma::ui
