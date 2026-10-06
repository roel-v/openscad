#pragma once
#include "core/AST.h"
#include <functional>
#include <memory>
#include <string>

class SourceFile;
// Only active under the GUI compilation lock. Native compilation uses nullptr.
struct ParserInspection {
  std::function<std::string(const std::string&)> read;
  std::function<void(const std::string&, const std::string&, const std::string&, const Location&)> dependency;
  std::function<bool()> alive;
  bool failed = false;
  int tokens = 0, nesting = 0;
};
extern ParserInspection *parserInspection;
bool parseInspection(SourceFile *&file, const std::string& text, const std::string& filename,
                     ParserInspection& inspection);
