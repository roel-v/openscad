#pragma once
#include <Qsci/qsciscintilla.h>
#include <QPointer>
namespace Vim {
class EditorController;
class Scintilla : public QsciScintilla {
  QPointer<EditorController> controller;
public:
  explicit Scintilla(QWidget *parent) : QsciScintilla(parent) {}
  void bind(EditorController *);
protected:
  bool event(QEvent *) override;
  void keyPressEvent(QKeyEvent *) override;
  void inputMethodEvent(QInputMethodEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void dropEvent(QDropEvent *) override;
  void focusOutEvent(QFocusEvent *) override;
};
}
