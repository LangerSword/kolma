#include "mainwindow.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include "benchview.hpp"
#include "compressview.hpp"
#include "extractview.hpp"
#include "settingsdialog.hpp"
#include "theme.hpp"

namespace kolma::ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("kolma");
  resize(1060, 760);
  setAcceptDrops(true);

  auto* central = new QWidget;
  auto* lay = new QHBoxLayout(central);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  // Sidebar.
  auto* side = new QWidget;
  side->setFixedWidth(220);
  side->setStyleSheet(QString("background:%1;").arg(Palette::surface));
  auto* sideLay = new QVBoxLayout(side);
  m_sidebarLay = sideLay;
  sideLay->setContentsMargins(18, 24, 18, 24);
  sideLay->setSpacing(6);

  auto* brand = new QLabel("kolma");
  brand->setStyleSheet(QString("font-size:26px;font-weight:700;color:%1;").arg(Palette::accent));
  auto* tagline = new QLabel("compression that\nshows its work");
  tagline->setObjectName("dim");
  tagline->setStyleSheet(QString("color:%1;font-size:12px;").arg(Palette::dim));
  sideLay->addWidget(brand);
  sideLay->addWidget(tagline);
  sideLay->addSpacing(20);

  m_stack = new QStackedWidget;
  m_compress = new CompressView;
  m_extract = new ExtractView;
  m_bench = new BenchView;
  m_stack->addWidget(m_compress);
  m_stack->addWidget(m_extract);
  m_stack->addWidget(m_bench);

  addNav("Compress", 0);
  addNav("Extract", 1);
  addNav("Benchmark", 2);
  sideLay->addStretch();

  auto* prefs = new QPushButton("Preferences");
  prefs->setObjectName("nav");
  sideLay->addWidget(prefs);
  connect(prefs, &QPushButton::clicked, this, [this] {
    SettingsDialog dlg(this);
    dlg.exec();
  });

  auto* about = new QPushButton("About kolma");
  about->setObjectName("nav");
  sideLay->addWidget(about);
  connect(about, &QPushButton::clicked, this, [this] {
    QMessageBox::about(this, "About kolma",
        "<h3>kolma</h3>"
        "<p>A free, open-source compression engine that shows its work.</p>"
        "<p>Seven methods, an analyser that measures before it commits, a parallel "
        "engine, authenticated encryption, and a friendly interface.</p>"
        "<p>MIT licensed. Built by LangerSword.</p>");
  });

  auto* ver = new QLabel("v1.1.0 · MIT");
  ver->setStyleSheet(QString("color:%1;font-size:11px;").arg(Palette::dim));
  sideLay->addWidget(ver);

  lay->addWidget(side);
  lay->addWidget(m_stack, 1);
  setCentralWidget(central);

  connect(m_compress, &CompressView::status, this, &MainWindow::showStatus);
  connect(m_extract, &ExtractView::status, this, &MainWindow::showStatus);
  connect(m_bench, &BenchView::status, this, &MainWindow::showStatus);

  m_statusLabel = new QLabel("Ready. Drop files to compress, or a .kolma to extract.");
  statusBar()->addWidget(m_statusLabel, 1);
  if (!m_nav.isEmpty()) m_nav.first()->setChecked(true);
}

void MainWindow::addNav(const QString& text, int index) {
  auto* btn = new QPushButton(text);
  btn->setObjectName("nav");
  btn->setCheckable(true);
  btn->setAutoExclusive(true);
  m_nav.append(btn);
  connect(btn, &QPushButton::clicked, this, [this, index] { m_stack->setCurrentIndex(index); });
  m_sidebarLay->addWidget(btn);
}

void MainWindow::showStatus(const QString& msg, bool isError) {
  m_statusLabel->setText(msg);
  m_statusLabel->setStyleSheet(QString("color:%1;").arg(isError ? Palette::bad : Palette::subtle));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
  QStringList files;
  for (const QUrl& u : e->mimeData()->urls())
    if (u.isLocalFile()) files.append(u.toLocalFile());
  handleFiles(files);
  e->acceptProposedAction();
}

void MainWindow::handleFiles(const QStringList& files) {
  if (files.isEmpty()) return;
  bool allArchives = true;
  for (const QString& f : files)
    if (!f.endsWith(".kolma", Qt::CaseInsensitive)) allArchives = false;

  if (allArchives && files.size() == 1) {
    m_stack->setCurrentIndex(1);
    if (m_nav.size() > 1) m_nav[1]->setChecked(true);
    m_extract->openArchive(files.first());
  } else {
    m_stack->setCurrentIndex(0);
    if (!m_nav.isEmpty()) m_nav[0]->setChecked(true);
    m_compress->addPaths(files);
  }
}

}  // namespace kolma::ui
