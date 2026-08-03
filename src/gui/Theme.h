#pragma once
class QApplication;
class QString;

namespace gtamm::theme {

// Apply a flat Fusion-based theme (dark or light) to the whole application:
// palette + stylesheet. Can be called at runtime to switch.
void apply(QApplication& app, bool dark);

// Apply an external Qt stylesheet (e.g. a Mod Organizer 2 .qss theme) on top of
// a Fusion base + dark/light palette. Mod Organizer themes are plain Qt style
// sheets, so they can be dropped in and applied directly.
void applyExternal(QApplication& app, const QString& qss, bool darkBase);

// Resolve and apply a theme id: the built-ins "dark"/"light", or a path to a
// .qss file (an MO2 theme). Falls back to the dark built-in if a file is missing.
void applyNamed(QApplication& app, const QString& id);

// True if the id is a built-in light theme (used to pick details-panel colors).
bool isLightId(const QString& id);

}  // namespace gtamm::theme
