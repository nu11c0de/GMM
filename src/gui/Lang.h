#pragma once
#include <QString>

class QWidget;

// Lightweight in-house localization. The UI source strings are Russian; when the
// language is English we map them through a dictionary. T() translates a runtime
// string; translateUi() walks a freshly-built widget tree and translates the
// static chrome (menus, toolbar, labels, tabs, …) in place. The language is set
// once at startup (changing it asks for a restart), so there is no need to keep
// the original Russian around for a live switch.
namespace gtamm::lang {

enum class Language { Russian, English };

void setLanguage(Language l);
Language language();
bool isEnglish();

// Persisted choice (QSettings "lang" = "ru" | "en"); load before building the UI.
Language loadSaved();
void save(Language l);

// Translate one string (returns it unchanged when Russian or when no mapping).
QString T(const QString& ru);

// Recursively translate the static chrome of a built widget tree (no-op in RU).
void translateUi(QWidget* root);

}  // namespace gtamm::lang
