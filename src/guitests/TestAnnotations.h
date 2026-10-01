#pragma once
#include "UXTest.h"
class TestAnnotations : public UXTest
{
  Q_OBJECT
private slots:
  void agentAccessLifecycle();
  void sourceInspection();
  void targetInspection();
  void documentLifecycle();
  void viewportWorkflow();
  void agentAndAutomaticReload();
};
