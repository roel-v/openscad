#pragma once
#include "gui/selection/SelectionTypes.h"
#include <QDockWidget>
#include <QPointer>
#include <functional>
#include <map>
class MainWindow;
class QLabel;
class QVBoxLayout;
class QAction;
namespace Selection {
struct InspectorContribution {
  QString id;
  int order=0;
  QObject *lifetime=nullptr;
  std::function<QWidget*(const State&,const Stamp&)> build;
};
class InspectorDock : public QDockWidget {
  Q_OBJECT
public:
  class Token {
  public:
    Token()=default;
    ~Token() { reset(); }
    Token(const Token&)=delete;
    Token& operator=(const Token&)=delete;
    Token(Token&& other) noexcept;
    Token& operator=(Token&& other) noexcept;
    explicit operator bool() const;
    void reset();
  private:
    friend class InspectorDock;
    Token(InspectorDock *dock,QString id) : dock(dock),id(std::move(id)) {}
    QPointer<InspectorDock> dock;
    QString id;
  };
  explicit InspectorDock(MainWindow *window);
  ~InspectorDock() override;
  Token registerContribution(InspectorContribution contribution);
  QAction *selectionAction() const { return selectAction; }
public slots:
  void beginSelection();
  void refresh();
private:
  struct Entry {
    InspectorContribution contribution;
    QPointer<QObject> owner;
    QPointer<QWidget> widget;
    QMetaObject::Connection lifetime;
  };
  QPointer<MainWindow> window;
  QLabel *properties,*message;
  QVBoxLayout *contributions;
  QAction *selectAction;
  uint64_t tool=0;
  std::map<QString,Entry> entries;
  void remove(const QString& id);
};
}
