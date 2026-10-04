#pragma once
#include "core/provenance/SourceProvenance.h"
#include <cassert>
#include <tuple>
class AbstractNode;
class ModuleInstantiation;
class UserModule;

namespace SourceProvenance {
class ProvenanceCollector {
public:
  explicit ProvenanceCollector(std::shared_ptr<const SyntaxTable> syntax);
  void definition(const UserModule& module);
  void emitted(size_t nodeIndex);
  std::shared_ptr<const Result> finish(const AbstractNode& root) const;
  size_t activeDepth() const { return stack.size(); }
  class Frame {
  public:
    Frame(ProvenanceCollector *collector, const ModuleInstantiation& call);
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
  private:
    ProvenanceCollector *collector, *previous;
    uint64_t id = 0;
  };
private:
  std::shared_ptr<const SyntaxTable> syntax;
  std::map<uint64_t, Instance> instances;
  std::map<size_t, uint64_t> nodeOwners;
  std::map<std::pair<uint64_t, SyntaxKey>, uint64_t> counts;
  std::vector<uint64_t> stack;
  uint64_t nextId = 0;
  uint64_t begin(const ModuleInstantiation& call);
};
extern thread_local ProvenanceCollector *nativeCollector;
}
