#pragma once
#include "core/semantic/SemanticTargets.h"
#include "gui/semantic/SemanticExportJob.h"
#include "gui/selection/SelectionTypes.h"
#include <QObject>
#include <QPointer>
#include <atomic>
class MainWindow;
namespace Semantic {
class TargetService : public QObject {
  Q_OBJECT
public:
  explicit TargetService(MainWindow *);
  ~TargetService() override;
  std::shared_ptr<Targets> targets() const { return currentTargets; }
  PreparedTarget resolve(TargetKey);
  void retry(TargetKey);
  bool validate(const Selection::Stamp&) const;
  void cancel();
  bool exportTarget(TargetKey, ExportSpace, QString destination, ExportInfo);
signals:
  void changed();
  void exportFinished(bool success, QString message);
private:
  QPointer<MainWindow> window;
  Selection::Stamp stamp;
  std::shared_ptr<Targets> currentTargets;
  std::shared_ptr<QObject> dispatcher;
  std::vector<std::shared_ptr<std::atomic<bool>>> jobs;
  void refresh();
  void request(TargetKey);
};
}
