#include "gui/vim/VimScintilla.h"
#include "gui/vim/VimEditorController.h"
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QDropEvent>
namespace Vim {
void Scintilla::bind(EditorController *value) { controller = value; }
bool Scintilla::event(QEvent *event)
{
  if (event->type() == QEvent::ShortcutOverride && controller &&
      controller->engine().claimsShortcut(fromEvent(*static_cast<QKeyEvent *>(event)))) {
    event->accept(); return true;
  }
  return QsciScintilla::event(event);
}
void Scintilla::keyPressEvent(QKeyEvent *event)
{
  QPointer<Scintilla> guard(this);
  if (controller && controller->engine().enabled()) {
    const auto result = controller->engine().key(fromEvent(*event));
    if (!guard || result == InputResult::Consumed) { event->accept(); return; }
    // Only a claimed Insert key is part of the insertion journal.
    if (result == InputResult::PassToEditor) {
      NativeInput input(controller);
      QsciScintilla::keyPressEvent(event); return;
    }
  }
  QsciScintilla::keyPressEvent(event);
}
void Scintilla::inputMethodEvent(QInputMethodEvent *event)
{
  if (controller && controller->engine().enabled()) {
    if (controller->engine().mode() != Mode::Insert) { event->accept(); return; }
    NativeInput input(controller);
    QsciScintilla::inputMethodEvent(event); return;
  }
  QsciScintilla::inputMethodEvent(event);
}
void Scintilla::mousePressEvent(QMouseEvent *event)
{
  if (controller && controller->engine().enabled() && controller->engine().mode() != Mode::Insert)
    controller->cancel();
  // Scintilla can accept a completion from the mouse, without a key event.
  NativeInput input(SendScintilla(SCI_AUTOCACTIVE) ? controller.data() : nullptr);
  QsciScintilla::mousePressEvent(event);
}
void Scintilla::mouseReleaseEvent(QMouseEvent *event)
{
  NativeInput input(SendScintilla(SCI_AUTOCACTIVE) ? controller.data() : nullptr);
  QsciScintilla::mouseReleaseEvent(event);
}
void Scintilla::dropEvent(QDropEvent *event)
{
  NativeInput input(controller);
  QsciScintilla::dropEvent(event);
}
void Scintilla::focusOutEvent(QFocusEvent *event)
{
  if (controller) controller->editorFocusLost();
  QsciScintilla::focusOutEvent(event);
}
}
