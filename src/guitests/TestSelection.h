#pragma once
#include "UXTest.h"

class TestSelection : public UXTest {
  Q_OBJECT
private slots:
  void initTestCase();
  void init();
  void cleanup();
  void dirtyDependencies();
  void conflictingBuffers();
  void staleAndWrongDocument();
  void retainedSceneAfterClose();
  void surfacePreview();
  void surfaceFinal();
  void surfaceCapabilities();
  void recenteredSurface();
  void inspectionMeshes();
  void inspectionProviderLifetime();
  void toolLifecycle();
  void inspectorExtensions();
  void overlayIsolation();
  void dragAndMeasurementOwnership();
  void resizedBordersAndInstances();
  void staleDependencyDuringPick();
private:
  int previousBackend = 0;
};
