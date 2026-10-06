#include "TestModuleCache.h"

#include <QString>
#include <QStringList>
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <memory>

#include "core/node.h"
#include "platform/PlatformUtils.h"

void touchFile(const QString& filename)
{
  auto timeStamp = QDateTime::currentDateTime();

  QFileInfo fileInfo(filename);
  QFile file(filename);
  file.open(QIODevice::ReadWrite);
  if (file.isOpen()) {
    file.setFileTime(timeStamp, QFileDevice::FileModificationTime);
    file.setFileTime(timeStamp, QFileDevice::FileAccessTime);
  }
}

void TestModuleCache::testBasicCache()
{
  restoreWindowInitialState();

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto filename = directory.filePath("module-cache.scad");
  QFile source(filename);
  QVERIFY(source.open(QIODevice::WriteOnly));
  QCOMPARE(source.write("cube(1);\n"), qint64(9));
  source.close();
  SourceFile *previousFile{nullptr};
  SourceFile *currentFile{nullptr};
  QObject connectionScope;
  connect(window, &MainWindow::compilationDone, &connectionScope,
          [&currentFile](SourceFile *file) { currentFile = file; });
  QSignalSpy completed(window, &MainWindow::compilationDone);

  window->designActionAutoReload->setChecked(false);  // Disable auto-reload  & preview
  window->tabManager->open(filename);                 // Open use.scad
  const auto firstCount = completed.count();
  window->actionReloadRenderPreview();                // F5

  QTRY_VERIFY(completed.count() > firstCount);
  QTRY_VERIFY2(currentFile != nullptr, "The nonempty temporary model should be loaded.");
  previousFile = currentFile;  // save the loaded Source from the

  const auto secondCount = completed.count();
  window->actionReloadRenderPreview();
  QTRY_VERIFY(completed.count() > secondCount);
  QVERIFY2(previousFile == currentFile,
           "The file should be the same as the file cache should have done its work.");
  QTest::qWait(1000);

  // Content-based document reload intentionally ignores timestamp-only changes.
  // Change the temporary model as well so this exercises actual cache invalidation.
  QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QCOMPARE(source.write("cube(2);\n"), qint64(9));
  source.close();
  touchFile(filename);
  const auto thirdCount = completed.count();
  window->actionReloadRenderPreview();
  QTRY_VERIFY(completed.count() > thirdCount);
  QTRY_VERIFY2(
    previousFile != currentFile,
    "The cache should be invalidated after the model content and timestamp change.");
}

std::vector<std::string>& findNode(std::shared_ptr<AbstractNode> node, std::vector<std::string>& path)
{
  path.push_back(node->verbose_name());
  for (auto child : node->getChildren()) return findNode(child, path);
  return path;
}

void TestModuleCache::testMCAD()
{
  restoreWindowInitialState();

  QString filename = fixturePath("modulecache-tests/use-mcad.scad");
  window->tabManager->open(filename);   // Open use-mcad.scad
  window->actionReloadRenderPreview();  // F5

  auto node = window->instantiateRootFromSource(window->rootFile.get());
  QVERIFY2(node->verbose_name().empty(), "Root node name must be empty");
  QVERIFY2(node->getChildren().size() != 0, "There must have at least a node");
  QCOMPARE(QString::fromStdString(node->getChildren()[0]->verbose_name()),
           QString::fromStdString("module roundedBox"));
}
