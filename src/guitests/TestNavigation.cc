#include "TestNavigation.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include "gui/NavigationCube.h"
#include "gui/ViewportPopup.h"
#ifdef ENABLE_NAVIGATION_CUBE_TUNING
#include "gui/NavigationTuning.h"
#endif
#include <QRadioButton>
#include <QCheckBox>
#include <QClipboard>
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
#include <QPainter>
#include "gui/TabManager.h"

void TestNavigation::cameraOperationBoundary()
{
  auto view = window->qglview;
  auto navigation = view->navigation();
  navigation->settings.animate = false;
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

void TestNavigation::cubeTargetsAndGestures()
{
  window->designActionAutoReload->setChecked(false);
  auto view=window->qglview;
  auto nav=view->navigation();
  auto cube=view->navigationCube();
  nav->settings.animate=false;
  const auto original=nav->current();
  QImage targetGrid(cube->width()*6,cube->height()*5,QImage::Format_ARGB32_Premultiplied);
  targetGrid.fill(window->palette().color(QPalette::Window));
  QPainter gridPainter(&targetGrid);
  for (const auto& face : NavigationCubeGeometry::facets(0.22)) {
    cube->snap(face.id);
    QVERIFY((CameraPose::matrix(view->cam.object_rot)*face.normal).isApprox(Eigen::Vector3d(0,-1,0),1e-10));
    QCOMPARE(view->cam.projection,original.projection);
    QCOMPARE(view->cam.fov,original.fov);
    QCOMPARE(view->cam.viewer_distance,original.distance);
    QVERIFY(view->cam.object_trans==original.translation);
    gridPainter.drawPixmap((face.id%6)*cube->width(),(face.id/6)*cube->height(),cube->grab());
  }
  nav->apply(original,false);
  for (int i=0;i<4;++i) cube->step(Eigen::Vector3d::UnitY(),90);
  QVERIFY(CameraPose::matrix(view->cam.object_rot).isApprox(CameraPose::matrix(original.rotation),1e-10));
  const auto position=cube->pos();
  const QPoint center(qRound(cube->width()*0.5),qRound(cube->width()*80.0/140));
  const QPoint end=center+QPoint(18,8);
  QSignalSpy selected(view,&QGLView::doLeftClick);
  nav->settings.freeOrbit=true;
  QTest::mousePress(cube,Qt::LeftButton,Qt::NoModifier,center);
  QTest::mouseMove(cube,end,20);
  QTest::mouseRelease(cube,Qt::LeftButton,Qt::NoModifier,end);
  QVERIFY(!CameraPose::matrix(view->cam.object_rot).isApprox(CameraPose::matrix(original.rotation),1e-8));
  QCOMPARE(cube->pos(),position);
  QCOMPARE(selected.count(),0);
  nav->settings.freeOrbit=false;
  const auto disabled=nav->current();
  QTest::mousePress(cube,Qt::LeftButton,Qt::NoModifier,center);
  QTest::mouseMove(cube,end,20);
  QTest::mouseRelease(cube,Qt::LeftButton,Qt::NoModifier,end);
  QVERIFY(nav->current()==disabled);
  nav->settings.freeOrbit=true;
  QTest::mousePress(cube,Qt::LeftButton,Qt::NoModifier,center);
  QTest::mouseMove(cube,center+QPoint(12,12),20);
  const auto beforeOutside=nav->current();
  QTest::mouseRelease(cube,Qt::LeftButton,Qt::NoModifier,QPoint(-20,-20));
  QVERIFY(nav->current()==beforeOutside);
  QCOMPARE(selected.count(),0);
  nav->apply(original,false);
  const auto output=qEnvironmentVariable("OPENSCAD_NAVIGATION_TEST_OUTPUT");
  gridPainter.end();
  if (!output.isEmpty()) {
    QVERIFY(window->grab().save(output+"/navigation-cube.png"));
    QVERIFY(targetGrid.save(output+"/all-26-targets.png"));
  }
}

void TestNavigation::interruptibleTransitions()
{
  auto view=window->qglview;
  auto nav=view->navigation();
  nav->settings.animate=true;
  nav->settings.duration=100;
  const auto original=nav->current();
  auto target=original;
  target.rotation={70,20,125};
  target.translation={5,7,11};
  target.distance=215;
  target.fov=40;
  target.projection=Camera::ProjectionType::ORTHOGONAL;
  QSignalSpy operations(view,&QGLView::cameraChanged);
  nav->navigate(target);
  QCOMPARE(nav->current().projection,target.projection);
  QTRY_VERIFY(!nav->animating());
  QVERIFY(nav->current()==target);
  QCOMPARE(operations.count(),1);
  nav->navigate(original);
  QTest::qWait(30);
  auto redirected=nav->current();
  redirected.rotation={0,0,0};
  nav->navigate(redirected);
  QTRY_VERIFY(!nav->animating());
  QVERIFY(nav->current()==redirected);
  QCOMPARE(operations.count(),2);
  nav->navigate(target);
  QTest::qWait(25);
  nav->cancel();
  const auto stopped=nav->current();
  QTest::qWait(150);
  QVERIFY(nav->current()==stopped);
  QCOMPARE(operations.count(),2);
  nav->navigate(target);
  view->cam.object_rot={12,34,56};
  QTest::qWait(150);
  QVERIFY(view->cam.object_rot==Eigen::Vector3d(12,34,56));
  QVERIFY(!nav->animating());
  nav->settings.animate=false;
  nav->apply(original,false);
}

void TestNavigation::popupAndTuning()
{
  auto view=window->qglview;
  auto settings=&view->navigation()->settings;
  auto popup=view->findChild<ViewportPopup *>();
  QVERIFY(popup);
  popup->toggle();
  QTRY_VERIFY(popup->isVisible());
  QVERIFY(popup->windowFlags().testFlag(Qt::FramelessWindowHint));
  popup->resize(330,540);
  QCOMPARE(popup->size(),QSize(330,540));
  auto ortho=popup->findChild<QRadioButton *>("viewportOrthographic");
  QVERIFY(ortho);
  QTest::mouseClick(ortho,Qt::LeftButton);
  QVERIFY(view->orthoMode());
  QVERIFY(window->viewActionOrthogonal->isChecked());
  const auto output=qEnvironmentVariable("OPENSCAD_NAVIGATION_TEST_OUTPUT");
  if (!output.isEmpty()) QVERIFY(popup->grab().save(output+"/viewport-popup.png"));
  QSignalSpy selected(view,&QGLView::doLeftClick);
  QTest::mouseClick(window->windowHandle(),Qt::LeftButton,Qt::NoModifier,view->mapTo(window,QPoint(4,4)));
  QTRY_VERIFY(!popup->isVisible());
  QCOMPARE(selected.count(),0);
  popup->toggle();
  QTest::keyClick(popup,Qt::Key_Escape);
  QVERIFY(!popup->isVisible());
  settings->setTemporary("scale",1.2);
  settings->setTemporary("cubeRight",25.0);
  settings->setTemporary("menuRight",17.0);
  QCOMPARE(view->navigationCube()->width(),168);
  QCOMPARE(view->navigationCube()->menuTriangle()->x(),view->width()-qRound(24*1.2)-17);
  const auto originalDuration=NavigationSettings::persistent("duration",200).toInt();
  settings->setTemporary("duration",480);
  NavigationSettings::setPersistent("duration",320);
  QCOMPARE(settings->duration,320);
  NavigationSettings fresh;
  QCOMPARE(fresh.duration,320);
  QCOMPARE(fresh.scale,0.75);
#ifdef ENABLE_NAVIGATION_CUBE_TUNING
  auto tuning=window->findChild<NavigationTuning *>();
  QVERIFY(tuning);
  tuning->show();
  QTest::qWait(20);
  auto copy=tuning->findChild<QPushButton *>("copyNavigationSettings");
  QVERIFY(copy);
  QTest::mouseClick(copy,Qt::LeftButton);
  const auto text=QApplication::clipboard()->text();
  QVERIFY(text.contains("scale = 1.2"));
  QVERIFY(text.contains("cube resolved ="));
  QVERIFY(text.contains("triangle resolved ="));
  QVERIFY(text.contains("DPR ="));
  QVERIFY(text.contains("freeOrbit ="));
  if (!output.isEmpty()) {
    QVERIFY(tuning->grab().save(output+"/tuning-window.png"));
    QFile settingsFile(output+"/copied-settings.txt");
    QVERIFY(settingsFile.open(QIODevice::WriteOnly)); settingsFile.write(text.toUtf8());
  }
  tuning->hide();
#endif
  settings->resetOverrides();
  QCOMPARE(settings->scale,0.75);
  QCOMPARE(settings->duration,320);
  NavigationSettings::setPersistent("visible",false);
  QVERIFY(!view->navigationCube()->isVisible());
  QVERIFY(!view->navigationCube()->menuTriangle()->isVisible());
  NavigationSettings::setPersistent("visible",true);
  NavigationSettings::setPersistent("duration",originalDuration);
}
