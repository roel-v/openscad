#pragma once
#include "gui/vim/engine/Engine.h"
#include <QObject>
#include <QPointer>
class ScintillaEditor;
class QLabel;
namespace Vim {
class Prompt;
class EditorController : public QObject {
  QPointer<ScintillaEditor> editor;
  Engine state;
  QLabel *modeLabel;
  Prompt *prompt;
  QChar promptKind;
  Position originalCaret = 0, originalAnchor = 0;
  bool openingPrompt = false;
  std::function<HostResult(const HostRequest&)> host;
  void update();
  void openPrompt(QChar);
  void submitPrompt();
public:
  explicit EditorController(ScintillaEditor *);
  ~EditorController() override;
  Engine& engine() { return state; }
  void setEnabled(bool);
  void bindHost(std::function<HostResult(const HostRequest&)>);
  void cancelPrompt(bool restore = true);
  void cancel();
  void editorFocusLost();
  Prompt *promptWidget() const { return prompt; }
  QString displayText() const;
};
// A guarded native Qt/Scintilla input scope. External commands never use this.
class NativeInput {
  QPointer<EditorController> controller;
public:
  explicit NativeInput(EditorController *value) : controller(value) {
    if (controller && controller->engine().enabled() && controller->engine().mode() == Mode::Insert)
      controller->engine().beginNativeInput();
    else controller.clear();
  }
  ~NativeInput() { if (controller) controller->engine().endNativeInput(); }
  NativeInput(const NativeInput&) = delete;
};
}
