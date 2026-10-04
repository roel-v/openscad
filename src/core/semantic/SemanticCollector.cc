#include "core/semantic/SemanticCollector.h"
#include "core/design/DesignCollector.h"
#include "core/design/DesignKinds.h"
#include "core/design/DesignSourceMap.h"
#include "core/Context.h"
#include "core/EvaluationInputs.h"
#include "core/EvaluationSession.h"
#include "core/Expression.h"
#include "core/LocalScope.h"
#include "core/ModuleInstantiation.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/UserModule.h"
#include "core/ScopeContext.h"
#include "core/TransformNode.h"
#include "core/ColorNode.h"
#include "core/node.h"
#include "core/provenance/ProvenanceCollector.h"
#include <cassert>
#include <stdexcept>
#include "utils/exceptions.h"
#include "utils/printutils.h"

namespace Semantic {
Collector::Collector(EvaluationSession& session) : session(session) { registerKinds(); scene.documentPath = session.documentRoot(); }
Design::DeclarationKey Collector::sourceKey(const ASTNode& node, Design::DeclarationHost host, const std::string& name) const {
  Design::DeclarationKey key{EvaluationInputs::fileKey(node.location().filePath()), "", name, host};
  if (auto *table = SourceProvenance::nativeSyntaxTable) {
    if (const auto *site = table->site(node)) {
      key.fileKey = site->syntax.fileKey;
      if (site->wholeComplete) {
        key.fingerprint = site->whole.fingerprint; key.beginByte = site->whole.begin; key.endByte = site->whole.end;
      }
    }
  }
  return key;
}
void Collector::error(const std::string& code, const std::string& message, const Design::DeclarationKey& source) {
  scene.diagnostics.push_back({code, message, source});
}
void Collector::discover(const SourceFile& root) {
  document = nativeEvaluationInputs ? nativeEvaluationInputs->root().fileKey : EvaluationInputs::fileKey(root.getFullpath());
  if (SourceProvenance::nativeSyntaxTable) scene.inputs = SourceProvenance::nativeSyntaxTable->inputs();
  discoverFile(root, false);
}
std::shared_ptr<AbstractNode> Collector::instantiateDefaults(const Definition& target, const std::shared_ptr<const Context>& lexicalContext) {
  if (!stack.empty() || finalized) throw std::logic_error("Definition defaults require a fresh evaluation");
  scene.defaultDefinitionId = target.id;
  auto root = std::make_shared<RootNode>();
  auto call = std::make_shared<ModuleInstantiation>(target.moduleName, AssignmentList{}, target.module->location());
  if (SourceProvenance::nativeSyntaxTable)
    SourceProvenance::nativeSyntaxTable->record(*call, SourceProvenance::SiteKind::ModuleDefinition, target.moduleName);
  auto record = std::make_shared<Design::Record>(); record->id = target.id; record->label = target.label;
  record->kind = "Component"; record->source = sourceKey(*call, Design::DeclarationHost::ModuleCall, call->name());
  pendingComponent = record;
  {
    SourceProvenance::ProvenanceCollector::Frame provenance(session.provenanceCollector.get(), *call);
    Frame frame(this, *call, target.module.get());
    if (!frame.valid()) return nullptr;
    auto node = target.module->instantiate(lexicalContext, call.get(), lexicalContext);
    frame.complete(node); if (node) root->children.push_back(node);
  }
  scene.syntheticCalls.push_back(call);
  if (!finalize(root)) return nullptr;
  return root;
}
void Collector::discoverFile(const SourceFile& file, bool useLibrary) {
  if (!files.insert(&file).second) return;
  auto owned = std::make_shared<const SourceFile>(file);
  scene.sourceFiles.push_back(owned);
  discoverScope(*file.scope, owned, true, useLibrary);
  for (const auto& path : file.usedlibs) if (auto *library = SourceFileCache::instance()->lookup(path)) discoverFile(*library, true);
}
void Collector::discoverScope(const LocalScope& scope, const std::shared_ptr<const SourceFile>& file, bool fileScope, bool useLibrary) {
  auto identity = [](const UserModule& module) {
    return EvaluationInputs::fileKey(module.location().filePath()) + ":" +
      std::to_string(module.location().firstLine()) + ":" + std::to_string(module.location().firstColumn());
  };
  std::map<std::string, std::set<std::string>> declarations;
  for (const auto& [name, module] : scope.getModuleDeclarations()) declarations[name].insert(identity(*module));
  for (const auto& [name, module] : scope.getModuleDeclarations()) {
    auto mark = metadata(module->nativeAttributes.get(), Design::DeclarationHost::ModuleDefinition);
    auto key = sourceKey(*module, Design::DeclarationHost::ModuleDefinition, module->name);
    if (mark.marked) {
      scene.marked = true;
      if (!mark.error.empty()) error("invalid-semantic-metadata", mark.error, key);
      else if (!fileScope) error("local-semantic-definition", "Semantic definitions must be at file scope", key);
      else if (declarations[name].size() != 1) error("semantic-redefinition", "A semantic module cannot be redeclared or shadowed in its file scope", key);
      else {
        // Declaration identity is read from literals without evaluating a file or module body.
        auto id = dynamic_cast<const Literal *>(mark.fields.at("id").get())->toString();
        auto label = mark.fields.count("label") ? dynamic_cast<const Literal *>(mark.fields.at("label").get())->toString() : name;
        auto *table = SourceProvenance::nativeSyntaxTable;
        const auto *syntax = table ? table->site(*module) : nullptr;
        if (occurrences.insert(identity(*module)).second) {
          Definition definition{id, label, name, mark.kind == "Part" ? DefinitionKind::Part : DefinitionKind::Assembly,
            key, syntax ? std::optional<SourceProvenance::Site>(*syntax) : std::nullopt, module, file};
          if (scene.invalidIds.count(id) || !scene.definitions.emplace(id, std::move(definition)).second) {
            scene.invalidIds.insert(id); scene.definitions.erase(id); error("duplicate-semantic-id", "Duplicate definition id: " + id, key);
          }
          modules[module.get()] = id;
          ObjectType value(nullptr); value.set("id", Value(id)); value.set("label", Value(label));
          session.designCollector->dispatch(mark.kind, Value(std::move(value)),
            {key, Design::DeclarationScope::File, nullptr, file->scope.get(), useLibrary ? Design::SourceOrigin::UseLibrary : Design::SourceOrigin::Root}, session);
        }
        else modules[module.get()] = id;
      }
    }
    discoverScope(*module->body, file, false, useLibrary);
  }
  for (const auto& call : scope.moduleInstantiations) {
    auto mark = metadata(call->nativeAttributes.get(), Design::DeclarationHost::ModuleCall);
    auto key = sourceKey(*call, Design::DeclarationHost::ModuleCall, call->name());
    if (mark.marked) scene.marked = true;
    if (mark.marked && !mark.error.empty()) error("invalid-semantic-metadata", mark.error, key);
    if (mark.marked && call->childrenSupplied) error("component-children", "Component calls cannot supply child geometry or blocks", key);
    discoverScope(*call->scope, file, false, useLibrary);
    if (const auto *branch = dynamic_cast<const IfElseModuleInstantiation *>(call.get()); branch && branch->getElseScope())
      discoverScope(*branch->getElseScope(), file, false, useLibrary);
  }
}
const Definition *Collector::definition(const UserModule& module) const {
  auto found = modules.find(&module); if (found == modules.end()) return nullptr;
  auto value = scene.definitions.find(found->second); return value == scene.definitions.end() ? nullptr : &value->second;
}
bool Collector::validateCall(const ModuleInstantiation& call, const AbstractModule *nativeModule, const std::shared_ptr<const Context>& context) {
  if (!scene.marked) return true;
  pendingComponent.reset();
  const auto *module = dynamic_cast<const UserModule *>(nativeModule);
  const auto *target = module ? definition(*module) : nullptr;
  auto mark = metadata(call.nativeAttributes.get(), Design::DeclarationHost::ModuleCall);
  auto key = sourceKey(call, Design::DeclarationHost::ModuleCall, call.name());
  if (target && !mark.marked) {
    error("component-required", "Calling a semantic template requires Component metadata", key); return false;
  }
  if (!mark.marked) return true;
  if (!mark.error.empty()) return false;
  if (!target) { error("component-target", "Component must resolve to a marked Part or Assembly module", key); return false; }
  if (call.childrenSupplied) return false;
  ObjectType value(nullptr);
  for (const auto& field : {"id", "label"}) {
    auto found = mark.fields.find(field);
    if (found != mark.fields.end()) value.set(field, found->second->evaluate(context));
  }
  if (!value.contains("label")) value.set("label", Value(target->label));
  auto origin = Design::SourceOrigin::Root;
  if (nativeEvaluationInputs && key.fileKey != nativeEvaluationInputs->root().fileKey) origin = Design::SourceOrigin::Include;
  session.designCollector->dispatch("Component", Value(std::move(value)),
    {key, Design::DeclarationScope::Local, context.get(), nullptr, origin}, session);
  return session.designCollector->finish()->diagnostics.empty();
}
void Collector::captureComponent(Design::Record record, const Design::EvaluatedDeclaration&) {
  pendingComponent = std::make_shared<const Design::Record>(std::move(record));
}
Collector::Frame::Frame(Collector *collector, const ModuleInstantiation& call, const AbstractModule *module)
  : collector(collector) {
  const auto *user = dynamic_cast<const UserModule *>(module);
  if (collector && user) { accepted = collector->begin(call, user); pushed = accepted; }
}
Collector::Frame::~Frame() { if (pushed) { assert(!collector->stack.empty()); collector->stack.pop_back(); } }
void Collector::Frame::complete(const std::shared_ptr<AbstractNode>& node) {
  if (!pushed || !node || !collector->stack.back().direct) return;
  auto& instance = collector->scene.instances.at(collector->stack.back().path);
  instance.localRoot = node;
  collector->groupInstances.emplace(node->index(), instance.path);
}
bool Collector::begin(const ModuleInstantiation& call, const UserModule *module) {
  const auto *target = definition(*module);
  std::string parent = stack.empty() ? "" : stack.back().path;
  if (!target) { stack.push_back({module, nullptr, parent, false}); return true; }
  auto record = std::move(pendingComponent);
  if (!record) { error("component-metadata", "Component metadata was not captured", sourceKey(call, Design::DeclarationHost::ModuleCall, call.name())); return false; }
  if (!parent.empty() && scene.definitions.at(scene.instances.at(parent).definitionId).kind == DefinitionKind::Part) {
    error("component-inside-part", "A Part cannot contain Component instances", *record->source); return false;
  }
  auto path = parent + "/" + escapeSegment(record->id);
  if (scene.instances.count(path)) { error("duplicate-component-id", "Sibling Component IDs must be unique: " + path, *record->source); return false; }
  Instance instance;
  instance.id = record->id; instance.label = record->label; instance.path = path; instance.parentPath = parent;
  instance.definitionId = target->id; instance.source = *record->source;
  instance.invocationId = session.provenanceCollector ? session.provenanceCollector->currentInvocation() : 0;
  if (auto *table = SourceProvenance::nativeSyntaxTable) if (auto *site = table->site(call)) {
    instance.call = *site; scene.callInstances[site->syntax].push_back(path);
  }
  scene.instances.emplace(path, std::move(instance)); stack.push_back({module, nullptr, path, true}); return true;
}
void Collector::bindScope(const Context& context, const UserModule& module) {
  if (!stack.empty() && stack.back().module == &module) stack.back().context = &context;
}
Parameters::BindingObserver Collector::parameterObserver(const UserModule& module) {
  if (stack.empty() || !stack.back().direct || stack.back().module != &module) return {};
  const auto path = stack.back().path;
  return [this, path](const Parameters::Binding& binding, const Value& value) {
    auto& instance = scene.instances.at(path);
    Parameter parameter;
    parameter.name = binding.name; parameter.type = value.type(); parameter.displayedValue = value.toString();
    // Data is detached from evaluator accounting. Closures/ranges are inspectable but have no literal edit control.
    try { parameter.value = std::make_shared<const Value>(Design::ownedValue(value)); } catch (const std::invalid_argument&) {}
    parameter.winningArgument = binding.argument; parameter.fromDefault = !binding.argument; parameter.ambiguous = binding.ambiguous;
    const auto& target = scene.definitions.at(instance.definitionId);
    if (binding.argument && instance.call && *binding.argument < instance.call->arguments.size())
      parameter.source = instance.call->arguments[*binding.argument];
    else if (!binding.argument && target.syntax) for (const auto& source : target.syntax->arguments)
      if (source.name && *source.name == binding.name) parameter.source = source;
    instance.parameters.emplace(binding.name, std::move(parameter));
  };
}
std::optional<ScopeOwner> Collector::currentScope(const Context *context, const LocalScope *scope) const {
  if (!context || !scope) return {};
  if (const auto *file = dynamic_cast<const FileContext *>(context); file && file->scopeIdentity() == scope && !file->isUseLibrary())
    return ScopeOwner{document, "", "", "", 0, false, scene.defaultDefinitionId};
  if (stack.empty() || stack.back().path.empty()) return {};
  const auto& active = stack.back();
  const auto& instance = scene.instances.at(active.path);
  const auto assembly = scene.definitions.at(instance.definitionId).kind == DefinitionKind::Assembly ? instance.path : instance.parentPath;
  ScopeOwner owner{document, instance.definitionId, instance.path, assembly, instance.invocationId,
    active.direct && active.context == context && active.module->body.get() == scope, scene.defaultDefinitionId};
  return owner;
}
void Collector::partition(const std::shared_ptr<AbstractNode>& root, bool original) {
  scene.partOwners.clear(); scene.parts.clear(); scene.residual.clear();
  for (auto& [path, instance] : scene.instances) instance.displayed = false;
  std::map<size_t, bool> hasComponent, hasGeometry;
  std::function<void(const AbstractNode&)> count = [&](const AbstractNode& node) {
    bool components = groupInstances.count(node.index()), geometry = dynamic_cast<const AbstractPolyNode *>(&node) != nullptr;
    for (const auto& child : node.children) { count(*child); components |= hasComponent[child->index()]; geometry |= hasGeometry[child->index()]; }
    hasComponent[node.index()] = components; hasGeometry[node.index()] = geometry;
  };
  count(*root);
  std::function<void(const std::shared_ptr<AbstractNode>&, Transform3d, Transform3d, std::string, std::string, bool, bool, bool, Color4f)> visit;
  visit = [&](const std::shared_ptr<AbstractNode>& node, Transform3d world, Transform3d relative, std::string parent,
              std::string part, bool background, bool highlight, bool residualParent, Color4f color) {
    auto before = world;
    if (const auto *transform = dynamic_cast<const TransformNode *>(node.get())) { world = world * transform->matrix; relative = relative * transform->matrix; }
    background |= node->modinst->isBackground(); highlight |= node->modinst->isHighlight();
    if (const auto *colored = dynamic_cast<const ColorNode *>(node.get()); colored && !color.isValid()) color = colored->color;
    auto owner = groupInstances.find(node->index());
    if (owner != groupInstances.end()) {
      auto& instance = scene.instances.at(owner->second);
      instance.localToParent = relative; instance.localToWorld = world;
      if (original) instance.originalLocalToParent = relative;
      instance.background = background; instance.highlight = highlight;
      parent = instance.path; relative = Transform3d::Identity();
      if (scene.definitions.at(instance.definitionId).kind == DefinitionKind::Part) {
        part = instance.path; scene.parts.push_back({part, node, world, background, highlight, color});
      }
    }
    if (!part.empty()) {
      scene.partOwners[node->index()] = part;
      if (node->modinst->isRoot() && node.get() != scene.instances.at(part).localRoot.get())
        error("unsupported-part-root", "A root modifier inside a Part discards its semantic boundary", scene.instances.at(part).source);
    } else {
      const auto name = node->name();
      if (hasComponent[node->index()] && owner == groupInstances.end() && name != "group" && name != "list" &&
          name != "transform" && name != "color" && name != "root")
        error("unsupported-component-composition", "Native " + name + " crosses a Component boundary", sourceKey(*node->modinst, Design::DeclarationHost::ModuleCall, node->modinst->name()));
      if (!parent.empty() && dynamic_cast<const LeafNode *>(node.get()))
        error("assembly-anonymous-geometry", "Assembly output must belong to explicit Components", scene.instances.at(parent).source);
      if (!residualParent && !hasComponent[node->index()] && hasGeometry[node->index()]) {
        scene.residual.push_back({"", node, before, background, highlight, color}); residualParent = true;
      }
    }
    for (const auto& child : node->children) visit(child, world, relative, parent, part, background, highlight, residualParent, color);
  };
  visit(root, Transform3d::Identity(), Transform3d::Identity(), "", "", false, false, false, Color4f());
  auto displayed = find_root_tag(root); if (!displayed) displayed = root;
  std::set<size_t> displayNodes;
  std::function<void(const AbstractNode&)> display = [&](const AbstractNode& node) {
    displayNodes.insert(node.index());
    auto found = groupInstances.find(node.index()); if (found != groupInstances.end()) scene.instances.at(found->second).displayed = true;
    for (const auto& child : node.children) display(*child);
  };
  display(*displayed);
  if (!original) {
    if (displayed != root) {
      scene.partOwners.clear(); scene.parts.clear(); scene.residual.clear();
      visit(displayed, Transform3d::Identity(), Transform3d::Identity(), "", "", false, false, false, Color4f());
    }
    scene.parts.erase(std::remove_if(scene.parts.begin(), scene.parts.end(), [&](const Piece& piece) { return !displayNodes.count(piece.root->index()); }), scene.parts.end());
    scene.residual.erase(std::remove_if(scene.residual.begin(), scene.residual.end(), [&](const Piece& piece) { return !displayNodes.count(piece.root->index()); }), scene.residual.end());
  }
}
bool Collector::finalize(const std::shared_ptr<AbstractNode>& root) {
  if (finalized || !stack.empty()) throw std::logic_error("Semantic finalization requires one closed native evaluation");
  finalized = true;
  if (SourceProvenance::nativeSyntaxTable) scene.syntax = std::make_shared<const SourceProvenance::SyntaxTable>(*SourceProvenance::nativeSyntaxTable);
  partition(root, true);
  if (!finish()->valid()) return false;
  PlacementContext context(session, scene, root);
  auto outcome = runPlacementPasses(context);
  if (!outcome.success) { scene.extensions.clear(); error(outcome.code, outcome.message, Design::DeclarationKey{document}); return false; }
  partition(root, false);
  return finish()->valid();
}
std::shared_ptr<const Scene> Collector::finish() const {
  auto result = std::make_shared<Scene>(scene);
  if (SourceProvenance::nativeSyntaxTable) result->syntax = std::make_shared<const SourceProvenance::SyntaxTable>(*SourceProvenance::nativeSyntaxTable);
  auto design = session.designCollector->finish();
  for (const auto& id : design->invalidIds) if (modules.size() && (result->definitions.count(id) || result->invalidIds.count(id))) {
    result->invalidIds.insert(id); result->definitions.erase(id);
  }
  if (result->marked) result->diagnostics.insert(result->diagnostics.end(), design->diagnostics.begin(), design->diagnostics.end());
  return result;
}
}
