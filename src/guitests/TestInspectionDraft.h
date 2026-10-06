#pragma once
#include "UXTest.h"
class TestInspectionDraft : public UXTest {
  Q_OBJECT
private slots:
  void initTestCase();
  void init();
  void cleanup();
  void cleanupTestCase();
  void baseCandidateBasePreservesModelAndRejectsCapture();
  void masksAndEffectsRestoreWithoutCandidateInstallation();
  void lateProviderAndOwnerTeardown();
  void realSourceAndOrdinaryCompileCancelOnce();
  void focusGroupAndSessionDrag();
  void replacementChecksTheOriginalEditor();
  void capturesAppliedCustomizerValues();
  void ordinaryEvaluationUsesCapturedValuesOnce();
private:
  int previousBackend=0;
};
