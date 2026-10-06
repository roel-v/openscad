#pragma once
#include "gui/InspectionPresentationController.h"
#include <QObject>
#include <QPointer>
#include <atomic>
class MainWindow;

// Geometry work is serialized with ordinary compilation. Every job owns its frozen scene/tree.
class ClipMeshJob : public QObject {
public:
  struct Metrics {
    std::atomic<uint64_t> jobs{0}, evaluations{0}, reusedFinal{0};
    double preparationMilliseconds = 0;
  };
  explicit ClipMeshJob(MainWindow *window);
  InspectionPresentationController::Cancel start(std::shared_ptr<const Selection::EvaluatedScene>,
    Selection::Stamp, InspectionPresentationController::Done);
  const Metrics& metrics() const { return *counts; }
private:
  QPointer<MainWindow> window;
  std::shared_ptr<Metrics> counts;
  QString cachedResult;
  std::shared_ptr<const InspectionMeshSet> cached;
};
