#pragma once
#include "UXTest.h"
class TestAnnotations : public UXTest
{
  Q_OBJECT
private slots:
  void agentAccessLifecycle();
  void sourceInspection();
  void targetInspection();
  void syntaxInspection();
  void modelInspection();
  void deferredModelInspection();
  void guardedViewport();
  void directHttpWorkflow();
  void nativeSavedViews();
  void documentLifecycle();
  void viewportWorkflow();
  void agentAndAutomaticReload();
};
