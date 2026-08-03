#pragma once
#include <QList>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QStyledItemDelegate>

#include "GameRules.h"

class QModelIndex;

namespace gtamm {

// Renders a mod's content types as small coloured badges ("CLEO", "ASI",
// "Lua", ...) in one column of the mod list, instead of spelling them into the
// mod's name. A Lua badge additionally gets a pencil next to it: clicking it
// opens that mod's script editor, which is otherwise buried in the right-click
// menu.
//
// The tag ids painted come from rules::modTypeId() and are read off the item's
// data role given to the constructor (a QStringList). Painting them here
// rather than embedding widgets keeps the list cheap with hundreds of rows and
// leaves sorting/drag-reorder untouched.
class ModTagDelegate : public QStyledItemDelegate
{
  Q_OBJECT

public:
  // `luaEditRole` marks rows whose Lua content is actually editable text
  // (a .lua source, not compiled .luac bytecode) -- only those get a pencil.
  ModTagDelegate(int tagsRole, int modIdRole, int luaEditRole,
                 QObject* parent = nullptr);

  // Localized label for a tag id (the ids themselves stay ASCII/stable).
  static QString label(const QString& tagId);

  // Pixel width a row with these tags needs (including the pencil, and the
  // trailing breathing room). Public so the view can size the whole column off
  // the widest row rather than off whatever is currently scrolled into view.
  static int widthForTags(const QStringList& tags, bool withPencil);

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override;
  QSize sizeHint(const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override;
  bool editorEvent(QEvent* event, QAbstractItemModel* model,
                   const QStyleOptionViewItem& option,
                   const QModelIndex& index) override;
  bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                 const QStyleOptionViewItem& option,
                 const QModelIndex& index) override;

signals:
  // The pencil next to a Lua badge was clicked for this mod.
  void editLuaRequested(const QString& modId);

private:
  // Badge rectangles for `index`, laid out inside `rect`; the last one is the
  // pencil button when the mod has a Lua tag (see pencilRect()).
  QList<QRect> badgeRects(const QRect& rect, const QStringList& tags) const;
  // Where the pencil sits for this row, or a null rect if the mod has no Lua
  // script to edit.
  QRect pencilRect(const QRect& rect, const QModelIndex& index) const;

  int m_tagsRole;
  int m_modIdRole;
  int m_luaEditRole;
};

}  // namespace gtamm
