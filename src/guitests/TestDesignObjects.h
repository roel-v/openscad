#pragma once
#include "UXTest.h"
class TestDesignObjects : public UXTest {
  Q_OBJECT
private slots:
  void initTestCase();
  void nativeDeclarationsAndParametricEdits();
  void promotionUndoRedoIdentityAndDelete();
  void transactionFailureHistoryAndLifetime();
  void dirtyIncludeGuardsAndSourceOnlyFields();
  void scopedLifecycleAdapters();
  void consumerTransientDraftValidationAndJointUndo();
  void personalViewsPromotionAndCamera();
  void documentSavesConflictsAndPartialOutcomes();
};
