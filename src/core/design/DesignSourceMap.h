#pragma once
#include "core/design/DesignObjects.h"
#include "core/EvaluationInputs.h"
#include "core/AST.h"

namespace Design {
std::optional<DeclarationKey> declarationKey(const Location&, const SourceInput&, const std::string& hostName);
}
