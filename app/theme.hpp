// kolma desktop -- the dark editorial theme, shared with the TUI.
// One near-black canvas, one mint accent, semantic support hues. Every colour
// has a job: mint = primary/success, amber = warning/encrypted, red = error,
// cyan = info. Nothing decorative.
//
// Tokens are substituted with QString::replace (not arg) so place markers like
// %1 / %10 can never collide and silently drop a colour.
#pragma once

#include <QString>

namespace kolma::ui {

struct Palette {
  static constexpr auto bg = "#0B0F14";
  static constexpr auto surface = "#10161D";
  static constexpr auto surface2 = "#151D26";
  static constexpr auto line = "#232D38";
  static constexpr auto text = "#E5E7EB";
  static constexpr auto subtle = "#9CA3AF";
  static constexpr auto dim = "#6B7280";
  static constexpr auto accent = "#7DD3A0";
  static constexpr auto accentHover = "#93DFB2";
  static constexpr auto ink = "#0B0F14";
  static constexpr auto warn = "#F0A868";
  static constexpr auto bad = "#F87171";
  static constexpr auto info = "#7DC4E4";
};

inline QString stylesheet() {
  QString qss = R"(
* { font-family: "Inter", "Segoe UI", "Cantarell", "Noto Sans", sans-serif; font-size: 14px; }
QMainWindow, QDialog { background: @BG@; }
QWidget { background: @BG@; color: @TEXT@; }
QLabel { background: transparent; color: @TEXT@; }
QLabel#h1 { font-size: 26px; font-weight: 700; color: @TEXT@; }
QLabel#h2 { font-size: 18px; font-weight: 600; color: @TEXT@; }
QLabel#sub { color: @SUBTLE@; }
QLabel#dim { color: @DIM@; }
QLabel#accent { color: @ACCENT@; font-weight: 600; }

QPushButton {
  background: @SURFACE@; color: @TEXT@; border: 1px solid @LINE@; border-radius: 9px;
  padding: 9px 18px; font-weight: 600;
}
QPushButton:hover { border-color: @ACCENT@; background: @SURFACE2@; }
QPushButton:pressed { background: @LINE@; }
QPushButton:disabled { color: @DIM@; border-color: @LINE@; }
QPushButton#primary { background: @ACCENT@; color: @INK@; border: none; }
QPushButton#primary:hover { background: @ACCENTHOVER@; }
QPushButton#primary:disabled { background: @LINE@; color: @DIM@; }
QPushButton#nav {
  background: transparent; border: none; border-radius: 9px;
  padding: 11px 16px; text-align: left; color: @SUBTLE@; font-weight: 600;
}
QPushButton#nav:hover { background: @SURFACE2@; color: @TEXT@; }
QPushButton#nav:checked { background: @SURFACE2@; color: @ACCENT@; }

QLineEdit, QComboBox, QSpinBox {
  background: @SURFACE@; color: @TEXT@; border: 1px solid @LINE@; border-radius: 8px;
  padding: 8px 10px; selection-background-color: @ACCENT@; selection-color: @INK@;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: @ACCENT@; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView {
  background: @SURFACE@; color: @TEXT@; border: 1px solid @LINE@;
  selection-background-color: @SURFACE2@; selection-color: @ACCENT@;
}

QProgressBar {
  background: @SURFACE@; border: 1px solid @LINE@; border-radius: 8px;
  min-height: 18px; text-align: center; color: @TEXT@; font-weight: 600;
}
QProgressBar::chunk { background: @ACCENT@; border-radius: 7px; }

QSlider::groove:horizontal { height: 6px; background: @SURFACE2@; border-radius: 3px; }
QSlider::sub-page:horizontal { background: @ACCENT@; border-radius: 3px; }
QSlider::handle:horizontal { background: @ACCENT@; width: 16px; margin: -6px 0; border-radius: 8px; }

QTreeWidget, QTableWidget, QListWidget {
  background: @SURFACE@; border: 1px solid @LINE@; border-radius: 10px;
  alternate-background-color: @SURFACE2@; outline: none; color: @TEXT@;
}
QTreeWidget::item, QTableWidget::item, QListWidget::item { padding: 6px; }
QTreeWidget::item:selected, QTableWidget::item:selected, QListWidget::item:selected {
  background: @SURFACE2@; color: @ACCENT@;
}
QHeaderView::section {
  background: @BG@; color: @DIM@; border: none; border-bottom: 1px solid @LINE@;
  padding: 8px; font-weight: 600;
}

QGroupBox {
  background: @SURFACE@; border: 1px solid @LINE@; border-radius: 12px;
  margin-top: 14px; padding: 14px; font-weight: 600; color: @TEXT@;
}
QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 6px; color: @ACCENT@; }

QStatusBar { background: @BG@; color: @SUBTLE@; border-top: 1px solid @LINE@; }
QStatusBar QLabel { color: @SUBTLE@; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @LINE@; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @DIM@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: @LINE@; border-radius: 5px; min-width: 30px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }

QCheckBox { spacing: 8px; color: @TEXT@; background: transparent; }
QCheckBox::indicator { width: 18px; height: 18px; border-radius: 5px; border: 1px solid @LINE@; background: @SURFACE@; }
QCheckBox::indicator:checked { background: @ACCENT@; border-color: @ACCENT@; }

QFrame#drop { background: @SURFACE@; border: 2px dashed @LINE@; border-radius: 14px; }
QFrame#drop:hover { border-color: @ACCENT@; }
QToolTip { background: @SURFACE2@; color: @TEXT@; border: 1px solid @LINE@; padding: 6px; }
)";
  // Replace longer tokens first so @SURFACE@ never eats @SURFACE2@, etc.
  qss.replace("@SURFACE2@", Palette::surface2)
      .replace("@ACCENTHOVER@", Palette::accentHover)
      .replace("@BG@", Palette::bg)
      .replace("@SURFACE@", Palette::surface)
      .replace("@LINE@", Palette::line)
      .replace("@TEXT@", Palette::text)
      .replace("@SUBTLE@", Palette::subtle)
      .replace("@DIM@", Palette::dim)
      .replace("@ACCENT@", Palette::accent)
      .replace("@INK@", Palette::ink)
      .replace("@WARN@", Palette::warn)
      .replace("@BAD@", Palette::bad)
      .replace("@INFO@", Palette::info);
  return qss;
}

}  // namespace kolma::ui
