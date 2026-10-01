#pragma once
#include "UXTest.h"
class TestAnnotations : public UXTest
{
  Q_OBJECT
private slots:
  void agentAccessLifecycle();
  void documentLifecycle();
  void viewportWorkflow();
  void agentAndAutomaticReload();
};
