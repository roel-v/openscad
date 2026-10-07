#include "UXTest.h"

#include <QString>
#include <QTabWidget>
#include <QTest>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include "gui/evaluation/EvaluationController.h"

#include "platform/PlatformUtils.h"

QString UXTest::fixturePath(const QString& relative)
{
  return QString::fromStdString(PlatformUtils::resourceBasePath()) + "/tests/data/" + relative;
}

void UXTest::setWindow(MainWindow *window_)
{
  window = window_;
}

void UXTest::restoreWindowInitialState()
{
  // Only discard documents in this test-owned window. Earlier cases may leave
  // dirty source or companion metadata, including when an assertion fails.
  QTimer discard;
  connect(&discard, &QTimer::timeout, window, [this] {
    auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (box && window->isAncestorOf(box) && box->button(QMessageBox::Discard))
      box->button(QMessageBox::Discard)->click();
  });
  discard.start(20);
  QString filename = fixturePath("basic-ux/default.scad");
  window->tabManager->open(filename);
  auto target = window->activeEditor;
  auto tabs = qobject_cast<QTabWidget *>(window->tabManager->getTabContent());
  Q_ASSERT(tabs);

  while (window->tabManager->count() > 1) {
    const auto previousCount = window->tabManager->count();
    for (int index = 0; index < tabs->count(); ++index) {
      if (tabs->widget(index) != target) { tabs->setCurrentIndex(index); break; }
    }
    window->tabManager->closeCurrentTab();
    QVERIFY2(window->tabManager->count() < previousCount, "Fixture tab was not closed");
  }

  window->designActionAutoReload->setChecked(true);  // Enable auto-reload  & preview
}

bool UXTest::waitForEvaluation()
{
  QElapsedTimer deadline;
  deadline.start();
  while (window->backgroundEvaluation->busy() && deadline.elapsed() < 20000) QTest::qWait(10);
  return !window->backgroundEvaluation->busy();
}
