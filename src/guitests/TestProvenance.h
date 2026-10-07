#pragma once
#include "UXTest.h"
#include "gui/selection/SelectionTypes.h"
#include <optional>
#include <QVariant>

class TestProvenance : public UXTest {
  Q_OBJECT
private slots:
  void initTestCase();
  void init();
  void cleanup();
  void previewInstances();
  void finalBoolean();
  void sourceTransactions();
  void dirtyDependencyNavigation();
  void staleRecompileAndFailure();
  void navigationTabRace();
  void closedSource();
  void cachedLibrariesAndSourceVersions();
  void cachedLookupShadowing();
  void inspectionOnDemand();
  void inspectionPreparationFailure();
private:
  int previousBackend = 0;
  QVariant previousInspectorPreference;
  bool compile(const QString& source, bool final = false);
  std::optional<Selection::Hit> pick(const Vector3d& point);
};
