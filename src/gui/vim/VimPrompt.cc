#include "gui/vim/VimPrompt.h"
#include <QKeyEvent>
namespace Vim {
void Prompt::keyPressEvent(QKeyEvent *event)
{
  if (event->key() == Qt::Key_Escape) {
    event->accept(); auto callback = cancel; if (callback) callback(true); return;
  }
  QLineEdit::keyPressEvent(event);
}
void Prompt::focusOutEvent(QFocusEvent *event)
{
  QLineEdit::focusOutEvent(event);
  auto callback = cancel; if (callback) callback(false);
}
}
