#include "core/design/DesignCollector.h"
#include "core/design/DesignKinds.h"
#include "core/design/DesignSourceMap.h"
#include "core/Assignment.h"
#include "core/Context.h"
#include "core/Expression.h"
#include "core/LocalScope.h"
#include "core/UserModule.h"
#include "core/ModuleInstantiation.h"
#include "core/ScopeContext.h"
#include "core/customizer/Annotation.h"
#include <map>

namespace Design {
Metadata metadata(const Assignment& assignment) {
  const auto *annotation = assignment.annotation("NativeAttributes");
  if (!annotation) return {};
  const auto *object = dynamic_cast<const ObjectExpression *>(annotation->getExpr().get());
  if (!object) return {};
  const Assignment *type = nullptr;
  for (const auto& member : object->getMembers()) if (member->getName() == "type") {
    if (type) return {true, "", "Duplicate type metadata"}; type = member.get();
  }
  if (!type) return {};
  if (object->getMembers().size() != 1) return {true, "", "Design metadata accepts only type"};
  const auto *lookup = dynamic_cast<const Lookup *>(type->getExpr().get());
  if (!lookup) return {true, "", "Design type must be a bare kind name"};
  return {true, lookup->get_name(), ""};
}
Collector::Collector(const EvaluationInputs *inputs, std::string parserRoot) : inputs(inputs), parserRoot(std::move(parserRoot)) { registerBuiltins(); }
std::optional<DeclarationKey> Collector::sourceKey(const Assignment& assignment) const {
  if (!inputs) return {};
  auto file = EvaluationInputs::fileKey(assignment.location().filePath());
  auto found = inputs->consumed().find(file);
  if (found == inputs->consumed().end()) {
    if (inputs->root().fileKey.rfind("session:",0)==0 && file==parserRoot)
      return declarationKey(assignment.location(),inputs->root(),assignment.getName());
    return {};
  }
  return declarationKey(assignment.location(), found->second, assignment.getName());
}
void Collector::collect(const Context& context, const LocalScope& scope, DeclarationScope declarationScope, bool useLibrary) {
  if (declarationScope==DeclarationScope::File) validateLocalDeclarations(scope);
  // A use library's top-level variables initialize helpers, but never export declarations.
  if (useLibrary) return;
  std::map<std::string, size_t> counts;
  const auto& occurrences = scope.assignmentOccurrences.empty() ? scope.assignments : scope.assignmentOccurrences;
  for (const auto& assignment : occurrences) ++counts[assignment->getName()];
  for (const auto& assignment : occurrences) {
    auto mark = metadata(*assignment); if (!mark.marked) continue;
    if (invalidLocal.count(assignment.get())) continue;
    auto key = sourceKey(*assignment);
    // Syntax/reassignment rejection must reach registered consumers even before a value can be dispatched.
    auto reject = [&](const std::string& code, const std::string& message) {
      Diagnostic diagnostic{code,message,key}; result.diagnostics.push_back(diagnostic);
      const auto *annotation = assignment->annotation("NativeAttributes");
      const auto *object = annotation ? dynamic_cast<const ObjectExpression *>(annotation->getExpr().get()) : nullptr;
      if (!object) return;
      std::set<std::string> notified;
      for (const auto& member : object->getMembers()) if (member->getName() == "type") {
        const auto *lookup = dynamic_cast<const Lookup *>(member->getExpr().get());
        if (!lookup || !notified.insert(lookup->get_name()).second) continue;
        auto handler = kind(lookup->get_name());
        if (!handler || handler->destination != RecordDestination::Consumer || !handler->reject) continue;
        auto origin = inputs && key && key->fileKey != inputs->root().fileKey ? SourceOrigin::Include : SourceOrigin::Root;
        for (auto parent=context.getParent();parent;parent=parent->getParent())
          if (const auto *file=dynamic_cast<const FileContext *>(parent.get());file && file->isUseLibrary()) { origin=SourceOrigin::UseLibrary;break; }
        handler->reject({diagnostic},{key.value_or(DeclarationKey{"","",assignment->getName()}),declarationScope,&context,&scope,origin},*context.session());
      }
    };
    if (!mark.error.empty()) { reject("invalid-metadata",mark.error); continue; }
    if (counts[assignment->getName()] != 1) { reject("design-reassignment","Marked assignment is reassigned: " + assignment->getName()); continue; }
    auto handler = kind(mark.kind);
    if (!handler) { result.diagnostics.push_back({"unknown-kind", "Unknown design kind: " + mark.kind, key}); continue; }
    DeclarationKey declaration = key.value_or(DeclarationKey{"", "", assignment->getName()});
    auto origin = SourceOrigin::Root;
    if (inputs && key && key->fileKey != inputs->root().fileKey) origin = SourceOrigin::Include;
    for (auto parent=context.getParent(); parent; parent=parent->getParent())
      if (const auto *file=dynamic_cast<const FileContext *>(parent.get()); file && file->isUseLibrary()) { origin=SourceOrigin::UseLibrary;break; }
    auto value = context.lookup_local_variable(assignment->getName());
    if (value) dispatch(mark.kind, *value, {declaration, declarationScope, &context, &scope, origin}, *context.session());
  }
}
void Collector::validateLocalDeclarations(const LocalScope& scope,bool file) {
  if(!file)for(const auto& assignment:scope.assignmentOccurrences){
    auto mark=metadata(*assignment);auto handler=kind(mark.kind);
    if(mark.marked && handler && !handler->scopes.count(DeclarationScope::Local) && invalidLocal.insert(assignment.get()).second)
      result.diagnostics.push_back({"unsupported-host-scope","Kind does not permit a local declaration",sourceKey(*assignment)});
  }
  for(const auto& [name,module]:scope.getUserModules())validateLocalDeclarations(*module->body,false);
  for(const auto& call:scope.moduleInstantiations){
    validateLocalDeclarations(*call->scope,false);
    if(const auto *branch=dynamic_cast<const IfElseModuleInstantiation *>(call.get());branch && branch->getElseScope())validateLocalDeclarations(*branch->getElseScope(),false);
  }
}
void Collector::dispatch(const std::string& name, const Value& value, const EvaluatedDeclaration& declaration, EvaluationSession& session) {
  auto handler = kind(name);
  if (!handler) { result.diagnostics.push_back({"unknown-kind", "Unknown design kind: " + name, declaration.source}); return; }
  if (declaration.origin == SourceOrigin::UseLibrary && declaration.scope == DeclarationScope::File &&
      declaration.source.host == DeclarationHost::Assignment) return;
  const auto diagnosticBegin=result.diagnostics.size();
  auto record = makeRecord(*handler, value, {declaration.source, declaration.scope}, result.diagnostics);
  if (!record) {
    if(handler->destination==RecordDestination::Consumer && handler->reject)
      handler->reject(std::vector<Diagnostic>(result.diagnostics.begin()+diagnosticBegin,result.diagnostics.end()),declaration,session);
    return;
  }
  if (handler->destination == RecordDestination::Consumer) {
    Record owned;
    owned.id = record->id; owned.kind = record->kind; owned.label = record->label;
    owned.definition = ownedValue(record->definition); owned.evaluated = record->evaluated; owned.source = record->source;
    for (const auto& dependency : record->dependencies) owned.dependencies.push_back({dependency.domain, dependency.id, ownedValue(dependency.data)});
    handler->consume(std::move(owned), declaration, session); return;
  }
  if (result.invalidIds.count(record->id) || result.records.count(record->id)) {
    auto previous = result.records.find(record->id);
    if (previous != result.records.end()) {
      result.diagnostics.push_back({"duplicate-id", "Duplicate design id: " + record->id, previous->second->source});
      result.records.erase(previous);
    }
    result.invalidIds.insert(record->id);
    result.diagnostics.push_back({"duplicate-id", "Duplicate design id: " + record->id, record->source});
  } else result.records.emplace(record->id, std::move(record));
}
std::shared_ptr<const Result> Collector::finish() const { return std::make_shared<const Result>(result); }
}
