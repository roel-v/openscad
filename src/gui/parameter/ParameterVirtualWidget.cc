#include "gui/parameter/ParameterVirtualWidget.h"
#include "gui/UIUtils.h"
#include "gui/Icons.h"
#include <QStyle>

#include <QBoxLayout>
#include <QEvent>
#include <QMouseEvent>
#include <QToolTip>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QWidget>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

ParameterDescriptionWidget::ParameterDescriptionWidget(QWidget *parent) : QWidget(parent)
{
  setupUi(this);
  helpIcon = new QLabel(this);
  helpIcon->setObjectName("parameterHelpIcon");
  helpIcon->setFocusPolicy(Qt::NoFocus);
  helpIcon->setAlignment(Qt::AlignCenter);
  helpIcon->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  horizontalLayout->insertWidget(1, helpIcon, 0, Qt::AlignVCenter);
  helpIcon->installEventFilter(this);
  helpTimer.setSingleShot(true);
  helpTimer.setInterval(250);
  connect(&helpTimer, &QTimer::timeout, this, &ParameterDescriptionWidget::showHelp);
  horizontalLayout->setStretch(horizontalLayout->indexOf(labelInline), 1);
  helpIcon->hide();
  QFont emphasis;
  emphasis.setWeight(QFont::Medium);
  labelParameter->setFont(emphasis);
  labelParameter->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  labelParameter->setWordWrap(true);
  labelInline->setWordWrap(true);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  updateSpacing();
}

void ParameterDescriptionWidget::updateSpacing()
{
  verticalLayout->setSpacing(UIUtils::layoutSpacing(this, Qt::Vertical) / 2);
  horizontalLayout->setSpacing(UIUtils::layoutSpacing(this, Qt::Horizontal) / 2);
  if (helpIcon) {
    const int size = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    helpIcon->setMargin(UIUtils::layoutSpacing(this, Qt::Horizontal) / 4);
    Icons::setLabelIcon(helpIcon, "help", size);
  }
}

void ParameterDescriptionWidget::changeEvent(QEvent *event)
{
  QWidget::changeEvent(event);
  if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) updateSpacing();
}

void ParameterDescriptionWidget::showHelp()
{
  helpTimer.stop();
  if (helpIcon->isVisible() && !helpIcon->toolTip().isEmpty()) {
    QToolTip::showText(helpIcon->mapToGlobal(helpIcon->rect().bottomLeft()),
                     helpIcon->toolTip(), helpIcon, helpIcon->rect());
  }
}

bool ParameterDescriptionWidget::eventFilter(QObject *object, QEvent *event)
{
  if (object == helpIcon) {
    switch (event->type()) {
    case QEvent::Enter:
      helpTimer.start();
      break;
    case QEvent::Leave:
    case QEvent::Hide:
      helpTimer.stop();
      QToolTip::hideText();
      break;
    case QEvent::ToolTip:
      showHelp();
      return true;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
      if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        if (event->type() == QEvent::MouseButtonPress) showHelp();
        return true;
      }
      break;
    default:
      break;
    }
  }
  return QWidget::eventFilter(object, event);
}

void ParameterDescriptionWidget::setDescription(ParameterObject *parameter,
                                                DescriptionStyle descriptionStyle)
{
  labelParameter->setText(UIUtils::sentenceCaseLabel(
    QString::fromStdString(parameter->name()).replace(QRegularExpression("([_]+)"), " ")));
  const auto description = QString::fromStdString(parameter->description());
  labelParameter->show();
  labelInline->show();
  labelDescription->show();
  helpIcon->hide();
  helpIcon->setToolTip({});
  setToolTip({});
  setAccessibleDescription(description);
  if (description.trimmed().isEmpty()) {
    labelDescription->hide();
    labelInline->setText("");
  } else if (descriptionStyle == DescriptionStyle::ShowDetails) {
    labelDescription->setText(QString::fromStdString(parameter->description()));
    labelInline->setText("");
  } else if (descriptionStyle == DescriptionStyle::Inline) {
    labelDescription->hide();
    labelInline->setText(QString(" - ") + QString::fromStdString(parameter->description()));
  } else if (descriptionStyle == DescriptionStyle::HideDetails) {
    labelDescription->hide();
    labelInline->setText("");
    // Escape author text before wrapping it for a native, multiline tooltip.
    helpIcon->setToolTip("<p>" + description.toHtmlEscaped().replace("\n", "<br/>") + "</p>");
    helpIcon->setAccessibleName(tr("Help for %1").arg(labelParameter->text()));
    helpIcon->setAccessibleDescription(description);
    helpIcon->show();
  } else if (descriptionStyle == DescriptionStyle::DescriptionOnly) {
    labelParameter->hide();
    labelDescription->setText(QString::fromStdString(parameter->description()));
    labelInline->hide();
  } else {
    assert(false);
  }
}

ParameterVirtualWidget::ParameterVirtualWidget(QWidget *parent, ParameterObject *parameter)
  : QWidget(parent), parameter(parameter)
{
  QSizePolicy policy;
  policy.setHorizontalPolicy(QSizePolicy::Ignored);
  policy.setVerticalPolicy(QSizePolicy::Preferred);
  policy.setHorizontalStretch(0);
  policy.setVerticalStretch(0);
  this->setSizePolicy(policy);

  // The group owns the content inset; individual fields must not add it again.
  setContentsMargins(0, 0, 0, 0);
}

bool ParameterVirtualWidget::event(QEvent *event)
{
  const bool result = QWidget::event(event);
  if (event->type() == QEvent::Polish || event->type() == QEvent::FontChange ||
      event->type() == QEvent::StyleChange) {
    // Derived widgets install their Designer layouts after our constructor.
    for (auto *box : findChildren<QBoxLayout *>()) {
      const auto direction = box->direction();
      const auto orientation = direction == QBoxLayout::LeftToRight || direction == QBoxLayout::RightToLeft
                                 ? Qt::Horizontal : Qt::Vertical;
      box->setSpacing(UIUtils::layoutSpacing(this, orientation) / 2);
    }
  }
  return result;
}

int ParameterVirtualWidget::decimalsRequired(double value)
{
  int decimals = 0;
  value = std::abs(value);
  while (std::floor(value) < std::ceil(value) && decimals < 7) {
    decimals++;
    value *= 10.0;
  }
  return decimals;
}

int ParameterVirtualWidget::decimalsRequired(const std::vector<double>& values)
{
  assert(!values.empty());
  int decimals = 0;
  for (double value : values) {
    decimals = std::max(decimals, decimalsRequired(value));
  }
  return decimals;
}
