#include <QApplication>
#include <QWidget>
#include <iostream>
#include "gui/parameter/GroupWidget.h"

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  int failures = 0;
  auto check = [&](bool condition, const char *message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  QWidget parent;
  GroupWidget group("Geometry", &parent);
  auto *first = new QWidget;
  auto *second = new QWidget;
  group.addWidget(first);
  group.addWidget(second);
  first->setHidden(true);
  second->setHidden(true);
  group.updateVisibility();
  check(group.isHidden(), "Initially empty group must hide");

  // Both the group and its parent are hidden; explicit child state must win.
  first->setHidden(false);
  group.updateVisibility();
  check(!group.isHidden(), "One available parameter must restore the group");
  check(!group.isExpanded(), "Restoring must preserve collapsed state");
  parent.show();
  group.setExpanded(true);
  first->setHidden(true);
  group.updateVisibility();
  check(group.isHidden(), "Hiding the last parameter must hide the header");
  second->setHidden(false);
  group.updateVisibility();
  check(!group.isHidden() && group.isExpanded(), "Expanded state must survive hide/show");

  group.setExpanded(false);
  group.updateVisibility();
  check(!group.isHidden(), "Collapsing must not hide an available group");
  second->setEnabled(false);
  group.updateVisibility();
  check(!group.isHidden(), "Locked parameters must still keep a group visible");
  first->setHidden(false);
  second->setHidden(true);
  group.updateVisibility();
  check(!group.isHidden(), "A remaining visible/global control must keep the group visible");
  first->setHidden(true);
  group.updateVisibility();
  check(group.isHidden(), "Repeated updates must hide an empty group again");
  if (!failures) std::cout << "All group visibility checks passed\n";
  return failures ? 1 : 0;
}
