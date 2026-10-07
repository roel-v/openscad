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
  QString cleanupError;
  connect(&discard, &QTimer::timeout, window, [this, &cleanupError] {
    auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog) return;
    bool owned = false;
    for (QObject *parent = dialog; parent; parent = parent->parent())
      if (parent == window) { owned = true; break; }
    auto box = qobject_cast<QMessageBox *>(dialog);
    if (owned && box && box->button(QMessageBox::Discard)) {
      box->button(QMessageBox::Discard)->click();
    } else {
      cleanupError = QString("Unexpected cleanup dialog %1: %2 %3")
        .arg(dialog->metaObject()->className(), dialog->windowTitle(), box ? box->text() : QString());
      // This is the isolated --run-all-gui-tests process. Fail instead of leaving it modal.
      dialog->reject();
    }
  });
  discard.start(20);
  QString filename = fixturePath("basic-ux/default.scad");
  window->tabManager->open(filename);
  QVERIFY2(cleanupError.isEmpty(), qPrintable(cleanupError));
  auto target = window->activeEditor;
  auto tabs = qobject_cast<QTabWidget *>(window->tabManager->getTabContent());
  Q_ASSERT(tabs);

  while (window->tabManager->count() > 1) {
    const auto previousCount = window->tabManager->count();
    for (int index = 0; index < tabs->count(); ++index) {
      if (tabs->widget(index) != target) { tabs->setCurrentIndex(index); break; }
    }
    window->tabManager->closeCurrentTab();
    QVERIFY2(cleanupError.isEmpty(), qPrintable(cleanupError));
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
