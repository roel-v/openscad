#pragma once
#include "UXTest.h"
class TestClipping : public UXTest {
  Q_OBJECT
private slots:
  void initTestCase();
  void init();
  void rearSurfacesAndFrozenPreview();
  void suppliedPiecesAndOwnership();
  void numericalGpuBoundaries();
  void backendNumericalBoundaries();
  void nativePieceRoles();
  void cancellationAndUnsupportedInputs();
  void uniformUpdatesAndPerformance();
  void canonicalDockLifecycle();
  void presetsLimitsAndSourceOnly();
  void routedPicksAndGestures();
  void sourceManipulatorRotationAndOwnership();
  void cavityAndTransformedPickAlignment();
};
