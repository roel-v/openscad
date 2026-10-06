#pragma once
#include "UXTest.h"
class TestSemanticHierarchy : public UXTest {
  Q_OBJECT
private slots:
  void initTestCase();
  void init();
  void cleanup();
  void hierarchyAndTargets();
  void viewportStructureInteractions();
  void initializeUnstructuredModel();
  void pickingAndMasks();
  void transformedFinishedParts();
  void effectsAndToolArbitration();
  void retainedJobsAndStaleDelivery();
  void nativePlacementAndDefaults();
  void sourceNavigationAndSection();
  void parameterWidgetsAndUndo();
  void sharedCallsAndDefaultEdits();
  void parameterOwnerAndContextGuards();
  void typedParameterControlsAndGlobalCustomizer();
  void closeWindowWithPendingTargets();
  void foreignPresentationDrawIdsRemainUnowned();
private:
  int previousBackend=0;
  void evaluate(const QString&,bool final=false);
};
