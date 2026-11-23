#include "compressview.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include "kolma/util.hpp"
#include "theme.hpp"
#include "worker.hpp"

namespace kolma::ui {

namespace {
QString hs(uint64_t b) { return QString::fromStdString(kolma::human_size(b)); }
}  // namespace

CompressView::CompressView(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(16);

  auto* title = new QLabel("Compress");
  title->setObjectName("h1");
  auto* sub = new QLabel("Drop files or a whole folder. By default you get a .zip anyone can open — "
                         "switch to kolma’s own format for maximum squeeze or a password.");
  sub->setObjectName("sub");
  sub->setWordWrap(true);
  root->addWidget(title);
  root->addWidget(sub);

  // Drop zone + add buttons.
  auto* drop = new QFrame;
  drop->setObjectName("drop");
  drop->setMinimumHeight(96);
  auto* dropLay = new QVBoxLayout(drop);
  auto* dropText = new QLabel("Drag & drop files or folders here");
  dropText->setObjectName("sub");
  dropText->setAlignment(Qt::AlignCenter);
  auto* btnRow = new QHBoxLayout;
  auto* addFiles = new QPushButton("Add files…");
  auto* addFolder = new QPushButton("Add folder…");
  auto* clear = new QPushButton("Clear");
  btnRow->addStretch();
  btnRow->addWidget(addFiles);
  btnRow->addWidget(addFolder);
  btnRow->addWidget(clear);
  btnRow->addStretch();
  dropLay->addWidget(dropText);
  dropLay->addLayout(btnRow);
  root->addWidget(drop);

  m_list = new QListWidget;
  m_list->setMaximumHeight(120);
  m_list->setAlternatingRowColors(true);
  root->addWidget(m_list);

  connect(addFiles, &QPushButton::clicked, this, [this] {
    addPaths(QFileDialog::getOpenFileNames(this, "Choose files"));
  });
  connect(addFolder, &QPushButton::clicked, this, [this] {
    QString d = QFileDialog::getExistingDirectory(this, "Choose a folder");
    if (!d.isEmpty()) addPaths({d});
  });
  connect(clear, &QPushButton::clicked, this, [this] {
    m_inputs.clear();
    refreshInputs();
  });

  // Options.
  auto* opts = new QGroupBox("Options");
  auto* form = new QVBoxLayout(opts);
  form->setSpacing(10);

  // Format choice — the single most important decision for a normal user.
  auto* fmtRow = new QHBoxLayout;
  fmtRow->addWidget(new QLabel("Format"));
  m_format = new QComboBox;
  m_format->addItems({"ZIP — opens on any computer (recommended)",
                      "KOLMA — smallest size + password protection"});
  fmtRow->addWidget(m_format, 1);
  form->addLayout(fmtRow);
  m_formatHint = new QLabel;
  m_formatHint->setObjectName("dim");
  m_formatHint->setWordWrap(true);
  form->addWidget(m_formatHint);

  auto* outRow = new QHBoxLayout;
  outRow->addWidget(new QLabel("Save to"));
  m_output = new QLineEdit;
  auto* browse = new QPushButton("Browse…");
  outRow->addWidget(m_output, 1);
  outRow->addWidget(browse);
  form->addLayout(outRow);
  connect(browse, &QPushButton::clicked, this, [this] {
    QString filter = useStd() ? "ZIP archive (*.zip);;All files (*)"
                              : "kolma archive (*.kolma);;All files (*)";
    QString f = QFileDialog::getSaveFileName(this, "Save archive", m_output->text(), filter);
    if (!f.isEmpty()) m_output->setText(f);
  });

  // Engine-only options, hidden when producing a standard .zip.
  m_engineOpts = new QWidget;
  auto* engLay = new QVBoxLayout(m_engineOpts);
  engLay->setContentsMargins(0, 0, 0, 0);
  engLay->setSpacing(10);

  auto* algoRow = new QHBoxLayout;
  algoRow->addWidget(new QLabel("Method"));
  m_algo = new QComboBox;
  m_algo->addItems({"Auto (recommended)", "lz77", "bwt-mtf-ari", "lzw", "huffman",
                    "delta-rle", "rbrc", "rle", "store"});
  algoRow->addWidget(m_algo, 1);
  algoRow->addWidget(new QLabel("Priority"));
  m_priority = new QComboBox;
  m_priority->addItems({"Balanced", "Best ratio", "Fastest"});
  algoRow->addWidget(m_priority, 1);
  engLay->addLayout(algoRow);

  auto* levelRow = new QHBoxLayout;
  levelRow->addWidget(new QLabel("Effort"));
  m_level = new QSlider(Qt::Horizontal);
  m_level->setRange(0, 9);
  m_level->setValue(0);
  m_levelLabel = new QLabel("Auto");
  m_levelLabel->setObjectName("dim");
  m_levelLabel->setMinimumWidth(60);
  levelRow->addWidget(m_level, 1);
  levelRow->addWidget(m_levelLabel);
  engLay->addLayout(levelRow);
  connect(m_level, &QSlider::valueChanged, this, [this](int v) {
    m_levelLabel->setText(v == 0 ? "Auto" : QString("Level %1").arg(v));
  });

  auto* encRow = new QHBoxLayout;
  m_encrypt = new QCheckBox("Encrypt with a password");
  m_password = new QLineEdit;
  m_password->setEchoMode(QLineEdit::Password);
  m_password->setPlaceholderText("password");
  m_password->setEnabled(false);
  encRow->addWidget(m_encrypt);
  encRow->addWidget(m_password, 1);
  engLay->addLayout(encRow);
  connect(m_encrypt, &QCheckBox::toggled, m_password, &QLineEdit::setEnabled);

  form->addWidget(m_engineOpts);
  root->addWidget(opts);

  connect(m_format, &QComboBox::currentIndexChanged, this, [this] { applyFormat(); });

  // Go + progress.
  m_go = new QPushButton("Compress");
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

  connect(m_go, &QPushButton::clicked, this, &CompressView::start);
  connect(m_openFolder, &QPushButton::clicked, this, [this] {
    QFileInfo fi(m_lastOutput);
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
  });

  applyFormat();
  refreshInputs();
}

bool CompressView::useStd() const { return m_format->currentIndex() == 0; }

void CompressView::applyFormat() {
  m_engineOpts->setVisible(!useStd());
  m_formatHint->setText(useStd()
      ? "A .zip opens everywhere — Windows, macOS, Linux, phones — with no extra software. "
        "Best for sharing."
      : "kolma’s own format usually compresses harder and can be locked with a password, "
        "but only kolma can open it.");
  // Re-suggest the output with the right extension unless the user typed their own.
  if (!m_output->isModified() || m_output->text().isEmpty()) defaultOutput();
}

void CompressView::addPaths(const QStringList& paths) {
  for (const QString& p : paths) {
    if (!p.isEmpty() && !m_inputs.contains(p)) m_inputs.append(p);
  }
  refreshInputs();
}

void CompressView::refreshInputs() {
  m_list->clear();
  for (const QString& p : m_inputs) {
    QFileInfo fi(p);
    QString label = fi.fileName();
    if (fi.isDir()) label += "  (folder)";
    else label += "  ·  " + hs(uint64_t(fi.size()));
    m_list->addItem(label);
  }
  defaultOutput();
  m_go->setEnabled(!m_inputs.isEmpty() && !m_running);
}

void CompressView::defaultOutput() {
  if (m_inputs.isEmpty()) {
    m_output->clear();
    return;
  }
  QFileInfo first(m_inputs.first());
  QString base = first.fileName();
  if (base.isEmpty()) base = "archive";
  QString dir = first.absolutePath();
  QString ext = useStd() ? ".zip" : ".kolma";
  QString suggested = QDir(dir).filePath(base + ext);
  if (m_output->text().isEmpty() || !m_output->isModified()) m_output->setText(suggested);
}

void CompressView::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void CompressView::dropEvent(QDropEvent* e) {
  QStringList paths;
  for (const QUrl& u : e->mimeData()->urls()) {
    if (u.isLocalFile()) paths.append(u.toLocalFile());
  }
  addPaths(paths);
  e->acceptProposedAction();
}

void CompressView::setRunning(bool running) {
  m_running = running;
  m_go->setEnabled(!running && !m_inputs.isEmpty());
  m_go->setText(running ? "Compressing…" : "Compress");
  m_format->setEnabled(!running);
}

void CompressView::start() {
  if (m_inputs.isEmpty()) {
    emit status("Add at least one file or folder first.", true);
    return;
  }
  QString out = m_output->text().trimmed();
  if (out.isEmpty()) {
    emit status("Choose where to save the archive.", true);
    return;
  }

  JobSpec spec;
  spec.inputs = m_inputs;
  spec.force = true;  // the destination is explicit; re-compressing replaces it

  if (useStd()) {
    if (!out.endsWith(".zip", Qt::CaseInsensitive)) out += ".zip";
    spec.kind = JobSpec::CompressStd;
    spec.format = "zip";
    spec.output = out;
  } else {
    if (!out.endsWith(".kolma")) out += ".kolma";
    if (m_encrypt->isChecked() && m_password->text().isEmpty()) {
      emit status("Enter a password, or turn encryption off.", true);
      return;
    }
    bool bundle = m_inputs.size() > 1;
    for (const QString& p : m_inputs)
      if (QFileInfo(p).isDir()) bundle = true;
    spec.kind = bundle ? JobSpec::CompressBundle : JobSpec::CompressFile;
    spec.output = out;
    spec.algorithm = m_algo->currentIndex() == 0 ? "auto" : m_algo->currentText();
    spec.priority = m_priority->currentIndex();
    spec.level = m_level->value();
    spec.password = m_encrypt->isChecked() ? m_password->text() : QString();
  }

  m_result->hide();
  m_bar->setValue(0);
  m_progressLine->setText("Starting…");
  setRunning(true);
  emit status("Compressing…", false);

  PendingJob job = prepareJob(spec);
  connect(job.worker, &EngineWorker::progress, this, &CompressView::onProgress);
  connect(job.worker, &EngineWorker::finished, this, &CompressView::onFinished);
  static_cast<QThread*>(job.thread)->start();
}

void CompressView::onProgress(int pct, const QString& line) {
  m_bar->setValue(pct);
  m_progressLine->setText(line);
}

void CompressView::onFinished(bool ok, const QString& error, const QVariantMap& result) {
  setRunning(false);
  if (!ok) {
    m_bar->setValue(0);
    m_progressLine->setText(" ");
    emit status(error, true);
    m_result->show();
    m_resultTitle->setText("Couldn’t compress");
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
  m_resultTitle->setText("Compressed ✓");
  m_resultTitle->setStyleSheet(QString("color:%1;").arg(Palette::accent));

  QString body;
  if (useStd()) {
    body = QString("%1  →  %2   ·   %3% smaller\nSaved to: %4\n"
                   "Opens in any file manager — no special software needed.")
               .arg(hs(orig), hs(comp))
               .arg(saved, 0, 'f', 1)
               .arg(m_lastOutput);
  } else {
    QString algo = result.value("algorithm").toString();
    bool enc = result.value("encrypted").toBool();
    bool auth = result.value("authenticated").toBool();
    body = QString("%1  →  %2   ·   ratio %3   ·   %4% saved\nMethod: %5")
               .arg(hs(orig), hs(comp))
               .arg(ratio, 0, 'f', 4)
               .arg(saved, 0, 'f', 1)
               .arg(algo);
    if (enc) body += auth ? "\nEncrypted · authenticated (HMAC-SHA256)"
                          : "\nEncrypted (legacy, unauthenticated)";
    body += "\nSaved to: " + m_lastOutput;
  }
  m_resultBody->setText(body);
  m_openFolder->show();
  emit status(QString("Compressed to %1 (%2% saved)")
                  .arg(QFileInfo(m_lastOutput).fileName())
                  .arg(saved, 0, 'f', 1), false);
}

}  // namespace kolma::ui
