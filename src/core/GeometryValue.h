#pragma once

#include <memory>
#include <string>

class AbstractNode;
class Geometry;
class ModuleInstantiation;

// Captured module parameters are instantiated immediately; Boolean evaluation is lazy.
// Keep the literal AST alive because nodes retain pointers to their instantiations.
struct GeometryValue {
  std::shared_ptr<const ModuleInstantiation> literal;
  std::shared_ptr<const AbstractNode> root;
  std::string documentPath;
  std::shared_ptr<const Geometry> evaluated;
  bool evaluationComplete = false;
};

using GeometryPtr = std::shared_ptr<GeometryValue>;
