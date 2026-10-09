#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QGroupBox>
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
#include <QEnterEvent>
#include <QMouseEvent>
#include <QEventLoop>
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
#include <QToolTip>
#include <QDockWidget>
#include <QThread>
#include <QWheelEvent>

#include "gui/OpenSCADApp.h"
#include "gui/Console.h"
#include "gui/ExportPdfDialog.h"
#include "gui/Export3mfDialog.h"
#include "gui/ExportSvgDialog.h"
#include "gui/LibraryInfoDialog.h"
#include "gui/LaunchingScreen.h"
#include "gui/AboutDialog.h"
#include "gui/PrintInitDialog.h"
#include "gui/OctoPrintApiKeyDialog.h"
#include "gui/OpenCSGWarningDialog.h"
#include "gui/ColorList.h"
#include "gui/ColorLabel.h"
#include "gui/QWordSearchField.h"
#include "gui/FontList.h"
#include "gui/ViewportControl.h"
#include "gui/ErrorLog.h"
#include "gui/ProgressWidget.h"
#include "gui/Animate.h"
#include "gui/ai/DiffDialog.h"
#include "gui/ai/ChatWidget.h"
#include "gui/ai/CollapsibleBubble.h"
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

TEST_CASE("Preferences native tabs synchronize actions and conditional pages", "[ui-dialogs]")
{
  typographyApp();
  auto *prefs = GlobalPreferences::inst();
  REQUIRE(prefs->categoryTabs);
  CHECK(prefs->toolBar->isHidden());
  int tabIndex = 0;
  for (auto *action : prefs->toolBar->actions()) {
    if (action->isVisible()) {
      REQUIRE(tabIndex < prefs->categoryTabs->count());
      CHECK(prefs->categoryTabs->tabData(tabIndex++).value<QAction *>() == action);
    }
  }
  CHECK(tabIndex == prefs->categoryTabs->count());
  prefs->prefsActionAdvanced->trigger();
  CHECK(prefs->stackedWidget->currentWidget() == prefs->pageAdvanced);
  CHECK(prefs->categoryTabs->tabData(prefs->categoryTabs->currentIndex()).value<QAction *>() == prefs->prefsActionAdvanced);
  prefs->categoryTabs->setCurrentIndex(0);
  CHECK(prefs->stackedWidget->currentWidget() == prefs->page3DView);
  const bool aiVisible = prefs->prefsActionAI->isVisible();
  prefs->prefsActionAI->setVisible(true);
  prefs->prefsActionAI->trigger();
  CHECK(prefs->stackedWidget->currentWidget() == prefs->pageAI);
  prefs->prefsActionAI->setVisible(false);
  CHECK(prefs->stackedWidget->currentWidget() == prefs->pageDialogs);
  for (int i = 0; i < prefs->categoryTabs->count(); ++i) {
    auto *action = prefs->categoryTabs->tabData(i).value<QAction *>();
    REQUIRE(action);
    CHECK(action->isVisible());
    prefs->categoryTabs->setCurrentIndex(i);
    CHECK(action->isChecked());
  }
  prefs->prefsActionAI->setVisible(aiVisible);
  prefs->prefsAction3DView->trigger();
  QKeyEvent next(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
  QApplication::sendEvent(prefs->categoryTabs, &next);
  CHECK(prefs->stackedWidget->currentWidget() == prefs->pageEditor);
  prefs->prefsAction3DView->trigger();
}

TEST_CASE("Preferences group spacing follows native metrics without changing settings", "[ui-dialogs]")
{
  typographyApp();
  auto *prefs = GlobalPreferences::inst();
  const auto value = prefs->getValue("editor/fontsize");
  const auto originalFont = prefs->font();
  QFont larger = originalFont;
  larger.setPointSize(18);
  prefs->setFont(larger);
  for (auto *box : prefs->findChildren<QGroupBox *>()) {
    if (prefs->pageAdvanced->isAncestorOf(box)) CHECK(box->isFlat());
    if (box->layout()) CHECK(box->layout()->spacing() == UIUtils::layoutSpacing(prefs, Qt::Vertical));
  }
  CHECK(prefs->getValue("editor/fontsize") == value);
  CHECK(prefs->pageDialogs->layout()->alignment().testFlag(Qt::AlignTop));
  CHECK(prefs->labelAIHeader->font().weight() == QFont::Medium);
  CHECK(prefs->labelAIHeader->font().pointSize() == 18);
  prefs->setFont(originalFont);
}

TEST_CASE("Native auxiliary controls retain data and state without stylesheets", "[ui-dialogs]")
{
  typographyApp();
  QWordSearchField search;
  search.resize(300, 30);
  search.setFindCount(123456);
  CHECK(search.styleSheet().isEmpty());
  CHECK(search.textMargins().right() >= search.fontMetrics().horizontalAdvance("123456"));
  search.setFindCount(0);
  CHECK(search.textMargins().right() == 0);
  ColorLabel swatch;
  swatch.setColorInfo("red", QColor("#ff0000"));
  const auto size = swatch.sizeHint();
  swatch.setSelected(true);
  CHECK(swatch.backgroundColor() == QColor("#ff0000"));
  CHECK(swatch.palette().color(QPalette::Window) == QColor("#ff0000"));
  CHECK(swatch.lineWidth() == 3);
  CHECK(swatch.sizeHint() == size);
  CHECK(swatch.styleSheet().isEmpty());
  auto *prefs = GlobalPreferences::inst();
  prefs->AxisConfig->AxesChanged(0, 0.5);
  auto *reading = prefs->AxisConfig->progressBarAxis0->findChild<QLabel *>("axisReading");
  REQUIRE(reading);
  CHECK(reading->text() == "0.50");
  CHECK(reading->backgroundRole() == QPalette::Highlight);
  prefs->AxisConfig->AxesChanged(0, 0.0);
  CHECK(reading->text() == "0.00");
  CHECK(reading->backgroundRole() == QPalette::Base);
  DiffDialog diff("cube(1);", "cube(2);", false);
  for (auto *button : diff.findChildren<QPushButton *>()) CHECK(button->styleSheet().isEmpty());
  MessageBubble bubble("Example", true);
  QFont enlarged;
  enlarged.setPointSize(18);
  bubble.setFont(enlarged);
  CHECK(bubble.findChild<QLabel *>()->font().pointSize() == 18);
  const QString renderer = "Renderer: <test> & literal\nSecond: value\n";
  LibraryInfoDialog info(renderer);
  CHECK(info.infoTextBox->toPlainText().endsWith(renderer));
  auto *buttons = diff.findChild<QDialogButtonBox *>();
  REQUIRE(buttons);
  for (auto *button : buttons->buttons()) {
    if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) {
      button->click();
      CHECK(diff.result() == QDialog::Accepted);
    } else if (buttons->buttonRole(button) == QDialogButtonBox::DestructiveRole) {
      button->click();
      CHECK(diff.result() == 2);
    }
  }
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&diff, &escape);
  CHECK(diff.result() == QDialog::Rejected);
  ErrorLog errors;
  errors.toErrorLog(Message("Example warning", message_group::Warning));
  auto *warning = errors.errorLogModel->item(0, errorLog_column::group);
  REQUIRE(warning);
  CHECK_FALSE(warning->icon().isNull());
  CHECK_FALSE(warning->data(Qt::ForegroundRole).isValid());
}

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
                          "advanced/customizerFontFamily", "advanced/customizerFontSize",
                          "advanced/consoleFontSize"}) {
    settings.remove(key);
  }
  // A chosen Console size (and its transient zoom) remains independent of UI changes.
  settings.setValue("advanced/consoleFontSize", prefs->getValue("advanced/applicationFontSize"));
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

