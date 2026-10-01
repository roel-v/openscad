#include "gui/parameter/GroupWidget.h"
#include "gui/UIUtils.h"

#include <QEvent>
#include <QLineEdit>
#include <QObject>
#include <QSizePolicy>
#include <QString>
#include <QWidget>

GroupWidget::GroupWidget(const QString& title, QWidget *parent) : QWidget(parent)
{
  this->toggleButton.setText(title);
  this->toggleButton.setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Maximum);
  this->toggleButton.setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  this->toggleButton.setCheckable(true);
  QFont heading;
  heading.setWeight(QFont::Medium);
  this->toggleButton.setFont(heading);
  setExpanded(false);

  this->mainLayout.setContentsMargins(0, 0, 0, 0);
  this->contentArea.setContentsMargins(0, 0, 0, 0);

  this->contentArea.setLayout(&contentLayout);
  this->contentArea.hide();
  this->mainLayout.addWidget(&toggleButton, 0, 0);
  this->mainLayout.addWidget(&contentArea, 1, 0);
  setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
  setLayout(&mainLayout);
  updateSpacing();

  QObject::connect(&toggleButton, &QToolButton::toggled, this, &GroupWidget::setExpanded);
}

void GroupWidget::updateSpacing()
{
  const int horizontal = UIUtils::layoutSpacing(this, Qt::Horizontal);
  const int vertical = UIUtils::layoutSpacing(this, Qt::Vertical);
  mainLayout.setVerticalSpacing(vertical);
  contentLayout.setSpacing(vertical);
  contentLayout.setContentsMargins(horizontal, 0, horizontal, vertical);
}

void GroupWidget::changeEvent(QEvent *event)
{
  QWidget::changeEvent(event);
  if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) updateSpacing();
}

void GroupWidget::addWidget(QWidget *widget)
{
  contentLayout.addWidget(widget);
}

void GroupWidget::setExpanded(bool expanded)
{
  toggleButton.setChecked(expanded);
  toggleButton.setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
  if (expanded) {
    contentArea.show();
  } else {
    contentArea.hide();
  }
}
