#pragma once
#include "core/AST.h"
#include "core/EvaluationInputs.h"
#include <functional>
#include <memory>
#include <string>

class SourceFile;
// Only active under the GUI compilation lock. Native compilation uses nullptr.
struct ParserInspection {
  SourceReader read;
  std::function<void(const std::string&, const std::string&, const std::string&, const Location&)> dependency;
  std::function<bool()> alive;
  bool failed = false;
  uint64_t tokens = 0, tokenLimit = 8192;
  int nesting = 0;
};
extern ParserInspection *parserInspection;
bool parseInspection(SourceFile *&file, const std::string& text, const std::string& filename,
                     ParserInspection& inspection);