TEST_CASE("Console size inherits the UI until explicitly selected", "[ui-typography]")
{
  typographyApp();
  auto *prefs = GlobalPreferences::inst();
  QSettingsCached settings;
  settings.remove("advanced/consoleFontSize");
  settings.setValue("advanced/applicationFontSize", 14);
  prefs->fireApplicationFontChanged();
  const auto family = prefs->getValue("advanced/consoleFontFamily").toString();
  Console console;
  console.setConsoleFont(family, prefs->getValue("advanced/consoleFontSize").toUInt());
  CHECK(console.document()->defaultFont().pointSize() == 14);
  settings.setValue("advanced/applicationFontSize", 18);
  prefs->fireApplicationFontChanged();
  CHECK(console.document()->defaultFont().pointSize() == 18);
  CHECK(console.document()->defaultFont().family() == family);
  CHECK(prefs->consoleFontSize->currentText() == "18");
  CHECK_FALSE(settings.contains("advanced/consoleFontSize"));

  // Selecting even the current inherited size turns it into an explicit choice.
  const auto selected = prefs->consoleFontSize->findText("18");
  REQUIRE(selected >= 0);
  prefs->consoleFontSize->activated(selected);
  settings.setValue("advanced/applicationFontSize", 20);
  prefs->fireApplicationFontChanged();
  CHECK(console.document()->defaultFont().pointSize() == 18);
  CHECK(prefs->consoleFontSize->currentText() == "18");
  settings.release();
  CHECK(prefs->getValue("advanced/consoleFontSize").toUInt() == 18);
  settings.remove("advanced/consoleFontSize");
}

