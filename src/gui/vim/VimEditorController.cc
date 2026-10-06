#include "gui/vim/VimEditorController.h"
#include "gui/vim/VimPrompt.h"
#include "gui/vim/VimScintilla.h"
#include "gui/ScintillaEditor.h"
#include <QLabel>
#include <QVBoxLayout>
#include <QTimer>
namespace Vim {
EditorController::EditorController(ScintillaEditor *value)
  : QObject(value), editor(value), state(value->qsci), modeLabel(new QLabel(value)), prompt(new Prompt(value))
{
  modeLabel->setObjectName("vimModeStatus"); prompt->setObjectName("vimPrompt");
  modeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  modeLabel->setToolTip(tr("Vim: h j k l, w b e, 0 ^ $, gg G, f F t T, ; ,; d c y + motion; "
    "dd cc yy; i a I A o O; v V; x p P; u Ctrl+R .; / ? n N; :w :q :e :e path. "
    "Text objects: iw aw, i/a with ( [ { and quotes. Escape returns to Normal. "
    "Other printable commands are refused."));
  auto layout = static_cast<QVBoxLayout *>(value->layout());
  layout->addWidget(modeLabel); layout->addWidget(prompt);
  modeLabel->hide(); prompt->hide();
  static_cast<Scintilla *>(value->qsci)->bind(this);
  state.callbacks([this] { update(); }, [this](QChar kind) { openPrompt(kind); },
    [this](const HostRequest& request) {
      QPointer<EditorController> guard(this);
      auto callback = host;
      auto result = callback ? callback(request) : HostResult::Failed;
      if (guard && result != HostResult::Success) {
        QString feedback;
        switch (request.command) {
        case HostCommand::Save: feedback = tr("Save cancelled or failed"); break;
        case HostCommand::Close: feedback = tr("Close cancelled or failed"); break;
        case HostCommand::Reload:
          feedback = editor && editor->filepath.isEmpty() ? tr("Reload unavailable: this document has no filename") : tr("Reload cancelled or failed"); break;
        case HostCommand::Open: feedback = tr("Open failed"); break;
        }
        modeLabel->setText("NORMAL | " + feedback);
      }
      return result;
    });
  connect(value, &EditorInterface::externalEditStarting, this, [this] {
    cancelPrompt(); state.beforeExternalMutation();
  });
  connect(value->qsci, &QsciScintillaBase::SCN_MODIFIED, this,
    [this](int position, int type, const char *text, int length, int, int, int, int, int, int) {
      state.modified(position, type, text, length);
    });
  connect(prompt, &QLineEdit::returnPressed, this, [this] {
    // A document command can close this editor, including while a Save As modal
    // dialog is open. Do not destroy QLineEdit inside its own key-processing stack.
    if (promptKind == ':') QTimer::singleShot(0, this, &EditorController::submitPrompt);
    else submitPrompt();
  });
  prompt->cancel = [this](bool returnFocus) { cancelPrompt(); if (returnFocus && editor) editor->setFocus(); };
}
EditorController::~EditorController()
{
  prompt->cancel = {}; state.callbacks({}, {}, {});
  state.beforeExternalMutation(); state.setEnabled(false);
}
void EditorController::update()
{
  QString name;
  switch (state.mode()) {
  case Mode::Normal: name = tr("NORMAL"); break;
  case Mode::Insert: name = tr("INSERT"); break;
  case Mode::VisualChar: name = tr("VISUAL"); break;
  case Mode::VisualLine: name = tr("VISUAL LINE"); break;
  }
  modeLabel->setText(name + (state.feedback().isEmpty() ? QString() : " | " + state.feedback()));
}
void EditorController::setEnabled(bool enabled)
{
  if (state.enabled() == enabled) return;
  cancelPrompt(); state.setEnabled(enabled); modeLabel->setVisible(enabled);
}
void EditorController::bindHost(std::function<HostResult(const HostRequest&)> callback) { host = std::move(callback); }
QString EditorController::displayText() const { return modeLabel->text(); }
void EditorController::openPrompt(QChar kind)
{
  if (!editor) return;
  cancelPrompt();
  originalCaret = editor->qsci->SendScintilla(S::SCI_GETCURRENTPOS);
  originalAnchor = editor->qsci->SendScintilla(S::SCI_GETANCHOR);
  promptKind = kind; prompt->clear(); prompt->setPlaceholderText(QString(kind));
  openingPrompt = true; prompt->show(); prompt->setFocus(); openingPrompt = false;
}
void EditorController::cancelPrompt(bool restore)
{
  if (promptKind.isNull()) return;
  promptKind = {}; prompt->hide();
  if (restore && editor) editor->qsci->SendScintilla(S::SCI_SETSEL, originalAnchor, originalCaret);
}
void EditorController::submitPrompt()
{
  if (!editor || promptKind.isNull()) return;
  const auto kind = promptKind; const auto text = prompt->text();
  QPointer<EditorController> guard(this); QPointer<ScintillaEditor> origin(editor);
  cancelPrompt();
  if (kind == ':') state.submitCommand(text);
  else state.submitSearch(text, kind == '/');
  if (guard && origin) origin->setFocus();
}
void EditorController::cancel() { cancelPrompt(); state.cancelPending(); }
void EditorController::editorFocusLost()
{
  if (!openingPrompt) cancel();
}
}
