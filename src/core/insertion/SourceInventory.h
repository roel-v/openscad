// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/insertion/InsertionTypes.h"
#include "core/semantic/SemanticScene.h"
#include <set>

class LocalScope;
class ModuleInstantiation;
class SourceFile;
class SourceFileCache;
namespace Insertion {
struct Diagnostic {std::string code,message;std::optional<SourceProvenance::Span> source;};
template<class T> struct Result {
  std::optional<T> value;
  std::optional<Diagnostic> diagnostic;
  explicit operator bool() const {return bool(value);}
};
struct EditableDocument {bool open=false,writable=false,userOwned=false;};
struct SourceTarget {
  enum class Kind {PartBody,OrdinaryStatement,AssemblyBody,DocumentBody} kind;
  SourceProvenance::Span region,body;
  std::optional<SourceProvenance::Site> statement;
  Transform3d localToWorld=Transform3d::Identity();
  std::string definitionId,selectedPath,label;
  std::vector<std::string> affectedPaths;
  bool shared=false,reusableDefinition=false;
};
struct TargetOption {std::optional<SourceTarget> target;std::optional<Diagnostic> diagnostic;};
struct ImportSite {
  std::string kind,spelling,resolved;
  SourceProvenance::Span source;
  bool fileScope=false;
};
class SourceInventory {
public:
  static Result<SourceInventory> capture(std::shared_ptr<const SourceFile> parsed,
    std::shared_ptr<const SourceFileCache> libraries,std::shared_ptr<const EvaluationInputs> inputs,
    std::shared_ptr<const AbstractNode> root,std::shared_ptr<const SourceProvenance::Result> provenance,
    std::shared_ptr<const Semantic::Scene> semantic,const std::string& defines={});
  std::vector<TargetOption> targets(size_t nodeIndex,bool component,
    const std::map<std::string,EditableDocument>& documents) const;
  Result<SourceTarget> documentTarget(const std::string& fileKey,
    const std::map<std::string,EditableDocument>& documents) const;
  Result<SourceTarget> definitionTarget(const std::string& definitionId,const std::string& selectedPath,
    bool component,const std::map<std::string,EditableDocument>& documents) const;
  Result<std::vector<ImportSite>> imports(const std::string& fileKey) const;
  Result<uint64_t> filePrefixPoint(const std::string& fileKey) const;
  Result<uint64_t> helperPrefixPoint(const std::string& fileKey) const;
  const SourceInput& input(const std::string& key) const;
  const std::set<std::string>& identifiers() const {return names;}
  std::string reserve(const std::string& purpose,std::set<std::string>& reservations) const;
  std::shared_ptr<const Semantic::Scene> semanticScene() const {return semantic;}
  std::shared_ptr<const SourceProvenance::Result> sourceProvenance() const {return provenance;}
  // This scanner only consumes whitespace, BOM and comments. Native ASTs establish every code boundary.
  static size_t triviaEnd(const std::string&,size_t begin,size_t end);
  static size_t trailingCommentEnd(const std::string&,size_t end);
  static std::string dominantEol(const std::string&);
private:
  std::shared_ptr<const SourceFile> parsed;
  std::shared_ptr<const SourceFileCache> libraries;
  std::shared_ptr<const EvaluationInputs> inputs;
  std::shared_ptr<const AbstractNode> root;
  std::shared_ptr<const SourceProvenance::Result> provenance;
  std::shared_ptr<const Semantic::Scene> semantic;
  std::string defines;
  std::set<std::string> names;
  std::map<SourceProvenance::SyntaxKey,std::shared_ptr<const ModuleInstantiation>> calls;
  std::map<SourceProvenance::SyntaxKey,std::shared_ptr<const LocalScope>> callScopes;
  Result<SourceProvenance::Span> body(const SourceProvenance::Site&) const;
  Result<SourceProvenance::Span> geometryRegion(const LocalScope&,const SourceProvenance::Span&) const;
  std::optional<Diagnostic> editable(const std::string&,const std::map<std::string,EditableDocument>&) const;
};
}
