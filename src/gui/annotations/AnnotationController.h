#pragma once
#include "AnnotationStore.h"
#include "AnnotationDrawing.h"
#include "glview/Camera.h"
#include <QPointer>
#include <QTimer>
#include <QMutex>
#include <QElapsedTimer>
#include <QJsonArray>
#include <functional>
#include <memory>
#include <map>

class MainWindow;
class EditorInterface;
class Dock;
class QGLView;
class QLabel;
class QListWidget;
class QLineEdit;
class QDialog;
class Renderer;
struct Message;

namespace AgentAnnotations {
class Controller : public QObject
{
  Q_OBJECT
public:
  explicit Controller(MainWindow *window);
  ~Controller() override;
  Dock *dock;
  const QString windowId, viewportId;
  EditorInterface *owner() const;
  QVector<EditorInterface *> editors() const;
  EditorInterface *actionOwner() const;
  bool busy() const;
  bool drafting() const { return state != Idle; }
  void beginEvaluation(const QString& source);
  void finishEvaluation(bool success);
  void rendererChanged();
  void startDrawing();
  void cancelDrawing();
  void refresh();
  QJsonObject status() const;
  QJsonObject camera() const;
  bool setCamera(const QJsonObject& camera, QString *error);
  bool cameraOperation(const QJsonObject& operation, QString *error);
  QString viewRevision() const;
  QJsonObject savedViews() const;
  QJsonObject savedViewOperation(const QString& action, const QString& id, const QJsonObject& request, QString *error);
  void cancelCapture() { finishFrame("Agent access stopped."); }
  void capture(const QString& expectedResult,
               std::function<void(QJsonObject, QByteArray, QString)> done);
  void setAccessWidget(QWidget *widget);
  QJsonObject diagnosticContext() const;
  void observeDiagnostic(const Message& message, const QJsonObject& context);
  QJsonObject inspection(const QString& document) const;
  void pruneInspection();
signals:
  void changed();

private:
  enum State { Idle, DrawingState, Capturing, Comment } state = Idle;
  MainWindow *window;
  QGLView *view;
  QWidget *overlay, *palette, *panel;
  QLabel *ownerLabel, *imageLabel, *commentLabel;
  QListWidget *list;
  QLineEdit *search;
  QPointer<EditorInterface> displayedOwner, evaluationOwner, draftOwner;
  QPointer<QDialog> commentDialog;
  QJsonObject result, evaluation, lastAttempt, draftResult;
  mutable QMutex contextMutex;
  QJsonObject messageContext;
  QHash<QString, QJsonArray> attemptHistory, resultHistory, diagnosticHistory;
  QElapsedTimer evaluationTimer;
  struct Installed {
    std::weak_ptr<Renderer> renderer;
    QPointer<EditorInterface> owner;
    QJsonObject result;
  };
  std::map<Renderer *, Installed> installed;
  quint64 generation = 0, draftToken = 0, captureToken = 0;
  mutable quint64 viewGeneration = 0;
  mutable QJsonObject observedView;
  Drawing drawing;
  QColor color = QColor("#ffcf33");
  Camera lockedCamera;
  QSize draftSize;
  qreal draftDpr = 1;
  QString draftSourceHash;
  bool reloadWasActive = false, animationWasActive = false;
  int animationInterval = 0;
  bool capturePending = false;
  QMetaObject::Connection frameConnection;
  QTimer captureTimeout;
  QTimer draftWatch;
  QJsonObject draftDisplay;
  QByteArray draftFiles;
  QString originalSaveText, originalUndoText;
  std::function<void(QJsonObject, QByteArray, QString)> captureDone;
  void bindEditors();
  void showSelected();
  void acceptDrawing();
  void requestFrame(bool annotation, const QString& expectedResult,
                    std::function<void(QJsonObject, QByteArray, QString)> done);
  void finishFrame(const QString& error = {});
  void updateLock();
  void updateActions();
  QJsonObject display() const;
  QByteArray fileState() const;
  bool eventFilter(QObject *object, QEvent *event) override;
};
}  // namespace AgentAnnotations
