#include "AgentSyntax.h"
#include "AnnotationStore.h"
#include "core/ParserInspection.h"
#include "core/SourceFile.h"
#include "core/ModuleInstantiation.h"
#include "core/UserModule.h"
#include "core/function.h"
#include "core/Expression.h"
#include "utils/ScopedOutput.h"
#include "utils/printutils.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <cmath>
#include <deque>

namespace {
QString str(const std::string& s) { return QString::fromStdString(s).left(4096); }
QJsonValue span(const Location& loc)
{
  if (loc.isNone() || loc.firstLine() < 1 || loc.firstColumn() < 1 || loc.lastLine() < loc.firstLine() ||
      loc.lastColumn() < 1) return QJsonValue::Null;
  return QJsonObject{{"path", str(loc.fileName())}, {"first_line", loc.firstLine()},
    {"first_column", loc.firstColumn()}, {"last_line", loc.lastLine()}, {"last_column", loc.lastColumn()},
    {"column_units", "utf8_bytes"}, {"end_exclusive", true}};
}
QJsonValue number(double value)
{
  if (std::isfinite(value)) return value;
  return QJsonObject{{"type", "non_finite"}, {"value", std::isnan(value) ? "nan" : value > 0 ? "+inf" : "-inf"}};
}
}

// Const access is limited to expression child links. Never evaluates or prints expressions.
class AgentSyntaxBuilder {
public:
  AgentAnnotations::InspectionTree build(const std::vector<std::unique_ptr<SourceFile>>& files,
                                        const QJsonObject& source)
  {
    AgentAnnotations::InspectionTree tree;
    const auto treeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    tree.metadata = {{"tree_id", treeId}, {"kind", "syntax"}, {"document_id", source.value("document_id")},
      {"snapshot_id", source.value("snapshot_id")}, {"source", source}, {"complete", true},
      {"representation", "native_syntax_in_source_order"}};
    struct Item { const ASTNode *ast; const LocalScope *scope; QString id; int depth; };
    std::deque<Item> queue;
    QJsonArray roots;
    int serial = 0;
    qint64 bytes = 0;
    for (const auto& file : files) {
      auto id = QString::number(++serial);
      roots.append(id);
      queue.push_back({file.get(), nullptr, id, 0});
    }
    while (!queue.empty()) {
      const auto item = queue.front();
      queue.pop_front();
      QJsonObject node{{"tree_id", treeId}, {"node_id", item.id}, {"kind", "unsupported"},
        {"name", ""}, {"location", item.ast ? span(item.ast->location()) : QJsonValue(QJsonValue::Null)},
        {"complete", true}};
      QJsonObject attrs;
      QJsonArray children;
      auto add = [&](const QString& role, const ASTNode *ast, const LocalScope *scope = nullptr) {
        if (!ast && !scope) return;
        if (serial >= 100000 || item.depth >= 256 || bytes > 12 * 1024 * 1024) {
          node["complete"] = false;
          node["omission_reason"] = "node_depth_or_byte_budget";
          tree.metadata["complete"] = false;
          return;
        }
        const auto id = QString::number(++serial);
        children.append(QJsonObject{{"role", role}, {"node_id", id}});
        queue.push_back({ast, scope, id, item.depth + 1});
      };
      auto args = [&](const AssignmentList& values, const QString& role) {
        for (const auto& value : values) add(role, value.get());
      };
      const auto ast = item.ast;
      if (item.scope) {
        node["kind"] = "scope";
        for (const auto& entry : item.scope->syntaxOrder) add("statement", entry.get());
      } else if (auto p = dynamic_cast<const SourceFile *>(ast)) {
        node["kind"] = "source_file"; node["name"] = str(p->getFilename());
        add("body", nullptr, p->scope.get());
      } else if (auto p = dynamic_cast<const UserModule *>(ast)) {
        node["kind"] = "module_definition"; node["name"] = str(p->name);
        args(p->parameters, "parameter"); add("body", nullptr, p->body.get());
      } else if (auto p = dynamic_cast<const UserFunction *>(ast)) {
        node["kind"] = "function_definition"; node["name"] = str(p->name);
        args(p->parameters, "parameter"); add("body", p->expr.get());
      } else if (auto p = dynamic_cast<const ModuleInstantiation *>(ast)) {
        node["kind"] = "module_call"; node["name"] = str(p->name());
        attrs = {{"root", p->tag_root}, {"highlight", p->tag_highlight},
          {"background", p->tag_background}, {"disabled", p->tag_disabled}};
        args(p->arguments, "argument");
        if (auto conditional = dynamic_cast<const IfElseModuleInstantiation *>(p)) {
          node["kind"] = "conditional";
          add("then", nullptr, p->scope.get()); add("else", nullptr, conditional->getElseScope().get());
        } else add("body", nullptr, p->scope.get());
        if (p->name() == "import") attrs["dependency_resolution"] = "not_evaluated";
      } else if (auto p = dynamic_cast<const Assignment *>(ast)) {
        node["kind"] = "assignment"; node["name"] = str(p->getName()); add("value", p->getExpr().get());
      } else if (auto p = dynamic_cast<const Literal *>(ast)) {
        node["kind"] = "literal";
        attrs["value"] = p->isBool() ? QJsonValue(p->toBool()) : p->isDouble() ? number(p->toDouble()) :
          p->isString() ? QJsonValue(str(p->toString())) : QJsonValue(QJsonObject{{"type", "undefined"}});
        if (p->isString() && p->toString().size() > 4096) { node["complete"] = false; tree.metadata["complete"] = false; }
      } else if (auto p = dynamic_cast<const Lookup *>(ast)) {
        node["kind"] = "lookup"; node["name"] = str(p->get_name());
      } else if (auto p = dynamic_cast<const UnaryOp *>(ast)) {
        node["kind"] = "unary"; node["name"] = p->opString(); add("operand", p->expr.get());
      } else if (auto p = dynamic_cast<const BinaryOp *>(ast)) {
        node["kind"] = "binary"; node["name"] = p->opString(); add("left", p->left.get()); add("right", p->right.get());
      } else if (auto p = dynamic_cast<const TernaryOp *>(ast)) {
        node["kind"] = "ternary"; add("condition", p->cond.get()); add("then", p->ifexpr.get()); add("else", p->elseexpr.get());
      } else if (auto p = dynamic_cast<const ArrayLookup *>(ast)) {
        node["kind"] = "index"; add("array", p->array.get()); add("index", p->index.get());
      } else if (auto p = dynamic_cast<const MemberLookup *>(ast)) {
        node["kind"] = "member"; node["name"] = str(p->member); add("object", p->expr.get());
      } else if (auto p = dynamic_cast<const Vector *>(ast)) {
        node["kind"] = "vector"; for (const auto& child : p->getChildren()) add("element", child.get());
      } else if (auto p = dynamic_cast<const Range *>(ast)) {
        node["kind"] = "range"; add("begin", p->getBegin()); add("step", p->getStep()); add("end", p->getEnd());
      } else if (auto p = dynamic_cast<const FunctionCall *>(ast)) {
        node["kind"] = "function_call"; node["name"] = str(p->get_name()); add("callee", p->expr.get()); args(p->arguments, "argument");
      } else if (auto p = dynamic_cast<const FunctionDefinition *>(ast)) {
        node["kind"] = "function_expression"; args(p->parameters, "parameter"); add("body", p->expr.get());
      } else if (auto p = dynamic_cast<const Assert *>(ast)) {
        node["kind"] = "assert"; args(p->arguments, "argument"); add("body", p->expr.get());
      } else if (auto p = dynamic_cast<const Echo *>(ast)) {
        node["kind"] = "echo"; args(p->arguments, "argument"); add("body", p->expr.get());
      } else if (auto p = dynamic_cast<const Let *>(ast)) {
        node["kind"] = "let"; args(p->arguments, "binding"); add("body", p->expr.get());
      } else if (auto p = dynamic_cast<const LcIf *>(ast)) {
        node["kind"] = "comprehension_if"; add("condition", p->cond.get()); add("then", p->ifexpr.get()); add("else", p->elseexpr.get());
      } else if (auto p = dynamic_cast<const LcFor *>(ast)) {
        node["kind"] = "comprehension_for"; args(p->arguments, "binding"); add("body", p->expr.get());
      } else if (auto p = dynamic_cast<const LcForC *>(ast)) {
        node["kind"] = "comprehension_for_c"; args(p->arguments, "initializer"); args(p->incr_arguments, "increment");
        add("condition", p->cond.get()); add("body", p->expr.get());
      } else if (auto p = dynamic_cast<const LcEach *>(ast)) {
        node["kind"] = "comprehension_each"; add("body", p->expr.get());
      } else if (auto p = dynamic_cast<const LcLet *>(ast)) {
        node["kind"] = "comprehension_let"; args(p->arguments, "binding"); add("body", p->expr.get());
      } else {
        node["name"] = QString::fromLatin1(typeid(*ast).name());
        node["complete"] = false; tree.metadata["complete"] = false;
        node["omission_reason"] = "unsupported_native_kind";
      }
      node["attributes"] = attrs;
      node["children"] = children;
      bytes += QJsonDocument(node).toJson(QJsonDocument::Compact).size();
      tree.nodes.insert(item.id, node);
    }
    tree.metadata["root_ids"] = roots;
    tree.metadata["node_count"] = tree.nodes.size();
    if (!tree.metadata.value("complete").toBool()) tree.metadata["omission_reason"] = "Inspect node completeness and budgets.";
    return tree;
  }
};

