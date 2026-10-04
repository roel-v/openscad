#pragma once
#include "UXTest.h"
#include "gui/selection/SelectionTypes.h"
#include <optional>

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
private:
  int previousBackend = 0;
  bool compile(const QString& source, bool final = false);
  std::optional<Selection::Hit> pick(const Vector3d& point);
};
