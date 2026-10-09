#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStyleHints>
#include <QToolBar>
#include <QToolButton>
#include <QMenu>
#include <QElapsedTimer>
#include <QThread>
#include "gui/Preferences.h"
#include "gui/SettingsWriter.h"
#include "gui/MainWindow.h"
#include "gui/Animate.h"
#include "gui/Editor.h"
#include "core/Builtins.h"
#include "core/parsersettings.h"
#include "gui/Icons.h"
#include "gui/MessageBox.h"
#include "gui/OpenSCADApp.h"
#include "core/Settings.h"

void initializeGuiTestApplication();

namespace {
QImage labelImage(const QLabel& label)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return label.pixmap().toImage();
#else
  const auto *pixmap = label.pixmap();
  return pixmap ? pixmap->toImage() : QImage();
#endif
}
void iconApp()
{
  initializeGuiTestApplication();
  Icons::install();
}
struct RestoreAppearance {
  Settings::StatusIconAppearance saved = Settings::Settings::statusIconAppearance.value();
  ~RestoreAppearance() { Settings::Settings::statusIconAppearance.setValue(saved); }
};
}  // namespace

TEST_CASE("Icon mappings render all assets at logical sizes", "[ui-icons]")
{
  iconApp();
  RestoreAppearance restore;
  Settings::Settings::statusIconAppearance.set("monochrome");
  REQUIRE(Icons::names().size() >= 83);
  for (const auto& name : Icons::names()) {
    INFO(name.toStdString());
    const auto icon = Icons::icon(name);
    for (const auto dpr : {1.0, 1.25, 1.5, 2.0}) {
      const auto pixmap = Icons::pixmap(icon, QSize(24, 24), dpr);
      REQUIRE_FALSE(pixmap.isNull());
      const auto image = pixmap.toImage();
      bool hasInk = false;
      for (int y = 0; y < image.height() && !hasInk; ++y)
        for (int x = 0; x < image.width(); ++x) hasInk = hasInk || qAlpha(image.pixel(x, y)) > 0;
      CHECK(hasInk);
      CHECK(pixmap.width() == qRound(24 * dpr));
      CHECK(pixmap.devicePixelRatio() == dpr);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
      CHECK(icon.pixmap(QSize(24, 24), dpr).toImage() == pixmap.toImage());
#endif
    }
  }
  CHECK(Icons::icon("not-an-icon").isNull());
}

TEST_CASE("Status choices default safely and refresh live pixmaps", "[ui-icons]")
{
  iconApp();
  RestoreAppearance restore;
  auto& setting = Settings::Settings::statusIconAppearance;
  setting.set("colored");
  setting.set("invalid-value");
  CHECK(Icons::appearance() == "native");
  QLabel first, second;
  Icons::setLabelIcon(&first, "information-icons-warning", 24);
  Icons::setLabelIcon(&second, "information-icons-warning", 24);
  const auto native = labelImage(first);
  setting.set("monochrome");
  Icons::refresh();
  CHECK(labelImage(first) != native);
  CHECK(labelImage(first) == labelImage(second));
  CHECK(setting.encode() == "monochrome");
  QPalette palette = first.palette();
  palette.setColor(QPalette::Active, QPalette::WindowText, QColor(160, 20, 180));
  palette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(160, 20, 180));
  first.setPalette(palette);
  CHECK(labelImage(first) != labelImage(second));
  const auto enabled = Icons::icon("save", &first).pixmap(24, 24).toImage();
  CHECK(Icons::icon("save", &first).pixmap(24, 24, QIcon::Disabled).toImage() != enabled);
}

