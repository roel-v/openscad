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
class InspectionPresentationController;
namespace SourceProvenance { class SyntaxTable; class Result; }
namespace Design { struct Result; }
namespace Semantic { struct Scene; }

namespace Selection {
// Destruction releases contexts before their session, and ASTs after the session.
struct EvaluationBacking {
  std::shared_ptr<EvaluationInputs> inputs;
  std::shared_ptr<SourceFileCache> libraries;
  std::shared_ptr<SourceFile> parsedRoot;
  std::shared_ptr<SourceProvenance::SyntaxTable> syntax;
  std::shared_ptr<EvaluationSession> session;
  std::shared_ptr<const Context> builtin;
  std::shared_ptr<const FileContext> file;
};
struct EvaluatedScene {
  std::shared_ptr<const Semantic::Scene> semantic;
  std::shared_ptr<const Design::Result> designObjects;
  std::shared_ptr<const EvaluationBacking> backing;
  std::string documentPath;
  std::shared_ptr<const EvaluationInputs> inputs;
  std::shared_ptr<const AbstractNode> root;
  std::shared_ptr<const SourceProvenance::Result> provenance;
};

class EvaluationBinding : public QObject {
  Q_OBJECT
public:
  explicit EvaluationBinding(MainWindow *window);
  static std::shared_ptr<EvaluationInputs> capture(MainWindow& window, EditorInterface& root);
  static bool inputsCurrent(const EvaluationInputs& inputs, MainWindow& window, QString *detail = nullptr);
  // Caller has validated bytes on the evaluation worker and rechecked revisions
  // and watcher epochs immediately before this non-reentrant publication.
  void install(std::shared_ptr<const EvaluatedScene> scene, EditorInterface *owner,
               const QString& resultId, const QString& configurationFingerprint);
  void invalidate(const QString& reason);
  void rendererChanged();
  bool replacePresentationRenderer(std::shared_ptr<Renderer> renderer, const Stamp& expected);
  void observeInputs();
  // Cheap display state. Disk notifications may arrive on the next event-loop turn.
  bool current() const;
  bool takeDiskChanges() { const bool changed=diskInputsChanged; diskInputsChanged=false; return changed; }
  // Display-only checks: no filesystem reads or source hashing. Do not use these
  // to authorize edits, exports, picking or asynchronous result installation.
  bool displayCurrent(const Stamp& expected) const;
  bool modelDisplayCurrent(const Stamp& expected) const;
  std::shared_ptr<const EvaluatedScene> displayScene(const Stamp& expected) const;
  // Actions must still verify exact dependency bytes through validate()/scene().
  bool validate(const Stamp& expected, QString *detail = nullptr) const;
  bool validateModel(const Stamp& expected, QString *detail = nullptr) const;
  bool draftPixels() const { return hypotheticalPixels; }
  const Stamp& stamp() const { return installedStamp; }
  std::shared_ptr<const EvaluatedScene> scene(const Stamp& expected) const;
  QString staleReason() const { return reason; }
signals:
  void invalidated();
  void installed();
private:
  friend class ::InspectionPresentationController;
  bool hypotheticalPixels = false;
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
