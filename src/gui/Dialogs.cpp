#include "Dialogs.h"

#include "Frame.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace gtamm::dialogs {

namespace {

// Widget to center a message-box-style dialog over: the top-level window of
// whatever was passed as `parent` (falls back to screen-centering inside
// centerOverWidget() itself when null).
QWidget* anchorFor(QWidget* parent)
{
  return parent ? parent->window() : nullptr;
}

// Builds the shared frame + icon + wrapped message (+ optional read-only
// "details" box) that warning()/information()/critical()/question() all
// show, with just the buttons left to the caller to add.
struct MessageDialog
{
  QDialog dlg;
  QDialogButtonBox* box = nullptr;

  MessageDialog(QWidget* parent, const QString& title, const QString& text,
               const QString& detailedText, QStyle::StandardPixmap icon)
      : dlg(parent)
  {
    dlg.setWindowTitle(title);
    auto* body = applyFrame(dlg, isDarkPalette(), /*showMinMax=*/false);
    body->setContentsMargins(20, 16, 20, 16);
    body->setSpacing(14);

    auto* row = new QHBoxLayout;
    row->setSpacing(14);
    auto* iconLabel = new QLabel(&dlg);
    iconLabel->setPixmap(qApp->style()->standardIcon(icon).pixmap(32, 32));
    iconLabel->setAlignment(Qt::AlignTop);
    row->addWidget(iconLabel);

    auto* textLabel = new QLabel(text, &dlg);
    textLabel->setWordWrap(true);
    textLabel->setMinimumWidth(280);
    textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(textLabel, 1);
    body->addLayout(row);

    if (!detailedText.isEmpty()) {
      auto* details = new QPlainTextEdit(detailedText, &dlg);
      details->setReadOnly(true);
      details->setMaximumHeight(160);
      body->addWidget(details);
    }

    box = new QDialogButtonBox(&dlg);
    body->addWidget(box);
  }
};

}  // namespace

void warning(QWidget* parent, const QString& title, const QString& text,
            const QString& detailedText)
{
  MessageDialog m(parent, title, text, detailedText, QStyle::SP_MessageBoxWarning);
  QPushButton* ok = m.box->addButton(QDialogButtonBox::Ok);
  ok->setDefault(true);
  QObject::connect(m.box, &QDialogButtonBox::accepted, &m.dlg, &QDialog::accept);
  centerOverWidget(m.dlg, anchorFor(parent));
  m.dlg.exec();
}

void information(QWidget* parent, const QString& title, const QString& text,
                 const QString& detailedText)
{
  MessageDialog m(parent, title, text, detailedText, QStyle::SP_MessageBoxInformation);
  QPushButton* ok = m.box->addButton(QDialogButtonBox::Ok);
  ok->setDefault(true);
  QObject::connect(m.box, &QDialogButtonBox::accepted, &m.dlg, &QDialog::accept);
  centerOverWidget(m.dlg, anchorFor(parent));
  m.dlg.exec();
}

void critical(QWidget* parent, const QString& title, const QString& text,
             const QString& detailedText)
{
  MessageDialog m(parent, title, text, detailedText, QStyle::SP_MessageBoxCritical);
  QPushButton* ok = m.box->addButton(QDialogButtonBox::Ok);
  ok->setDefault(true);
  QObject::connect(m.box, &QDialogButtonBox::accepted, &m.dlg, &QDialog::accept);
  centerOverWidget(m.dlg, anchorFor(parent));
  m.dlg.exec();
}

QMessageBox::StandardButton question(QWidget* parent, const QString& title,
                                     const QString& text,
                                     QMessageBox::StandardButtons buttons,
                                     QMessageBox::StandardButton defaultButton)
{
  MessageDialog m(parent, title, text, QString(), QStyle::SP_MessageBoxQuestion);
  // QMessageBox::StandardButton and QDialogButtonBox::StandardButton share
  // identical bit values for every name they have in common (Ok, Cancel,
  // Yes, No, Save, Discard, ...) -- Qt designed them that way since
  // QMessageBox is itself built on a QDialogButtonBox internally.
  m.box->setStandardButtons(
      static_cast<QDialogButtonBox::StandardButtons>(static_cast<int>(buttons)));
  if (defaultButton != QMessageBox::NoButton) {
    if (auto* b = m.box->button(
            static_cast<QDialogButtonBox::StandardButton>(static_cast<int>(defaultButton))))
      b->setDefault(true);
  }
  QMessageBox::StandardButton result = QMessageBox::Cancel;  // closed via X / Escape
  QObject::connect(m.box, &QDialogButtonBox::clicked, &m.dlg,
                   [&m, &result](QAbstractButton* btn) {
                     result = static_cast<QMessageBox::StandardButton>(
                         static_cast<int>(m.box->standardButton(btn)));
                     m.dlg.accept();
                   });
  centerOverWidget(m.dlg, anchorFor(parent));
  m.dlg.exec();
  return result;
}

QString getText(QWidget* parent, const QString& title, const QString& label,
                QLineEdit::EchoMode mode, const QString& text, bool* ok)
{
  QDialog dlg(parent);
  dlg.setWindowTitle(title);
  auto* body = applyFrame(dlg, isDarkPalette(), /*showMinMax=*/false);
  body->setContentsMargins(20, 16, 20, 16);
  body->setSpacing(10);

  body->addWidget(new QLabel(label, &dlg));
  auto* edit = new QLineEdit(text, &dlg);
  edit->setEchoMode(mode);
  edit->setMinimumWidth(280);
  body->addWidget(edit);

  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  body->addWidget(box);
  QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

  edit->setFocus();
  edit->selectAll();

  centerOverWidget(dlg, anchorFor(parent));
  const bool accepted = dlg.exec() == QDialog::Accepted;
  if (ok)
    *ok = accepted;
  return accepted ? edit->text() : QString();
}

QString getItem(QWidget* parent, const QString& title, const QString& label,
                const QStringList& items, int current, bool editable, bool* ok)
{
  QDialog dlg(parent);
  dlg.setWindowTitle(title);
  auto* body = applyFrame(dlg, isDarkPalette(), /*showMinMax=*/false);
  body->setContentsMargins(20, 16, 20, 16);
  body->setSpacing(10);

  body->addWidget(new QLabel(label, &dlg));
  auto* combo = new QComboBox(&dlg);
  combo->addItems(items);
  combo->setEditable(editable);
  combo->setMinimumWidth(280);
  if (current >= 0 && current < combo->count())
    combo->setCurrentIndex(current);
  body->addWidget(combo);

  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  body->addWidget(box);
  QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

  centerOverWidget(dlg, anchorFor(parent));
  const bool accepted = dlg.exec() == QDialog::Accepted;
  if (ok)
    *ok = accepted;
  return accepted ? combo->currentText() : QString();
}

}  // namespace gtamm::dialogs
