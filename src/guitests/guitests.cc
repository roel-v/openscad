#include <QTest>
#include <iostream>

#include "TestMainWindow.h"
#include "TestModuleCache.h"
#include "TestTabManager.h"
#include "TestNavigation.h"

template <typename TestClass>
int runTests(MainWindow *window)
{
  TestClass tc;
  tc.setWindow(window);
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
  std::cout << "******************************* RUN UX TESTS ********************************"
            << std::endl;
  totalTestFailures += runTests<TestTabManager>(window);
  totalTestFailures += runTests<TestMainWindow>(window);
  totalTestFailures += runTests<TestModuleCache>(window);
  totalTestFailures += runTests<TestNavigation>(window);
  std::cout << "********************************** RESULTS *********************************"
            << std::endl;
  std::cout << "Failures: " << totalTestFailures << std::endl;
  return totalTestFailures;
}
