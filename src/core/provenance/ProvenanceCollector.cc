#include "core/provenance/ProvenanceCollector.h"
#include "core/ModuleInstantiation.h"
#include "core/UserModule.h"
#include "core/TransformNode.h"
#include "core/node.h"
#include "core/CSGNode.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>

namespace SourceProvenance {
thread_local ProvenanceCollector *nativeCollector = nullptr;
ProvenanceCollector::ProvenanceCollector(std::shared_ptr<const SyntaxTable> table) : syntax(std::move(table)) {
  if (!syntax) throw std::invalid_argument("Provenance collection requires a syntax table");
}
ProvenanceCollector::Frame::Frame(ProvenanceCollector *collector, const ModuleInstantiation& call)
  : collector(collector), previous(nativeCollector) {
  if (collector) id = collector->begin(call);
  nativeCollector = collector;
}
ProvenanceCollector::Frame::~Frame() {
  if (collector) {
    assert(!collector->stack.empty() && collector->stack.back() == id);
    collector->stack.pop_back();
  }
  nativeCollector = previous;
}
uint64_t ProvenanceCollector::begin(const ModuleInstantiation& call) {
  const auto *site = syntax->site(call);
  if (!site) { stack.push_back(0); return 0; }
  if (nextId == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("Provenance invocation ID exhausted");
  const uint64_t parent = stack.empty() ? 0 : stack.back();
  auto& ordinal = counts[{parent, site->syntax}];
  if (ordinal == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("Provenance call ordinal exhausted");
  Instance instance;
  instance.id = ++nextId;
  if (parent) {
    instance.parent = parent;
    instance.path = instances.at(parent).path;
  }
  instance.path.push_back({site->syntax, ++ordinal});
  instance.call = *site;
  const auto id = instance.id;
  instances.emplace(id, std::move(instance));
  stack.push_back(id);
  return id;
}
void ProvenanceCollector::definition(const UserModule& module) {
  if (stack.empty() || !stack.back()) return;
  if (const auto *site = syntax->site(module)) instances.at(stack.back()).definition = *site;
}
void ProvenanceCollector::emitted(size_t nodeIndex) {
  if (stack.empty()) return;
  const auto owner = stack.back();
  nodeOwners[nodeIndex] = owner;
  if (owner) instances.at(owner).emittedNodeIndices.push_back(nodeIndex);
}
void ProvenanceCollector::linkEmitted(size_t nodeIndex, uint64_t invocation) {
  if (invocation && !instances.count(invocation)) throw std::invalid_argument("Unknown provenance invocation");
  if (!nodeOwners.emplace(nodeIndex, invocation).second) throw std::invalid_argument("Node already has a provenance owner");
  if (invocation) instances.at(invocation).emittedNodeIndices.push_back(nodeIndex);
}

std::shared_ptr<const Result> ProvenanceCollector::finish(const AbstractNode& root) const {
  if (!stack.empty()) throw std::logic_error("Cannot publish provenance with an open invocation");
  auto result = std::shared_ptr<Result>(new Result);
  // Publication owns a separate immutable table; later parser work cannot change it.
  result->table = std::shared_ptr<const SyntaxTable>(new SyntaxTable(*syntax, true));
  result->invocations = instances;
  std::function<std::set<uint64_t>(const AbstractNode&, Transform3d, int)> visit;
  visit = [&](const AbstractNode& node, Transform3d transform, int polarity) {
    if (auto transformed = dynamic_cast<const TransformNode *>(&node)) transform = transform * transformed->matrix;
    auto owner = nodeOwners.find(node.index());
    const uint64_t id = owner == nodeOwners.end() ? 0 : owner->second;
    const auto name = node.name();
    const bool derived = name == "render" || name == "hull" || name == "minkowski" || name == "projection" || name == "import";
    std::set<uint64_t> children;
    for (size_t i = 0; i < node.children.size(); ++i) {
      auto contributors = visit(*node.children[i], transform, name == "difference" && i > 0 ? -polarity : polarity);
      children.insert(contributors.begin(), contributors.end());
    }
    Mapping mapping;
    mapping.localToWorld = transform;
    mapping.invertible = transform.matrix().allFinite() && Eigen::FullPivLU<Matrix3d>(transform.linear()).isInvertible();
    mapping.boundaryPolarity = polarity;
    if (!id) {
      mapping.candidateInstances.assign(children.begin(), children.end());
      mapping.confidence = children.size() > 1 ? Confidence::Multiple : Confidence::Unavailable;
      mapping.reason = "This grouping node has no direct source invocation; descendants are navigation candidates";
    } else if (!node.children.empty() && !derived) {
      children.insert(id);
      mapping.candidateInstances.assign(children.begin(), children.end());
      mapping.confidence = Confidence::Multiple;
      mapping.reason = "Composite geometry has no unique face-to-source relation";
    } else {
      mapping.candidateInstances = {id};
      mapping.confidence = Confidence::Unique;
      mapping.reason = derived ? "Known producing call; inner face contributors are unavailable"
        : polarity < 0 ? "Source of a subtraction boundary; containing boolean calls are in the invocation path"
                       : "Current emitting invocation; this is a boundary contributor, not a persistent part identity";
    }
    auto existing = result->nodes.find(node.index());
    if (existing != result->nodes.end() && existing->second.localToWorld &&
        !existing->second.localToWorld->matrix().isApprox(transform.matrix(), 1e-12)) {
      existing->second.confidence = Confidence::Multiple;
      existing->second.localToWorld.reset();
      existing->second.invertible = false;
      existing->second.reason = "One evaluated node occurs in multiple transform paths";
    } else result->nodes[node.index()] = std::move(mapping);
    if (derived || node.children.empty()) return id ? std::set<uint64_t>{id} : std::set<uint64_t>{};
    return children;
  };
  visit(root, Transform3d::Identity(), 1);
  // Remove pure grouping/transform invocations from the final geometry candidate set.
  std::set<uint64_t> producing;
  std::function<void(const AbstractNode&)> collect = [&](const AbstractNode& node) {
    const auto name = node.name();
    const bool derived = name == "render" || name == "hull" || name == "minkowski" || name == "projection" || name == "import";
    if (derived || node.children.empty()) {
      auto owner = nodeOwners.find(node.index());
      if (owner != nodeOwners.end() && owner->second) producing.insert(owner->second);
    } else for (const auto& child : node.children) collect(*child);
  };
  collect(root);
  result->finalMapping.candidateInstances.assign(producing.begin(), producing.end());
  result->finalMapping.confidence = producing.size() == 1 ? Confidence::Unique
    : producing.empty() ? Confidence::Unavailable : Confidence::Multiple;
  result->finalMapping.reason = producing.size() == 1 ? "Known producing call; final mesh has no per-face inner contributor map"
    : "Final merged faces have multiple or unknown source contributors; no candidate receives edit priority";
  return result;
}

std::shared_ptr<const Result> Result::withPreview(const std::vector<std::shared_ptr<CSGNode>>& roots) const {
  auto result = std::shared_ptr<Result>(new Result(*this));
  std::map<size_t, std::pair<Transform3d, int>> seen;
  std::function<void(const std::shared_ptr<CSGNode>&, int)> visit;
  visit = [&](const std::shared_ptr<CSGNode>& node, int polarity) {
    if (auto leaf = std::dynamic_pointer_cast<CSGLeaf>(node)) {
      const auto index = static_cast<size_t>(leaf->index);
      auto found = result->nodes.find(index);
      if (found == result->nodes.end()) return;
      auto& mapping = found->second;
      const auto [previous, inserted] = seen.emplace(index, std::make_pair(leaf->matrix, polarity));
      if (!inserted && (!previous->second.first.matrix().isApprox(leaf->matrix.matrix(), 1e-12) ||
                        previous->second.second != polarity)) {
        mapping.confidence = Confidence::Multiple;
        mapping.localToWorld.reset();
        mapping.invertible = false;
        mapping.boundaryPolarity = 0;
        mapping.reason = "This CSG leaf occurs in multiple transform or boundary paths";
      } else if (inserted) {
        mapping.localToWorld = leaf->matrix;
        mapping.invertible = leaf->matrix.matrix().allFinite() && Eigen::FullPivLU<Matrix3d>(leaf->matrix.linear()).isInvertible();
        mapping.boundaryPolarity = polarity;
      }
    } else if (auto op = std::dynamic_pointer_cast<CSGOperation>(node)) {
      visit(op->left(), polarity);
      visit(op->right(), op->getType() == OpenSCADOperator::DIFFERENCE ? -polarity : polarity);
    }
  };
  for (const auto& root : roots) visit(root, 1);
  return result;
}
}
