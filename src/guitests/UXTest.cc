#include "UXTest.h"

#include <QString>
#include <QApplication>
#include <QMessageBox>
#include <QTabWidget>
#include <QTest>
#include <QTimer>

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
    // This reset owns the disposable test tabs. Earlier suites can deliberately
    // leave session-only records dirty even when their source buffer is clean.
    bool unexpectedDialog = false;
    QTimer discard;
    discard.setInterval(20);
    QObject::connect(&discard, &QTimer::timeout, [&] {
      auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog) return;
      auto box = qobject_cast<QMessageBox *>(dialog);
      if (box && box->standardButtons().testFlag(QMessageBox::Discard))
        box->done(QMessageBox::Discard);
      else { unexpectedDialog = true; dialog->reject(); }
    });
    discard.start();
    window->tabManager->closeCurrentTab();
    discard.stop();
    QVERIFY2(!unexpectedDialog, "Unexpected dialog while discarding a fixture tab");
    QVERIFY2(window->tabManager->count() < previousCount, "Fixture tab was not closed");
  }

  window->designActionAutoReload->setChecked(true);  // Enable auto-reload  & preview
}
