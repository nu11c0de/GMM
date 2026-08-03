#pragma once
#include <QDialog>

#include "Lang.h"

class QLabel;
class QPushButton;

namespace gtamm {

// First-run-only welcome screen: lets the user pick a language and shows a
// short, visually distinct greeting before the instance picker ever appears.
// Deliberately standalone (no dependency on MainWindow's shared frameless()/
// TitleBar machinery -- MainWindow doesn't exist yet at this point in
// startup): frameless, fixed-size, drag-to-move via any empty area, dismissed
// by its own "get started" button (or Escape, which just keeps whatever
// language was last toggled rather than aborting startup).
class WelcomeDialog : public QDialog
{
  Q_OBJECT
public:
  explicit WelcomeDialog(QWidget* parent = nullptr);

  lang::Language selectedLanguage() const { return m_lang; }

protected:
  void mousePressEvent(QMouseEvent* event) override;

private:
  void applyLanguage(lang::Language l);

  lang::Language m_lang = lang::Language::Russian;
  QLabel* m_subtitle     = nullptr;
  QLabel* m_greeting     = nullptr;
  QPushButton* m_ruBtn   = nullptr;
  QPushButton* m_enBtn   = nullptr;
  QPushButton* m_start   = nullptr;
};

}  // namespace gtamm
