/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright The OpenSCAD Developers.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this program; if not, see
 *  <https://www.gnu.org/licenses/>.
 */

#include "gui/ColorLabel.h"

#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QString>
#include <algorithm>

#include "core/ColorUtil.h"

bool ColorLabel::isValidButton(QMouseEvent *event)
{
  // Check the button directly triggering the event.
  const auto leftButton = event->button() == Qt::LeftButton;
  const auto rightButton = event->button() == Qt::RightButton;
  return leftButton || rightButton;
}

bool ColorLabel::isValidButtonState(QMouseEvent *event)
{
  // Check the whole current button state to be a single button.
  const auto leftButton = event->buttons() == Qt::LeftButton;
  const auto rightButton = event->buttons() == Qt::RightButton;
  return leftButton || rightButton;
}

void ColorLabel::mousePressEvent(QMouseEvent *event)
{
  if (!isValidButton(event) || doubleClick) {
    return;
  }
  dragStartPosition = event->pos();
  QLabel::mousePressEvent(event);
}

void ColorLabel::mouseReleaseEvent(QMouseEvent *event)
{
  if (!isValidButton(event) || doubleClick) {
    doubleClick = false;
    return;
  }
  emit clicked();
  QLabel::mouseReleaseEvent(event);
}

void ColorLabel::mouseDoubleClickEvent(QMouseEvent *event)
{
  doubleClick = true;
  if (!isValidButton(event)) {
    return;
  }
  if (selected) {
    emit doubleClicked();
  }
  QLabel::mouseDoubleClickEvent(event);
}

void ColorLabel::mouseMoveEvent(QMouseEvent *event)
{
  if (!isValidButtonState(event)) {
    return;
  }
  if ((event->pos() - dragStartPosition).manhattanLength() < QApplication::startDragDistance()) {
    return;
  }
  if (!selected) {
    emit clicked();
  }

  const auto dragText = labelText();

  const QFontMetrics fm{font()};
  QRect rect(0, 0, fm.boundingRect(dragText).width() + fm.height() + 16, fm.height() + 8);
  QPixmap pixmap{rect.width(), rect.height()};
  pixmap.fill({240, 240, 240, 160});

  const auto b = 4;
  QPainter painter(&pixmap);
  painter.setFont(font());
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QColor(Qt::GlobalColor::black));
  painter.drawText(rect.height(), 0, rect.width() - rect.height() - 1, rect.height() - 1,
                   Qt::AlignCenter, dragText);
  painter.fillRect(b, b, rect.height() - 2 * b - 1, rect.height() - 2 * b - 1, QBrush(bgColor));
  painter.drawRect(0, 0, rect.width() - 1, rect.height() - 1);
  painter.setPen(fgColor);
  painter.drawRect(b, b, rect.height() - 2 * b - 1, rect.height() - 2 * b - 1);

  auto mimeData = new QMimeData;
  mimeData->setText(dragText);
  auto drag = new QDrag(this);
  drag->setPixmap(pixmap);
  drag->setMimeData(mimeData);
  drag->setHotSpot({-10, rect.height() + 6});
  drag->exec(Qt::CopyAction);
}

QString ColorLabel::labelText()
{
  const auto isShift = QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
  const auto isXkcdColorMap =
    property(PROPERTY_COLOR_MAP_NAME).toString() == OpenSCAD::COLOR_MAP_NAME_XKCD_COLORS;
  const auto rightButton = QApplication::mouseButtons() == Qt::RightButton;
  const auto tmpl = isShift || rightButton ? QString("color(\"%1\") ") : QString("\"%1\"");
  return tmpl.arg(isXkcdColorMap ? QString("xkcd:%1").arg(text()) : text());
}

void ColorLabel::setColorInfo(const QString& name, const QColor& color)
{
  setText(name);
  setObjectName(name);
  bgColor = color.toRgb();
  const auto dark = bgColor.toHsl().lightnessF() < 0.5;
  fgColor = QColor(dark ? Qt::GlobalColor::white : Qt::GlobalColor::black).toRgb();
  updateAppearance();
}

void ColorLabel::setSelected(const bool selected)
{
  this->selected = selected;
  updateAppearance();
}

void ColorLabel::updateAppearance()
{
  // Swatch colors are content, not application theme colors.
  auto colors = palette();
  colors.setColor(QPalette::Window, bgColor);
  colors.setColor(QPalette::WindowText, fgColor);
  setPalette(colors);
  setAutoFillBackground(true);
  setFrameStyle(QFrame::Box | QFrame::Plain);
  setIndent(0);
  setLineWidth(selected ? 3 : 1);
  setMargin(selected ? 4 : 6);
}
