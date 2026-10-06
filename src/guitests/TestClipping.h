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
  void deleteAndSessionPersistence();
  void presetsLimitsAndSourceOnly();
  void routedPicksAndGestures();
  void sourceDirectionAndOwnership();
  void directionControlsAndDocumentState();
  void positionSpinButtons();
  void emptyDocumentSection();
  void openExampleAfterSection();
  void worldOffsetPoseMapping();
  void cavityAndTransformedPickAlignment();
};
