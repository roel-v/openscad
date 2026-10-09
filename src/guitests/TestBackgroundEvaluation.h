#pragma once
#include "UXTest.h"

class TestBackgroundEvaluation : public UXTest {
  Q_OBJECT
private slots:
  void explicitPriorityAndCoalescing();
  void cancellationKeepsEventLoopResponsive();
  void cancelLanguageThenEvaluateAgain();
  void renderBothBackends();
  void capturedParametersRejectMissingOrIncompatibleValues();
  void cancelledQueuedJobNeverComputes();
  void obsoleteQueuedPreviewRetainsLastDisplay();
  void failedPreviewRetainsLastDisplay();
  void openPreviewsWithoutAutoReload();
  void startupPreviewsWithoutAutoReload();
};
