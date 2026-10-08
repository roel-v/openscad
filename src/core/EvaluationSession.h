#pragma once

#include <boost/optional.hpp>
#include <cstddef>
#include <memory>
#include <functional>
#include <stdexcept>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/AST.h"
#include "core/ContextMemoryManager.h"  // FIXME: don't use as value type so we don't need to include header
#include "core/callables.h"

class Value;
class ContextFrame;
namespace SourceProvenance { class ProvenanceCollector; }
namespace Design { class Collector; struct Payload; }
namespace Semantic { class Collector; }
class EvaluationInputs;

enum class NativeCapability { ExternalInput, FontInput, UnseededRandom, ExternalCallback };
class EvaluationCapabilityError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class EvaluationSession
{
public:
  // Shared captured inputs remain available even when source tracing is disabled.
  std::shared_ptr<const EvaluationInputs> capturedInputs;
  std::function<void(NativeCapability)> capabilityGuard;
  void requireCapability(NativeCapability capability) const { if (capabilityGuard) capabilityGuard(capability); }
  std::shared_ptr<SourceProvenance::ProvenanceCollector> provenanceCollector;
  EvaluationSession(std::string documentRoot) : document_root(std::move(documentRoot)) {}
  std::shared_ptr<Design::Collector> designCollector;
  std::shared_ptr<Semantic::Collector> semanticCollector;
  // Evaluation-local, move-owned consumer data; no borrowed native Context is retained.
  std::map<std::string, std::shared_ptr<Design::Payload>> consumerStates;

  size_t push_frame(ContextFrame *frame);
  void replace_frame(size_t index, ContextFrame *frame);
  void pop_frame(size_t index);

  [[nodiscard]] boost::optional<const Value&> try_lookup_special_variable(const std::string& name) const;
  [[nodiscard]] const Value& lookup_special_variable(const std::string& name, const Location& loc) const;
  [[nodiscard]] boost::optional<CallableFunction> lookup_special_function(const std::string& name,
                                                                          const Location& loc) const;
  [[nodiscard]] boost::optional<InstantiableModule> lookup_special_module(const std::string& name,
                                                                          const Location& loc) const;

  [[nodiscard]] const std::string& documentRoot() const { return document_root; }
  ContextMemoryManager& contextMemoryManager() { return context_memory_manager; }
  HeapSizeAccounting& accounting() { return context_memory_manager.accounting(); }

private:
  std::string document_root;
  std::vector<ContextFrame *> stack;
  ContextMemoryManager context_memory_manager;
};
