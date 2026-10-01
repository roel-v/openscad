#pragma once

#include <QMessageBox>
#include "gui/Icons.h"

// Keep QMessageBox behavior while applying the application's status artwork before exec().
class MessageBox : public QMessageBox
{
public:
  using QMessageBox::QMessageBox;
  int exec() override
  {
    Icons::prepareMessageBox(this);
    return QMessageBox::exec();
  }
  static StandardButton information(QWidget *parent, const QString& title, const QString& text,
                                    StandardButtons buttons = Ok,
                                    StandardButton defaultButton = NoButton)
  {
    return message(Information, parent, title, text, buttons, defaultButton);
  }
  static StandardButton warning(QWidget *parent, const QString& title, const QString& text,
                                StandardButtons buttons = Ok, StandardButton defaultButton = NoButton)
  {
    return message(Warning, parent, title, text, buttons, defaultButton);
  }
  static StandardButton critical(QWidget *parent, const QString& title, const QString& text,
                                 StandardButtons buttons = Ok, StandardButton defaultButton = NoButton)
  {
    return message(Critical, parent, title, text, buttons, defaultButton);
  }
  static StandardButton question(QWidget *parent, const QString& title, const QString& text,
                                 StandardButtons buttons = StandardButtons(Yes | No),
                                 StandardButton defaultButton = NoButton)
  {
    return message(Question, parent, title, text, buttons, defaultButton);
  }

private:
  static StandardButton message(Icon icon, QWidget *parent, const QString& title, const QString& text,
                                StandardButtons buttons, StandardButton defaultButton)
  {
    MessageBox box(icon, title, text, buttons, parent);
    box.setDefaultButton(defaultButton);
    return static_cast<StandardButton>(box.exec());
  }
};
