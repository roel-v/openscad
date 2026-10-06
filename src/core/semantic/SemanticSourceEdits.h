#pragma once
#include "core/semantic/SemanticTargets.h"
namespace Semantic {
struct SourcePatch { SourceProvenance::Span span; std::string expected, replacement; };
struct ParameterEdit {
  TargetKey target{TargetRole::Instance,""};
  std::string parameter, reason;
  std::optional<SourceProvenance::Site> owner;
  std::vector<SourcePatch> patches;
  bool editable=false, removable=false;
};
// These plans only reference immutable captured native spans. The GUI separately
// requires the exact open writable captured editor before applying a transaction.
ParameterEdit inspectParameter(const Scene&,TargetKey,const Parameter&);
ParameterEdit changeParameter(const Scene&,TargetKey,const Parameter&,const Value&);
ParameterEdit resetParameter(const Scene&,TargetKey,const Parameter&);
std::optional<std::string> encodeLiteral(const Value&);
bool literalParameterSource(const Scene&,const Parameter&);
ParameterEdit initializeStructure(const Scene&,const std::string& fileKey,const std::string& label);
}
