#include <QTest>
#include <QDir>
#include <iostream>

#include "TestMainWindow.h"
#include "TestModuleCache.h"
#include "TestTabManager.h"
#include "TestSourceTransactions.h"
#include "TestVimMode.h"
#include "TestDesignObjects.h"
#include "TestAnnotations.h"
#include "TestNavigation.h"
#include "TestSelection.h"
#include "TestProvenance.h"
#include "TestInspectionDraft.h"

template <typename TestClass>
int runTests(MainWindow *window)
{
  TestClass tc;
  tc.setWindow(window);
  const auto output = qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");
  if (!output.isEmpty()) {
    const auto log = QDir(output).filePath(QString::fromLatin1(tc.metaObject()->className()) + ".txt");
    return QTest::qExec(&tc, QStringList{"gui-tests", "-o", log + ",txt"});
  }
  const auto log = qEnvironmentVariable("OPENSCAD_ANNOTATION_TEST_LOG");
  if (!log.isEmpty()) return QTest::qExec(&tc, QStringList{"annotation-tests", "-o", log + ",txt"});
  return QTest::qExec(&tc);
}

int runAllTest(MainWindow *window)
{
  if(qEnvironmentVariableIsSet("OPENSCAD_INSPECTION_DRAFT_TESTS_ONLY"))return runTests<TestInspectionDraft>(window);
  if (qEnvironmentVariableIsSet("OPENSCAD_VIM_PERSISTENCE_ONLY")) return runTests<TestVimPersistence>(window);
  if (qEnvironmentVariableIsSet("OPENSCAD_VIM_TESTS_ONLY")) return runTests<TestVimMode>(window);
  if (qEnvironmentVariableIsSet("OPENSCAD_NAVIGATION_TESTS_ONLY")) {
    TestNavigation test;
    test.setWindow(window);
    QStringList arguments{"navigation-tests"};
    const auto output = qEnvironmentVariable("OPENSCAD_NAVIGATION_TEST_OUTPUT");
    if (!output.isEmpty()) arguments << "-o" << output + "/test-results.xml,junitxml";
    return QTest::qExec(&test, arguments);
  }
  int totalTestFailures = 0;
  if (qEnvironmentVariableIsSet("OPENSCAD_PROVENANCE_TESTS_ONLY")) return runTests<TestProvenance>(window);
  if (qEnvironmentVariableIsSet("OPENSCAD_DESIGN_OBJECT_TESTS_ONLY")) return runTests<TestDesignObjects>(window);
  if (qEnvironmentVariableIsSet("OPENSCAD_SELECTION_TESTS_ONLY")) return runTests<TestSelection>(window);
  if (qEnvironmentVariableIsSet("OPENSCAD_ANNOTATION_TESTS_ONLY")) return runTests<TestAnnotations>(window);
  std::cout << "******************************* RUN UX TESTS ********************************"
            << std::endl;
  totalTestFailures += runTests<TestTabManager>(window);
  totalTestFailures += runTests<TestSourceTransactions>(window);
  totalTestFailures += runTests<TestVimMode>(window);
  totalTestFailures += runTests<TestDesignObjects>(window);
  totalTestFailures += runTests<TestMainWindow>(window);
  totalTestFailures += runTests<TestModuleCache>(window);
  totalTestFailures += runTests<TestAnnotations>(window);
  totalTestFailures += runTests<TestNavigation>(window);
  totalTestFailures += runTests<TestSelection>(window);
  totalTestFailures += runTests<TestProvenance>(window);
  totalTestFailures += runTests<TestInspectionDraft>(window);
  std::cout << "********************************** RESULTS *********************************"
            << std::endl;
  std::cout << "Failures: " << totalTestFailures << std::endl;
  return totalTestFailures;
}
