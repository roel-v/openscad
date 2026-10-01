#pragma once
#include "UXTest.h"

class TestNavigation : public UXTest {
  Q_OBJECT
private slots:
  void settingsOnStartup();
  void cameraOperationBoundary();
  void nativeOverlayComposition();
  void documentMetadataLifetimeProbe();
  void cubeTargetsAndGestures();
  void interruptibleTransitions();
  void popupAndTuning();
  void savedViewTransactions();
  void savedViewDocumentLifecycle();
  void statisticsAndCleanCapture();
  void visualCalibrationAndClose();
  void resultStatisticsStates();
  void settingsBeforeExit();
};
