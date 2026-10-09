#include "gui/parameter/GroupWidget.h"
#include "gui/UIUtils.h"

#include <QEvent>
#include <QAccessibleWidget>
#include <QAccessibilityHints>
#include <QGuiApplication>
#include <QLineEdit>
#include <QObject>
#include <QSizePolicy>
#include <QString>
#include <QStyleHints>
#include <QStyleOptionToolButton>
#include <QStylePainter>
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

GroupDisclosureButton::GroupDisclosureButton(QWidget *parent) : QToolButton(parent)
{
  connect(QGuiApplication::styleHints()->accessibility(),
          &QAccessibilityHints::contrastPreferenceChanged, this, [this] { update(); });
}

void GroupDisclosureButton::paintEvent(QPaintEvent *event)
{
  // Preserve the native disabled and high-contrast surfaces instead of diluting them.
  // https://doc.qt.io/qt-6/qaccessibilityhints.html#contrastPreference-prop
  if (!isEnabled() || QGuiApplication::styleHints()->accessibility()->contrastPreference() ==
                        Qt::ContrastPreference::HighContrast) {
    QToolButton::paintEvent(event);
    return;
  }

  QStyleOptionToolButton option;
  initStyleOption(&option);

  // QPalette supplies semantic roles, not a complete list of rendered surface colors.
  // Qt explicitly allows styles to derive brushes from those roles:
  // https://doc.qt.io/qt-6/style-drawing.html
  // Derive a quiet section surface from Window + Accent on EVERY paint: this follows
  // light/dark palettes and live palette changes without caching a light-only color.
  // Modify only the temporary style option; setting a widget/application palette would
  // make explicit colors persist across theme changes and can affect child controls.
  // https://doc.qt.io/qt-6/qwidget.html#palette-prop
  // https://doc.qt.io/qt-6/qstylehints.html#colorScheme-prop
  const auto window = option.palette.color(QPalette::Window);
  const auto accent = option.palette.color(QPalette::Accent);
  const qreal amount = option.state.testFlag(QStyle::State_Sunken) ? 0.08 :
                       option.state.testFlag(QStyle::State_MouseOver) ? 0.06 : 0.04;
  const auto surface = QColor::fromRgbF(
    window.redF() + amount * (accent.redF() - window.redF()),
    window.greenF() + amount * (accent.greenF() - window.greenF()),
    window.blueF() + amount * (accent.blueF() - window.blueF()));
  option.palette.setColor(QPalette::Button, surface);

  // Windows 11's controlFillBrush() honors an explicit Button brush before its own
  // hover/press fills, so the blend above supplies those states (press wins).
  // https://github.com/qt/qtbase/blob/v6.11.1/src/plugins/styles/modernwindows/qwindows11style.cpp
  // Keep the button non-checkable: expansion is disclosure state, not selection.
  // Delegate borders, text, arrow, focus and DPI metrics to the native complex control.
  // https://doc.qt.io/qt-6/qstylepainter.html#drawComplexControl
  QStylePainter painter(this);
  painter.drawComplexControl(QStyle::CC_ToolButton, option);
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
