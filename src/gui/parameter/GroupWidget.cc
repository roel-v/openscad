#include "gui/parameter/GroupWidget.h"
#include "gui/UIUtils.h"

#include <QEvent>
#include <QAccessibleWidget>
#include <QLineEdit>
#include <QObject>
#include <QSizePolicy>
#include <QString>
#include <QWidget>

namespace {
class AccessibleDisclosure : public QAccessibleWidget
{
public:
  explicit AccessibleDisclosure(QToolButton *button) : QAccessibleWidget(button, QAccessible::Button) {}
  QAccessible::State state() const override
  {
    auto result = QAccessibleWidget::state();
    const auto *group = qobject_cast<GroupWidget *>(widget()->parentWidget());
    result.expandable = true;
    result.expanded = group->isExpanded();
    result.collapsed = !group->isExpanded();
    return result;
  }
  QString text(QAccessible::Text type) const override
  {
    if (type == QAccessible::Name) return static_cast<QToolButton *>(widget())->text();
    return QAccessibleWidget::text(type);
  }
  QStringList actionNames() const override { return {QAccessibleActionInterface::pressAction()}; }
  void doAction(const QString& action) override
  {
    if (action == QAccessibleActionInterface::pressAction() && widget()->isEnabled())
      static_cast<QToolButton *>(widget())->click();
  }
  QStringList keyBindingsForAction(const QString&) const override { return {QStringLiteral("Space")}; }
};
}

GroupWidget::GroupWidget(const QString& title, QWidget *parent) : QWidget(parent), groupTitle(title)
{
  static const bool accessibleInstalled = [] {
    QAccessible::installFactory([](const QString&, QObject *object) -> QAccessibleInterface * {
      auto *button = qobject_cast<QToolButton *>(object);
      if (button && button->objectName() == QStringLiteral("customizerDisclosure") &&
          qobject_cast<GroupWidget *>(button->parentWidget())) return new AccessibleDisclosure(button);
      return nullptr;
    });
    return true;
  }();
  Q_UNUSED(accessibleInstalled);
  this->toggleButton.setObjectName("customizerDisclosure");
  this->toggleButton.setText(UIUtils::sentenceCaseLabel(title));
  this->toggleButton.setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Maximum);
  this->toggleButton.setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  this->toggleButton.setCheckable(false);
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

  QObject::connect(&toggleButton, &QToolButton::clicked, this, [this] { setExpanded(!expanded); });
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
  const bool changed = this->expanded != expanded;
  this->expanded = expanded;
  toggleButton.setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
  if (expanded) {
    contentArea.show();
  } else {
    contentArea.hide();
  }
  if (changed) {
    QAccessible::State state;
    state.expanded = state.collapsed = true;
    QAccessibleStateChangeEvent event(&toggleButton, state);
    QAccessible::updateAccessibility(&event);
  }
}
