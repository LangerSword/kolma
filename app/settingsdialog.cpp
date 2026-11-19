#include "settingsdialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include "settings.hpp"

namespace kolma::ui {

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Preferences");
  setMinimumWidth(420);
  auto* root = new QVBoxLayout(this);
  auto* intro = new QLabel("Sensible defaults are already set. Change these only if you know you want to.");
  intro->setObjectName("sub");
  intro->setWordWrap(true);
  root->addWidget(intro);

  auto* form = new QFormLayout;
  m_threads = new QSpinBox;
  m_threads->setRange(0, 256);
  m_threads->setSpecialValueText("Auto (all cores)");
  m_threads->setValue(prefThreads());
  form->addRow("Worker threads", m_threads);

  m_priority = new QComboBox;
  m_priority->addItems({"Balanced", "Best ratio", "Fastest"});
  m_priority->setCurrentIndex(prefPriority());
  form->addRow("Default priority", m_priority);

  m_algo = new QComboBox;
  m_algo->addItems({"auto", "lz77", "bwt-mtf-ari", "lzw", "huffman", "delta-rle", "rle", "store"});
  m_algo->setCurrentText(prefAlgo());
  form->addRow("Default method", m_algo);

  m_level = new QSpinBox;
  m_level->setRange(0, 9);
  m_level->setSpecialValueText("Auto");
  m_level->setValue(prefLevel());
  form->addRow("Default effort", m_level);
  root->addLayout(form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  root->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    save();
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SettingsDialog::save() {
  QSettings s = prefs();
  s.setValue("threads", m_threads->value());
  s.setValue("priority", m_priority->currentIndex());
  s.setValue("algo", m_algo->currentText());
  s.setValue("level", m_level->value());
}

}  // namespace kolma::ui
