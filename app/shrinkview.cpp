#include "shrinkview.hpp"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include "kolma/file.hpp"
#include "kolma/media.hpp"
#include "kolma/util.hpp"
#include "theme.hpp"
#include "worker.hpp"

namespace kolma::ui {

namespace {
QString hs(uint64_t b) { return QString::fromStdString(kolma::human_size(b)); }
}  // namespace

ShrinkView::ShrinkView(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(16);

  auto* title = new QLabel("Shrink media");
  title->setObjectName("h1");
  auto* sub = new QLabel("Make a video or photo smaller without zipping it. The result stays a "
                         "normal file you can play or view — kolma re-encodes it, trading a little "
                         "quality for a lot of size.");
  sub->setObjectName("sub");
  sub->setWordWrap(true);
  root->addWidget(title);
  root->addWidget(sub);

  // Drop zone / picker.
  auto* drop = new QFrame;
  drop->setObjectName("drop");
  drop->setMinimumHeight(96);
  auto* dropLay = new QVBoxLayout(drop);
  auto* dropText = new QLabel("Drag & drop a video or image here");
  dropText->setObjectName("sub");
  dropText->setAlignment(Qt::AlignCenter);
  auto* btnRow = new QHBoxLayout;
  auto* choose = new QPushButton("Choose video or image…");
  auto* clear = new QPushButton("Clear");
  btnRow->addStretch();
  btnRow->addWidget(choose);
  btnRow->addWidget(clear);
  btnRow->addStretch();
  dropLay->addWidget(dropText);
  dropLay->addLayout(btnRow);
  root->addWidget(drop);

  m_fileLabel = new QLabel("No file selected.");
  m_fileLabel->setObjectName("dim");
  root->addWidget(m_fileLabel);

  connect(choose, &QPushButton::clicked, this, [this] {
    QString f = QFileDialog::getOpenFileName(
        this, "Choose a video or image", QString(),
        "Media (*.mp4 *.m4v *.mkv *.webm *.mov *.avi *.jpg *.jpeg *.png *.webp *.bmp);;All files (*)");
    if (!f.isEmpty()) setInput(f);
  });
  connect(clear, &QPushButton::clicked, this, [this] {
    m_input.clear();
    refreshInput();
  });

  // Options.
  auto* opts = new QGroupBox("Options");
  auto* form = new QVBoxLayout(opts);
  form->setSpacing(10);

  auto* qRow = new QHBoxLayout;
  qRow->addWidget(new QLabel("Quality"));
  m_quality = new QComboBox;
  m_quality->addItems({"Balanced — good size, looks great (recommended)",
                       "Keep quality — closer to the original, larger",
                       "Smallest — most savings, a little softer"});
  qRow->addWidget(m_quality, 1);
  form->addLayout(qRow);

  auto* outRow = new QHBoxLayout;
  outRow->addWidget(new QLabel("Save to"));
  m_output = new QLineEdit;
  auto* browse = new QPushButton("Browse…");
  outRow->addWidget(m_output, 1);
  outRow->addWidget(browse);
  form->addLayout(outRow);
  connect(browse, &QPushButton::clicked, this, [this] {
    QString f = QFileDialog::getSaveFileName(this, "Save shrunk file", m_output->text(),
                                             "MP4 video (*.mp4);;JPEG image (*.jpg);;All files (*)");
    if (!f.isEmpty()) m_output->setText(f);
  });
  root->addWidget(opts);

  m_ffmpegNote = new QLabel;
  m_ffmpegNote->setObjectName("dim");
  m_ffmpegNote->setWordWrap(true);
  root->addWidget(m_ffmpegNote);

  // Go + progress.
  m_go = new QPushButton("Shrink");
  m_go->setObjectName("primary");
  m_go->setMinimumHeight(46);
  root->addWidget(m_go);
  m_bar = new QProgressBar;
  m_bar->setRange(0, 100);
  m_bar->setValue(0);
  m_bar->setTextVisible(true);
  m_progressLine = new QLabel(" ");
  m_progressLine->setObjectName("dim");
  root->addWidget(m_bar);
  root->addWidget(m_progressLine);

  // Result card.
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
  root->addStretch();

  connect(m_go, &QPushButton::clicked, this, &ShrinkView::start);
  connect(m_openFolder, &QPushButton::clicked, this, [this] {
    QFileInfo fi(m_lastOutput);
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
  });

  refreshInput();
}

QString ShrinkView::preset() const {
  switch (m_quality->currentIndex()) {
    case 1: return "quality";
    case 2: return "smallest";
    default: return "balanced";
  }
}

void ShrinkView::setInput(const QString& path) {
  m_input = path;
  refreshInput();
}

void ShrinkView::refreshInput() {
  const bool have = !m_input.isEmpty();
  const bool ffmpeg = kolma::media_available();
  if (have) {
    QFileInfo fi(m_input);
    kolma::FileInfo info = kolma::inspect_file(m_input.toStdString());
    QString kind = QString::fromStdString(kolma::file_type_name(info.type));
    m_fileLabel->setText(QString("%1  ·  %2  ·  %3").arg(fi.fileName(), hs(uint64_t(fi.size())), kind));
  } else {
    m_fileLabel->setText("No file selected.");
  }
  m_ffmpegNote->setText(ffmpeg
      ? "Uses ffmpeg to re-encode. The original file is never modified."
      : "Media shrinking needs ffmpeg installed and on your PATH. Everything else in kolma "
        "works without it.");
  m_go->setEnabled(have && ffmpeg && !m_running);
  defaultOutput();
}

void ShrinkView::defaultOutput() {
  if (m_input.isEmpty()) {
    m_output->clear();
    return;
  }
  QFileInfo fi(m_input);
  kolma::FileInfo info = kolma::inspect_file(m_input.toStdString());
  QString ext = info.type == kolma::FileType::Image ? ".jpg" : ".mp4";
  QString base = fi.completeBaseName();
  if (base.isEmpty()) base = "media";
  QString suggested = QDir(fi.absolutePath()).filePath(base + "-shrunk" + ext);
  if (m_output->text().isEmpty() || !m_output->isModified()) m_output->setText(suggested);
}

void ShrinkView::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void ShrinkView::dropEvent(QDropEvent* e) {
  for (const QUrl& u : e->mimeData()->urls()) {
    if (u.isLocalFile()) {
      setInput(u.toLocalFile());
      break;  // one media file at a time
    }
  }
  e->acceptProposedAction();
}

void ShrinkView::setRunning(bool running) {
  m_running = running;
  m_go->setEnabled(!running && !m_input.isEmpty() && kolma::media_available());
  m_go->setText(running ? "Shrinking…" : "Shrink");
  m_quality->setEnabled(!running);
}

void ShrinkView::start() {
  if (m_input.isEmpty()) {
    emit status("Choose a video or image first.", true);
    return;
  }
  if (!kolma::media_available()) {
    emit status("Media shrinking needs ffmpeg installed and on PATH.", true);
    return;
  }
  QString out = m_output->text().trimmed();
  if (out.isEmpty()) {
    emit status("Choose where to save the shrunk file.", true);
    return;
  }
  JobSpec spec;
  spec.kind = JobSpec::ShrinkMedia;
  spec.inputs = QStringList{m_input};
  spec.output = out;
  spec.quality = preset();
  spec.force = true;  // explicit destination; re-shrinking replaces it

  m_result->hide();
  m_bar->setValue(0);
  m_progressLine->setText("Starting…");
  setRunning(true);
  emit status("Shrinking…", false);

  PendingJob job = prepareJob(spec);
  connect(job.worker, &EngineWorker::progress, this, &ShrinkView::onProgress);
  connect(job.worker, &EngineWorker::finished, this, &ShrinkView::onFinished);
  static_cast<QThread*>(job.thread)->start();
}

void ShrinkView::onProgress(int pct, const QString& line) {
  Q_UNUSED(line);
  m_bar->setValue(pct);
  m_progressLine->setText(QString("Re-encoding… %1%").arg(pct));
}

void ShrinkView::onFinished(bool ok, const QString& error, const QVariantMap& result) {
  setRunning(false);
  if (!ok) {
    m_bar->setValue(0);
    m_progressLine->setText(" ");
    emit status(error, true);
    m_result->show();
    m_resultTitle->setText("Couldn’t shrink");
    m_resultTitle->setStyleSheet(QString("color:%1;").arg(Palette::bad));
    m_resultBody->setText(error);
    m_openFolder->hide();
    return;
  }
  m_bar->setValue(100);
  m_progressLine->setText("Done.");
  m_lastOutput = result.value("output").toString();
  double ratio = result.value("ratio").toDouble();
  uint64_t orig = result.value("orig_size").toULongLong();
  uint64_t comp = result.value("comp_size").toULongLong();
  double saved = orig > 0 ? (1.0 - ratio) * 100.0 : 0.0;

  m_result->show();
  m_resultTitle->setText("Shrunk ✓");
  m_resultTitle->setStyleSheet(QString("color:%1;").arg(Palette::accent));
  m_resultBody->setText(QString("%1  →  %2   ·   %3% smaller\nSaved to: %4\n"
                                "Still a normal file — plays or opens anywhere, no kolma needed.")
                            .arg(hs(orig), hs(comp))
                            .arg(saved, 0, 'f', 1)
                            .arg(m_lastOutput));
  m_openFolder->show();
  emit status(QString("Shrunk to %1 (%2% saved)")
                  .arg(QFileInfo(m_lastOutput).fileName())
                  .arg(saved, 0, 'f', 1), false);
}

}  // namespace kolma::ui
