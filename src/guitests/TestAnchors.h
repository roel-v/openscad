#pragma once
#include "UXTest.h"
class TestAnchors : public UXTest {
  Q_OBJECT
private slots:
  void initTestCase();
  void init();
  void cleanup();
  void nativePreviewRenderAndSolvedPicks();
  void hiddenTargetsAndParameterReevaluation();
  void failedRelationRetainsOnlyAStaleLastGoodResult();
  void fixedPartCaptureRoutingSaveUndoAndReopen();
  void anonymousCaptureAndStaleSingularGuards();
  void explicitSharedEditsAndAttachmentControls();
  void sourceOwnerReadOnlyAndConcurrentGuards();
  void affineCaptureAndTypedHelpers();
  void includedOwnerAndRetainedResolverGuards();
  void sourceAnnotationAndAnchorJointUndo();
private:
  int previousBackend=0;
  QString fixture() const;
  void evaluate(const QString&,bool final=false);
};
