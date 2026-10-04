#pragma once
#include "gui/selection/SelectionTypes.h"
#include <QObject>
#include <QPointer>
#include <QSize>
#include <functional>
#include <map>
class MainWindow;
namespace Selection {
enum class OverlayLayer { WorldDepth,WorldOverlay,HUD };
struct OverlayContext {
  Stamp stamp;
  Eigen::Matrix4d modelview,projection;
  QSize logicalViewport;
  double dpr;
};
class ViewportOverlays : public QObject {
public:
  class Token {
  public:
    Token()=default;
    ~Token() { reset(); }
    Token(const Token&)=delete;
    Token& operator=(const Token&)=delete;
    Token(Token&& other) noexcept;
    Token& operator=(Token&& other) noexcept;
    void reset();
    explicit operator bool() const;
  private:
    friend class ViewportOverlays;
    Token(ViewportOverlays *registry,uint64_t id) : registry(registry),id(id) {}
    QPointer<ViewportOverlays> registry;
    uint64_t id=0;
  };
  explicit ViewportOverlays(MainWindow *window);
  Token registerOverlay(QObject *owner,OverlayLayer layer,int order,std::function<void(const OverlayContext&)> draw);
  void draw();
private:
  struct Entry {
    QPointer<QObject> owner;
    OverlayLayer layer;
    int order;
    std::function<void(const OverlayContext&)> draw;
    QMetaObject::Connection lifetime;
  };
  QPointer<MainWindow> window;
  std::map<uint64_t,Entry> entries;
  uint64_t next=0;
  void remove(uint64_t id);
};
}
