#pragma once
#include "UXTest.h"

class TestNavigation : public UXTest {
  Q_OBJECT
private slots:
  void cameraOperationBoundary();
  void nativeOverlayComposition();
  void documentMetadataLifetimeProbe();
  void cubeTargetsAndGestures();
  void interruptibleTransitions();
};
