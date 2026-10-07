#pragma once
#include "gui/evaluation/EvaluationPipeline.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QSet>
#include <map>

class MainWindow;
class EditorInterface;
class QLabel;
class QPushButton;
namespace BackgroundEvaluation {
class Controller final : public QObject {
  Q_OBJECT
public:
  explicit Controller(MainWindow *window);
  ~Controller() override;
  void request(EditorInterface *editor, Mode mode, bool automatic = false);
  void cancel(EditorInterface *editor);
  void bind(EditorInterface *editor);
  void activate(EditorInterface *editor);
  bool busy() const { return !pending.empty(); }
  bool busy(EditorInterface *editor) const;
  bool hasDeferred(EditorInterface *editor) const;
  QJsonArray status() const;
signals:
  void attemptFinished(QJsonObject metrics);
  void framePresented(QJsonObject metrics);
private:
  struct Pending {
    QPointer<EditorInterface> owner;
    uint64_t revision = 0, parameterRevision = 0;
    Request request;
    std::shared_ptr<Result> result{new Result, [](Result *value) {
      Service::instance().retire([value] { delete value; });
    }};
    Job::Ptr job;
    size_t delivered = 0;
    QString cameraRevision;
    bool validating = false;
    bool terminalReported = false;
    QString publicationError;
    QJsonObject publicationMetadata;
    quint64 validationEpoch = 0;
    std::vector<SourceInput> validationEditors;
    std::set<std::string> validationUsedEditors;
    std::map<QString, std::pair<QPointer<EditorInterface>, uint64_t>> editors;
    std::vector<QMetaObject::Connection> inputConnections;
    QJsonObject lifecycle;
    Job::Clock::time_point cancelled;
    ~Pending() { for (auto connection : inputConnections) QObject::disconnect(connection); }
  };
  MainWindow *window;
  QLabel *label;
  QPushButton *stop;
  QTimer timer;
  QFileSystemWatcher watcher;
  quint64 watcherEpoch = 0;
  std::map<QString, std::shared_ptr<Pending>> pending;
  QSet<EditorInterface *> boundEditors;
  std::map<QString, QString> terminal;
  std::shared_ptr<Pending> deferred;
  struct CancelledInput { uint64_t source, parameters; double time; };
  std::map<QString, CancelledInput> cancelledRevisions;
  QJsonObject awaitingFrame;
  Job::Clock::time_point installedAt;
  bool current(const Pending& job) const;
  bool configurationCurrent(const Request& request) const;
  void finish(const QString& key, const std::shared_ptr<Pending>& job);
  void install(const std::shared_ptr<Pending>& job);
  void validate(const std::shared_ptr<Pending>& job);
  void poll();
  void drain(const std::shared_ptr<Pending>& attempt, size_t limit);
};
}
