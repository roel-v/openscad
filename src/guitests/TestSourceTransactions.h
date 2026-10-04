#pragma once
#include "UXTest.h"

class TestSourceTransactions : public UXTest {
  Q_OBJECT
private slots:
  void capturedDocumentUndoAndHttp();
  void conflictsCancelSaveAsCopyAndPersistence();
  void companionFailureReportsSavedSource();
  void sourceDecodingPreservesExistingPolicy();
  void overwriteApprovalIsBoundToObservedVersion();
  void annotationFailureReportsSavedSource();
};
