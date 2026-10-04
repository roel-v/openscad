#pragma once
#include "core/anchors/ReferenceScene.h"
class EvaluationSession;
namespace Anchors {
void collectReference(Design::Record, const Design::EvaluatedDeclaration&, EvaluationSession&);
void rejectReference(const std::string& kind, const std::vector<Design::Diagnostic>&, const Design::EvaluatedDeclaration&, EvaluationSession&);
void registerCollectionPass();
std::optional<std::string> resolvePath(const std::string& authored, const std::string& relativeOwner);
}
