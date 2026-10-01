#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QAccessible>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QProxyStyle>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QToolButton>
#include <QWheelEvent>

#include "gui/OpenSCADApp.h"
#include "gui/Console.h"
#include "gui/ExportPdfDialog.h"
#include "gui/MainWindow.h"
#include "core/Builtins.h"
#include "core/parsersettings.h"
#include "gui/ScintillaEditor.h"
#include "gui/Preferences.h"
#include "gui/QSettingsCached.h"
#include "gui/UIUtils.h"
#include "gui/Icons.h"
#include "gui/parameter/GroupWidget.h"
#include "gui/parameter/ParameterCheckBox.h"
#include "gui/parameter/ParameterComboBox.h"
#include "gui/parameter/ParameterSlider.h"
#include "gui/parameter/ParameterSpinBox.h"
#include "gui/parameter/ParameterText.h"
#include "gui/parameter/ParameterVector.h"
#include "gui/parameter/ParameterWidget.h"
#include "platform/PlatformUtils.h"

// Initialize resources outside the anonymous namespace (Q_INIT_RESOURCE declares global symbols).
static void initTypographyResources()
{
  Q_INIT_RESOURCE(common);
  Q_INIT_RESOURCE(icons_tabler);
  QIcon::setThemeName("openscad");
  Icons::install();
}

namespace {

OpenSCADApp& typographyApp()
{
  // Keep one application alive across Catch test cases. Settings never use the user's profile.
  static int argc = 1;
  static char name[] = "OpenSCADTypographyTests";
  static char *argv[] = {name, nullptr};
  static auto *app = [&] {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") &&
        qEnvironmentVariableIsEmpty("OPENSCAD_TYPOGRAPHY_OUTPUT")) {
      qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    auto *application = new OpenSCADApp(argc, argv);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (!qEnvironmentVariableIsEmpty("OPENSCAD_TYPOGRAPHY_THEME")) {
      application->styleHints()->setColorScheme(
        qEnvironmentVariable("OPENSCAD_TYPOGRAPHY_THEME") == "dark" ? Qt::ColorScheme::Dark
                                                                    : Qt::ColorScheme::Light);
    }
#endif
    return application;
  }();
  static const auto initialized = [] {
    static QTemporaryDir settingsDirectory;
    QCoreApplication::setOrganizationName("OpenSCADTests");
    QCoreApplication::setApplicationName("Typography");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    const auto path = qEnvironmentVariable("OPENSCAD_TYPOGRAPHY_SETTINGS", settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, path);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, path);
    QStandardPaths::setTestModeEnabled(true);
    PlatformUtils::registerApplicationPath(
      QDir(QString(OPENSCAD_TEST_DATA_DIR) + "/../..").absolutePath().toStdString());
    initTypographyResources();
    return true;
  }();
  (void)initialized;
  QObject::connect(GlobalPreferences::inst(), &Preferences::applicationFontChanged, app,
                   &OpenSCADApp::setApplicationFont, Qt::UniqueConnection);
  return *app;
}

void applyConfiguredFont()
{
  auto *prefs = GlobalPreferences::inst();
  typographyApp();
  prefs->fireApplicationFontChanged();
  QApplication::processEvents();
}

}  // namespace

void initializeGuiTestApplication() { typographyApp(); }

TEST_CASE("Customizer disclosure preserves native activation and accessible expansion", "[ui-customizer]")
{
  typographyApp();
  GroupWidget group("geometry");
  group.show();
  auto *button = group.findChild<QToolButton *>("customizerDisclosure");
  REQUIRE(button);
  CHECK_FALSE(button->isCheckable());
  auto *accessible = QAccessible::queryAccessibleInterface(button);
  REQUIRE(accessible);
  CHECK(accessible->state().collapsed);
  button->click();
  CHECK(group.isExpanded());
  CHECK_FALSE(button->isChecked());
  CHECK(accessible->state().expanded);
  CHECK(button->arrowType() == Qt::DownArrow);
  QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
  QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
  QApplication::sendEvent(button, &press);
  QApplication::sendEvent(button, &release);
  CHECK_FALSE(group.isExpanded());
  REQUIRE(accessible->actionInterface());
  accessible->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
  CHECK(group.isExpanded());
  CHECK(group.title() == "geometry");
}

