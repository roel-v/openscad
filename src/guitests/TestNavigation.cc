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
#include "gui/Animate.h"
#include "gui/SavedViews.h"
#include <QLockFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLineEdit>
#include <QDateTime>
#include <QAbstractButton>
#include <QMessageBox>
#include <QTimer>

void TestNavigation::settingsOnStartup()
{
  if (qEnvironmentVariable("OPENSCAD_NAVIGATION_RESTART_STAGE")!="read") return;
  const auto& settings=window->qglview->navigation()->settings;
  QCOMPARE(settings.duration,345);
  QVERIFY(settings.showInformation);
  QCOMPARE(settings.cubeRight,10);
  QCOMPARE(settings.scale,1.1);
}

void TestNavigation::settingsBeforeExit()
{
  if (qEnvironmentVariable("OPENSCAD_NAVIGATION_RESTART_STAGE")!="write") return;
  auto& settings=window->qglview->navigation()->settings;
  NavigationSettings::setPersistent("duration",345);
  NavigationSettings::setPersistent("showInformation",true);
  settings.setTemporary("cubeRight",33);
  settings.setTemporary("scale",1.2);
}

void TestNavigation::savedViewTransactions()
{
  QTemporaryDir directory;
  const auto path=directory.filePath("store.scad");
  SavedViews store;
  QVERIFY(store.load(path));
  auto pose=window->qglview->navigation()->current();
  pose.rotation={70,22,93}; pose.translation={3,5,7}; pose.distance=123.5; pose.fov=51;
  pose.projection=Camera::ProjectionType::ORTHOGONAL;
  const auto first=store.add(pose);
  store.rename(first,QString::fromUtf8("視点 café"));
  pose.projection=Camera::ProjectionType::PERSPECTIVE;
  const auto second=store.add(pose);
  store.rename(second,QString::fromUtf8("視点 café"));
  QVERIFY(!QFile::exists(SavedViews::companion(path)));
  QString error; int sourceWrites=0;
  auto write=[&]{++sourceWrites;return true;};
  QVERIFY(store.save(path,false,write,error));
  QVERIFY(!store.dirty());
  SavedViews reopened;
  QVERIFY(reopened.load(path));
  QCOMPARE(reopened.entries().size(),2);
  QVERIFY(reopened.find(second)->camera==pose);
  QCOMPARE(reopened.find(first)->name,reopened.find(second)->name);
  QVERIFY(reopened.find(first)->camera.projection==Camera::ProjectionType::ORTHOGONAL);
  store.rename(first,"Changed");
  QLockFile lock(SavedViews::companion(path)+".lock"); QVERIFY(lock.tryLock());
  QVERIFY(!store.save(path,false,write,error)); QCOMPARE(sourceWrites,1); QVERIFY(store.dirty());
  lock.unlock();
  reopened.rename(second,"Other window"); QVERIFY(reopened.save(path,false,write,error));
  QVERIFY(!store.save(path,false,write,error)); QCOMPARE(sourceWrites,2); QVERIFY(store.dirty());
  const auto alternate=directory.filePath("alternate.scad");
  QVERIFY(store.save(alternate,true,write,error)); QVERIFY(store.dirty());
  QVERIFY(!store.save(alternate,false,write,error)); // destination collision
  const auto moved=directory.filePath("moved.scad");
  QVERIFY(store.save(moved,false,write,error)); QVERIFY(!store.dirty());
  store.remove(first); store.remove(second);
  QVERIFY(store.save(moved,false,write,error));
  QVERIFY(reopened.load(moved)); QVERIFY(reopened.entries().isEmpty());
  const auto backup=directory.filePath("backup.scad");
  store.add(pose); QVERIFY(store.backup(backup,error));
  QVERIFY(reopened.load(backup)); QCOMPARE(reopened.entries().size(),1);
  auto invalid=QJsonDocument::fromJson(store.serialize()).object(); invalid["version"]=99;
  QFile damaged(SavedViews::companion(moved)); QVERIFY(damaged.open(QIODevice::WriteOnly));
  damaged.write(QJsonDocument(invalid).toJson()); damaged.close();
  QVERIFY(!reopened.load(moved)); QVERIFY(!reopened.problem().isEmpty()); QVERIFY(reopened.entries().isEmpty());
  QVERIFY(reopened.add(pose).isEmpty());
  QVERIFY(damaged.open(QIODevice::WriteOnly|QIODevice::Truncate)); damaged.write("{ broken"); damaged.close();
  QVERIFY(!reopened.load(moved));
  // Failure after the source commit retains metadata and can be retried.
  const auto partial=directory.filePath("partial.scad");
  QVERIFY(!store.save(partial,false,[&] {
    return QDir().mkdir(SavedViews::companion(partial));
  },error));
  QVERIFY(store.dirty());
  QVERIFY(QDir().rmdir(SavedViews::companion(partial)));
  QVERIFY(store.save(partial,false,write,error));
  QVERIFY(!store.dirty());
  const auto validRoot=QJsonDocument::fromJson(store.serialize()).object();
  const auto validEntry=validRoot.value("views").toArray().first().toObject();
  QList<QByteArray> invalidFiles{QByteArray(4*1024*1024+1,' ')};
  for (int scenario=0;scenario<5;++scenario) {
    auto entry=validEntry;
    if (scenario==0) entry["distance"]=-1;
    if (scenario==1) entry["rotation"]=QJsonArray{0,QJsonValue(),0};
    if (scenario==2) entry["name"]=QString(257,'x');
    QJsonArray entries{entry};
    if (scenario==3) entries.append(entry);
    if (scenario==4) { for (int i=0;i<1001;++i) entries.append(entry); }
    auto root=validRoot;root["views"]=entries;invalidFiles.append(QJsonDocument(root).toJson());
  }
  for (const auto& bytes:invalidFiles) {
    QVERIFY(damaged.open(QIODevice::WriteOnly|QIODevice::Truncate));
    QCOMPARE(damaged.write(bytes),qint64(bytes.size()));damaged.close();
    QVERIFY(!reopened.load(moved));
    QVERIFY(damaged.open(QIODevice::ReadOnly));QCOMPARE(damaged.readAll(),bytes);damaged.close();
  }
}

