#pragma once
#include "AgentSyntax.h"
class AbstractNode;
class SourceFile;
namespace AgentAnnotations {
InspectionTree inspectModel(const AbstractNode *root, const AbstractNode *effective,
                            const SourceFile *source, const QJsonObject& result);
}