TEST_CASE("UI font inheritance preserves independent content fonts", "[ui-typography]")
{
  auto& app = typographyApp();
  auto *prefs = GlobalPreferences::inst();
  QSettingsCached settings;
  for (const auto *key : {"advanced/applicationFontFamily", "advanced/applicationFontSize",
                          "advanced/customizerFontFamily", "advanced/customizerFontSize"}) {
    settings.remove(key);
  }
  applyConfiguredFont();
  ParameterWidget first;
  ParameterWidget second;
  Console console;
  console.setConsoleFont(prefs->getValue("advanced/consoleFontFamily").toString(),
                         prefs->getValue("advanced/consoleFontSize").toUInt());
  const auto consoleFont = console.document()->defaultFont();
  QWheelEvent zoom(QPointF(4, 4), QPointF(4, 4), QPoint(), QPoint(0, 120), Qt::NoButton,
                   Qt::ControlModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(console.viewport(), &zoom);
  const auto zoomedConsoleFont = console.document()->defaultFont();
  CHECK(zoomedConsoleFont.pointSize() > consoleFont.pointSize());
  const auto editorFamily = prefs->getValue("editor/fontfamily");
  const auto editorSize = prefs->getValue("editor/fontsize");
  ScintillaEditor editor(nullptr);
  editor.initFont(editorFamily.toString(), editorSize.toUInt());
  editor.zoomIn();
  const auto editorZoom = editor.qsci->SendScintilla(QsciScintilla::SCI_GETZOOM);
  const auto editorTextSize = editor.qsci->SendScintilla(QsciScintilla::SCI_STYLEGETSIZE, 0UL);

  SECTION("No overrides follows live application changes in existing and new panels")
  {
    settings.setValue("advanced/applicationFontFamily", "Arial");
    settings.setValue("advanced/applicationFontSize", 14);
    prefs->fireApplicationFontChanged();
    QApplication::processEvents();
    ParameterWidget third;
    for (auto *panel : {&first, &second, &third}) {
      CHECK(panel->font().family() == "Arial");
      CHECK(panel->scrollArea->font().pointSize() == 14);
      CHECK(panel->comboBoxDetails->font().pointSize() == 14);
    }
    prefs->updateGUI();
    CHECK_FALSE(settings.contains("advanced/customizerFontFamily"));
    CHECK_FALSE(settings.contains("advanced/customizerFontSize"));
    CHECK(prefs->customizerFontSize->currentText() == "14");
  }
  SECTION("Partial overrides inherit only the unspecified property")
  {
    settings.setValue("advanced/customizerFontFamily", "Courier New");
    settings.setValue("advanced/applicationFontSize", 16);
    prefs->fireApplicationFontChanged();
    CHECK(first.scrollArea->font().family() == "Courier New");
    CHECK(first.scrollArea->font().pointSize() == 16);
    settings.remove("advanced/customizerFontFamily");
    settings.setValue("advanced/customizerFontSize", 11);
    settings.setValue("advanced/applicationFontFamily", "Arial");
    prefs->fireApplicationFontChanged();
    CHECK(first.scrollArea->font().family() == "Arial");
    CHECK(first.scrollArea->font().pointSize() == 11);
  }
  SECTION("Both overrides survive application updates and a settings reload")
  {
    prefs->on_customizerFontChooser_currentFontChanged(QFont("Courier New"));
    const auto sizeIndex = prefs->customizerFontSize->findText("12");
    REQUIRE(sizeIndex >= 0);
    prefs->on_customizerFontSize_currentIndexChanged(sizeIndex);
    settings.setValue("advanced/applicationFontSize", 18);
    prefs->fireApplicationFontChanged();
    CHECK(first.scrollArea->font().family() == "Courier New");
    CHECK(first.scrollArea->font().pointSize() == 12);
    settings.release();
    QSettingsCached reloaded;
    ParameterWidget third;
    CHECK(reloaded.value("advanced/customizerFontFamily").toString() == "Courier New");
    CHECK(third.scrollArea->font().pointSize() == 12);
  }
  QApplication::processEvents();
  CHECK(console.document()->defaultFont() == zoomedConsoleFont);
  CHECK(prefs->getValue("editor/fontfamily") == editorFamily);
  CHECK(prefs->getValue("editor/fontsize") == editorSize);
  CHECK(editor.qsci->SendScintilla(QsciScintilla::SCI_GETZOOM) == editorZoom);
  CHECK(editor.qsci->SendScintilla(QsciScintilla::SCI_STYLEGETSIZE, 0UL) == editorTextSize);
  (void)app;
}

TEST_CASE("Customizer help is compact hover-only and modes can be reused", "[ui-customizer]")
{
  typographyApp();
  ParameterWidget panel;
  CHECK(panel.comboBoxDetails->currentIndex() == int(DescriptionStyle::HideDetails));
  BoolParameter parameter("test_parameter", "Use <5 mm & keep units.\nSecond line", "Group", true);
  ParameterDescriptionWidget description(&panel);
  auto *help = description.findChild<QLabel *>("parameterHelpIcon");
  REQUIRE(help);
  for (auto mode : {DescriptionStyle::DescriptionOnly, DescriptionStyle::HideDetails,
                    DescriptionStyle::ShowDetails, DescriptionStyle::Inline, DescriptionStyle::HideDetails}) {
    description.setDescription(&parameter, mode);
    CHECK(help->isHidden() == (mode != DescriptionStyle::HideDetails));
    CHECK(help->focusPolicy() == Qt::NoFocus);
    CHECK(description.toolTip().isEmpty());
    CHECK(description.labelParameter->isHidden() == (mode == DescriptionStyle::DescriptionOnly));
    if (mode == DescriptionStyle::HideDetails) {
      QTextDocument tooltip;
      tooltip.setHtml(help->toolTip());
      CHECK(tooltip.toPlainText() == QString::fromStdString(parameter.description()));
      CHECK_FALSE(help->findChild<QAbstractButton *>());
    }
    CHECK(parameter.value);
  }
  BoolParameter empty("empty", " \n ", "Group", false);
  description.setDescription(&empty, DescriptionStyle::HideDetails);
  CHECK(help->isHidden());
  CHECK(help->toolTip().isEmpty());
}

TEST_CASE("Customizer capitalization changes presentation only", "[ui-customizer]")
{
  typographyApp();
  for (const auto& pair : std::vector<std::pair<QString, QString>>{
         {"show USB port", "Show USB port"}, {"iPhone", "iPhone"}, {"showUSBPort", "showUSBPort"},
         {"USB port", "USB port"}, {"  width", "  Width"}, {QString::fromUtf8("échelle"), QString::fromUtf8("Échelle")},
         {QString::fromUtf8("尺寸"), QString::fromUtf8("尺寸")}, {"", ""}}) {
    CHECK(UIUtils::sentenceCaseLabel(pair.first) == pair.second);
  }
  GroupWidget first("view"), second("View");
  CHECK(first.title() != second.title());
  CHECK(first.findChild<QToolButton *>()->text() == second.findChild<QToolButton *>()->text());
  BoolParameter value("show_USB_port", "Do not Change THIS.", "view", false);
  ParameterCheckBox checkbox(nullptr, &value, DescriptionStyle::ShowDetails);
  CHECK(checkbox.findChild<QLabel *>("labelParameter")->text() == "Show USB port");
  CHECK(checkbox.findChild<QLabel *>("labelDescription")->text() == "Do not Change THIS.");
  CHECK(value.name() == "show_USB_port");
  CHECK(checkbox.findChild<QCheckBox *>()->styleSheet().isEmpty());
}

TEST_CASE("Customizer hierarchy and geometry follow font and native style changes", "[ui-typography]")
{
  typographyApp();
  const std::string name = "A_long_translated_parameter_label_that_needs_more_than_one_line";
  const std::string description = "An explanation that wraps across multiple lines in a narrow panel.";
  BoolParameter boolean(name, description, "Geometry", true);
  StringParameter text(name, description, "Geometry", "sample", boost::none);
  NumberParameter number(name, description, "Geometry", 64, 0, 100, 1);
  VectorParameter vector(name, description, "Geometry", {1, 2, 3, 4}, 0, 100, 1);
  EnumParameter choice(name, description, "Geometry", 0, {{"assembly", std::string("assembly")}});
  for (auto mode : {DescriptionStyle::ShowDetails, DescriptionStyle::Inline,
                    DescriptionStyle::HideDetails, DescriptionStyle::DescriptionOnly}) {
    INFO("Description mode " << static_cast<int>(mode));
    ParameterWidget panel;
    auto *group = new GroupWidget("Geometry", &panel);
    group->addWidget(new ParameterCheckBox(group, &boolean, mode));
    group->addWidget(new ParameterComboBox(group, &choice, mode));
    group->addWidget(new ParameterSlider(group, &number, mode));
    group->addWidget(new ParameterSpinBox(group, &number, mode));
    group->addWidget(new ParameterText(group, &text, mode));
    group->addWidget(new ParameterVector(group, &vector, mode));
    panel.scrollAreaWidgetContents->layout()->addWidget(group);
    group->setExpanded(true);
    panel.resize(480, 600);
    panel.show();
    for (int size : {10, 18}) {
      INFO("Point size " << size);
      panel.setFontFamilySize("Segoe UI", size);
      for (int i = 0; i < 4; ++i) QApplication::processEvents();
      for (auto *label : group->findChildren<QLabel *>("labelParameter")) {
        CHECK(label->font().pointSize() == size);
        CHECK(label->font().weight() == QFont::Medium);
        if (!label->isVisible()) continue;
        INFO("Label width/height " << label->width() << "/" << label->height() << ", parent height "
                                   << label->parentWidget()->height());
        CHECK(label->height() >= label->heightForWidth(label->width()));
        CHECK(label->parentWidget()->rect().contains(label->geometry()));
      }
      for (auto *field : group->findChildren<ParameterVirtualWidget *>()) {
        CHECK(field->width() <= panel.scrollArea->viewport()->width());
        for (auto *label : field->findChildren<QLabel *>()) {
          if (!label->isVisible()) continue;
          CHECK(label->height() >= label->heightForWidth(label->width()));
          CHECK(field->rect().contains(QRect(label->mapTo(field, QPoint()), label->size())));
        }
      }
      auto *input = group->findChild<QComboBox *>();
      REQUIRE(input);
      CHECK(input->font().pointSize() == size);
      CHECK(input->font().weight() == QFont::Normal);
      auto *heading = group->findChild<QToolButton *>();
      REQUIRE(heading);
      CHECK(heading->font().pointSize() == size);
      CHECK(heading->font().weight() == QFont::Medium);
      const auto output = qEnvironmentVariable("OPENSCAD_TYPOGRAPHY_OUTPUT");
      if (!output.isEmpty()) {
        REQUIRE(panel.grab().save(output + QString("/geometry-%1-%2.png").arg(int(mode)).arg(size)));
      }
    }
    auto *checkbox = group->findChild<QCheckBox *>();
    REQUIRE(checkbox);
    checkbox->setFocus(Qt::TabFocusReason);
    CHECK(checkbox->focusPolicy() & Qt::TabFocus);
    const bool checked = checkbox->isChecked();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(checkbox, &press);
    QApplication::sendEvent(checkbox, &release);
    CHECK(checkbox->isChecked() != checked);
    group->setExpanded(false);
    CHECK_FALSE(checkbox->isVisible());
    group->setExpanded(true);
    CHECK(checkbox->isVisible());
  }
}

TEST_CASE("Unspecified native spacing falls back safely", "[ui-typography]")
{
  typographyApp();
  class UnspecifiedSpacing : public QProxyStyle
  {
  public:
    UnspecifiedSpacing() : QProxyStyle("Fusion") {}
    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override
    {
      if (metric == PM_LayoutHorizontalSpacing || metric == PM_LayoutVerticalSpacing) return -1;
      return QProxyStyle::pixelMetric(metric, option, widget);
    }
    int layoutSpacing(QSizePolicy::ControlType, QSizePolicy::ControlType, Qt::Orientation,
                      const QStyleOption *, const QWidget *) const override
    {
      return -1;
    }
  } style;
  ParameterWidget panel;
  panel.setStyle(&style);
  CHECK(panel.layout()->spacing() > 0);
  const int initial = panel.layout()->spacing();
  panel.setFontFamilySize("Segoe UI", panel.font().pointSize() * 2);
  CHECK(panel.layout()->spacing() > initial);
}

TEST_CASE("Persisted UI fonts load in a fresh process", "[ui-typography]")
{
  typographyApp();
  QTemporaryDir directory;
  REQUIRE(directory.isValid());
  for (int overrides = 0; overrides < 4; ++overrides) {
    INFO("Override combination " << overrides);
    const QString path = directory.path() + "/" + QString::number(overrides);
    QSettings saved(path + "/OpenSCADTests/Typography.ini", QSettings::IniFormat);
    saved.setValue("advanced/applicationFontFamily", "Arial");
    saved.setValue("advanced/applicationFontSize", 14);
    if (overrides & 1) saved.setValue("advanced/customizerFontFamily", "Courier New");
    if (overrides & 2) saved.setValue("advanced/customizerFontSize", 11);
    saved.sync();
    REQUIRE(saved.status() == QSettings::NoError);
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("OPENSCAD_TYPOGRAPHY_SETTINGS", path);
    environment.insert("QT_QPA_PLATFORM", "offscreen");
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(), {"[typography-restart]"});
    REQUIRE(child.waitForFinished(30000));
    INFO(child.readAllStandardOutput().toStdString());
    INFO(child.readAllStandardError().toStdString());
    CHECK(child.exitStatus() == QProcess::NormalExit);
    CHECK(child.exitCode() == 0);
  }
}

