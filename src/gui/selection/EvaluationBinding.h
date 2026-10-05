#pragma once
#include "core/EvaluationInputs.h"
#include "gui/selection/SelectionTypes.h"
#include <QObject>
#include <QPointer>
#include <QFileSystemWatcher>
#include <memory>

class MainWindow;
class EditorInterface;
class AbstractNode;
class SourceFile;
class SourceFileCache;
class EvaluationSession;
class Context;
class FileContext;
class Renderer;

namespace Selection {
// Destruction releases contexts before their session, and ASTs after the session.
struct EvaluationBacking {
  std::shared_ptr<EvaluationInputs> inputs;
  std::shared_ptr<SourceFileCache> libraries;
  std::shared_ptr<SourceFile> parsedRoot;
  std::shared_ptr<EvaluationSession> session;
  std::shared_ptr<const Context> builtin;
  std::shared_ptr<const FileContext> file;
};
struct EvaluatedScene {
  std::shared_ptr<const EvaluationBacking> backing;
  std::string documentPath;
  std::shared_ptr<const EvaluationInputs> inputs;
  std::shared_ptr<const AbstractNode> root;
};

class EvaluationBinding : public QObject {
  Q_OBJECT
public:
  explicit EvaluationBinding(MainWindow *window);
  static std::shared_ptr<EvaluationInputs> capture(MainWindow& window, EditorInterface& root);
  static bool inputsCurrent(const EvaluationInputs& inputs, MainWindow& window, QString *detail = nullptr);
  void install(std::shared_ptr<const EvaluatedScene> scene, EditorInterface *owner,
               const QString& resultId, const QString& configurationFingerprint);
  void invalidate(const QString& reason);
  void rendererChanged();
  bool replacePresentationRenderer(std::shared_ptr<Renderer> renderer, const Stamp& expected);
  void observeInputs();
  // Cheap display state. Disk notifications may arrive on the next event-loop turn.
  bool current() const;
  bool takeDiskChanges() { const bool changed=diskInputsChanged; diskInputsChanged=false; return changed; }
  // Actions must still verify exact dependency bytes through validate()/scene().
  bool validate(const Stamp& expected, QString *detail = nullptr) const;
  const Stamp& stamp() const { return installedStamp; }
  std::shared_ptr<const EvaluatedScene> scene(const Stamp& expected) const;
  QString staleReason() const { return reason; }
signals:
  void invalidated();
  void installed();
private:
  QPointer<MainWindow> window;
  QPointer<EditorInterface> owner;
  std::shared_ptr<const EvaluatedScene> retainedScene;
  Stamp installedStamp;
  quint64 viewportGeneration = 0;
  bool fresh = false;
  QString reason;
  struct RendererBinding {
    std::weak_ptr<Renderer> renderer;
    QPointer<EditorInterface> owner;
    std::shared_ptr<const EvaluatedScene> scene;
    Stamp stamp;
    bool eligible = true;
  };
  std::map<Renderer *, RendererBinding> renderers;
  std::vector<QMetaObject::Connection> inputConnections;
  QFileSystemWatcher inputWatcher;
  void editorsChanged();
  bool diskInputsChanged = false;
};
}