TEST_CASE("Customizer help is compact unfocusable and modes can be reused", "[ui-customizer]")
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

TEST_CASE("Customizer help aligns with text and opens promptly by mouse", "[ui-customizer]")
{
  typographyApp();
  ParameterDescriptionWidget description(nullptr);
  BoolParameter parameter("test_parameter", "Immediate click help", "Group", true);
  description.setDescription(&parameter, DescriptionStyle::HideDetails);
  description.resize(300, 60);
  description.show();
  QApplication::processEvents();
  auto *help = description.findChild<QLabel *>("parameterHelpIcon");
  REQUIRE(help);
  CHECK(std::abs(help->geometry().center().y() - description.labelParameter->geometry().center().y()) <= 1);
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(1, 1), help->mapToGlobal(QPoint(1, 1)),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(help, &press);
  // Text is set synchronously; the platform may animate the tooltip's appearance.
  CHECK(QToolTip::text() == help->toolTip());
  CHECK(help->focusPolicy() == Qt::NoFocus);
  CHECK(parameter.value);
  BoolParameter hovered("hovered", "Prompt hover help", "Group", false);
  description.setDescription(&hovered, DescriptionStyle::HideDetails);
  QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), help->mapToGlobal(QPoint(1, 1)));
  QApplication::sendEvent(help, &enter);
  QEventLoop wait;
  QTimer::singleShot(350, &wait, &QEventLoop::quit);
  wait.exec();
  CHECK(QToolTip::isVisible());
  CHECK(QToolTip::text() == help->toolTip());
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(help, &leave);
  description.hide();
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
    if (overrides & 2) saved.setValue("advanced/consoleFontSize", 10);
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
  const bool consoleOverride = saved.contains("advanced/consoleFontSize");
  auto *prefs = GlobalPreferences::inst();
  Console console;
  console.setConsoleFont(prefs->getValue("advanced/consoleFontFamily").toString(),
                         prefs->getValue("advanced/consoleFontSize").toUInt());
  CHECK(console.document()->defaultFont().pointSize() == (consoleOverride ? 10 : 14));
  // This hidden case runs alone in a child process. Exercise actual shutdown cleanup:
  // an explicit 10 pt choice must survive even though it equals the Windows UI default.
  delete prefs;
  CHECK(saved.contains("advanced/consoleFontSize") == consoleOverride);
  if (consoleOverride) CHECK(saved.value("advanced/consoleFontSize").toUInt() == 10);
}

