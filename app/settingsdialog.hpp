// kolma desktop -- preferences dialog. Defaults that keep the app friendly.
#pragma once

#include <QDialog>

class QSpinBox;
class QComboBox;

namespace kolma::ui {

class SettingsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit SettingsDialog(QWidget* parent = nullptr);

 private:
  void save();
  QSpinBox* m_threads;
  QComboBox* m_priority;
  QComboBox* m_algo;
  QSpinBox* m_level;
};

}  // namespace kolma::ui