void TestNavigation::savedViewDocumentLifecycle()
{
  // Earlier annotation cases intentionally leave dirty metadata in temporary documents.
  // Close those test-owned tabs before this case exercises Save All on its own document.
  QTimer discard, deadline;
  bool unexpectedDialog = false, timedOut = false;
  discard.setInterval(20);
  deadline.setSingleShot(true); deadline.setInterval(4000);
  connect(&discard, &QTimer::timeout, window, [&] {
    auto widget = QApplication::activeModalWidget();
    if (!widget) return;
    auto box = qobject_cast<QMessageBox *>(widget);
    if (box && box->standardButtons().testFlag(QMessageBox::Discard)) {
      box->button(QMessageBox::Discard)->click();
    } else {
      unexpectedDialog = true;
      if (auto dialog = qobject_cast<QDialog *>(widget)) dialog->reject();
    }
  });
  connect(&deadline, &QTimer::timeout, window, [&] {
    timedOut = true;
    if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
  });
  discard.start(); deadline.start();
  restoreWindowInitialState();
  discard.stop(); deadline.stop();
  QVERIFY(!unexpectedDialog && !timedOut);
  QCOMPARE(window->tabManager->count(), size_t(1));
  QTemporaryDir directory;
  window->tabManager->createTab("");
  auto editor=window->activeEditor;
  editor->setPlainText("cube(12);\n");
  auto store=SavedViews::forEditor(editor);
  const auto pose=window->qglview->navigation()->current();
  const auto id=store->add(pose);
  QVERIFY(editor->hasViewChanges());
  const auto path=directory.filePath("document.scad");
  QVERIFY(window->tabManager->saveAs(editor,path));
  QVERIFY(!editor->hasViewChanges());
  const auto timestamp=QFileInfo(path).lastModified();
  QTest::qWait(20);
  store->rename(id,"Metadata only");
  QVERIFY(window->tabManager->save(editor));
  QCOMPARE(QFileInfo(path).lastModified(),timestamp);
  store->rename(id,"Unsaved copy");
  QVERIFY(window->tabManager->saveACopy(editor,directory.filePath("copy.scad")));
  QCOMPARE(editor->filepath,path); QVERIFY(editor->hasViewChanges());
  QVERIFY(window->tabManager->refreshDocument()); QVERIFY(editor->hasViewChanges());
  QCOMPARE(store->find(id)->name,QString("Unsaved copy"));
  QVERIFY(window->tabManager->saveAll()); QVERIFY(!editor->hasViewChanges());
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  QTRY_COMPARE(window->displayedDocument(),editor);
  auto popup=window->findChild<ViewportPopup *>(); popup->toggle();
  QTRY_VERIFY(popup->addView->isEnabled());
  QTest::mouseClick(popup->addView,Qt::LeftButton);
  QTRY_COMPARE(store->entries().size(),2);
  auto edit=popup->views->findChild<QLineEdit *>(); QVERIFY(edit);
  edit->setText("Named camera"); QTest::keyClick(edit,Qt::Key_Return);
  QTRY_COMPARE(store->entries().last().name,QString("Named camera"));
  QTest::qWait(20);
  auto changed=pose; changed.rotation.z()+=39;
  window->qglview->navigation()->apply(changed,false);
  window->qglview->navigation()->settings.animate=false;
  popup->views->setFocus();
  QTest::keyClick(popup->views,Qt::Key_Return);
  QTRY_VERIFY(window->qglview->navigation()->current()==pose);
  QVERIFY(popup->isVisible());
  const auto output=qEnvironmentVariable("OPENSCAD_NAVIGATION_TEST_OUTPUT");
  if (!output.isEmpty()) QVERIFY(popup->grab().save(output+"/saved-views.png"));
  popup->hide(); QVERIFY(editor->hasViewChanges());
  QVERIFY(window->tabManager->save(editor));
  window->tabManager->closeCurrentTab();
}

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
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
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
  // Exercise actual pointer clicks: direct snap() calls do not cover click/drag arbitration.
  for (const int hold : {1,250}) {
    nav->apply(original,false);
    nav->settings.animate=true;
    nav->settings.duration=200;
    window->activeEditor->setFocus();
    QTest::mouseMove(window->windowHandle(),QPoint(20,100));
    QTest::qWait(450);
    const auto visible=cube->projected();
    const auto front=std::find_if(visible.begin(),visible.end(),[](const auto& face){return face.label=="FRONT";});
    QVERIFY(front!=visible.end());
    const auto click=cube->mapTo(window,front->center.toPoint());
    QTest::mouseMove(window->windowHandle(),click,1);
    QTest::mousePress(window->windowHandle(),Qt::LeftButton,Qt::NoModifier,click,1);
    QTest::qWait(hold);
    QTest::mouseRelease(window->windowHandle(),Qt::LeftButton,Qt::NoModifier,click,1);
    QTest::qWait(350);
    QVERIFY2(CameraPose::matrix(nav->current().rotation).isApprox(Eigen::Matrix3d::Identity(),1e-10),
             qPrintable(QString("Window-routed Front click held %1 ms stopped at %2,%3,%4")
               .arg(hold).arg(nav->current().rotation.x()).arg(nav->current().rotation.y()).arg(nav->current().rotation.z())));
  }
  for (const QPoint jitter : {QPoint(0,0),QPoint(3,3)}) {
    nav->apply(original,false);
    nav->settings.animate=true;
    nav->settings.duration=100;
    const auto visible=cube->projected();
    const auto front=std::find_if(visible.begin(),visible.end(),[](const auto& face){return face.label=="FRONT";});
    QVERIFY(front!=visible.end());
    const QPoint click=front->center.toPoint();
    QTest::mousePress(cube,Qt::LeftButton,Qt::NoModifier,click);
    QTest::mouseMove(cube,click+jitter,20);
    QTest::mouseRelease(cube,Qt::LeftButton,Qt::NoModifier,click+jitter);
    QTest::qWait(180);
    QVERIFY2(CameraPose::matrix(nav->current().rotation).isApprox(Eigen::Matrix3d::Identity(),1e-10),
             qPrintable(QString("Front-face click (%1,%2 jitter) stopped at %3,%4,%5")
               .arg(jitter.x()).arg(jitter.y()).arg(nav->current().rotation.x())
               .arg(nav->current().rotation.y()).arg(nav->current().rotation.z())));
    QVERIFY(!nav->animating());
  }
  nav->settings.animate=false;
  nav->apply(original,false);
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
  // Unchanged automatic-reload polls must not interrupt an in-flight camera operation.
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  window->actionReloadRenderPreview();
  nav->settings.duration=350;
  nav->navigate(target);
  QTest::qWait(30);
  window->actionReloadRenderPreview();
  QVERIFY2(nav->animating(),"An unchanged file poll cancelled the camera transition");
  window->designActionAutoReload->setChecked(true);
  QTest::qWait(450);
  window->designActionAutoReload->setChecked(false);
  QVERIFY2(nav->current()==target,"Periodic automatic reload prevented the exact camera endpoint");
  nav->navigate(original);
  QTest::qWait(30);
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  QVERIFY2(!nav->animating(),"An explicit preview must still cancel pending camera movement");
  nav->settings.animate=false;
  nav->apply(original,false);
}

