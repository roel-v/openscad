// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/ScadFragmentWriter.h"
#include "core/providers/CatalogLoader.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Providers {
namespace {
double numeric(const ScalarValue& value)
{
  if (auto v = std::get_if<double>(&value)) return *v;
  if (auto v = std::get_if<int64_t>(&value)) return static_cast<double>(*v);
  throw std::runtime_error("Arithmetic/frame requires numeric scalar");
}
ScalarValue evaluate(const Expression& e, const ParameterValues& values)
{
  if (e.kind == Expression::Kind::Constant) return e.constant;
  if (e.kind == Expression::Kind::Parameter) return values.at(e.name);
  if (e.kind == Expression::Kind::Vector) throw std::runtime_error("Expected scalar, found vector");
  auto a = numeric(evaluate(*e.arguments.at(0),values));
  double out;
  if (e.name == "neg") out = -a;
  else {
    auto b = numeric(evaluate(*e.arguments.at(1),values));
    if (e.name == "add") out = a+b;
    else if (e.name == "sub") out = a-b;
    else if (e.name == "mul") out = a*b;
    else { if (b == 0) throw std::runtime_error("Division by zero"); out = a/b; }
  }
  if (!std::isfinite(out)) throw std::runtime_error("Nonfinite arithmetic result");
  return out;
}
std::string expressionSource(const Expression& e, const ParameterValues& values, bool symbolic)
{
  if (e.kind == Expression::Kind::Vector) {
    std::string out = "[";
    for (size_t i = 0; i < e.arguments.size(); ++i) { if (i) out += ", "; out += expressionSource(*e.arguments[i],values,symbolic); }
    return out + "]";
  }
  if (!symbolic || e.kind == Expression::Kind::Constant) return scalarSource(evaluate(e,values));
  if (e.kind == Expression::Kind::Parameter) return e.name;
  auto a = expressionSource(*e.arguments.at(0),values,true);
  if (e.name == "neg") return "(-" + a + ")";
  auto op = e.name == "add" ? "+" : e.name == "sub" ? "-" : e.name == "mul" ? "*" : "/";
  return "(" + a + " " + op + " " + expressionSource(*e.arguments.at(1),values,true) + ")";
}
std::string statementSource(const Statement& s, const ParameterValues& values, const SymbolBindings& names,
                            bool symbolic, size_t indent = 0)
{
  std::string out(indent,' '); out += s.helper ? names.at(s.module) : s.module; out += "(";
  for (size_t i = 0; i < s.arguments.size(); ++i) {
    if (i) out += ", "; out += s.arguments[i].first + " = " + expressionSource(*s.arguments[i].second,values,symbolic);
  }
  out += ")";
  if (s.children.empty()) return out + ";\n";
  out += " {\n";
  for (const auto& child : s.children) out += statementSource(child,values,names,symbolic,indent+4);
  return out + std::string(indent,' ') + "}\n";
}
std::string importSource(const ImportRequest& imp, const ProviderBinding& binding)
{
  auto it = std::find_if(binding.libraries.begin(),binding.libraries.end(),[&](const auto& lib){ return lib.libraryId == imp.libraryId; });
  if (it == binding.libraries.end()) throw std::runtime_error("Import lacks matched library binding");
  auto prefix = it->importPrefix;
  return std::string(imp.kind == ImportRequest::Kind::Include ? "include <" : "use <") +
    (prefix.empty() ? "" : prefix + "/") + imp.path + ">\n";
}
std::array<double,3> vector(const std::array<std::shared_ptr<const Expression>,3>& expr, const ParameterValues& values)
{
  std::array<double,3> v; for (size_t i=0;i<3;++i) { v[i] = numeric(evaluate(*expr[i],values)); if (!std::isfinite(v[i])) throw std::runtime_error("Nonfinite frame coordinate"); } return v;
}
void validateFrame(const LocalFrame& f)
{
  auto norm = [](const auto& v){ return std::hypot(v[0],v[1],v[2]); };
  auto n = norm(f.normal), u = norm(f.up);
  if (n == 0 || u == 0 || !std::isfinite(n) || !std::isfinite(u)) throw std::runtime_error("Degenerate contact frame");
  std::array<double,3> a,b; for (size_t i=0;i<3;++i) { a[i]=f.normal[i]/n; b[i]=f.up[i]/u; }
  auto cross = std::array<double,3>{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
  if (norm(cross) <= 1e-10) throw std::runtime_error("Parallel contact normal/up");
}
}
std::string scalarSource(const ScalarValue& value)
{
  if (const auto *s = std::get_if<std::string>(&value)) {
    std::string out = "\"";
    for (unsigned char c : *s) {
      switch(c) { case '"':out += "\\\"";break; case '\\':out += "\\\\";break;
        case '\n':out += "\\n";break; case '\r':out += "\\r";break; case '\t':out += "\\t";break;
        default: if (c < 32 || c == 127) throw std::runtime_error("Unsupported string control character"); out += static_cast<char>(c); }
    }
    return out + "\"";
  }
  if (const auto *v = std::get_if<bool>(&value)) return *v ? "true" : "false";
  if (const auto *v = std::get_if<int64_t>(&value)) return std::to_string(*v);
  auto n = std::get<double>(value); if (!std::isfinite(n)) throw std::runtime_error("Nonfinite literal");
  if (n == 0) return "0";
  std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(std::numeric_limits<double>::max_digits10) << n;
  return out.str();
}
Result<GeneratedFragment> generateFragment(const ProviderBinding& binding, const ParameterValues& values,
                                           const SymbolBindings& names)
{
  auto fail = [&](const std::string& message)->Result<GeneratedFragment> { return {{},Diagnostic{"InvalidRecipe",binding.qualifiedId,"",message}}; };
  if (!binding.snapshot || !binding.snapshot->items.count(binding.qualifiedId)) return fail("Binding lacks owned item snapshot");
  const auto& item = binding.snapshot->items.at(binding.qualifiedId);
  if (binding.catalogDigest != item.catalogDigest || binding.recipeIndex >= item.recipes.size()) return fail("Binding differs from catalog");
  auto validated = validateValues(item,values); if (!validated) return {{},validated.diagnostic};
  const auto& recipe = item.recipes[binding.recipeIndex];
  try {
    std::set<std::string> identifiers;
    for (const auto& helper : recipe.helpers) {
      auto it=names.find(helper.id);
      if (it==names.end() || !validIdentifier(it->second) || it->second.front() == '$' || !identifiers.insert(it->second).second) return fail("Missing/invalid/duplicate helper symbol binding");
    }
    for (const auto& [key,value]:names) if (std::none_of(recipe.helpers.begin(),recipe.helpers.end(),[&](const auto& h){ return h.id==key; })) return fail("Unknown helper symbol binding");
    GeneratedFragment f; f.binding=binding; f.parameters=std::move(*validated.value); f.operation=item.operation; f.insertionRole=item.insertionRole;
    f.helpers=recipe.helpers; f.geometry=recipe.geometry; f.moduleBindings=recipe.moduleBindings;
    auto frame = validateRecipeValues(recipe,f.parameters);
    if (!frame) return fail(frame.diagnostic->message);
    f.frame=std::move(*frame.value);
    f.provenanceComment="// " + item.qualifiedId + "; catalog " + item.providerVersion + "; sha256 " + item.catalogDigest;
    for (const auto& lib:binding.libraries) f.provenanceComment += "; " + lib.profileId + " " + lib.digest;
    f.provenanceComment += "\n";
    std::string imports;
    for (const auto& imp:recipe.imports) if (imp.scope==ImportRequest::Scope::File) { f.fileImports.push_back(imp); imports+=importSource(imp,binding); }
    for (const auto& helper:recipe.helpers) {
      f.helperSource += "module " + names.at(helper.id) + "(";
      for (size_t i=0;i<helper.parameters.size();++i) { if(i) f.helperSource+=", "; f.helperSource+=helper.parameters[i]; }
      f.helperSource += ") {\n";
      for (const auto& imp:helper.localImports) f.helperSource+="    " + importSource(imp,binding);
      // Parameter references stay ordinary source parameters inside helper definitions.
      f.helperSource += statementSource(helper.body,f.parameters,names,true,4) + "}\n";
    }
    f.geometrySource=statementSource(recipe.geometry,f.parameters,names,false);
    f.standaloneSource=f.provenanceComment+imports+f.helperSource+f.geometrySource;
    if(f.standaloneSource.size()>64*1024) return fail("Emitted source exceeds 64 KiB");
    return {std::move(f),{}};
  } catch (const std::exception& e) { return fail(e.what()); }
}
Result<LocalFrame> validateRecipeValues(const Recipe& recipe, const ParameterValues& values)
{
  try {
    std::function<void(const Statement&)> check = [&](const Statement& statement) {
      for (const auto& [name,expression] : statement.arguments) expressionSource(*expression,values,false);
      for (const auto& child : statement.children) check(child);
    };
    check(recipe.geometry);
    for (const auto& helper : recipe.helpers) check(helper.body);
    LocalFrame frame{vector(recipe.origin,values),vector(recipe.normal,values),vector(recipe.up,values),recipe.frameLabel};
    validateFrame(frame);
    return {std::move(frame),{}};
  } catch (const std::exception& e) { return {{},Diagnostic{"InvalidRecipe","","",e.what()}}; }
}
}