TEST_CASE("Message icon changes preserve modal buttons and results", "[ui-icons]")
{
  iconApp();
  RestoreAppearance restore;
  for (const auto mode : {"monochrome", "native", "colored"}) {
    Settings::Settings::statusIconAppearance.set(mode);
    StatusMessageBox box(QMessageBox::Warning, "Unsaved changes", "Save the document?",
                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);
    box.setEscapeButton(QMessageBox::Cancel);
    QTimer::singleShot(0, &box, [&] {
      CHECK(box.defaultButton() == box.button(QMessageBox::Save));
      CHECK(box.escapeButton() == box.button(QMessageBox::Cancel));
      CHECK_FALSE(box.iconPixmap().isNull());
      const auto before = box.iconPixmap().toImage();
      Settings::Settings::statusIconAppearance.set(QString(mode) == "monochrome" ? "native"
                                                                                 : "monochrome");
      Icons::refresh();
      CHECK(box.iconPixmap().toImage() != before);
      box.button(QMessageBox::Cancel)->click();
    });
    CHECK(box.exec() == QMessageBox::Cancel);
  }
  StatusMessageBox plain;
  Icons::prepareMessageBox(&plain);
  CHECK(plain.icon() == QMessageBox::NoIcon);
}

TEST_CASE("Palette alpha and new action consumers remain theme-aware", "[ui-icons]")
{
  iconApp();
  RestoreAppearance restore;
  Settings::Settings::statusIconAppearance.set("monochrome");
  QLabel label;
  QPalette palette = label.palette();
  palette.setColor(QPalette::WindowText, QColor(100, 40, 180, 80));
  label.setPalette(palette);
  auto image = Icons::pixmap(Icons::icon("circle-checkmark", &label), QSize(24, 24), 1).toImage();
  int maximum = 0;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) maximum = std::max(maximum, qAlpha(image.pixel(x, y)));
  CHECK(maximum > 0);
  CHECK(maximum <= 80);
  QToolBar toolbar;
  toolbar.show();
  QApplication::processEvents();
  palette.setColor(QPalette::ButtonText, QColor(160, 20, 180));
  toolbar.setPalette(palette);
  auto *action = toolbar.addAction(QIcon::fromTheme("openscad-save"), "Save");
  QApplication::processEvents();
  CHECK(action->icon().pixmap(24, 24).toImage() ==
        Icons::icon("save", &toolbar).pixmap(24, 24).toImage());
}

TEST_CASE("Shared measurement action updates its button icon without window activation", "[ui-icons]")
{
  iconApp();
  QToolBar toolbar;
  QMenu menu;
  auto *action = toolbar.addAction(Icons::icon("openscad-measure-distance"), "Measure distance");
  action->setCheckable(true);
  menu.addAction(action);
  toolbar.show();
  QApplication::processEvents();
  auto *button = qobject_cast<QToolButton *>(toolbar.widgetForAction(action));
  REQUIRE(button);
  auto colors = button->palette();
  colors.setColor(QPalette::ButtonText, QColor(170, 20, 100));
  colors.setColor(QPalette::HighlightedText, QColor(30, 170, 100));
  button->setPalette(colors);
  for (const bool checked : {true, false, true}) {
    action->setChecked(checked);
    QApplication::processEvents();
    const auto state = checked ? QIcon::On : QIcon::Off;
    CHECK(button->icon().pixmap(QSize(24, 24), QIcon::Normal, state).toImage() ==
          Icons::icon("openscad-measure-distance", button).pixmap(QSize(24, 24), QIcon::Normal, state).toImage());
  }
}

TEST_CASE("Status preference persists through the existing writer and a fresh process", "[ui-icons]")
{
  iconApp();
  RestoreAppearance restore;
  QTemporaryDir directory;
  REQUIRE(directory.isValid());
  for (const auto mode : {"monochrome", "native", "colored", "invalid", "missing"}) {
    const auto path = directory.path() + "/" + mode;
    QSettings saved(path + "/OpenSCADTests/Typography.ini", QSettings::IniFormat);
    if (QString(mode) != "missing") saved.setValue("advanced/statusIconAppearance", mode);
    saved.sync();
    QProcess child;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("OPENSCAD_TYPOGRAPHY_SETTINGS", path);
    env.insert("OPENSCAD_ICON_EXPECTED",
               QString(mode) == "invalid" || QString(mode) == "missing" ? "native" : mode);
    env.insert("QT_QPA_PLATFORM", "offscreen");
    child.setProcessEnvironment(env);
    child.start(QCoreApplication::applicationFilePath(), {"[icon-restart]"});
    REQUIRE(child.waitForFinished(30000));
    INFO(child.readAllStandardOutput().toStdString());
    INFO(child.readAllStandardError().toStdString());
    CHECK(child.exitStatus() == QProcess::NormalExit);
    CHECK(child.exitCode() == 0);
  }
  auto& setting = Settings::Settings::statusIconAppearance;
  setting.set("colored");
  SettingsWriter writer;
  writer.handle(setting);
  QSettings saved;
  CHECK(saved.value("advanced/statusIconAppearance").toString() == "colored");
  setting.set("native");
  writer.handle(setting);
  CHECK_FALSE(saved.contains("advanced/statusIconAppearance"));
}

