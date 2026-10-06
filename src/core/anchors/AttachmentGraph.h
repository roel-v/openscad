#pragma once
#include "core/anchors/ReferenceScene.h"
namespace Anchors {
struct GraphResult {
  std::vector<std::string> order;
  std::map<std::string,ReferenceKey> drivers;
  std::vector<Design::Diagnostic> diagnostics;
  bool valid() const {return diagnostics.empty();}
};
GraphResult attachmentGraph(const std::map<std::string,Semantic::Instance>&,
                            const std::map<ReferenceKey,Attachment>&);
}
