#include "../../src/gui/SurfaceWidgets.h"
#include "../../src/gui/Dock.h"
#include <QApplication>
#include <iostream>
int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  auto require = [](bool value, const char *message) { if (!value) { std::cerr << message << '\n'; exit(1); } };
  QMainWindow window;
  Dock actual(&window);
  auto *styled = actual.titleBarWidget();
  require(styled && styled->objectName() == "scadmaxxDockTitle", "Dock starts with native title");
  actual.setTitleBarVisibility(false);
  require(actual.titleBarWidget() != styled, "Dock hide title failed");
  actual.setTitleBarVisibility(true);
  require(actual.titleBarWidget() == styled, "Dock replaced styled title when restoring visibility");
  auto *first = new QDockWidget("First", &window);
  window.addDockWidget(Qt::LeftDockWidgetArea, first);
  new SurfaceWidgets::DockInstaller(&window);
  require(first->titleBarWidget(), "existing dock missed");
  auto *later = new QDockWidget("Later", &window);
  window.addDockWidget(Qt::RightDockWidgetArea, later); app.processEvents();
  require(later->titleBarWidget(), "dynamic dock missed");
  later->setWindowTitle("Renamed");
  require(later->titleBarWidget()->findChild<QLabel *>()->text() == "Renamed", "title stale");
  later->setFeatures(QDockWidget::DockWidgetMovable);
  for (auto *button : later->titleBarWidget()->findChildren<QToolButton *>())
    require(button->isHidden(), "capability button visible");
  auto *temporary = new QDockWidget("Temporary", &window); delete temporary; app.processEvents();
  QPalette palette; palette.setColor(QPalette::Window, QColor("#f0f0f0"));
  palette.setColor(QPalette::Accent, QColor("#0078d4"));
  require(SurfaceWidgets::tint(palette, 0.16).name() == "#caddec", "wrong dock blend");
  SurfaceWidgets::CategoryTabs tabs(nullptr); tabs.addTab("One"); tabs.addTab("Two");
  tabs.setCurrentIndex(1); require(tabs.currentIndex() == 1, "selection broken");
  std::cout << "dock lifecycle, capabilities, palette and tab checks passed\n";
}
