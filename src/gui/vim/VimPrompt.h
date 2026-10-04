#pragma once
#include <QLineEdit>
#include <functional>
namespace Vim {
class Prompt : public QLineEdit {
public:
  explicit Prompt(QWidget *parent) : QLineEdit(parent) {}
  std::function<void(bool)> cancel;
protected:
  void keyPressEvent(QKeyEvent *) override;
  void focusOutEvent(QFocusEvent *) override;
};
}
