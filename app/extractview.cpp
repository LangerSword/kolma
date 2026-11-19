#include "extractview.hpp"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "kolma/archive.hpp"
#include "kolma/util.hpp"
#include "theme.hpp"
#include "worker.hpp"

namespace kolma::ui {

namespace {
QString hs(uint64_t b) { return QString::fromStdString(kolma::human_size(b)); }
}  // namespace

ExtractView::ExtractView(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(16);

  auto* title = new QLabel("Extract");
  title->setObjectName("h1");
  auto* sub = new QLabel("Open a .kolma archive to see what’s inside and check it’s intact, then extract.");
  sub->setObjectName("sub");
  sub->setWordWrap(true);
  root->addWidget(title);
  root->addWidget(sub);

  auto* openRow = new QHBoxLayout;
  auto* open = new QPushButton("Open archive…");
  open->setObjectName("primary");
  openRow->addWidget(open);
  auto* dropHint = new QLabel("…or drop a .kolma file anywhere on this page");
  dropHint->setObjectName("dim");
  openRow->addWidget(dropHint, 1);
  root->addLayout(openRow);
  connect(open, &QPushButton::clicked, this, [this] {
    QString f = QFileDialog::getOpenFileName(this, "Open archive", QString(),
                                             "kolma archive (*.kolma);;All files (*)");
    if (!f.isEmpty()) openArchive(f);
  });

  // Info card.
  auto* info = new QGroupBox("Archive");
  auto* infoLay = new QVBoxLayout(info);
  m_infoTitle = new QLabel("No archive open");
  m_infoTitle->setObjectName("h2");
  m_infoBody = new QLabel("Open or drop a .kolma archive to begin.");
  m_infoBody->setObjectName("sub");
  m_infoBody->setWordWrap(true);
  infoLay->addWidget(m_infoTitle);
  infoLay->addWidget(m_infoBody);
  root->addWidget(info);

  m_tree = new QTreeWidget;
  m_tree->setHeaderLabels({"Name", "Size"});
  m_tree->setRootIsDecorated(false);
  m_tree->setAlternatingRowColors(true);
  m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_tree->setMinimumHeight(140);
  root->addWidget(m_tree, 1);

  // Destination + password.
  auto* destRow = new QHBoxLayout;
  destRow->addWidget(new QLabel("Extract to"));
  m_dest = new QLineEdit;
  auto* browse = new QPushButton("Browse…");
  destRow->addWidget(m_dest, 1);
  destRow->addWidget(browse);
  root->addLayout(destRow);
  connect(browse, &QPushButton::clicked, this, [this] {
    QString d = QFileDialog::getExistingDirectory(this, "Extract to", m_dest->text());
    if (!d.isEmpty()) m_dest->setText(d);
  });

  auto* pwRow = new QHBoxLayout;
  pwRow->addWidget(new QLabel("Password"));
  m_password = new QLineEdit;
  m_password->setEchoMode(QLineEdit::Password);
  m_password->setPlaceholderText("only if the archive is encrypted");
  m_password->setEnabled(false);
  pwRow->addWidget(m_password, 1);
  m_overwrite = new QCheckBox("Overwrite existing files");
  pwRow->addWidget(m_overwrite);
  root->addLayout(pwRow);

  m_go = new QPushButton("Extract");
  m_go->setObjectName("primary");
  m_go->setMinimumHeight(46);
  m_go->setEnabled(false);
  root->addWidget(m_go);
  m_bar = new QProgressBar;
  m_bar->setRange(0, 100);
  m_bar->setValue(0);
  m_progressLine = new QLabel(" ");
  m_progressLine->setObjectName("dim");
  root->addWidget(m_bar);
  root->addWidget(m_progressLine);

  m_result = new QGroupBox("Result");
  auto* resLay = new QVBoxLayout(m_result);
  m_resultTitle = new QLabel;
  m_resultTitle->setObjectName("h2");
  m_resultBody = new QLabel;
  m_resultBody->setObjectName("sub");
  m_resultBody->setWordWrap(true);
  m_openFolder = new QPushButton("Open folder");
  resLay->addWidget(m_resultTitle);
  resLay->addWidget(m_resultBody);
  resLay->addWidget(m_openFolder, 0, Qt::AlignLeft);
  m_result->hide();
  root->addWidget(m_result);

  connect(m_go, &QPushButton::clicked, this, &ExtractView::start);
  connect(m_openFolder, &QPushButton::clicked, this, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastDest));
  });
}

