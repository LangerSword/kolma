#include "toolsview.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "kolma/util.hpp"
#include "theme.hpp"
#include "worker.hpp"

namespace kolma::ui {

namespace {
QString hs(uint64_t b) { return QString::fromStdString(kolma::human_size(b)); }
}  // namespace

ToolsView::ToolsView(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(16);

  auto* title = new QLabel("Tools");
  title->setObjectName("h1");
  auto* sub = new QLabel("Quick answers about your files — no compression required.");
  sub->setObjectName("sub");
  sub->setWordWrap(true);
  root->addWidget(title);
  root->addWidget(sub);

  // ---- Inspect a file -----------------------------------------------------
  auto* inspectBox = new QGroupBox("Inspect a file");
  auto* il = new QVBoxLayout(inspectBox);
  il->setSpacing(10);
  auto* pickRow = new QHBoxLayout;
  m_path = new QLineEdit;
  m_path->setReadOnly(true);
  m_path->setPlaceholderText("Choose a file to inspect");
  auto* choose = new QPushButton("Choose file…");
  m_inspectBtn = new QPushButton("Inspect");
  m_inspectBtn->setObjectName("primary");
  m_inspectBtn->setEnabled(false);
  pickRow->addWidget(m_path, 1);
  pickRow->addWidget(choose);
  pickRow->addWidget(m_inspectBtn);
  il->addLayout(pickRow);

  m_bar = new QProgressBar;
  m_bar->setRange(0, 100);
  m_bar->setValue(0);
  m_bar->hide();
  il->addWidget(m_bar);

  m_result = new QGroupBox("What we found");
  auto* rl = new QVBoxLayout(m_result);
  rl->setSpacing(8);
  m_verdict = new QLabel;
  m_verdict->setObjectName("h2");
  m_verdict->setWordWrap(true);
  m_details = new QLabel;
  m_details->setObjectName("sub");
  m_details->setWordWrap(true);
  m_hashes = new QLabel;
  m_hashes->setObjectName("sub");
  m_hashes->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_hashes->setWordWrap(true);
  QFont mono;
  mono.setFamilies({"DejaVu Sans Mono", "Noto Sans Mono", "Consolas", "Menlo", "monospace"});
  mono.setStyleHint(QFont::Monospace);
  m_hashes->setFont(mono);
  rl->addWidget(m_verdict);
  rl->addWidget(m_details);
  rl->addWidget(m_hashes);
  m_result->hide();
  il->addWidget(m_result);
  root->addWidget(inspectBox);

  connect(choose, &QPushButton::clicked, this, &ToolsView::chooseFile);
  connect(m_inspectBtn, &QPushButton::clicked, this, &ToolsView::inspect);

  // ---- Split into volumes -------------------------------------------------
  auto* splitBox = new QGroupBox("Split into volumes");
  auto* sl = new QVBoxLayout(splitBox);
  sl->setSpacing(10);
  auto* splitDesc = new QLabel(
      "Cut a large file into numbered parts (.001, .002, …) for sharing or a FAT32 "
      "drive. Extract or rejoin them later — the original is left untouched.");
  splitDesc->setObjectName("sub");
  splitDesc->setWordWrap(true);
  sl->addWidget(splitDesc);
  auto* splitPickRow = new QHBoxLayout;
  m_splitPath = new QLineEdit;
  m_splitPath->setReadOnly(true);
  m_splitPath->setPlaceholderText("Choose a file to split");
  auto* splitChoose = new QPushButton("Choose file…");
  splitPickRow->addWidget(m_splitPath, 1);
  splitPickRow->addWidget(splitChoose);
  sl->addLayout(splitPickRow);
  auto* splitOptRow = new QHBoxLayout;
  auto* partLabel = new QLabel("Part size");
  m_partSize = new QComboBox;
  m_partSize->addItem("1 MB", qulonglong(1 << 20));
  m_partSize->addItem("10 MB", qulonglong(10 << 20));
  m_partSize->addItem("100 MB", qulonglong(100 << 20));
  m_partSize->addItem("700 MB (CD)", qulonglong(700 << 20));
  m_partSize->addItem("4 GB (FAT32-safe)", qulonglong((4ull << 30) - 1));
  m_partSize->setCurrentIndex(2);
  m_splitBtn = new QPushButton("Split");
  m_splitBtn->setObjectName("primary");
  m_splitBtn->setEnabled(false);
  splitOptRow->addWidget(partLabel);
  splitOptRow->addWidget(m_partSize);
  splitOptRow->addStretch(1);
  splitOptRow->addWidget(m_splitBtn);
  sl->addLayout(splitOptRow);
  m_splitResult = new QLabel;
  m_splitResult->setObjectName("sub");
  m_splitResult->setWordWrap(true);
  m_splitResult->hide();
  sl->addWidget(m_splitResult);
  root->addWidget(splitBox);
  connect(splitChoose, &QPushButton::clicked, this, &ToolsView::chooseSplitFile);
  connect(m_splitBtn, &QPushButton::clicked, this, &ToolsView::split);

  // ---- Peek inside an archive --------------------------------------------
  auto* peekBox = new QGroupBox("Peek inside an archive");
  auto* pl = new QVBoxLayout(peekBox);
  pl->setSpacing(10);
  auto* archRow = new QHBoxLayout;
  auto* archChoose = new QPushButton("Choose archive…");
  m_archName = new QLabel("See what’s inside a .zip, .7z, .tar.gz or .kolma without extracting it.");
  m_archName->setObjectName("dim");
  m_archName->setWordWrap(true);
  archRow->addWidget(archChoose);
  archRow->addWidget(m_archName, 1);
  pl->addLayout(archRow);
  m_tree = new QTreeWidget;
  m_tree->setHeaderLabels({"Name", "Size"});
  m_tree->setRootIsDecorated(false);
  m_tree->setAlternatingRowColors(true);
  m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_tree->setMinimumHeight(120);
  pl->addWidget(m_tree, 1);
  root->addWidget(peekBox, 1);

  connect(archChoose, &QPushButton::clicked, this, &ToolsView::chooseArchive);
}

void ToolsView::chooseFile() {
  QString f = QFileDialog::getOpenFileName(this, "Choose a file to inspect");
  if (f.isEmpty()) return;
  m_file = f;
  m_path->setText(f);
  m_inspectBtn->setEnabled(true);
  m_result->hide();
}

void ToolsView::inspectFile(const QString& path) {
  m_file = path;
  m_path->setText(path);
  m_inspectBtn->setEnabled(true);
  m_result->hide();
  inspect();
}

void ToolsView::inspect() {
  if (m_file.isEmpty() || m_busy) return;
  m_busy = true;
  m_inspectBtn->setEnabled(false);
  m_result->hide();
  m_bar->setValue(0);
  m_bar->show();
  emit status("Inspecting " + QFileInfo(m_file).fileName() + "…", false);
  JobSpec spec;
  spec.kind = JobSpec::Inspect;
  spec.inputs = QStringList{m_file};
  PendingJob job = prepareJob(spec);
  connect(job.worker, &EngineWorker::progress, this, &ToolsView::onInspectProgress);
  connect(job.worker, &EngineWorker::finished, this, &ToolsView::onInspected);
  static_cast<QThread*>(job.thread)->start();
}

void ToolsView::onInspectProgress(int pct, const QString&) { m_bar->setValue(pct); }

void ToolsView::onInspected(bool ok, const QString& err, const QVariantMap& m) {
  m_busy = false;
  m_inspectBtn->setEnabled(true);
  m_bar->hide();
  if (!ok) {
    emit status(err, true);
    m_result->show();
    m_verdict->setText("Couldn’t inspect that file");
    m_verdict->setStyleSheet(QString("color:%1;").arg(Palette::bad));
    m_details->setText(err);
    m_hashes->clear();
    return;
  }
  m_result->show();
  bool storeRaw = m.value("store_raw").toBool();
  double comp = m.value("compressibility").toDouble();
  QString type = m.value("type").toString();
  QString detail = m.value("type_detail").toString();
  QString recommended = m.value("recommended").toString();

  if (storeRaw) {
    m_verdict->setText("Already compressed — archiving won’t shrink it much");
    m_verdict->setStyleSheet(QString("color:%1;").arg(Palette::warn));
  } else {
    m_verdict->setText(QString("Compressible — about %1% smaller is realistic")
                           .arg(comp * 100.0, 0, 'f', 0));
    m_verdict->setStyleSheet(QString("color:%1;").arg(Palette::accent));
  }

  uint64_t size = m.value("size").toULongLong();
  double entropy = m.value("entropy").toDouble();
  qint64 mtime = m.value("mtime").toLongLong();
  QString typeStr = detail.isEmpty() ? type : QString("%1 (%2)").arg(type, detail);
  QString details = QString("Type: %1   ·   Size: %2   ·   Entropy: %3 / 8 bits per byte")
                        .arg(typeStr, hs(size))
                        .arg(entropy, 0, 'f', 2);
  if (mtime > 0)
    details += "\nModified: " + QDateTime::fromSecsSinceEpoch(mtime).toString("yyyy-MM-dd hh:mm");
  if (!storeRaw)
    details += "\nRecommended method: " + recommended;
  QString rationale = m.value("rationale").toString();
  if (!rationale.isEmpty()) details += "\n" + rationale;
  m_details->setText(details);

  m_hashes->setText(QString("CRC-32:  %1\nSHA-256: %2")
                        .arg(m.value("crc32").toString(), m.value("sha256").toString()));
  emit status("Inspected " + m.value("name").toString(), false);
}

void ToolsView::chooseArchive() {
  QString f = QFileDialog::getOpenFileName(this, "Choose an archive to peek inside", QString(),
                                           "Archives (*.zip *.7z *.tar *.tar.gz *.tgz *.tar.xz "
                                           "*.tar.bz2 *.kolma);;All files (*)");
  if (f.isEmpty()) return;
  m_archive = f;
  m_archName->setText(QFileInfo(f).fileName());
  m_tree->clear();
  emit status("Reading " + QFileInfo(f).fileName() + "…", false);
  JobSpec spec;
  spec.kind = JobSpec::List;
  spec.archive = f;
  PendingJob job = prepareJob(spec);
  connect(job.worker, &EngineWorker::listed, this, &ToolsView::onListed);
  connect(job.worker, &EngineWorker::finished, this,
          [this](bool ok, const QString& err, const QVariantMap&) {
            if (!ok && !err.isEmpty()) {
              auto* item = new QTreeWidgetItem(m_tree);
              item->setText(0, "Couldn’t read archive: " + err);
              emit status(err, true);
            }
          });
  static_cast<QThread*>(job.thread)->start();
}

void ToolsView::onListed(const QVariantList& entries) {
  m_tree->clear();
  uint64_t total = 0;
  for (const QVariant& v : entries) {
    QVariantMap m = v.toMap();
    auto* item = new QTreeWidgetItem(m_tree);
    QString path = m.value("path").toString();
    bool isDir = m.value("is_dir").toBool();
    uint64_t size = m.value("size").toULongLong();
    item->setText(0, isDir ? path + "  (folder)" : path);
    item->setText(1, isDir ? "—" : hs(size));
    total += size;
  }
  if (entries.isEmpty()) {
    auto* item = new QTreeWidgetItem(m_tree);
    item->setText(0, "(empty archive)");
  }
  emit status(QString("%1 entries · %2 uncompressed").arg(entries.size()).arg(hs(total)), false);
}

void ToolsView::chooseSplitFile() {
  QString f = QFileDialog::getOpenFileName(this, "Choose a file to split");
  if (f.isEmpty()) return;
  m_splitFile = f;
  m_splitPath->setText(f);
  m_splitBtn->setEnabled(true);
  m_splitResult->hide();
}

void ToolsView::split() {
  if (m_splitFile.isEmpty() || m_busy) return;
  m_busy = true;
  m_splitBtn->setEnabled(false);
  m_splitResult->hide();
  emit status("Splitting " + QFileInfo(m_splitFile).fileName() + "…", false);
  JobSpec spec;
  spec.kind = JobSpec::Split;
  spec.inputs = QStringList{m_splitFile};
  spec.part_size = m_partSize->currentData().toLongLong();
  PendingJob job = prepareJob(spec);
  connect(job.worker, &EngineWorker::finished, this, &ToolsView::onSplit);
  static_cast<QThread*>(job.thread)->start();
}

void ToolsView::onSplit(bool ok, const QString& err, const QVariantMap& m) {
  m_busy = false;
  m_splitBtn->setEnabled(true);
  m_splitResult->show();
  if (!ok) {
    m_splitResult->setText("Couldn’t split that file: " + err);
    m_splitResult->setStyleSheet(QString("color:%1;").arg(Palette::bad));
    emit status(err, true);
    return;
  }
  int n = m.value("volumes").toInt();
  QString base = m.value("base").toString();
  m_splitResult->setText(QString("Created %1 volumes:  %2.001 … %2.%3")
                             .arg(n)
                             .arg(base)
                             .arg(n, 3, 10, QChar('0')));
  m_splitResult->setStyleSheet(QString("color:%1;").arg(Palette::accent));
  emit status(QString("Split into %1 volumes").arg(n), false);
}

}  // namespace kolma::ui