void TestNavigation::popupAndTuning()
{
  auto view=window->qglview;
  auto settings=&view->navigation()->settings;
  auto popup=view->findChild<ViewportPopup *>();
  QVERIFY(popup);
  auto triangle=view->navigationCube()->menuTriangle();
  for (int cycle=0; cycle<3; ++cycle) {
    QTest::mouseClick(window->windowHandle(),Qt::LeftButton,Qt::NoModifier,
                      triangle->mapTo(window,triangle->rect().center()));
    QTRY_VERIFY(popup->isVisible());
    QTest::mouseClick(window->windowHandle(),Qt::LeftButton,Qt::NoModifier,
                      triangle->mapTo(window,triangle->rect().center()));
    QTRY_VERIFY(!popup->isVisible());
    // A replay is posted; checking only the synchronous hide misses reopening.
    QTest::qWait(50);
    QVERIFY(!popup->isVisible());
  }
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
  auto cube=view->navigationCube();
  const auto arrows=cube->findChildren<QToolButton *>(QString(),Qt::FindDirectChildrenOnly);
  QCOMPARE(arrows.size(),6);
  const auto cubePosition=cube->pos();
  const auto rollPosition=arrows[0]->pos();
  const auto arrowPosition=arrows[2]->pos();
  settings->setTemporary("arrowInset",10.0);
  QCOMPARE(arrows[0]->pos(),rollPosition);
  QVERIFY(arrows[2]->y()>arrowPosition.y());
  const auto insetArrowPosition=arrows[2]->pos();
  settings->setTemporary("rollInset",10.0);
  QVERIFY(arrows[0]->y()>rollPosition.y());
  QCOMPARE(arrows[2]->pos(),insetArrowPosition);
  QCOMPARE(cube->pos(),cubePosition);
  QVERIFY(settings->description().contains("arrowInset = 10 logical px"));
  QVERIFY(settings->description().contains("rollInset = 10 logical px"));
  if (!output.isEmpty()) QVERIFY(cube->grab().save(output+"/arrow-insets.png"));
  settings->setTemporary("arrowInset",0.0);
  settings->setTemporary("rollInset",0.0);
  QImage scaleSheet(880,2320,QImage::Format_ARGB32_Premultiplied);
  scaleSheet.fill(window->palette().color(QPalette::Window));
  QPainter scalePainter(&scaleSheet);
  int row=0;
  for (double scale : {0.35,0.75,1.5,3.0}) {
    settings->setTemporary("scale",scale);
    int smallerButton=0,column=0;
    for (double size : {12.0,30.0}) {
      settings->setTemporary("arrowSize",size);
      for (auto arrow : arrows) {
        QVERIFY2(cube->rect().contains(arrow->geometry()),"Scaled arrows must not extend outside the navigation widget");
      }
      if (size==12) smallerButton=arrows.first()->width();
      else QVERIFY(arrows.first()->width()>smallerButton);
      scalePainter.drawPixmap(column*440,row*580,cube->grab());
      ++column;
    }
    ++row;
  }
  scalePainter.end();
  if (!output.isEmpty()) QVERIFY(scaleSheet.save(output+"/arrow-scales.png"));
  settings->setTemporary("arrowSize",20.0);
  settings->setTemporary("scale",1.2);
  settings->setTemporary("cubeRight",25.0);
  settings->setTemporary("menuRight",17.0);
  QCOMPARE(view->navigationCube()->width(),168);
  QCOMPARE(view->navigationCube()->menuTriangle()->x(),view->width()-qRound(24*1.2)-17);
  settings->setTemporary("idleOpacity",1.0);
  settings->setTemporary("activeOpacity",1.0);
  for (auto arrow : arrows) {
    arrow->clearFocus();
    // Render both hover states deterministically without competing with the user's desktop pointer.
    arrow->setAttribute(Qt::WA_UnderMouse,false);
    const auto normal=arrow->grab().toImage();
    arrow->setAttribute(Qt::WA_UnderMouse,true);
    const auto hover=arrow->grab().toImage();
    QVERIFY2(hover!=normal,"Hover must visibly highlight the arrow glyph");
    for (const auto point : {QPoint(0,0),QPoint(normal.width()-1,0),
                            QPoint(0,normal.height()-1),QPoint(normal.width()-1,normal.height()-1)})
      QCOMPARE(hover.pixelColor(point),normal.pixelColor(point));
    arrow->setAttribute(Qt::WA_UnderMouse,false);
  }
  arrows.last()->setAttribute(Qt::WA_UnderMouse,true);
  if (!output.isEmpty()) QVERIFY(window->grab().save(output+"/arrow-hover.png"));
  arrows.last()->setAttribute(Qt::WA_UnderMouse,false);
  const auto originalDuration=NavigationSettings::persistent("duration",200).toInt();
  settings->setTemporary("duration",480);
  NavigationSettings::setPersistent("duration",320);
  QCOMPARE(settings->duration,320);
  NavigationSettings fresh;
  QCOMPARE(fresh.duration,320);
  QCOMPARE(fresh.scale,1.1);
#ifdef ENABLE_NAVIGATION_CUBE_TUNING
  auto tuning=window->findChild<NavigationTuning *>();
  QVERIFY(tuning);
  QVERIFY(window->menuWindow->actions().contains(tuning->toggleViewAction()));
  QVERIFY(!window->menu_View->actions().contains(tuning->toggleViewAction()));
  tuning->toggleViewAction()->trigger();
  QTRY_VERIFY(tuning->isVisible());
  const auto tuningValues=settings->values();
  for (auto it=tuningValues.cbegin();it!=tuningValues.cend();++it) {
    auto help=tuning->findChild<QToolButton *>("help_"+it.key());
    QVERIFY2(help,qPrintable("Missing help icon for "+it.key()));
    QVERIFY(!help->icon().isNull());
    QVERIFY2(!help->toolTip().isEmpty(),qPrintable("Missing help text for "+it.key()));
    auto control=tuning->findChild<QWidget *>("tune_"+it.key());
    QVERIFY(control);
    QCOMPARE(control->whatsThis(),help->toolTip());
    QCOMPARE(control->accessibleDescription(),help->toolTip());
  }
  tuning->setFloating(false);
  QCOMPARE(window->dockWidgetArea(tuning),Qt::RightDockWidgetArea);
  QVERIFY(!tuning->isFloating());
  tuning->setFloating(true);
  QVERIFY(tuning->isFloating());
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
  tuning->close();
  QVERIFY(!tuning->toggleViewAction()->isChecked());
  tuning->toggleViewAction()->trigger();
  QTRY_VERIFY(tuning->isVisible());
  tuning->hide();
#endif
  settings->resetOverrides();
  QCOMPARE(settings->scale,1.1);
  QCOMPARE(settings->duration,320);
  NavigationSettings::setPersistent("visible",false);
  QVERIFY(!view->navigationCube()->isVisible());
  QVERIFY(!view->navigationCube()->menuTriangle()->isVisible());
  NavigationSettings::setPersistent("visible",true);
  NavigationSettings::setPersistent("duration",originalDuration);
}
#include "gui/ViewportStatistics.h"
#include "gui/ai/ViewportGrabber.h"
#include <stdexcept>
#include <QMessageBox>
#include <QTimer>

