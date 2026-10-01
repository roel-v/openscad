#include <QTest>
#include <iostream>

#include "TestMainWindow.h"
#include "TestModuleCache.h"
#include "TestTabManager.h"
#include "TestNavigation.h"
#include "TestAnnotations.h"

template <typename TestClass>
int runTests(MainWindow *window)
{
  TestClass tc;
  tc.setWindow(window);
  const auto log = qEnvironmentVariable("OPENSCAD_ANNOTATION_TEST_LOG");
  if (!log.isEmpty()) return QTest::qExec(&tc, QStringList{"annotation-tests", "-o", log + ",txt"});
  return QTest::qExec(&tc);
  return 0;
}

int runAllTest(MainWindow *window)
{
  if (qEnvironmentVariableIsSet("OPENSCAD_NAVIGATION_TESTS_ONLY")) {
    TestNavigation test;
    test.setWindow(window);
    QStringList arguments{"navigation-tests"};
    const auto output = qEnvironmentVariable("OPENSCAD_NAVIGATION_TEST_OUTPUT");
    if (!output.isEmpty()) arguments << "-o" << output + "/test-results.xml,junitxml";
    return QTest::qExec(&test, arguments);
  }
  int totalTestFailures = 0;
  if (qEnvironmentVariableIsSet("OPENSCAD_ANNOTATION_TESTS_ONLY")) return runTests<TestAnnotations>(window);
  std::cout << "******************************* RUN UX TESTS ********************************"
            << std::endl;
  totalTestFailures += runTests<TestTabManager>(window);
  totalTestFailures += runTests<TestMainWindow>(window);
  totalTestFailures += runTests<TestModuleCache>(window);
  totalTestFailures += runTests<TestNavigation>(window);
  totalTestFailures += runTests<TestAnnotations>(window);
  std::cout << "********************************** RESULTS *********************************"
            << std::endl;
  std::cout << "Failures: " << totalTestFailures << std::endl;
  return totalTestFailures;
}