// Opt-in native widget evidence; excluded from ordinary unit-test runs.
TEST_CASE("Capture application-owned dialogs without external services", "[.][dialogs-visual]")
{
  auto& app = typographyApp();
#ifdef Q_OS_WIN
  REQUIRE(QApplication::setStyle("windows11"));
#endif
  app.setFont(QFont("Segoe UI", 10));
  const auto output = qEnvironmentVariable("OPENSCAD_TYPOGRAPHY_OUTPUT");
  REQUIRE_FALSE(output.isEmpty());
  REQUIRE(QDir().mkpath(output));
  auto capture = [&](QWidget& widget, const QString& name) {
    widget.show();
    for (int i = 0; i < 8; ++i) { QApplication::processEvents(); QThread::msleep(10); }
    CHECK(widget.styleSheet().isEmpty());
    for (auto *child : widget.findChildren<QWidget *>()) CHECK(child->styleSheet().isEmpty());
    REQUIRE(widget.grab().save(output + "/" + name + ".png"));
    widget.hide();
  };
  auto *prefs = GlobalPreferences::inst();
  const bool aiVisible = prefs->prefsActionAI->isVisible();
  prefs->prefsActionAI->setVisible(true);
  for (int i = 0; i < prefs->categoryTabs->count(); ++i) {
    prefs->categoryTabs->setCurrentIndex(i);
    auto *action = prefs->categoryTabs->tabData(i).value<QAction *>();
    capture(*prefs, action->objectName());
  }
  prefs->prefsActionAI->setVisible(aiVisible);
  prefs->prefsActionAdvanced->trigger();
  const auto original = prefs->font();
  prefs->setFont(QFont("Segoe UI", 18));
  capture(*prefs, "preferences-large-font");
  prefs->setFont(original);
  const auto advancedText = prefs->prefsActionAdvanced->text();
  prefs->prefsActionAdvanced->setText("Advanced preferences with a deliberately long category label");
  capture(*prefs, "preferences-long-category");
  bool overflow = false;
  for (auto *button : prefs->categoryTabs->findChildren<QToolButton *>())
    overflow = overflow || !button->isHidden();
  CHECK(overflow);
  prefs->prefsActionAdvanced->setText(advancedText);
  ExportPdfDialog pdf; capture(pdf, "export-pdf");
  Export3mfDialog threeMf; capture(threeMf, "export-3mf");
  ExportSvgDialog svg; capture(svg, "export-svg");
  LibraryInfoDialog info("Renderer: offline audit\n"); capture(info, "library-info");
  AboutDialog about(nullptr); capture(about, "about");
  LaunchingScreen welcome; capture(welcome, "welcome");
  PrintInitDialog print; capture(print, "print-init");
  OctoPrintApiKeyDialog key; capture(key, "octoprint-key-offline");
  OpenCSGWarningDialog warning(nullptr); capture(warning, "opencsg-warning");
  DiffDialog diff("cube(10);", "cube(20);", app.palette().color(QPalette::Window).lightness() < 128);
  capture(diff, "ai-diff");
  MessageBubble user("Create a bracket with rounded corners.", true); capture(user, "ai-user");
  MessageBubble assistant("The bracket is ready for review.", false); capture(assistant, "ai-assistant");
  CollapsibleBubble details("Read model", "cube(10);"); capture(details, "ai-tool-call");
  ChatWidget chat; chat.resize(500, 600); capture(chat, "ai-chat-offline");
  ColorList colors; colors.resize(400, 650); capture(colors, "color-list");
  FontList fonts; fonts.resize(600, 600); capture(fonts, "font-list");
  ViewportControl viewport; capture(viewport, "viewport-control");
  ProgressWidget progress; progress.setValue(450); capture(progress, "progress");
  progress.stopButton->click(); CHECK(progress.wasCanceled());
  ErrorLog errors;
  errors.toErrorLog(Message("Example warning", message_group::Warning));
  errors.toErrorLog(Message("Example error", message_group::Error));
  errors.QWidget::resize(700, 250); capture(errors, "error-log");
  Animate animation; capture(animation, "animation");
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  const auto scheme = app.styleHints()->colorScheme();
  const auto previous = prefs->palette().color(QPalette::Window);
  app.styleHints()->setColorScheme(scheme == Qt::ColorScheme::Dark ? Qt::ColorScheme::Light
                                                                 : Qt::ColorScheme::Dark);
  capture(*prefs, "preferences-live-theme-switch");
  CHECK(prefs->palette().color(QPalette::Window) != previous);
  capture(user, "ai-user-live-theme-switch");
  CHECK(user.palette().color(QPalette::Window) == prefs->palette().color(QPalette::Window));
  app.styleHints()->setColorScheme(scheme);
#endif
}

