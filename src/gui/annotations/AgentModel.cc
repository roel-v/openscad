#include "AgentModel.h"
#include "core/node.h"
#include "core/primitives.h"
#include "core/TransformNode.h"
#include "core/ColorNode.h"
#include "core/CsgOpNode.h"
#include "core/LinearExtrudeNode.h"
#include "core/RotateExtrudeNode.h"
#include "core/ProjectionNode.h"
#include "core/OffsetNode.h"
#include "core/ImportNode.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <deque>
#include <set>
#include <cmath>

namespace AgentAnnotations {
namespace {
QJsonValue scalar(double value)
{
  if (std::isfinite(value)) return value;
  return QJsonObject{{"type", "non_finite"}, {"value", std::isnan(value) ? "nan" : value > 0 ? "+inf" : "-inf"}};
}
QString text(const std::string& value) { return QString::fromStdString(value).left(4096); }
}
InspectionTree inspectModel(const AbstractNode *root, const AbstractNode *effective,
                            const SourceFile *source, const QJsonObject& result)
{
  InspectionTree tree;
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  tree.metadata = {{"tree_id", id}, {"kind", "evaluated"}, {"document_id", result.value("document_id")},
    {"attempt_id", result.value("attempt_id")}, {"result_id", result.value("attempt_id")},
    {"representation", "instantiated_nodes"}, {"source", result}, {"complete", true},
    {"empty", !root || root->children.empty()}};
  QJsonArray roots, effectiveRoots, dependencies;
  struct Work { const AbstractNode *node; QString id; int depth; };
  std::deque<Work> queue;
  int serial = 0;
  if (root) { roots.append("1"); queue.push_back({root, "1", 0}); ++serial; }
  // Bound admitted nodes before queuing a very broad expansion. The wire node cap remains 100,000;
  // a conservative byte estimate can truncate earlier without dangling child references.
  constexpr int maxNodes = 12000;
  while (!queue.empty()) {
    const auto work = queue.front(); queue.pop_front();
    const auto p = work.node;
    if (p == effective) effectiveRoots.append(work.id);
    QJsonObject attributes;
    bool known = true;
    if (auto n = dynamic_cast<const CubeNode *>(p)) attributes = {{"size", QJsonArray{scalar(n->x), scalar(n->y), scalar(n->z)}}, {"center", n->center}};
    else if (auto n = dynamic_cast<const SphereNode *>(p)) attributes = {{"radius", scalar(n->r)}, {"style", text(n->style)}};
    else if (auto n = dynamic_cast<const CylinderNode *>(p)) attributes = {{"height", scalar(n->h)}, {"radius1", scalar(n->r1)}, {"radius2", scalar(n->r2)}, {"center", n->center}};
    else if (auto n = dynamic_cast<const SquareNode *>(p)) attributes = {{"size", QJsonArray{scalar(n->x), scalar(n->y)}}, {"center", n->center}};
    else if (auto n = dynamic_cast<const CircleNode *>(p)) attributes = {{"radius", scalar(n->r)}};
    else if (auto n = dynamic_cast<const TransformNode *>(p)) {
      QJsonArray rows;
      for (int row = 0; row < 4; ++row) { QJsonArray values; for (int col = 0; col < 4; ++col) values.append(scalar(n->matrix.matrix()(row, col))); rows.append(values); }
      attributes["matrix"] = rows;
    } else if (auto n = dynamic_cast<const ColorNode *>(p)) attributes = {{"rgba", QJsonArray{scalar(n->color.r()), scalar(n->color.g()), scalar(n->color.b()), scalar(n->color.a())}}};
    else if (auto n = dynamic_cast<const LinearExtrudeNode *>(p)) attributes = {
      {"height", QJsonArray{scalar(n->height.x()), scalar(n->height.y()), scalar(n->height.z())}},
      {"scale", QJsonArray{scalar(n->scale_x), scalar(n->scale_y)}}, {"twist", scalar(n->twist)},
      {"center", n->center}, {"slices", int(n->slices)}, {"segments", int(n->segments)}};
    else if (auto n = dynamic_cast<const RotateExtrudeNode *>(p)) attributes = {{"angle", scalar(n->angle)}, {"start", scalar(n->start)}, {"convexity", n->convexity}};
    else if (auto n = dynamic_cast<const ProjectionNode *>(p)) attributes = {{"cut", n->cut_mode}, {"convexity", n->convexity}};
    else if (auto n = dynamic_cast<const OffsetNode *>(p)) attributes = {{"delta", scalar(n->delta)}, {"chamfer", n->chamfer}};
    else if (auto n = dynamic_cast<const PolyhedronNode *>(p)) attributes = {{"point_count", double(n->points.size())}, {"face_count", double(n->faces.size())}, {"arrays", "omitted"}};
    else if (auto n = dynamic_cast<const PolygonNode *>(p)) attributes = {{"point_count", double(n->points.size())}, {"path_count", double(n->paths.size())}, {"arrays", "omitted"}};
    else if (auto n = dynamic_cast<const ImportNode *>(p)) {
      attributes = {{"path", text(n->filename)}, {"scale", scalar(n->scale)}, {"dpi", scalar(n->dpi)}, {"center", n->center}};
      if (dependencies.size() < 256) dependencies.append(QJsonObject{{"kind", "import"}, {"scope", "instantiated"},
        {"resolved_path", text(n->filename)}, {"disk_sha256", QJsonValue::Null}, {"bytes_observed", false}, {"node_id", work.id}});
    } else if (!dynamic_cast<const CsgOpNode *>(p) && !dynamic_cast<const GroupNode *>(p) &&
               !dynamic_cast<const ListNode *>(p) && !dynamic_cast<const AbstractIntersectionNode *>(p)) known = false;
    QJsonValue location(QJsonValue::Null);
    if (p->modinst) {
      const auto& loc = p->modinst->location();
      if (!loc.isNone() && loc.firstLine() > 0 && loc.firstColumn() > 0 && loc.lastLine() >= loc.firstLine() && loc.lastColumn() > 0)
        location = QJsonObject{{"path", text(loc.fileName())}, {"first_line", loc.firstLine()}, {"first_column", loc.firstColumn()},
          {"last_line", loc.lastLine()}, {"last_column", loc.lastColumn()}, {"column_units", "utf8_bytes"}, {"end_exclusive", true}};
      attributes["modifiers"] = QJsonObject{{"root", p->modinst->isRoot()}, {"highlight", p->modinst->isHighlight()}, {"background", p->modinst->isBackground()}};
    }
    QJsonArray children;
    bool complete = true;
    for (const auto& child : p->children) {
      if (serial >= maxNodes || work.depth >= 256) { complete = false; tree.metadata["complete"] = false; break; }
      const auto childId = QString::number(++serial);
      children.append(QJsonObject{{"role", "child"}, {"node_id", childId}});
      queue.push_back({child.get(), childId, work.depth + 1});
    }
    QJsonObject node{{"tree_id", id}, {"node_id", work.id}, {"kind", text(p->name())},
      {"name", text(p->verbose_name())}, {"location", location}, {"attributes", attributes},
      {"children", children}, {"complete", complete}, {"property_adapter_available", known},
      {"properties_complete", false},
      {"synthetic", !p->modinst || p->modinst->location().isNone()}};
    if (!known) node["properties_unavailable_reason"] = "No typed property adapter for this native node.";
    if (!complete) node["omission_reason"] = "inspection_node_or_depth_budget";
    tree.nodes.insert(work.id, node);
  }
  std::deque<const SourceFile *> files;
  std::set<const SourceFile *> visited;
  if (source) files.push_back(source);
  while (!files.empty() && dependencies.size() < 256) {
    const auto file = files.front(); files.pop_front();
    if (!visited.insert(file).second) continue;
    for (const auto& include : file->getIncludes()) {
      if (dependencies.size() >= 256) break;
      dependencies.append(QJsonObject{{"kind", "include"}, {"scope", "native_compile"},
        {"requested_path", text(include.first)}, {"resolved_path", text(include.second)},
        {"disk_sha256", QJsonValue::Null}, {"bytes_observed", false}});
    }
    for (const auto& library : file->usedlibs) {
      if (dependencies.size() >= 256) break;
      dependencies.append(QJsonObject{{"kind", "use"}, {"scope", "native_compile"},
        {"requested_path", QJsonValue::Null}, {"resolved_path", text(library)},
        {"disk_sha256", QJsonValue::Null}, {"bytes_observed", false}});
      if (auto cached = SourceFileCache::instance()->lookup(library)) files.push_back(cached);
    }
  }
  tree.metadata["root_ids"] = roots;
  tree.metadata["effective_root_ids"] = effectiveRoots;
  tree.metadata["node_count"] = tree.nodes.size();
  tree.metadata["node_budget"] = maxNodes;
  tree.metadata["dependencies"] = dependencies;
  tree.metadata["dependencies_complete"] = false; // Native compilation does not retain all input bytes.
  return tree;
}
}