TEST_CASE("Load persisted status appearance", "[.][icon-restart]")
{
  REQUIRE_FALSE(qEnvironmentVariableIsEmpty("OPENSCAD_ICON_EXPECTED"));
  iconApp();
  CHECK(Icons::appearance() == qEnvironmentVariable("OPENSCAD_ICON_EXPECTED"));
  CHECK(GlobalPreferences::inst()->statusIconAppearance->currentData().toString() ==
        Icons::appearance());
}

TEST_CASE("Capture integrated native icon modes", "[.][icons-visual]")
{
  iconApp();
  RestoreAppearance restore;
  const auto output = qEnvironmentVariable("OPENSCAD_TYPOGRAPHY_OUTPUT");
  REQUIRE_FALSE(output.isEmpty());
  REQUIRE(QDir().mkpath(output));
  REQUIRE(QApplication::setStyle("windows11"));
  qApp->setFont(QFont("Segoe UI", 10));
  Builtins::initialize();
  parser_init();
  auto *first = new MainWindow(QStringList());
  auto *second = new MainWindow(QStringList());
  first->resize(1200, 800);
  second->resize(800, 600);
  first->show();
  second->show();
  QApplication::processEvents();
  for (auto *window : {first, second}) {
    CHECK(window->viewerToolBar->iconSize() == QSize(24, 24));
    CHECK(window->editortoolbar->iconSize() == QSize(24, 24));
    CHECK(window->fileActionNew->icon().name() == "openscad-new");
  }
  auto *prefs = GlobalPreferences::inst();
  for (auto *action : {first->designActionMeasureDist, first->designActionMeasureAngle}) {
    const bool enabled = action->isEnabled();
    action->setEnabled(true);
    action->setChecked(true);
    QApplication::processEvents();
    auto *button = qobject_cast<QToolButton *>(first->viewerToolBar->widgetForAction(action));
    REQUIRE(button);
    CHECK(button->icon().pixmap(QSize(24, 24), QIcon::Normal, QIcon::On).toImage() ==
          Icons::icon(action->icon().name(), button).pixmap(QSize(24, 24), QIcon::Normal, QIcon::On).toImage());
    REQUIRE(first->viewerToolBar->grab().save(output + "/" + action->objectName() + "-selected.png"));
    action->setChecked(false);
    action->setEnabled(enabled);
  }
  Settings::Settings::showWarningsIn3dView.setValue(true);
  first->activeEditor->setPlainText("cube(10); echo(icon_verification_missing_variable);");
  REQUIRE(QMetaObject::invokeMethod(first, "actionRenderPreview"));
  QElapsedTimer timer;
  timer.start();
  while (!first->frameCompileResult->isVisible() && timer.elapsed() < 10000) {
    QApplication::processEvents();
    QThread::msleep(10);
  }
  REQUIRE(first->compileWarnings > 0);
  REQUIRE(first->frameCompileResult->isVisible());
  const auto warningName = first->labelCompileResultIcon->property("openscadIcon").toString();
  CHECK(warningName == "information-icons-warning");

  for (const auto mode : {"monochrome", "native", "colored"}) {
    const auto index = prefs->statusIconAppearance->findData(mode);
    prefs->statusIconAppearance->setCurrentIndex(index);
    REQUIRE(QMetaObject::invokeMethod(prefs->statusIconAppearance, "activated", Q_ARG(int, index)));
    CHECK(Icons::appearance() == mode);
    QApplication::processEvents();
    first->raise();
    first->activateWindow();
    QApplication::processEvents();
    REQUIRE(first->grab().save(output + "/" + mode + "-main.png"));
    for (auto *button : first->viewerToolBar->findChildren<QToolButton *>()) {
      if (button->defaultAction() && !button->icon().isNull())
        CHECK(button->icon().name().startsWith("openscad-"));
    }
    for (auto status : {QMessageBox::Warning, QMessageBox::Critical, QMessageBox::Information,
                        QMessageBox::Question}) {
      StatusMessageBox box(status, "Status icon verification", "The operation has a message to show.",
                           QMessageBox::Ok, first);
      Icons::prepareMessageBox(&box);
      box.show();
      QApplication::processEvents();
      REQUIRE(box.grab().save(output + QString("/%1-dialog-%2.png").arg(mode).arg(int(status))));
      const auto image = box.iconPixmap().toImage();
      Icons::refresh();
      CHECK(box.iconPixmap().toImage() == image);
      box.hide();
    }
  }
  prefs->show();
  prefs->prefsActionAdvanced->trigger();
  QApplication::processEvents();
  REQUIRE(prefs->grab().save(output + "/preferences-advanced.png"));
  prefs->hide();
  first->activeEditor->setPlainText("cube( ;");
  REQUIRE(QMetaObject::invokeMethod(first, "actionRenderPreview"));
  timer.restart();
  // Background evaluation publishes the error count before the queued UI completion updates
  // the banner. Verify its final visible state rather than racing that completion callback.
  while ((first->compileErrors == 0 || !first->frameCompileResult->isVisible() ||
          first->labelCompileResultIcon->property("openscadIcon").toString() != "information-icons-error") &&
         timer.elapsed() < 10000) {
    QApplication::processEvents();
    QThread::msleep(10);
  }
  REQUIRE(first->compileErrors > 0);
  CHECK(first->labelCompileResultIcon->property("openscadIcon").toString() == "information-icons-error");
  for (const auto mode : {"monochrome", "native", "colored"}) {
    Settings::Settings::statusIconAppearance.set(mode);
    Icons::refresh();
    REQUIRE(first->frameCompileResult->grab().save(output + "/" + mode + "-error-banner.png"));
  }
  auto *animation = first->findChild<Animate *>();
  REQUIRE(animation);
  first->animateDock->setFloating(true);
  first->animateDock->resize(800, 200);
  first->animateDock->show();
  first->animateDock->raise();
  for (int i = 0; i < 8; ++i) {
    QApplication::processEvents();
    QThread::msleep(20);
  }
  REQUIRE(animation->isVisible());
  for (auto *button : animation->findChildren<QPushButton *>())
    if (!button->icon().isNull()) CHECK(button->iconSize() == QSize(24, 24));
  REQUIRE(animation->grab().save(output + "/animation.png"));
  first->animateDock->hide();
  qApp->setFont(QFont("Segoe UI", 18));
  for (int i = 0; i < 8; ++i) {
    QApplication::processEvents();
    QThread::msleep(20);
  }
  REQUIRE(first->grab().save(output + "/enlarged-font.png"));
  CHECK(first->viewerToolBar->iconSize() == QSize(24, 24));
  qApp->setFont(QFont("Segoe UI", 10));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  const auto scheme = qApp->styleHints()->colorScheme();
  const auto before = first->fileActionNew->icon().pixmap(24, 24).toImage();
  qApp->styleHints()->setColorScheme(scheme == Qt::ColorScheme::Dark ? Qt::ColorScheme::Light
                                                                     : Qt::ColorScheme::Dark);
  QApplication::processEvents();
  CHECK(first->fileActionNew->icon().pixmap(24, 24).toImage() != before);
  CHECK(first->fileActionNew->icon().pixmap(24, 24).toImage() ==
        second->fileActionNew->icon().pixmap(24, 24).toImage());
  qApp->styleHints()->setColorScheme(scheme);
  QApplication::processEvents();
#endif
  first->hide();
  second->hide();
  delete second;
  delete first;
}
