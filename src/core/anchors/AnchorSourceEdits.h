#pragma once
#include "core/anchors/ReferenceKinds.h"
#include "core/design/DesignSourceFields.h"
namespace Anchors {
struct LiteralChange {std::string path;std::shared_ptr<const Value> value;};
std::shared_ptr<const Design::Record> applyReferenceChanges(const Design::Record&,
    const std::vector<LiteralChange>&,std::string& error);
std::optional<std::vector<Design::SourcePatch>> planReferenceEdit(const Design::Record&,const SourceInput&,
    const std::vector<LiteralChange>&,size_t declarationUses,bool editShared,std::string& error);
Value capturedDefinition(std::string id,std::string label,const Anchor&,std::optional<std::string> owner);
std::optional<std::string> serializeReference(const Design::Record&,const std::string& variable);
}
