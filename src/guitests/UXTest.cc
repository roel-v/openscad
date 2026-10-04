#include "UXTest.h"

#include <QString>
#include <QTabWidget>
#include <QTest>

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
