#include "ModTags.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include "Lang.h"

namespace gtamm {

namespace {

constexpr int kPad      = 6;   // horizontal padding inside a badge
constexpr int kGap      = 4;   // gap between badges
constexpr int kRadius   = 4;   // badge corner radius
constexpr int kPencil   = 16;  // pencil button side
constexpr int kBadgeTop = 3;   // vertical inset of a badge within the row

// Badge colour per tag id. Deliberately fixed rather than palette-derived:
// these read as labels, and white-on-colour stays legible in both themes.
QColor tagColor(const QString& id)
{
  if (id == "lua")
    return QColor(0x2f, 0x6f, 0xdb);  // blue, as asked for
  if (id == "cleo")
    return QColor(0x7a, 0x4f, 0xd6);  // violet
  if (id == "asi")
    return QColor(0xc2, 0x63, 0x2a);  // orange
  if (id == "modloader")
    return QColor(0x2a, 0x8f, 0x8f);  // teal
  if (id == "data")
    return QColor(0x4a, 0x6f, 0x9e);  // steel blue
  return QColor(0x6b, 0x72, 0x80);    // "other": neutral grey
}

QFont badgeFont(const QFont& base)
{
  QFont f = base;
  f.setPointSizeF(std::max(6.5, base.pointSizeF() - 1.5));
  f.setBold(true);
  return f;
}

// A pencil, drawn rather than shipped as an icon so it follows the row's text
// colour (and therefore the active theme) without needing two asset variants.
void drawPencil(QPainter* p, const QRect& r, const QColor& color)
{
  p->save();
  p->setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color);
  pen.setWidthF(1.4);
  pen.setJoinStyle(Qt::MiterJoin);
  p->setPen(pen);

  const QRectF box = QRectF(r).adjusted(3.5, 3.5, -3.5, -3.5);
  // Body: a slanted quad from the lower-left tip up to the upper-right end.
  QPolygonF body;
  body << QPointF(box.left(), box.bottom())                       // tip
       << QPointF(box.left() + box.width() * 0.22, box.bottom())   // tip base
       << QPointF(box.right(), box.top() + box.height() * 0.22)    // top-right
       << QPointF(box.right() - box.width() * 0.22, box.top());
  p->drawPolygon(body);
  // The ferrule line that separates the nib from the shaft.
  p->drawLine(QPointF(box.left() + box.width() * 0.22, box.bottom()),
              QPointF(box.left() + box.width() * 0.44,
                      box.bottom() - box.height() * 0.22));
  p->restore();
}

}  // namespace

ModTagDelegate::ModTagDelegate(int tagsRole, int modIdRole, int luaEditRole,
                               QObject* parent)
    : QStyledItemDelegate(parent),
      m_tagsRole(tagsRole),
      m_modIdRole(modIdRole),
      m_luaEditRole(luaEditRole)
{
}

QString ModTagDelegate::label(const QString& tagId)
{
  // Names of the loaders stay as they are written everywhere else; only the
  // generic buckets are translated.
  if (tagId == "lua")
    return "Lua";
  if (tagId == "cleo")
    return "CLEO";
  if (tagId == "asi")
    return "ASI";
  if (tagId == "modloader")
    return "Mod Loader";
  if (tagId == "data")
    return lang::T("Данные");
  return lang::T("Прочее");
}

QList<QRect> ModTagDelegate::badgeRects(const QRect& rect,
                                        const QStringList& tags) const
{
  QList<QRect> out;
  const QFontMetrics fm(badgeFont(QApplication::font()));
  const int h = std::max(14, rect.height() - 2 * kBadgeTop);
  int x        = rect.left() + 2;
  for (const QString& id : tags) {
    const int w = fm.horizontalAdvance(label(id)) + 2 * kPad;
    out.append(QRect(x, rect.top() + (rect.height() - h) / 2, w, h));
    x += w + kGap;
  }
  return out;
}

