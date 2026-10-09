#pragma once
#include <QAction>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QMap>
#include <QString>

namespace ShortcutPreset {
inline QJsonObject snapshot(const QList<QAction *>& actions)
{
  QJsonObject bindings;
  for (auto *action : actions) {
    if (action->objectName().isEmpty()) continue;
    QJsonArray keys;
    for (const auto& key : action->shortcuts()) keys.append(key.toString(QKeySequence::PortableText));
    bindings.insert(action->objectName(), keys);
  }
  return bindings;
}

// Validate the whole map before persisting or changing any QAction. Unknown actions are retained on disk.
inline QString validate(const QJsonObject& bindings, const QList<QAction *>& actions,
                        const QMap<QAction *, QList<QKeySequence>>& defaults)
{
  QMap<QString, QString> occupied;
  QMap<QString, QAction *> known;
  for (auto *action : actions) if (!action->objectName().isEmpty()) known.insert(action->objectName(), action);
  for (auto it = bindings.begin(); it != bindings.end(); ++it) {
    if (!it.value().isArray()) return QStringLiteral("Bindings for %1 must be an array.").arg(it.key());
    for (const auto& value : it.value().toArray()) {
      if (!value.isString()) return QStringLiteral("Invalid shortcut for %1.").arg(it.key());
      const QString text = value.toString();
      const QKeySequence key(text, QKeySequence::PortableText);
      if (text.isEmpty() || key.isEmpty() || key.toString(QKeySequence::PortableText) != text)
        return QStringLiteral("Invalid shortcut '%1' for %2.").arg(text, it.key());
    }
  }
  for (auto it = known.begin(); it != known.end(); ++it) {
    auto *action = it.value();
    QList<QKeySequence> keys = defaults.value(action);
    if (bindings.contains(it.key())) {
      keys.clear();
      for (const auto& value : bindings.value(it.key()).toArray())
        keys.append(QKeySequence(value.toString(), QKeySequence::PortableText));
    }
    for (const auto& key : keys) {
      if (key.isEmpty()) continue;
      const QString text = key.toString(QKeySequence::PortableText);
      if (occupied.contains(text) && occupied[text] != it.key()) {
        // Preserve deliberately shared upstream defaults; newly introduced collisions are rejected.
        auto *other = known.value(occupied[text]);
        if (!defaults.value(action).contains(key) || !defaults.value(other).contains(key))
          return QStringLiteral("%1 conflicts between %2 and %3.").arg(text, occupied[text], it.key());
      }
      occupied.insert(text, it.key());
    }
  }
  return {};
}
} // namespace ShortcutPreset
