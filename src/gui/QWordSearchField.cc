#include "gui/QWordSearchField.h"

#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QResizeEvent>
#include <QString>
#include <QStyle>

QWordSearchField::QWordSearchField(QFrame *parent) : QLineEdit(parent)
{
  findcount = 0;
  fieldLabel = new QLabel(this);
  fieldLabel->setTextFormat(Qt::PlainText);
  fieldLabel->setText(QString("00"));
  fieldLabel->setCursor(Qt::ArrowCursor);
  fieldLabel->setForegroundRole(QPalette::Text);
  fieldLabel->hide();
  connect(this, &QWordSearchField::findCountChanged, this, &QWordSearchField::updateFieldLabel);
  auto frameWidth = style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
  setTextMargins(0, 0, fieldLabel->sizeHint().width() + frameWidth + 1, 0);
  auto minsize = minimumSizeHint();
  setMinimumSize(qMax(minsize.width(), fieldLabel->sizeHint().height() + frameWidth * 2 + 2),
                 qMax(minsize.height(), fieldLabel->sizeHint().height() + frameWidth * 2 + 2));
  fieldLabel->setAlignment(Qt::AlignRight);
}

void QWordSearchField::resizeEvent(QResizeEvent *)
{
  resizeSearchField();
}

void QWordSearchField::resizeSearchField()
{
  auto size = fieldLabel->sizeHint();
  auto frameWidth = style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
  fieldLabel->resize(size);
  setTextMargins(0, 0, fieldLabel->isHidden() ? 0 : size.width() + frameWidth + 1, 0);
  fieldLabel->move(rect().right() - frameWidth - size.width(),
                   (rect().bottom() + 1 - size.height()) / 2);
}

void QWordSearchField::changeEvent(QEvent *event)
{
  QLineEdit::changeEvent(event);
  if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
    fieldLabel->setMinimumSize(fieldLabel->minimumSizeHint());
    resizeSearchField();
  }
}

void QWordSearchField::updateFieldLabel()
{
  if (findcount > 0) {
    fieldLabel->setText(QString::number(findcount));
    // Fixes issue #2962 : Due to that fieldLabel->setText above does not seem to change the size of the
    // field correct (seems to always be to short field to accommodate all digits) when the field changes
    // many times during several searches, we need to work around that by setting minimum size.
    fieldLabel->setMinimumSize(fieldLabel->minimumSizeHint());
    fieldLabel->setVisible(true);
  } else {
    fieldLabel->setText(QString(""));
    fieldLabel->setVisible(false);
  }
  resizeSearchField();
}

void QWordSearchField::setFindCount(int value)
{
  if (value != findcount) {
    findcount = value;
    emit findCountChanged();
  }
}
