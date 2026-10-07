#include "TestMainWindow.h"

#include <QString>
#include <QStringList>
#include <QTest>
#include <QFile>
#include <QTemporaryDir>

#include "platform/PlatformUtils.h"

void TestMainWindow::customizerLoadsUseBeforeEvaluatingAssignments()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto write = [](const QString& path, const QByteArray& text) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
  };
  // Unique paths ensure this cannot pass through a previously warmed cache.
  QVERIFY(write(directory.filePath("nested.scad"), "function nested_value()=7;\n"));
  QVERIFY(write(directory.filePath("helper.scad"),
    "use <nested.scad>\nfunction helper_value()=nested_value();\n"));
  const auto root = directory.filePath("root.scad");
  QVERIFY(write(root, "use <helper.scad>\nwidth=3;\n/* [Hidden] */\n"
    "derived=assert(helper_value()==7, \"Customizer must resolve use first\") helper_value();\n"
    "cube([width,derived,1]);\n"));
  const bool autoReload = window->designActionAutoReload->isChecked();
  window->designActionAutoReload->setChecked(false);
  window->tabManager->open(root);
  // parseDocument initializes Customizer before any geometry evaluation.
  // The old ordering throws from the assertion while creating FileContext.
  const auto parsed = window->parseDocument(window->activeEditor);
  QVERIFY(parsed != nullptr);
  window->tabManager->closeCurrentTab();
  window->designActionAutoReload->setChecked(autoReload);
}

void TestMainWindow::checkOpenTabPropagateToWindow()
{
  restoreWindowInitialState();

  QString filename =
    QString::fromStdString(PlatformUtils::resourceBasePath()) + "/tests/basic-ux/empty.scad";

  // When we open a new file,
  window->tabManager->open(filename);

  // The window title must also have the name of open file
  QCOMPARE(window->windowTitle(), QFileInfo(filename).fileName());

  filename = QString::fromStdString(PlatformUtils::resourceBasePath()) + "/tests/basic-ux/empty2.scad";

  // When we open a new file,
  window->tabManager->open(filename);

  // The window title must also have the name of open file
  QCOMPARE(window->windowTitle(), QFileInfo(filename).fileName());
}

void TestMainWindow::checkSaveToShouldUpdateWindowTitle()
{
  restoreWindowInitialState();

  QString filename =
    QString::fromStdString(PlatformUtils::resourceBasePath()) + "/tests/basic-ux/empty.scad";

  // When we open a new file,
  window->tabManager->open(filename);

  window->tabManager->saveAs(window->activeEditor, "test-tmp.scad");

  // The window title must also have the name of open file
  QCOMPARE(window->windowTitle(), "test-tmp.scad");
}
