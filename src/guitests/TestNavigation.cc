#include "TestNavigation.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include <QFrame>
#include <QLabel>
#include <QSizeGrip>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include <QDoubleSpinBox>
#include "gui/TabManager.h"

void TestNavigation::cameraOperationBoundary()
{
  auto view = window->qglview;
  auto navigation = view->navigation();
  const auto original = navigation->current();
  QSignalSpy display(navigation, &CameraNavigation::displayChanged);
  QSignalSpy operation(view, &QGLView::cameraChanged);
  auto target = original;
  target.rotation = {31,17,83};
  navigation->apply(target, false);
  QCOMPARE(display.count(), 1);
  QCOMPARE(operation.count(), 0);
  navigation->apply(original);
  QCOMPARE(operation.count(), 1);
  navigation->observe();
  QCOMPARE(display.count(), 2);
  navigation->allowNavigation = [] { return false; };
  navigation->apply(target);
  QVERIFY(navigation->current() == original);
  navigation->allowNavigation = {};
  view->cam.object_rot = target.rotation;
  navigation->observe();
  QCOMPARE(display.count(), 3);
  QCOMPARE(operation.count(), 1);
  navigation->apply(original, false);
  operation.clear();
  window->viewActionTop->trigger();
  QCOMPARE(view->cam.object_rot.x(), 90.0);
  QCOMPARE(operation.count(), 1);
  auto field = window->findChild<QDoubleSpinBox *>("doubleSpinBox_rx");
  QVERIFY(field);
  field->setValue(42.0);
  navigation->observe();
  QCOMPARE(view->cam.getVpr().x(), 42.0);
  QCOMPARE(operation.count(), 1);
  navigation->apply(original, false);
}

void TestNavigation::nativeOverlayComposition()
{
  auto view = window->qglview;
  // Windows launchers may suppress the first ShowWindow call. The compositor probe needs exposure.
  window->hide();
  window->show();
  QVERIFY(QTest::qWaitForWindowExposed(window));
  QTRY_VERIFY(view->isValid());
  auto editor = window->activeEditor;
  const auto source = editor->toPlainText();
  const bool wasModified = editor->isContentModified();
  editor->setPlainText("$vpr=[30,0,50]; cube([30,15,8]); translate([25,0,8]) sphere(7);");
  QSignalSpy compilation(window, &MainWindow::compilationDone);
  window->actionRenderPreview();
  QTRY_VERIFY(!compilation.isEmpty());
  QVERIFY(view->getRenderer());
  QCOMPARE(window->displayedDocument(), editor);
  QVERIFY(view->cam.getVpr().isApprox(Eigen::Vector3d(30,0,50), 1e-10));
  QLabel overlay(QStringLiteral("X  Y  Z"), view);
  overlay.setStyleSheet("color: #e05030; background: transparent;");
  overlay.setGeometry(qMax(0, view->width()-120), 12, 100, 35);
  overlay.show();
  auto reposition = connect(view, &QGLView::resized, &overlay, [&] {
    overlay.move(qMax(0, view->width()-120), 12);
  });
  view->update();
  QTest::qWait(100);
  const auto pose = view->navigation()->current();
  auto moved = pose;
  moved.rotation.z() += 23;
  view->navigation()->apply(moved, false);
  QTest::qWait(100);
  const auto frame = window->grab();
  QVERIFY(!frame.isNull());
  const auto output = qEnvironmentVariable("OPENSCAD_NAVIGATION_TEST_OUTPUT");
  if (!output.isEmpty()) {
    QDir().mkpath(output);
    QVERIFY(frame.save(output + "/native-child-composition.png"));
  }
  QFrame popup(view, Qt::Popup | Qt::FramelessWindowHint);
  popup.setAttribute(Qt::WA_NoMouseReplay);
  popup.setFrameShape(QFrame::Box);
  auto layout = new QVBoxLayout(&popup);
  layout->addWidget(new QLabel(tr("Viewport settings"), &popup));
  layout->addWidget(new QSizeGrip(&popup), 0, Qt::AlignRight);
  popup.resize(230, 100);
  popup.move(view->mapToGlobal(QPoint(qMax(0,view->width()-250),50)));
  popup.show();
  QTRY_VERIFY(popup.isVisible());
  popup.resize(280,150);
  QCOMPARE(popup.size(), QSize(280,150));
  if (!output.isEmpty()) QVERIFY(popup.grab().save(output + "/native-popup-probe.png"));
  QTest::mouseClick(window->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                    view->mapTo(window, QPoint(4,4)));
  QTRY_VERIFY(!popup.isVisible());
  disconnect(reposition);
  view->navigation()->apply(pose, false);
  editor->setPlainText(source);
  editor->setContentModified(wasModified);
}

void TestNavigation::documentMetadataLifetimeProbe()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto path = directory.filePath("metadata-probe.scad");
  QFile source(path);
  QVERIFY(source.open(QIODevice::WriteOnly));
  source.write("cube(10);\n");
  source.close();
  window->tabManager->open(path);
  auto editor = window->activeEditor;
  const auto displayedOwner = window->displayedDocument();
  editor->setProperty("navigation-metadata-probe", "unsaved metadata");
  QVERIFY(!editor->isContentModified());
  QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
  source.write("sphere(10);\n");
  source.close();
  QVERIFY(window->tabManager->refreshDocument());
  QCOMPARE(window->activeEditor, editor);
  QCOMPARE(window->displayedDocument(), displayedOwner);
  QCOMPARE(editor->property("navigation-metadata-probe").toString(), QString("unsaved metadata"));
  QVERIFY(editor->toPlainText().contains("sphere"));
  editor->setContentModified(false);
  window->tabManager->closeCurrentTab();
}