void ExtractView::openArchive(const QString& path) {
  kolma::ArchiveInfo info = kolma::read_archive_info(path.toStdString(), false);
  if (!info.ok) {
    emit status(QString::fromStdString(info.error), true);
    m_infoTitle->setText("Can’t open archive");
    m_infoBody->setText(QString::fromStdString(info.error));
    m_go->setEnabled(false);
    return;
  }
  m_archive = path;
  m_encrypted = info.encrypted;
  m_bundle = info.bundle;
  QFileInfo fi(path);
  m_infoTitle->setText(fi.fileName());
  QString body = QString("%1  ·  %2  ·  %3")
                     .arg(hs(info.header.orig_size))
                     .arg(QString::fromStdString(info.algorithm_id))
                     .arg(m_bundle ? "folder / multiple files" : "single file");
  if (m_encrypted) {
    body += info.authenticated ? "\nEncrypted · authenticated ✓" : "\nEncrypted · legacy (no authentication)";
  } else {
    body += "\nNot encrypted";
  }
  body += info.header_consistent ? "\nHeader consistent ✓" : "\nHeader INCONSISTENT ✗";
  m_infoBody->setText(body);
  m_password->setEnabled(m_encrypted);
  m_dest->setText(fi.absolutePath());
  m_go->setEnabled(true);
  m_result->hide();
  m_tree->clear();
  emit status("Opened " + fi.fileName(), false);
  loadContents();
}

void ExtractView::loadContents() {
  JobSpec spec;
  spec.kind = JobSpec::List;
  spec.archive = m_archive;
  spec.password = m_password->text();
  PendingJob job = prepareJob(spec);
  connect(job.worker, &EngineWorker::listed, this, &ExtractView::onListed);
  connect(job.worker, &EngineWorker::finished, this,
          [this](bool ok, const QString& err, const QVariantMap&) {
            if (!ok && !err.isEmpty()) {
              auto* item = new QTreeWidgetItem(m_tree);
              item->setText(0, "Contents unavailable: " + err);
            }
          });
  static_cast<QThread*>(job.thread)->start();
}

void ExtractView::onListed(const QVariantList& entries) {
  m_tree->clear();
  for (const QVariant& v : entries) {
    QVariantMap m = v.toMap();
    auto* item = new QTreeWidgetItem(m_tree);
    QString path = m.value("path").toString();
    bool isDir = m.value("is_dir").toBool();
    item->setText(0, isDir ? path + "  (folder)" : path);
    item->setText(1, isDir ? "—" : hs(m.value("size").toULongLong()));
  }
  if (entries.isEmpty()) {
    auto* item = new QTreeWidgetItem(m_tree);
    item->setText(0, "(empty)");
  }
}

void ExtractView::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void ExtractView::dropEvent(QDropEvent* e) {
  for (const QUrl& u : e->mimeData()->urls()) {
    if (u.isLocalFile()) {
      openArchive(u.toLocalFile());
      break;
    }
  }
  e->acceptProposedAction();
}

void ExtractView::setRunning(bool running) {
  m_running = running;
  m_go->setEnabled(!running && !m_archive.isEmpty());
  m_go->setText(running ? "Extracting…" : "Extract");
}

void ExtractView::start() {
  if (m_archive.isEmpty()) {
    emit status("Open an archive first.", true);
    return;
  }
  QString dest = m_dest->text().trimmed();
  if (dest.isEmpty()) {
    emit status("Choose a destination folder.", true);
    return;
  }
  if (m_encrypted && m_password->text().isEmpty()) {
    emit status("This archive is encrypted — enter its password.", true);
    return;
  }
  JobSpec spec;
  spec.kind = JobSpec::Extract;
  spec.archive = m_archive;
  spec.output = dest;
  spec.password = m_password->text();
  spec.force = m_overwrite->isChecked();

  m_result->hide();
  m_bar->setValue(0);
  m_progressLine->setText("Starting…");
  setRunning(true);
  emit status("Extracting…", false);
  PendingJob job = prepareJob(spec);
  connect(job.worker, &EngineWorker::progress, this, &ExtractView::onProgress);
  connect(job.worker, &EngineWorker::finished, this, &ExtractView::onFinished);
  static_cast<QThread*>(job.thread)->start();
}

void ExtractView::onProgress(int pct, const QString& line) {
  m_bar->setValue(pct);
  m_progressLine->setText(line);
}

void ExtractView::onFinished(bool ok, const QString& error, const QVariantMap& result) {
  setRunning(false);
  if (!ok) {
    m_bar->setValue(0);
    m_progressLine->setText(" ");
    m_result->show();
    m_resultTitle->setText("Couldn’t extract");
    m_resultTitle->setStyleSheet(QString("color:%1;").arg(Palette::bad));
    m_resultBody->setText(error);
    m_openFolder->hide();
    emit status(error, true);
    return;
  }
  m_bar->setValue(100);
  m_progressLine->setText("Done.");
  m_lastDest = result.value("output").toString();
  if (m_lastDest.isEmpty()) m_lastDest = m_dest->text();
  bool verified = result.value("verified").toBool();
  m_result->show();
  m_resultTitle->setText(verified ? "Extracted ✓" : "Extracted (checksum not verified)");
  m_resultTitle->setStyleSheet(QString("color:%1;").arg(verified ? Palette::accent : Palette::warn));
  m_resultBody->setText("Contents written to:\n" + m_lastDest +
                        (verified ? "\nIntegrity verified against the archive checksum." : ""));
  m_openFolder->show();
  emit status("Extracted to " + m_lastDest, false);
}

}  // namespace kolma::ui