void TestNavigation::visualCalibrationAndClose()
{
  window->tabManager->createTab("");
  auto editor=window->activeEditor;
  editor->setPlainText("difference(){cube([24,18,12],center=true);cylinder(h=20,r=4,center=true,$fn=32);}\n");
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  auto view=window->qglview;
  view->navigation()->settings.animate=false;
  view->viewAll();
  auto popup=window->findChild<ViewportPopup *>();
  if (!popup->isVisible()) popup->toggle();
  QTest::mouseClick(popup->addView,Qt::LeftButton);
  auto store=SavedViews::forEditor(editor);
  QTRY_COMPARE(store->entries().size(),1);
  auto currentEditor=[&]() -> QLineEdit * {
    for (auto candidate:popup->views->findChildren<QLineEdit *>()) if (candidate->isVisible()) return candidate;
    return nullptr;
  };
  QLineEdit *name=nullptr;QTRY_VERIFY(name=currentEditor());
  name->clear();QTest::keyClick(name,Qt::Key_Return);
  QTRY_COMPARE(popup->views->item(0)->text(),QString("View 1"));
  QTest::mouseClick(popup->addView,Qt::LeftButton);
  QTRY_COMPARE(store->entries().size(),2);
  QTRY_VERIFY(name=currentEditor());
  name->setText("Focus-loss name");
  QTest::mouseClick(popup->findChild<QRadioButton *>("viewportPerspective"),Qt::LeftButton);
  QTRY_VERIFY(popup->findChild<QRadioButton *>("viewportPerspective")->isChecked());
  QVERIFY(!view->orthoMode());
  QTRY_COMPARE(store->entries().last().name,QString("Focus-loss name"));
  const auto output=qEnvironmentVariable("OPENSCAD_NAVIGATION_TEST_OUTPUT");
  const auto oldSize=window->size();
  const auto oldFont=popup->font();
  const auto oldStyle=popup->styleSheet();
  popup->setStyleSheet("QWidget { font-size: 14pt; }");
  auto largeFont=oldFont;largeFont.setPointSize(14);popup->setFont(largeFont);
  popup->resize(410,690);
  QTest::qWait(50);
  if (!output.isEmpty()) QVERIFY(popup->grab().save(output+"/popup-large-font.png"));
  popup->hide();popup->setFont(oldFont);popup->setStyleSheet(oldStyle);
  window->resize(820,620);
  popup->toggle();QTest::qWait(50);
  if (!output.isEmpty()) QVERIFY(window->grab().save(output+"/small-window.png"));
  popup->hide();window->resize(oldSize);
  editor->setContentModified(false);
  const auto count=window->tabManager->count();
  QTimer::singleShot(0,window,[] {
    if (auto box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->done(QMessageBox::Cancel);
  });
  window->tabManager->closeCurrentTab();
  QCOMPARE(window->tabManager->count(),count);QVERIFY(editor->hasViewChanges());
  QTimer::singleShot(0,window,[] {
    if (auto box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->done(QMessageBox::Discard);
  });
  window->tabManager->closeCurrentTab();
  QCOMPARE(window->tabManager->count(),count-1);
}

void TestNavigation::statisticsAndCleanCapture()
{
  window->tabManager->createTab("");
  auto editor=window->activeEditor;
  editor->setPlainText("cube(10); translate([15,0,0]) cube(10);\n");
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  auto view=window->qglview;
  QTRY_VERIFY(view->getRenderer());
  const auto statistics=view->getRenderer()->triangleStatistics();
  QVERIFY(statistics.count.has_value()); QCOMPARE(*statistics.count,uint64_t(24));
  auto nav=view->navigation(); nav->settings.animate=false;
  nav->settings.setTemporary("showInformation",true);
  nav->settings.setTemporary("frameTime",true);
  nav->settings.setTemporary("statsIdle",150);
  nav->settings.setTemporary("statsRefresh",50);
  QSignalSpy paints(view,&QGLView::viewportPaintCompleted);
  for (int i=0;i<6;++i) {view->update();QTest::qWait(25);}
  QVERIFY(view->statistics()->sampleCount()>0);
  QTest::qWait(400);
  QVERIFY(view->statistics()->text().contains("Idle"));
  const int idlePaints=paints.count(); QTest::qWait(300);
  QVERIFY2(paints.count()-idlePaints<=1,"Statistics must not drive an idle redraw loop");
  nav->settings.setTemporary("visible",false);
  QVERIFY(view->statistics()->isVisible());
  nav->settings.setTemporary("visible",true);
  view->setShowAxes(false); view->setShowScaleProportional(false); view->setShowCrosshairs(false);
  view->setShowEdges(true);
  const QImage reference=view->grabFrame(QGLView::CaptureContent::ModelOnly);
  QVERIFY(!reference.isNull());
  view->setShowAxes(true); view->setShowScaleProportional(true); view->setShowCrosshairs(true);
  auto popup=window->findChild<ViewportPopup *>(); if (!popup->isVisible()) popup->toggle();
  const auto pose=nav->current();
  const int samples=view->statistics()->sampleCount();
  const QImage clean=view->grabFrame();
  QCOMPARE(view->statistics()->sampleCount(),samples);
  QCOMPARE(clean,reference);
  QVERIFY(view->showAxes() && view->showScaleProportional() && view->showCrosshairs() && view->showEdges());
  QVERIFY(nav->current()==pose);
  window->editActionCopyViewport->trigger();
  QCOMPARE(QApplication::clipboard()->image(),reference);
  const auto encoded=ViewportGrabber::captureViewportBase64(view,10000);
  auto decoded=QImage::fromData(QByteArray::fromBase64(QByteArray::fromStdString(encoded)),"PNG");
  decoded.setDevicePixelRatio(reference.devicePixelRatio());
  QCOMPARE(decoded.convertToFormat(reference.format()),reference);
  // Exercise the real animation-dump caller, keeping its timer beyond this synchronous frame.
  QTemporaryDir animationOutput;
  QVERIFY(animationOutput.isValid());
  editor->filepath=animationOutput.filePath("animation.scad");
  auto animation=window->findChild<Animate *>();
  QVERIFY(animation);
  animation->e_dump->setChecked(true);
  animation->animateTimer->start(60000);
  window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  animation->animateTimer->stop();
  animation->e_dump->setChecked(false);
  const auto frames=QDir(animationOutput.path()).entryList({"frame*.png"},QDir::Files);
  QCOMPARE(frames.size(),1);
  QImage frame(animationOutput.filePath(frames.first()));
  frame.setDevicePixelRatio(reference.devicePixelRatio());
  QCOMPARE(frame.convertToFormat(reference.format()),reference);
  editor->filepath.clear();
  int compositions=0;
  view->composeAnnotations=[&](QImage& image){++compositions;image.setPixelColor(2,2,Qt::red);};
  const QImage annotated=view->grabFrame();
  QCOMPARE(compositions,1); QCOMPARE(annotated.pixelColor(2,2),QColor(Qt::red));
  QCOMPARE(view->grabFrame(QGLView::CaptureContent::ModelOnly),reference); QCOMPARE(compositions,1);
  view->composeAnnotations=[](QImage&){throw std::runtime_error("Injected composition failure");};
  bool caught=false;
  try {view->grabFrame();} catch(const std::runtime_error&) {caught=true;}
  QVERIFY(caught); QVERIFY(view->showAxes() && view->showCrosshairs() && view->showScaleProportional());
  view->composeAnnotations={};
  nav->settings.animate=true; nav->settings.duration=500;
  auto target=pose;target.rotation.z()+=75;nav->navigate(target);QTest::qWait(30);
  view->grabFrame();const auto stopped=nav->current();QTest::qWait(80);
  QVERIFY(!nav->animating());QVERIFY(nav->current()==stopped);
  const auto output=qEnvironmentVariable("OPENSCAD_NAVIGATION_TEST_OUTPUT");
  if (!output.isEmpty()) {
    QVERIFY(clean.save(output+"/clean-viewport.png"));
    QVERIFY(window->grab().save(output+"/statistics-ui.png"));
    view->grabFrame(); QVERIFY(view->save((output+"/export-path.png").toStdString().c_str()));
  }
  popup->hide();
  nav->settings.resetOverrides();
  editor->setContentModified(false);
  window->tabManager->closeCurrentTab();
}
#include "glview/RenderSettings.h"

void TestNavigation::resultStatisticsStates()
{
  window->tabManager->createTab("");
  auto editor=window->activeEditor;
  auto view=window->qglview;
  view->navigation()->settings.setTemporary("showInformation",true);
  editor->setPlainText("square(10);\n");window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  QTRY_VERIFY(view->getRenderer());
  QCOMPARE(view->getRenderer()->triangleStatistics().dimension,2);
  view->statistics()->refresh();QVERIFY(view->statistics()->text().contains("2D"));
  editor->setPlainText("cube(10);\n");window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  QTRY_COMPARE(window->displayedDocument(),editor);
  QCOMPARE(*view->getRenderer()->triangleStatistics().count,uint64_t(12));
  const auto oldBackend=RenderSettings::inst()->backend3D;
  RenderSettings::inst()->backend3D=RenderBackend3D::ManifoldBackend;
  window->designActionRender->trigger();
  QTRY_VERIFY_WITH_TIMEOUT(view->getRenderer() && view->getRenderer()->triangleStatistics().kind==Renderer::TriangleStatistics::Kind::Final,10000);
  QCOMPARE(*view->getRenderer()->triangleStatistics().count,uint64_t(12));
  RenderSettings::inst()->backend3D=oldBackend;
  editor->setPlainText("invalid syntax ?\n");window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  QTRY_VERIFY(view->statistics()->text().contains("failed"));
  QCOMPARE(window->displayedDocument(),editor);
  editor->setPlainText("");window->actionRenderPreview(); QVERIFY(waitForEvaluation());
  QTRY_COMPARE(window->displayedDocument(),editor);
  const auto empty=view->getRenderer();
  QVERIFY(empty);QVERIFY(empty->triangleStatistics().count.has_value());
  QCOMPARE(*empty->triangleStatistics().count,uint64_t(0));
  view->navigation()->settings.resetOverrides();
  editor->setContentModified(false);window->tabManager->closeCurrentTab();
}
