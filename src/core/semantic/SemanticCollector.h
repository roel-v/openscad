#pragma once
#include "core/semantic/SemanticScene.h"
#include "core/semantic/SemanticPlacement.h"
#include "core/Parameters.h"
#include <functional>
class EvaluationSession;
class ModuleInstantiation;
class AbstractModule;
class LocalScope;
namespace Semantic {
class Collector {
public:
  explicit Collector(EvaluationSession& session);
  void discover(const SourceFile& root);
  bool validateCall(const ModuleInstantiation&, const AbstractModule *, const std::shared_ptr<const Context>&);
  void captureComponent(Design::Record, const Design::EvaluatedDeclaration&);
  std::shared_ptr<const Scene> finish() const;
  const Definition *definition(const UserModule&) const;
  class Frame {
  public:
    Frame(Collector *, const ModuleInstantiation&, const AbstractModule *);
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    bool valid() const { return accepted; }
    void complete(const std::shared_ptr<AbstractNode>&);
  private:
    Collector *collector;
    bool accepted = true, pushed = false;
  };
  void bindScope(const Context&, const UserModule&);
  Parameters::BindingObserver parameterObserver(const UserModule&);
  std::optional<ScopeOwner> currentScope(const Context *, const LocalScope *) const;
  bool finalize(const std::shared_ptr<AbstractNode>&);
  std::shared_ptr<AbstractNode> instantiateDefaults(const Definition&, const std::shared_ptr<const Context>&);
  size_t activeDepth() const { return stack.size(); }
private:
  EvaluationSession& session;
  Scene scene;
  std::map<const UserModule *, std::string> modules;
  std::set<const SourceFile *> files;
  std::set<std::string> occurrences;
  std::shared_ptr<const Design::Record> pendingComponent;
  struct Active {
    const UserModule *module = nullptr;
    const Context *context = nullptr;
    std::string path;
    bool direct = false;
  };
  std::vector<Active> stack;
  std::map<size_t, std::string> groupInstances;
  std::string document;
  bool finalized = false;
  bool begin(const ModuleInstantiation&, const UserModule *);
  void partition(const std::shared_ptr<AbstractNode>&, bool original);
  Design::DeclarationKey sourceKey(const ASTNode&, Design::DeclarationHost, const std::string&) const;
  void discoverFile(const SourceFile&, bool useLibrary);
  void discoverScope(const LocalScope&, const std::shared_ptr<const SourceFile>&, bool fileScope, bool useLibrary);
  void error(const std::string& code, const std::string& message, const Design::DeclarationKey& source);
};
}