TEST_CASE("Verify native quiet Customizer with real model and presets", "[.][customizer-visual]")
{
  auto& app = typographyApp();
  const auto output = qEnvironmentVariable("OPENSCAD_TYPOGRAPHY_OUTPUT");
  REQUIRE_FALSE(output.isEmpty());
  REQUIRE(QDir().mkpath(output));
  REQUIRE(QApplication::setStyle("windows11"));
  app.setFont(QFont("Segoe UI", 10));
  Builtins::initialize();
  parser_init();
  auto *window = new MainWindow(QStringList());
  window->resize(1280, 850);
  window->show();
  window->activeEditor->setPlainText(R"(/* [view] */
// Exchange the boards; keep clearance <5 mm & retain the original units.
swap_boards = false;
show_USB_port = true;
view = "assembly"; // [assembly, cutaway]
/* [View] */
// Overall width in millimeters.
width = 64; // [40:100]
// A number without a slider.
count = 3;
// A long explanatory sentence to verify native tooltip wrapping in a narrow panel.
label = "sample";
direction = [1, 2, 3];
module model() { cube([width, 10, 10]); }
model();
)");
  REQUIRE(QMetaObject::invokeMethod(window, "actionRenderPreview"));
  auto *panel = window->findChild<ParameterWidget *>();
  REQUIRE(panel);
  auto *dock = qobject_cast<QDockWidget *>(panel->parentWidget());
  REQUIRE(dock);
  dock->show();
  window->resizeDocks({dock}, {380}, Qt::Horizontal);
  auto settle = [] {
    for (int i = 0; i < 12; ++i) { QApplication::processEvents(); QThread::msleep(10); }
  };
  settle();
  REQUIRE(panel->findChildren<GroupWidget *>().size() == 2);
  CHECK(panel->comboBoxDetails->currentIndex() == int(DescriptionStyle::HideDetails));
  panel->checkBoxAutoPreview->setChecked(false);
  int previewRequests = 0;
  QObject::connect(panel, &ParameterWidget::parametersChanged, panel, [&] { ++previewRequests; });
  auto groupNamed = [&](const QString& name) -> GroupWidget * {
    for (auto *group : panel->findChildren<GroupWidget *>()) if (group->title() == name) return group;
    return nullptr;
  };
  auto fieldNamed = [&](const std::string& name) -> ParameterVirtualWidget * {
    for (auto *field : panel->findChildren<ParameterVirtualWidget *>())
      if (field->getParameter()->name() == name) return field;
    return nullptr;
  };
  REQUIRE(groupNamed("view"));
  REQUIRE(groupNamed("View"));
  auto *header = groupNamed("view")->findChild<QToolButton *>("customizerDisclosure");
  REQUIRE(header);
  // The asynchronous initial preview temporarily disables the Customizer.
  // Wait for its enabled state before exercising interactive surfaces.
  for (int i = 0; i < 50 && !header->isEnabled(); ++i) settle();
  REQUIRE(header->isEnabled());
  const auto inheritedPalette = header->palette();
  auto headerSurface = [&](const QString& name) {
    auto *currentHeader = groupNamed("view")->findChild<QToolButton *>("customizerDisclosure");
    REQUIRE(currentHeader);
    const auto image = currentHeader->grab().toImage();
    REQUIRE(image.save(output + "/header-" + name + ".png"));
    // Sample clear fill near the right edge, away from text, arrow and border.
    return image.pixelColor(image.width() - qRound(12 * image.devicePixelRatio()), image.height() / 2);
  };
  header->setAttribute(Qt::WA_UnderMouse, false);
  const auto restingSurface = headerSurface("rest");
  CHECK(restingSurface != header->palette().color(QPalette::Window));
  header->setAttribute(Qt::WA_UnderMouse, true);
  const auto hoverSurface = headerSurface("hover");
  header->setDown(true);
  const auto pressedSurface = headerSurface("pressed");
  CHECK(hoverSurface != restingSurface);
  CHECK(pressedSurface != hoverSurface);
  header->setDown(false);
  header->setAttribute(Qt::WA_UnderMouse, false);
  CHECK(headerSurface("restored") == restingSurface);
  CHECK(header->palette() == inheritedPalette);
  CHECK_FALSE(header->isCheckable());
  auto *boolean = fieldNamed("swap_boards");
  REQUIRE(boolean);
  boolean->findChild<QCheckBox *>()->click();
  CHECK(static_cast<BoolParameter *>(boolean->getParameter())->value);
  REQUIRE(QMetaObject::invokeMethod(panel, "createSet", Q_ARG(QString, QString("Verification preset"))));
  groupNamed("View")->setExpanded(false);
  const int beforeModes = previewRequests;
  for (int mode : {0, 1, 3, 2}) {
    panel->comboBoxDetails->setCurrentIndex(mode);
    settle();
    CHECK(groupNamed("view")->isExpanded());
    CHECK_FALSE(groupNamed("View")->isExpanded());
    REQUIRE(fieldNamed("swap_boards"));
    CHECK(static_cast<BoolParameter *>(fieldNamed("swap_boards")->getParameter())->value);
    CHECK(previewRequests == beforeModes);
    REQUIRE(panel->grab().save(output + QString("/mode-%1.png").arg(mode)));
  }
  REQUIRE(QMetaObject::invokeMethod(panel, "onExpandAll"));
  for (auto *group : panel->findChildren<GroupWidget *>()) CHECK(group->isExpanded());
  auto *help = fieldNamed("swap_boards")->findChild<QLabel *>("parameterHelpIcon");
  REQUIRE(help);
  CHECK(help->isVisible());
  CHECK(help->focusPolicy() == Qt::NoFocus);
  QMouseEvent helpPress(QEvent::MouseButtonPress, QPointF(1, 1), help->mapToGlobal(QPoint(1, 1)),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(help, &helpPress);
  CHECK(QToolTip::text() == help->toolTip());
  settle();
  REQUIRE(QToolTip::isVisible());
  for (auto *widget : QApplication::topLevelWidgets()) {
    if (widget->windowType() == Qt::ToolTip && widget->isVisible())
      REQUIRE(widget->grab().save(output + "/help-tooltip.png"));
  }
  QToolTip::hideText();
  REQUIRE(QMetaObject::invokeMethod(panel, "on_comboBoxPreset_activated", Q_ARG(int, 0)));
  CHECK_FALSE(static_cast<BoolParameter *>(fieldNamed("swap_boards")->getParameter())->value);
  REQUIRE(QMetaObject::invokeMethod(panel, "on_comboBoxPreset_activated", Q_ARG(int, 1)));
  CHECK(static_cast<BoolParameter *>(fieldNamed("swap_boards")->getParameter())->value);
  settle();
  REQUIRE(window->grab().save(output + "/main-window.png"));
  // Exercise native geometry independent of the dock's width constraints.
  auto *dockParent = panel->parentWidget();
  panel->setParent(nullptr);
  panel->resize(350, 700);
  panel->show();
  settle();
  REQUIRE(panel->grab().save(output + "/customizer.png"));
  panel->setFontFamilySize("Segoe UI", 18);
  settle();
  REQUIRE(panel->grab().save(output + "/enlarged-font.png"));
  for (auto *label : panel->findChildren<QLabel *>("labelParameter")) {
    if (label->isVisible()) CHECK(label->height() >= label->heightForWidth(label->width()));
  }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  const auto scheme = app.styleHints()->colorScheme();
  const auto color = panel->palette().color(QPalette::Window);
  app.styleHints()->setColorScheme(scheme == Qt::ColorScheme::Dark ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark);
  settle();
  CHECK(panel->palette().color(QPalette::Window) != color);
  CHECK(headerSurface("live-theme-switch") != restingSurface);
  app.styleHints()->setColorScheme(scheme);
#endif
  panel->hide();
  panel->setParent(dockParent);
  window->hide();
  delete window;
}

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
