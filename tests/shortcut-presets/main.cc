#include "../../src/gui/ShortcutPreset.h"
#include <QApplication>
#include <QJsonDocument>
#include <iostream>
int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QAction a, b; a.setObjectName("a"); b.setObjectName("b");
  a.setShortcuts({QKeySequence("Ctrl+1"), QKeySequence("Alt+1")});
  b.setShortcut(QKeySequence("Ctrl+2"));
  const QList<QAction *> actions{&a, &b};
  const QMap<QAction *, QList<QKeySequence>> defaults{{&a, a.shortcuts()}, {&b, b.shortcuts()}};
  auto bindings = ShortcutPreset::snapshot(actions);
  auto require = [](bool value, const char *message) { if (!value) { std::cerr << message << '\n'; exit(1); } };
  require(ShortcutPreset::validate(bindings, actions, defaults).isEmpty(), "valid snapshot rejected");
  require(bindings["a"].toArray().size() == 2, "alternatives lost");
  require(QJsonDocument::fromJson(QJsonDocument(bindings).toJson()).object() == bindings, "round trip changed data");
  bindings["a"] = QJsonArray{};
  require(ShortcutPreset::validate(bindings, actions, defaults).isEmpty(), "explicit unbound rejected");
  bindings["optional-action"] = QJsonArray{"Alt+9"};
  require(ShortcutPreset::validate(bindings, actions, defaults).isEmpty(), "unknown action rejected");
  bindings["a"] = QJsonArray{"Ctrl+2"};
  require(!ShortcutPreset::validate(bindings, actions, defaults).isEmpty(), "collision accepted");
  bindings["a"] = QJsonArray{42};
  require(!ShortcutPreset::validate(bindings, actions, defaults).isEmpty(), "malformed shortcut accepted");
  bindings["a"] = QJsonArray{"not a key"};
  require(!ShortcutPreset::validate(bindings, actions, defaults).isEmpty(), "invalid sequence accepted");
  bindings.remove("a"); bindings.remove("b");
  require(ShortcutPreset::validate(bindings, actions, defaults).isEmpty(), "new-action defaults rejected");
  std::cout << "8 shortcut preset checks passed\n";
}