namespace AgentAnnotations {
ParseInspectionResult inspectSyntax(const QJsonObject& source, const QString& text)
{
  ParseInspectionResult result;
  QElapsedTimer elapsed;
  elapsed.start();
  QJsonArray diagnostics, dependencies;
  QHash<QString, QByteArray> captured;
  QSet<QString> used;
  std::deque<QString> libraries;
  qint64 bytes = 0;
  bool complete = true;
  ParserInspection context;
  context.alive = [&] { return elapsed.elapsed() < 2000; };
  context.dependency = [&](const std::string& kind, const std::string& requested, const std::string& resolved, const Location& loc) {
    if (dependencies.size() >= 256) { context.failed = true; return; }
    dependencies.append(QJsonObject{{"kind", str(kind)}, {"requested_path", str(requested)},
      {"resolved_path", str(resolved)}, {"origin", "disk"}, {"scope", "parse"}, {"location", span(loc)},
      {"available", !resolved.empty()}});
    if (resolved.empty()) complete = false;
    if (kind == "use" && !resolved.empty()) libraries.push_back(str(resolved));
  };
  context.read = [&](const std::string& path) {
    const auto name = QString::fromStdString(path);
    if (captured.contains(name)) return captured.value(name).toStdString();
    QFile file(name);
    if (captured.size() >= 256 || !file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
      context.failed = true; return std::string();
    }
    auto data = file.read(4 * 1024 * 1024 + 1);
    bytes += data.size();
    if (data.size() > 4 * 1024 * 1024 || bytes > 32 * 1024 * 1024 || file.error() != QFile::NoError || data.contains('\0')) {
      context.failed = true; return std::string();
    }
    auto decoded = QString::fromUtf8(data);
    auto original = data;
    if (original.startsWith("\xef\xbb\xbf")) original.remove(0, 3);
    auto encoded = decoded.toUtf8();
    if (encoded.startsWith("\xef\xbb\xbf")) encoded.remove(0, 3);
    if (original != encoded) { context.failed = true; return std::string(); }
    captured.insert(name, data);
    return data.toStdString();
  };
  auto observe = [](const Message& message, void *data) {
    auto& records = *static_cast<QJsonArray *>(data);
    if (records.size() >= 128) return;
    records.append(QJsonObject{{"scope", "parse"}, {"message", str(message.msg)}, {"location", span(message.loc)},
      {"severity", message.group == message_group::Error || message.group == message_group::Parser_Error ? "error" : "warning"}});
  };
  ScopedOutput output(observe, &diagnostics);
  std::vector<std::unique_ptr<SourceFile>> files;
  auto parseOne = [&](const QString& input, const QString& path) {
    if (input.contains(QChar(0))) return false;
    SourceFile *file = nullptr;
    bool success = false;
    try { success = parseInspection(file, input.toUtf8().toStdString(), path.toUtf8().toStdString(), context); }
    catch (const std::exception& e) {
      diagnostics.append(QJsonObject{{"scope", "parse"}, {"severity", "error"}, {"message", str(e.what())}, {"location", QJsonValue::Null}});
    }
    files.emplace_back(file);
    return success && file;
  };
  const auto path = source.value("path").toString();
  used.insert(path);
  bool success = parseOne(text, path);
  while (success && !libraries.empty()) {
    const auto library = libraries.front(); libraries.pop_front();
    if (used.contains(library)) continue;
    used.insert(library);
    const auto data = context.read(library.toStdString());
    success = !context.failed && parseOne(QString::fromUtf8(data.data(), int(data.size())), library);
  }
  for (int i = 0; i < dependencies.size(); ++i) {
    auto dep = dependencies[i].toObject();
    const auto name = dep.value("resolved_path").toString();
    dep["disk_sha256"] = captured.contains(name) ? QJsonValue(QString::fromLatin1(Store::hash(captured.value(name)))) : QJsonValue(QJsonValue::Null);
    dep["filesystem_atomic"] = false;
    dep["dependency_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    dependencies[i] = dep;
  }
  const auto parseId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  for (int i = 0; i < diagnostics.size(); ++i) { auto d = diagnostics[i].toObject(); d["parse_id"] = parseId; diagnostics[i] = d; }
  result.outcome = {{"parse_id", parseId}, {"success", success}, {"snapshot_id", source.value("snapshot_id")},
    {"document_id", source.value("document_id")}, {"diagnostics", diagnostics}, {"dependencies", dependencies},
    {"dependencies_complete", complete && !context.failed}, {"duration_ms", double(elapsed.elapsed())},
    {"source", source}, {"limits_reached", context.failed}};
  if (success) {
    result.tree = AgentSyntaxBuilder().build(files, source);
    result.outcome["tree_id"] = result.tree.metadata.value("tree_id");
    result.tree.metadata["parse_id"] = parseId;
    for (const auto& node : result.tree.nodes) {
      if (node.value("attributes").toObject().value("dependency_resolution") != "not_evaluated") continue;
      dependencies.append(QJsonObject{{"kind", "import"}, {"scope", "syntax"}, {"available", false},
        {"requested_path", QJsonValue::Null}, {"resolved_path", QJsonValue::Null}, {"disk_sha256", QJsonValue::Null},
        {"reason", "Import arguments have not been evaluated."}, {"node_id", node.value("node_id")},
        {"tree_id", result.tree.metadata.value("tree_id")}, {"location", node.value("location")}});
      result.outcome["dependencies_complete"] = false;
    }
    result.outcome["dependencies"] = dependencies;
  }
  if (!success && diagnostics.isEmpty()) result.outcome["error"] = "Source/dependency read, encoding, parser or budget failure.";
  return result;
}
QHash<QString, QJsonObject> treePages(const InspectionTree& tree)
{
  QHash<QString, QJsonObject> pages;
  const auto base = "/v2/trees/" + tree.metadata.value("tree_id").toString();
  pages.insert(base, tree.metadata);
  for (auto it = tree.nodes.cbegin(); it != tree.nodes.cend(); ++it) {
    const auto route = base + "/nodes/" + it.key();
    const auto children = it->value("children").toArray();
    for (int offset = 0; offset < children.size() || offset == 0; offset += 500) {
      QJsonArray slice;
      for (int n = offset; n < qMin(offset + 500, int(children.size())); ++n) slice.append(children[n]);
      const QJsonValue next = offset + 500 < children.size() ? QJsonValue(route + "/children/pages/" + QString::number(offset / 500 + 1)) : QJsonValue(QJsonValue::Null);
      QJsonObject page{{"tree_id", tree.metadata.value("tree_id")}, {"node_id", it.key()}, {"children", slice}, {"next_cursor", next}};
      pages.insert(route + "/children/pages/" + QString::number(offset / 500), page);
      if (offset == 0) {
        pages.insert(route + "/children", page);
        auto node = it.value(); node["children"] = slice; node["next_cursor"] = next;
        pages.insert(route, node);
      }
    }
  }
  return pages;
}
}
