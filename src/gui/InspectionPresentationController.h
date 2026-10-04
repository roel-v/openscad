#pragma once
#include "glview/InspectionPresentation.h"
#include "gui/selection/SelectionTypes.h"
#include <QObject>
#include <QPointer>
#include <functional>
class MainWindow;
class Renderer;
class InspectionMeshRenderer;
class QLabel;
class QPushButton;
namespace Selection { struct EvaluatedScene; }

class InspectionPresentationController : public QObject {
  Q_OBJECT
public:
  enum class Status { Inactive, Preparing, Ready, Empty, Unsupported, Cancelled, Failed, Stale };
  struct Completion {
    Status status = Status::Failed;
    std::shared_ptr<const InspectionMeshSet> meshes;
    QString reason;
  };
  using Done = std::function<void(Completion)>;
  using Cancel = std::function<void()>;
  using Start = std::function<Cancel(std::shared_ptr<const Selection::EvaluatedScene>, Selection::Stamp, Done)>;
  using Factory = std::function<std::shared_ptr<Renderer>(std::shared_ptr<Renderer>)>;
  enum class Kind { Provider, Effect, Request };
  class Token {
  public:
    Token() = default;
    ~Token() { reset(); }
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
    Token(Token&& other) noexcept;
    Token& operator=(Token&& other) noexcept;
    explicit operator bool() const;
    void reset();
    bool setEnabled(bool enabled);
    bool setMask(const InspectionMask& mask, const Selection::Stamp& stamp, uint64_t generation);
    void clearMask();
    void uniformsChanged();
  private:
    friend class InspectionPresentationController;
    Token(InspectionPresentationController *controller, Kind kind, uint64_t id);
    QPointer<InspectionPresentationController> controller;
    Kind kind = Kind::Request;
    uint64_t id = 0;
  };
  explicit InspectionPresentationController(MainWindow *window);
  ~InspectionPresentationController() override;
  Token registerMeshProvider(std::string id, int priority, QObject *lifetime, Start start);
  Token registerMeshEffect(std::string id, int order, QObject *lifetime, Factory factory);
  Token requestMeshes(std::string owner, QObject *lifetime);
  void cancelPreparation();
  Status status() const { return currentStatus; }
  QString reason() const { return statusReason; }
  std::shared_ptr<const InspectionMeshSet> meshes() const { return registry.meshes(); }
  Selection::Stamp stamp() const;
  uint64_t meshUploadCount() const;
signals:
  void statusChanged();
  void meshesChanged();
private:
  struct Provider { QPointer<QObject> lifetime; Start start; };
  struct Effect { QPointer<QObject> lifetime; Factory factory; };
  QPointer<MainWindow> window;
  InspectionPresentation registry;
  std::map<uint64_t,Provider> providers;
  std::map<uint64_t,Effect> effects;
  std::map<uint64_t,QPointer<QObject>> requests;
  std::map<uint64_t,QMetaObject::Connection> lifetimes;
  std::shared_ptr<QObject> dispatcher;
  std::shared_ptr<Renderer> defaultRenderer;
  std::shared_ptr<InspectionMeshRenderer> meshRenderer;
  Selection::Stamp jobStamp;
  Cancel cancel;
  Status currentStatus = Status::Inactive;
  QString statusReason;
  bool replacing = false;
  QLabel *label;
  QPushButton *cancelButton;
  bool contains(Kind kind,uint64_t id) const;
  Token scoped(Kind kind,uint64_t id,QObject *lifetime);
  void release(Kind kind,uint64_t id);
  void refresh(bool newResult=false);
  void stopJob();
  void complete(uint64_t ticket,uint64_t provider,Completion completion);
  void compose();
  void replace(std::shared_ptr<Renderer> renderer);
  void presentationChanged();
  void setStatus(Status status,QString reason={});
};
