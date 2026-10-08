#pragma once
#include "AgentSyntax.h"
class AbstractNode;
class SourceFile;
class SourceFileCache;
namespace AgentAnnotations {
InspectionTree inspectModel(const AbstractNode *root, const AbstractNode *effective,
                            const SourceFile *source, SourceFileCache *libraries, const QJsonObject& result);
}