TEST_CASE("Load persisted typography fixture", "[.][typography-restart]")
{
  REQUIRE_FALSE(qEnvironmentVariableIsEmpty("OPENSCAD_TYPOGRAPHY_SETTINGS"));
  typographyApp();
  QSettingsCached saved;
  const bool familyOverride = saved.contains("advanced/customizerFontFamily");
  const bool sizeOverride = saved.contains("advanced/customizerFontSize");
  applyConfiguredFont();
  ParameterWidget panel;
  CHECK(panel.font().family() == (familyOverride ? "Courier New" : "Arial"));
  CHECK(panel.font().pointSize() == (sizeOverride ? 11 : 14));
  GlobalPreferences::inst()->updateGUI();
  CHECK(saved.contains("advanced/customizerFontFamily") == familyOverride);
  CHECK(saved.contains("advanced/customizerFontSize") == sizeOverride);
}

// Opt-in native widget evidence; excluded from ordinary unit-test runs.
TEST_CASE("Capture native typography reference", "[.][typography-visual]")
{
  auto& app = typographyApp();
  const auto output = qEnvironmentVariable("OPENSCAD_TYPOGRAPHY_OUTPUT");
  REQUIRE_FALSE(output.isEmpty());
  REQUIRE(QDir().mkpath(output));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  app.styleHints()->setColorScheme(qEnvironmentVariable("OPENSCAD_TYPOGRAPHY_THEME") == "dark"
                                     ? Qt::ColorScheme::Dark
                                     : Qt::ColorScheme::Light);
#endif
#ifdef Q_OS_WIN
  REQUIRE(QApplication::setStyle("windows11"));
#endif
  applyConfiguredFont();
  ParameterWidget panel;
  panel.setWindowTitle("OpenSCAD typography reference");
  panel.comboBoxDetails->setCurrentIndex(2);
  panel.resize(480, 600);

  EnumParameter view("view", "", "View", 0, {{"assembly", std::string("assembly")}});
  EnumParameter attachment("attachment", "", "View", 0, {{"insert", std::string("insert")}});
  BoolParameter door("show_doorframe", "", "View", true);
  BoolParameter hardware("show_hardware", "", "View", true);
  BoolParameter swap("swap_boards", "Exchange the boards between their identical slots.", "View", false);
  NumberParameter width("width", "Overall bracket width", "Dimensions", 64, 40, 100, 1);
  auto *group = new GroupWidget("View", &panel);
  group->addWidget(new ParameterComboBox(group, &view, DescriptionStyle::HideDetails));
  group->addWidget(new ParameterComboBox(group, &attachment, DescriptionStyle::HideDetails));
  group->addWidget(new ParameterCheckBox(group, &door, DescriptionStyle::HideDetails));
  group->addWidget(new ParameterCheckBox(group, &hardware, DescriptionStyle::HideDetails));
  group->addWidget(new ParameterCheckBox(group, &swap, DescriptionStyle::HideDetails));
  group->addWidget(new ParameterSlider(group, &width, DescriptionStyle::HideDetails));
  group->setExpanded(true);
  panel.scrollAreaWidgetContents->layout()->addWidget(group);
  panel.scrollAreaWidgetContents->layout()->addWidget(new GroupWidget("Measured doorway", &panel));
  panel.scrollAreaWidgetContents->layout()->addWidget(new GroupWidget("Plywood", &panel));
  panel.show();
  QApplication::processEvents();
  REQUIRE(panel.grab().save(output + "/customizer.png"));
  QJsonObject facts{{"qt", qVersion()},
                    {"style", app.style()->objectName()},
                    {"applicationFont", app.font().toString()},
                    {"customizerFont", panel.scrollArea->font().toString()},
                    {"devicePixelRatio", panel.devicePixelRatioF()},
                    {"logicalDpi", panel.logicalDpiY()},
                    {"windowColor", panel.palette().color(QPalette::Window).name()},
                    {"labelWeight", group->findChild<QLabel *>("labelParameter")->font().weight()}};
  QFile metadata(output + "/fonts.json");
  REQUIRE(metadata.open(QIODevice::WriteOnly));
  metadata.write(QJsonDocument(facts).toJson());
  ExportPdfDialog pdf;
  pdf.show();
  QApplication::processEvents();
  CHECK(pdf.checkBoxAlwaysShowDialog->font().pointSize() == app.font().pointSize());
  REQUIRE(pdf.grab().save(output + "/export-pdf.png"));
  pdf.hide();
  auto *prefs = GlobalPreferences::inst();
  prefs->show();
  QApplication::processEvents();
  REQUIRE(prefs->grab().save(output + "/preferences.png"));
  prefs->prefsActionAdvanced->trigger();
  QApplication::processEvents();
  REQUIRE(prefs->grab().save(output + "/preferences-advanced.png"));
  prefs->hide();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  const auto scheme = app.styleHints()->colorScheme();
  const auto windowColor = panel.palette().color(QPalette::Window);
  app.styleHints()->setColorScheme(scheme == Qt::ColorScheme::Dark ? Qt::ColorScheme::Light
                                                                   : Qt::ColorScheme::Dark);
  QApplication::processEvents();
  CHECK(panel.palette().color(QPalette::Window) != windowColor);
  CHECK(panel.font().pointSize() == app.font().pointSize());
  app.styleHints()->setColorScheme(scheme);
  QApplication::processEvents();
  CHECK(panel.palette().color(QPalette::Window) == windowColor);
#endif
  if (panel.devicePixelRatioF() == 1) {
    QIcon::setThemeName("openscad");
    Builtins::initialize();
    parser_init();
    auto *window = new MainWindow(QStringList());
    window->resize(1200, 800);
    QApplication::processEvents();
    REQUIRE(window->grab().save(output + "/main-window.png"));
    window->hide();
    delete window;
  }
  // Do not close Preferences: its close handler writes user-facing AI configuration.
  panel.hide();
}