QRect ModTagDelegate::pencilRect(const QRect& rect, const QModelIndex& index) const
{
  if (!index.data(m_luaEditRole).toBool())
    return {};
  const QStringList tags = index.data(m_tagsRole).toStringList();
  const QList<QRect> badges = badgeRects(rect, tags);
  if (badges.isEmpty())
    return {};
  const QRect last = badges.last();
  return QRect(last.right() + kGap, rect.top() + (rect.height() - kPencil) / 2,
               kPencil, kPencil);
}

void ModTagDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const
{
  // Selection/alternating background and focus come from the style; only the
  // cell's content is ours (the display text is deliberately not drawn -- it
  // exists purely so the column sorts).
  QStyleOptionViewItem opt(option);
  initStyleOption(&opt, index);
  opt.text.clear();
  QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
  style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

  const QStringList tags = index.data(m_tagsRole).toStringList();
  if (tags.isEmpty())
    return;

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->setFont(badgeFont(QApplication::font()));

  const QList<QRect> rects = badgeRects(option.rect, tags);
  for (int i = 0; i < rects.size() && i < tags.size(); ++i) {
    const QColor c = tagColor(tags[i]);
    QPainterPath path;
    path.addRoundedRect(QRectF(rects[i]), kRadius, kRadius);
    painter->fillPath(path, c);
    painter->setPen(QColor(0xff, 0xff, 0xff));
    painter->drawText(rects[i], Qt::AlignCenter, label(tags[i]));
  }

  const QRect pencil = pencilRect(option.rect, index);
  if (!pencil.isNull() && pencil.right() <= option.rect.right())
    drawPencil(painter, pencil,
               opt.palette.color(opt.state & QStyle::State_Selected
                                     ? QPalette::HighlightedText
                                     : QPalette::Text));
  painter->restore();
}

int ModTagDelegate::widthForTags(const QStringList& tags, bool withPencil)
{
  if (tags.isEmpty())
    return 0;
  const QFontMetrics fm(badgeFont(QApplication::font()));
  // 2px leading inset (badgeRects) + a trailing gap, so badges never touch the
  // cell edge on either side.
  int w = 2 + kGap;
  for (const QString& id : tags)
    w += fm.horizontalAdvance(label(id)) + 2 * kPad + kGap;
  if (withPencil)
    w += kPencil + kGap;
  return w;
}

QSize ModTagDelegate::sizeHint(const QStyleOptionViewItem& option,
                               const QModelIndex& index) const
{
  const QStringList tags = index.data(m_tagsRole).toStringList();
  if (tags.isEmpty())
    return QStyledItemDelegate::sizeHint(option, index);
  const int w =
      widthForTags(tags, index.data(m_luaEditRole).toBool());
  return QSize(w, std::max(20, QStyledItemDelegate::sizeHint(option, index).height()));
}

bool ModTagDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                 const QStyleOptionViewItem& option,
                                 const QModelIndex& index)
{
  if (event->type() == QEvent::MouseButtonRelease) {
    auto* me = static_cast<QMouseEvent*>(event);
    if (me->button() == Qt::LeftButton) {
      const QRect pencil = pencilRect(option.rect, index);
      if (!pencil.isNull() && pencil.contains(me->position().toPoint())) {
        const QString modId = index.data(m_modIdRole).toString();
        if (!modId.isEmpty()) {
          emit editLuaRequested(modId);
          return true;  // consumed: don't also treat it as a plain row click
        }
      }
    }
  }
  return QStyledItemDelegate::editorEvent(event, model, option, index);
}

bool ModTagDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view,
                               const QStyleOptionViewItem& option,
                               const QModelIndex& index)
{
  if (event && event->type() == QEvent::ToolTip) {
    const QRect pencil = pencilRect(option.rect, index);
    if (!pencil.isNull() && pencil.contains(event->pos())) {
      QToolTip::showText(event->globalPos(),
                         lang::T("Редактировать Lua-скрипт мода"), view);
      return true;
    }
  }
  return QStyledItemDelegate::helpEvent(event, view, option, index);
}

}  // namespace gtamm
