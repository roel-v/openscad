#pragma once
#include <QHash>
#include <QJsonObject>
#include <QString>
namespace AgentAnnotations {
struct InspectionTree {
  QJsonObject metadata;
  QHash<QString, QJsonObject> nodes;
};
struct ParseInspectionResult {
  InspectionTree tree;
  QJsonObject outcome;
};
ParseInspectionResult inspectSyntax(const QJsonObject& source, const QString& text);
QHash<QString, QJsonObject> treePages(const InspectionTree& tree);
}
