// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "UXTest.h"
#include <QTemporaryDir>
#include <QSet>
#include <memory>
class TestContextualInsertion:public UXTest {
  Q_OBJECT
private slots:
  void initTestCase();void init();void cleanup();void cleanupTestCase();
  void isolatedCandidatePreservesLiveBase();
  void invalidCandidatePreservesLiveBase();
  void cancellationDuringGeometryIsAsynchronous();
  void changedSourceRejectsLateDelivery();
  void ownerTeardownCancelsOnce();
  void paletteFiltersMetadataAndUnavailableItems();
  void actualCutPreviewCancelsWithoutSourceOrUndo();
  void invalidFieldsAndRapidChangesKeepOnlyLatestCandidate();
  void groupedFocusAndEditorShortcutsStaySeparate();
  void numericFieldsAcceptBeforeViewportCommit();
  void realPlaneMoveSpinFlipAndNavigation();
  void sharedScopeAndNamedAnchorRemainExplicit();
  void allCatalogCommitUndoRedoSaveReopen_data();
  void allCatalogCommitUndoRedoSaveReopen();
  void includedTargetCommitsExactlyItsCapturedEditor();
  void readonlyAndSynchronousPreEditChangesRejectCommit();
  void sourceAndTabChangesCancelReadyCommit();
  void precedingTypingHasItsOwnUndoGroup();
  void postCommitEvaluationFailureRetainsSourceAndUndo();
  void libraryOrderAndCatalogReloadInvalidateProof();
  void closedIncludedTargetOpensProvenSource();
  void unavailableLibrariesRemainVisibleWithReasons();
  void httpSnapshotsAndConcurrentSourceTransaction();
private:
  int previousBackend=0;
  QSet<EditorInterface *> originalTabs;
  std::unique_ptr<QTemporaryDir> fixtureFolder;
  QMetaObject::Connection traceConnection;
  QStringList criticalErrors;
};
