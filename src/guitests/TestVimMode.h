#pragma once
#include "UXTest.h"
class ScintillaEditor;
class TestVimMode : public UXTest {
  Q_OBJECT
  ScintillaEditor *editor = nullptr;
private slots:
  void initTestCase();
  void init();
  void cleanup();
  void preferencePreservesTextHistoryAndUpdatesWindows();
  void shortcutPriorityAndOtherFields();
  void completionAndCalltipOneEscape();
  void nativeCompletionImeAndRepeat();
  void pendingInputAndPromptStayWithTheirDocument();
  void annotationSourceTransactionAndRepeatHistory();
  void externalActionClosesInsertionBeforeUndoGroup();
  void literalSearchAndCancellation();
  void protectedSaveCloseReloadAndUnicodeOpen();
  void saveAsConflictAndFailedSaveFeedback();
  void unsupportedAndReadOnly();
};
class TestVimPersistence : public UXTest {
  Q_OBJECT
private slots:
  void persistedPreferenceIsAppliedAtStartup();
};
