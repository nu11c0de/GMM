#pragma once
#include <QLineEdit>
#include <QMessageBox>
#include <QString>
#include <QStringList>

class QWidget;

// Drop-in, custom-framed replacements for QMessageBox::warning/information/
// critical/question and QInputDialog::getText -- built on gtamm::applyFrame()
// (see Frame.h) so every popup in the app shares the same look as its own
// windows, instead of falling back to the native system dialog frame.
//
// Deliberately NOT used for: QFileDialog (native folder/file pickers can't
// reasonably be reskinned this deeply -- the whole point of using the native
// one is that it already matches the user's actual file system/shell), or a
// literal QMessageBox instance built ad hoc with custom ActionRole buttons
// (rare enough in this codebase to just leave as-is case by case).
namespace gtamm::dialogs {

void warning(QWidget* parent, const QString& title, const QString& text,
            const QString& detailedText = QString());
void information(QWidget* parent, const QString& title, const QString& text,
                 const QString& detailedText = QString());
void critical(QWidget* parent, const QString& title, const QString& text,
              const QString& detailedText = QString());

QMessageBox::StandardButton question(
    QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No,
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

QString getText(QWidget* parent, const QString& title, const QString& label,
                QLineEdit::EchoMode mode, const QString& text, bool* ok);

QString getItem(QWidget* parent, const QString& title, const QString& label,
                const QStringList& items, int current, bool editable, bool* ok);

}  // namespace gtamm::dialogs
