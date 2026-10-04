// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/CatalogLoader.h"
#include "core/providers/ContentDigest.h"
#include "core/providers/ScadFragmentWriter.h"
#include <algorithm>
#include <cmath>
#include <regex>
#include <set>
#include <stdexcept>
#include <tuple>

namespace Providers {
namespace {
using Json = nlohmann::json;
struct Invalid : std::exception {
  Diagnostic diagnostic;
  explicit Invalid(Diagnostic d) : diagnostic(std::move(d)) {}
};
std::string escape(const std::string& s)
{
  std::string out;
  for (char c : s) out += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
  return out;
}
struct Sax : nlohmann::json_sax<Json> {
  struct Frame { Json value; std::string pointer, key; std::set<std::string> keys; };
  std::vector<Frame> frames;
  Json root;
  std::string error, pointer;
  std::string nextPointer() const {
    if (frames.empty()) return "";
    const auto& f = frames.back();
    return f.pointer + "/" + (f.value.is_array() ? std::to_string(f.value.size()) : escape(f.key));
  }
  bool add(Json value) {
    if (frames.empty()) root = std::move(value);
    else if (frames.back().value.is_array()) frames.back().value.push_back(std::move(value));
    else frames.back().value[frames.back().key] = std::move(value);
    return true;
  }
  bool null() override { return add(nullptr); }
  bool boolean(bool v) override { return add(v); }
  bool number_integer(number_integer_t v) override { return add(v); }
  bool number_unsigned(number_unsigned_t v) override { return add(v); }
  bool number_float(number_float_t v, const string_t&) override {
    if (!std::isfinite(v)) { error = "Nonfinite number"; pointer = nextPointer(); return false; }
    return add(v);
  }
  bool string(string_t& v) override { return add(v); }
  bool binary(binary_t&) override { error = "Binary JSON is unsupported"; return false; }
  bool start(bool object) {
    if (frames.size() >= 128) { error = "JSON nesting limit exceeded"; pointer = nextPointer(); return false; }
    auto p = nextPointer();
    frames.push_back({object ? Json::object() : Json::array(), p, "", {}});
    return true;
  }
  bool start_object(std::size_t) override { return start(true); }
  bool start_array(std::size_t) override { return start(false); }
  bool key(string_t& v) override {
    auto& f = frames.back();
    if (!f.keys.insert(v).second) {
      error = "Duplicate JSON key: " + v; pointer = f.pointer + "/" + escape(v); return false;
    }
    f.key = v; return true;
  }
  bool end() { auto v = std::move(frames.back().value); frames.pop_back(); return add(std::move(v)); }
  bool end_object() override { return end(); }
  bool end_array() override { return end(); }
  bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception& e) override {
    error = e.what(); pointer = nextPointer(); return false;
  }
};
class Reader {
  std::string path;
public:
  explicit Reader(std::string p) : path(std::move(p)) {}
  [[noreturn]] void fail(const std::string& p, const std::string& message) const {
    throw Invalid({"InvalidCatalog", path, p, message});
  }
  void fields(const Json& j, const std::string& p, std::initializer_list<const char *> allowed,
              std::initializer_list<const char *> required = {}) const {
    if (!j.is_object()) fail(p, "Expected object");
    std::set<std::string> keys(allowed.begin(), allowed.end());
    for (const auto& [key, value] : j.items()) if (!keys.count(key)) fail(p + "/" + escape(key), "Unknown field");
    for (const auto *key : required) if (!j.contains(key)) fail(p + "/" + key, "Missing field");
  }
  std::string str(const Json& j, const std::string& p) const {
    if (!j.is_string()) fail(p, "Expected string");
    return j.get<std::string>();
  }
  std::string field(const Json& j, const char *key, const std::string& p) const { return str(j.at(key), p + "/" + key); }
  std::string id(const Json& j, const char *key, const std::string& p) const {
    auto s = field(j, key, p);
    static const std::regex pattern("[a-z][a-z0-9-]*(\\.[a-z][a-z0-9-]*)*");
    if (!std::regex_match(s, pattern)) fail(p + "/" + key, "Invalid catalog ID");
    return s;
  }
  void array(const Json& j, const std::string& p, size_t limit = 1000) const {
    if (!j.is_array() || j.size() > limit) fail(p, "Expected array within resource limit");
  }
  double number(const Json& j, const std::string& p) const {
    if (!j.is_number()) fail(p, "Expected finite number");
    auto d = j.get<double>(); if (!std::isfinite(d)) fail(p, "Expected finite number"); return d;
  }
  ScalarValue scalar(const Json& j, const std::string& p) const {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number_integer()) {
      if (j.is_number_unsigned() && j.get<uint64_t>() > uint64_t(INT64_MAX)) fail(p, "Integer exceeds int64");
      return j.get<int64_t>();
    }
    if (j.is_number_float()) return number(j, p);
    fail(p, "Expected scalar");
  }
  bool boolean(const Json& j, const std::string& p) const {
    if (!j.is_boolean()) fail(p, "Expected boolean"); return j.get<bool>();
  }
  std::string relative(const Json& j, const char *key, const std::string& p, bool empty = false) const {
    auto s = field(j, key, p); if (!validRelativePath(s, empty)) fail(p + "/" + key, "Invalid library-relative path"); return s;
  }
  std::string hash(const Json& j, const char *key, const std::string& p) const {
    auto s = field(j, key, p);
    if (s.size() != 64 || s.find_first_not_of("0123456789abcdef") != std::string::npos) fail(p + "/" + key, "Expected lowercase SHA-256");
    return s;
  }
  std::vector<std::string> strings(const Json& j, const std::string& p, size_t limit = 1000) const {
    array(j, p, limit); std::vector<std::string> out;
    for (size_t i = 0; i < j.size(); ++i) out.push_back(str(j[i], p + "/" + std::to_string(i)));
    return out;
  }
  std::shared_ptr<const Expression> expression(const Json& j, const std::string& p,
                                             const std::set<std::string>& params, size_t depth = 0) const {
    if (depth > 32) fail(p, "Expression depth exceeds 32");
    fields(j, p, {"constant", "parameter", "vector", "operator", "arguments"});
    auto e = std::make_shared<Expression>();
    if (j.size() == 1 && j.contains("constant")) { e->kind = Expression::Kind::Constant; e->constant = scalar(j["constant"], p + "/constant"); }
    else if (j.size() == 1 && j.contains("parameter")) {
      e->kind = Expression::Kind::Parameter; e->name = field(j, "parameter", p);
      if (!params.count(e->name)) fail(p, "Unknown parameter");
    } else if (j.size() == 1 && j.contains("vector")) {
      e->kind = Expression::Kind::Vector; array(j["vector"], p + "/vector", 128);
      for (size_t i = 0; i < j["vector"].size(); ++i) e->arguments.push_back(expression(j["vector"][i], p + "/vector/" + std::to_string(i), params, depth + 1));
    } else if (j.size() == 2 && j.contains("operator") && j.contains("arguments")) {
      e->kind = Expression::Kind::Arithmetic; e->name = field(j, "operator", p);
      if (e->name != "add" && e->name != "sub" && e->name != "mul" && e->name != "div" && e->name != "neg") fail(p, "Unknown arithmetic operator");
      array(j["arguments"], p + "/arguments", 2);
      if (j["arguments"].size() != (e->name == "neg" ? 1u : 2u)) fail(p, "Wrong arithmetic arity");
      for (size_t i = 0; i < j["arguments"].size(); ++i) e->arguments.push_back(expression(j["arguments"][i], p + "/arguments/" + std::to_string(i), params, depth + 1));
    } else fail(p, "Expected exactly one expression form");
    return e;
  }
  Statement statement(const Json& j, const std::string& p, const std::set<std::string>& params, size_t depth = 0) const {
    if (depth > 32) fail(p, "Statement depth exceeds 32");
    fields(j, p, {"module", "arguments", "children"}, {"module", "arguments"}); Statement s;
    if (j["module"].is_object()) {
      fields(j["module"], p + "/module", {"helper"}, {"helper"}); s.helper = true; s.module = field(j["module"], "helper", p + "/module");
    } else { s.module = field(j, "module", p); if (!validIdentifier(s.module)) fail(p + "/module", "Invalid SCAD identifier"); }
    array(j["arguments"], p + "/arguments", 128); std::set<std::string> names;
    for (size_t i = 0; i < j["arguments"].size(); ++i) {
      auto a = p + "/arguments/" + std::to_string(i); const auto& arg = j["arguments"][i];
      fields(arg, a, {"name", "value"}, {"name", "value"}); auto name = field(arg, "name", a);
      if (!validIdentifier(name) || !names.insert(name).second) fail(a, "Invalid/duplicate argument name");
      s.arguments.emplace_back(name, expression(arg["value"], a + "/value", params));
    }
    if (j.contains("children")) {
      array(j["children"], p + "/children");
      for (size_t i = 0; i < j["children"].size(); ++i) s.children.push_back(statement(j["children"][i], p + "/children/" + std::to_string(i), params, depth + 1));
    }
    return s;
  }
  ParameterSpec parameter(const Json& j, const std::string& p) const {
    fields(j, p, {"id","label","type","default","unit","min","max","exclusiveMin","enumValues","maxLength"}, {"id","label","type","default"});
    ParameterSpec s; s.id = field(j,"id",p); if (!validIdentifier(s.id)) fail(p + "/id", "Invalid parameter identifier");
    s.label = field(j,"label",p); auto t = field(j,"type",p);
    if (t == "number") s.type = ParameterSpec::Type::Number;
    else if (t == "integer") s.type = ParameterSpec::Type::Integer;
    else if (t == "bool") s.type = ParameterSpec::Type::Bool;
    else if (t == "string") s.type = ParameterSpec::Type::String;
    else if (t == "enum") s.type = ParameterSpec::Type::Enum;
    else fail(p + "/type", "Unknown parameter type");
    s.defaultValue = scalar(j["default"],p + "/default");
    if (j.contains("unit")) { s.unit = field(j,"unit",p); if (s.unit != "mm" && s.unit != "deg") fail(p + "/unit","Unknown unit"); }
    if (j.contains("min")) s.min = number(j["min"],p + "/min");
    if (j.contains("max")) s.max = number(j["max"],p + "/max");
    if (j.contains("exclusiveMin")) s.exclusiveMin = boolean(j["exclusiveMin"],p + "/exclusiveMin");
    if (s.min && s.max && *s.min > *s.max) fail(p,"Reversed bounds");
    if (s.exclusiveMin && !s.min) fail(p,"exclusiveMin requires min");
    if (j.contains("maxLength")) {
      auto length = number(j["maxLength"],p + "/maxLength");
      if (length < 0 || length > 4*1024*1024 || std::floor(length) != length) fail(p,"Invalid maxLength");
      s.maxLength = static_cast<size_t>(length);
    }
    if (j.contains("enumValues")) s.enumValues = strings(j["enumValues"],p + "/enumValues");
    std::set<std::string> enums(s.enumValues.begin(),s.enumValues.end());
    if (enums.size() != s.enumValues.size() || (s.type == ParameterSpec::Type::Enum && enums.empty())) fail(p,"Invalid enum values");
    if ((s.min || s.max || !s.unit.empty()) && s.type != ParameterSpec::Type::Number && s.type != ParameterSpec::Type::Integer) fail(p,"Numeric metadata on nonnumeric parameter");
    if (j.contains("enumValues") && s.type != ParameterSpec::Type::Enum) fail(p,"enumValues on non-enum");
    if (j.contains("maxLength") && s.type != ParameterSpec::Type::String) fail(p,"maxLength on non-string");
    return s;
  }
  LibraryRequirement library(const Json& j, const std::string& p) const {
    fields(j,p,{"libraryId","label","defaultImportPrefix","profiles"},{"libraryId","label","defaultImportPrefix","profiles"});
    LibraryRequirement lib; lib.id = id(j,"libraryId",p); lib.label = field(j,"label",p); lib.defaultImportPrefix = relative(j,"defaultImportPrefix",p,true);
    array(j["profiles"],p + "/profiles"); if (j["profiles"].empty()) fail(p,"Library needs a profile");
    std::set<std::string> ids, digests;
    for (size_t i = 0; i < j["profiles"].size(); ++i) {
      auto q = p + "/profiles/" + std::to_string(i); const auto& v = j["profiles"][i];
      fields(v,q,{"profileId","releaseLabel","revisionEvidence","fileHashes","importEdges","digest"},{"profileId","releaseLabel","revisionEvidence","fileHashes","importEdges","digest"});
      LibraryProfile profile; profile.id = field(v,"profileId",q);
      if (!std::regex_match(profile.id,std::regex("[a-z][a-z0-9-]*(\\.[a-z0-9][a-z0-9-]*)*"))) fail(q + "/profileId","Invalid profile ID");
      profile.releaseLabel = field(v,"releaseLabel",q);
      profile.revisionEvidence = field(v,"revisionEvidence",q); profile.digest = hash(v,"digest",q);
      if (!ids.insert(profile.id).second || !digests.insert(profile.digest).second) fail(q,"Duplicate profile ID/fingerprint");
      array(v["fileHashes"],q + "/fileHashes",4096);
      for (const auto& f : v["fileHashes"]) {
        fields(f,q + "/fileHashes",{"path","sha256"},{"path","sha256"}); auto name = relative(f,"path",q);
        if (!profile.fileHashes.emplace(name,hash(f,"sha256",q)).second) fail(q,"Duplicate profile path");
      }
      if (profile.fileHashes.empty() || closureDigest(profile.fileHashes) != profile.digest) fail(q,"Profile digest does not match file set");
      array(v["importEdges"],q + "/importEdges",16384);
      std::set<std::tuple<std::string,std::string,std::string>> edges;
      for (const auto& e : v["importEdges"]) {
        fields(e,q + "/importEdges",{"from","kind","import","to"},{"from","kind","import","to"});
        ImportEdge edge{relative(e,"from",q),field(e,"kind",q),relative(e,"import",q),relative(e,"to",q)};
        if ((edge.kind != "include" && edge.kind != "use") || !profile.fileHashes.count(edge.from) || !profile.fileHashes.count(edge.to)) fail(q,"Invalid profile edge");
        if (!edges.emplace(edge.from,edge.kind,edge.spelling).second) fail(q,"Duplicate import edge");
        profile.importEdges.push_back(std::move(edge));
      }
      lib.profiles.push_back(std::move(profile));
    }
    return lib;
  }
  Recipe recipe(const Json& j, const std::string& p, const ItemDefinition& item) const {
    fields(j,p,{"profileIds","imports","helpers","moduleBindings","geometry","placement"},{"profileIds","imports","helpers","moduleBindings","geometry","placement"});
    Recipe r; r.profileIds = strings(j["profileIds"],p + "/profileIds");
    std::map<std::string,const LibraryRequirement *> libs;
    std::map<std::string,std::pair<const LibraryRequirement *, const LibraryProfile *>> profiles;
    for (const auto& lib : item.libraries) { libs.emplace(lib.id,&lib); for (const auto& pr : lib.profiles) profiles.emplace(pr.id,std::make_pair(&lib,&pr)); }
    std::map<std::string,const LibraryProfile *> selected;
    for (const auto& id : r.profileIds) {
      if (!profiles.count(id) || !selected.emplace(profiles.at(id).first->id,profiles.at(id).second).second) fail(p,"Unknown/repeated library profile");
    }
    std::set<std::string> params; for (const auto& spec : item.parameters) params.insert(spec.id);
    array(j["helpers"],p + "/helpers",128); std::set<std::string> helpers;
    for (size_t i = 0; i < j["helpers"].size(); ++i) {
      auto q = p + "/helpers/" + std::to_string(i); const auto& h = j["helpers"][i];
      fields(h,q,{"id","parameters","body"},{"id","parameters","body"});
      HelperDefinition helper; helper.id = id(h,"id",q); helper.parameters = strings(h["parameters"],q + "/parameters",128);
      if (!helpers.insert(helper.id).second) fail(q,"Duplicate helper ID");
      std::set<std::string> hp;
      for (const auto& key : helper.parameters) if (!params.count(key) || !hp.insert(key).second) fail(q,"Unknown/duplicate helper parameter");
      helper.body = statement(h["body"],q + "/body",hp); r.helpers.push_back(std::move(helper));
    }
    array(j["imports"],p + "/imports",128); std::map<std::string,std::set<std::string>> roots;
    for (const auto& imp : j["imports"]) {
      fields(imp,p + "/imports",{"libraryId","path","kind","scope","helperId"},{"libraryId","path","kind","scope"});
      ImportRequest req; req.libraryId = field(imp,"libraryId",p); req.path = relative(imp,"path",p);
      if (!selected.count(req.libraryId) || !selected.at(req.libraryId)->fileHashes.count(req.path)) fail(p,"Import outside selected profile");
      auto kind = field(imp,"kind",p), scope = field(imp,"scope",p);
      if (kind != "include" && kind != "use") fail(p,"Invalid import kind");
      req.kind = kind == "include" ? ImportRequest::Kind::Include : ImportRequest::Kind::Use;
      if (scope != "file" && scope != "helper") fail(p,"Invalid import scope");
      req.scope = scope == "file" ? ImportRequest::Scope::File : ImportRequest::Scope::Helper;
      if (imp.contains("helperId")) req.helperId = field(imp,"helperId",p);
      if ((req.scope == ImportRequest::Scope::Helper && !helpers.count(req.helperId)) || (req.scope == ImportRequest::Scope::File && !req.helperId.empty())) fail(p,"Invalid import helper");
      roots[req.libraryId].insert(req.path);
      if(req.scope==ImportRequest::Scope::Helper)
        std::find_if(r.helpers.begin(),r.helpers.end(),[&](const auto& h){return h.id==req.helperId;})->localImports.push_back(req);
      r.imports.push_back(std::move(req));
    }
    array(j["moduleBindings"],p + "/moduleBindings",128); std::set<std::string> modules;
    for (const auto& b : j["moduleBindings"]) {
      fields(b,p,{"module","libraryId","path"},{"module","libraryId","path"});
      ModuleBinding binding{field(b,"module",p),field(b,"libraryId",p),relative(b,"path",p)};
      if (!validIdentifier(binding.module) || !modules.insert(binding.module).second || !selected.count(binding.libraryId) || !selected.at(binding.libraryId)->fileHashes.count(binding.path)) fail(p,"Invalid module binding");
      r.moduleBindings.push_back(std::move(binding));
    }
    static const std::set<std::string> builtins{"cube","sphere","cylinder","union","difference","intersection","translate","rotate","scale","mirror","multmatrix","color","hull","minkowski"};
    std::map<std::string,std::set<std::string>> calls;
    std::function<void(const Statement&,const std::string&)> check = [&](const Statement& s,const std::string& owner) {
      if (s.helper) {
        if (!helpers.count(s.module)) fail(p,"Unknown helper call");
        const auto& definition=*std::find_if(r.helpers.begin(),r.helpers.end(),[&](const auto& h){return h.id==s.module;});
        std::set<std::string> supplied;
        for (const auto& [name,value] : s.arguments) supplied.insert(name);
        if (supplied != std::set<std::string>(definition.parameters.begin(),definition.parameters.end()) || !s.children.empty()) fail(p,"Helper call must supply its exact parameters without children");
        calls[owner].insert(s.module);
      }
      else if (!builtins.count(s.module) && !modules.count(s.module)) fail(p,"Library call requires moduleBindings");
      for (const auto& child : s.children) check(child,owner);
    };
    r.geometry = statement(j["geometry"],p + "/geometry",params); check(r.geometry,"");
    for (const auto& h : r.helpers) check(h.body,h.id);
    std::set<std::string> active, done;
    std::function<void(const std::string&)> visit = [&](const std::string& h) {
      if (active.count(h)) fail(p,"Recursive helper calls"); if (done.count(h)) return;
      active.insert(h); for (const auto& child : calls[h]) visit(child); active.erase(h); done.insert(h);
    };
    for (const auto& h : r.helpers) visit(h.id);
    for (const auto& [lib,profile] : selected) {
      auto reachable = roots[lib]; bool changed = true;
      while (changed) { changed = false; for (const auto& edge : profile->importEdges) if (reachable.count(edge.from)) changed |= reachable.insert(edge.to).second; }
      if (reachable.size() != profile->fileHashes.size()) fail(p,"Profile closure is not reachable from recipe imports");
    }
    auto pp = p + "/placement"; const auto& placement = j["placement"];
    fields(placement,pp,{"origin","normal","up","label"},{"origin","normal","up","label"});
    auto frame = [&](const char *key) { std::array<std::shared_ptr<const Expression>,3> out;
      array(placement[key],pp + "/" + key,3); if (placement[key].size() != 3) fail(pp,"Frame requires three coordinates");
      for (size_t i = 0; i < 3; ++i) out[i] = expression(placement[key][i],pp + "/" + key + "/" + std::to_string(i),params); return out;
    };
    r.origin = frame("origin"); r.normal = frame("normal"); r.up = frame("up"); r.frameLabel = field(placement,"label",pp);
    return r;
  }
  ProviderDefinition catalog(const Json& j, const std::string& digest) const {
    fields(j,"",{"schemaVersion","providerId","providerVersion","label","items","libraries"},{"schemaVersion","providerId","providerVersion","label","items","libraries"});
    if (!j["schemaVersion"].is_number_integer() || j["schemaVersion"] != 1) fail("/schemaVersion","Unsupported schema version");
    ProviderDefinition provider; provider.id = id(j,"providerId",""); provider.version = field(j,"providerVersion","");
    if (!std::regex_match(provider.version,std::regex("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"))) fail("/providerVersion","Expected exact three-integer release label");
    provider.label = field(j,"label",""); provider.path = path; provider.digest = digest;
    array(j["libraries"],"/libraries",128); std::vector<LibraryRequirement> libraries; std::set<std::string> lids,pids;
    for (size_t i = 0; i < j["libraries"].size(); ++i) {
      auto lib = library(j["libraries"][i],"/libraries/" + std::to_string(i));
      if (!lids.insert(lib.id).second) fail("/libraries","Duplicate library ID");
      for (const auto& pr : lib.profiles) if (!pids.insert(pr.id).second) fail("/libraries","Duplicate profile ID");
      libraries.push_back(std::move(lib));
    }
    array(j["items"],"/items"); std::set<std::string> ids;
    for (size_t i = 0; i < j["items"].size(); ++i) {
      auto p = "/items/" + std::to_string(i); const auto& v = j["items"][i];
      fields(v,p,{"id","label","category","description","parameters","operation","insertionRole","recipes"},{"id","label","category","description","parameters","operation","recipes"});
      ItemDefinition item; item.id = id(v,"id",p); if (!ids.insert(item.id).second) fail(p,"Duplicate item ID");
      item.qualifiedId = provider.id + ":" + item.id; item.label = field(v,"label",p); item.description = field(v,"description",p);
      item.category = strings(v["category"],p + "/category",32); item.providerId = provider.id; item.providerVersion = provider.version;
      item.catalogPath = path; item.catalogDigest = digest; item.libraries = libraries;
      auto operation = field(v,"operation",p); if (operation != "additive" && operation != "subtractive") fail(p,"Unknown operation");
      item.operation = operation == "additive" ? Operation::Additive : Operation::Subtractive;
      auto role = v.contains("insertionRole") ? field(v,"insertionRole",p) : "geometry";
      if (role != "geometry" && role != "component") fail(p,"Unknown insertion role");
      item.insertionRole = role == "geometry" ? InsertionRole::Geometry : InsertionRole::Component;
      if (item.insertionRole == InsertionRole::Component && item.operation == Operation::Subtractive) fail(p,"Subtractive Component recipe is invalid");
      array(v["parameters"],p + "/parameters",128); std::set<std::string> params;
      for (size_t k = 0; k < v["parameters"].size(); ++k) {
        auto spec = parameter(v["parameters"][k],p + "/parameters/" + std::to_string(k));
        if (!params.insert(spec.id).second) fail(p,"Duplicate parameter"); item.parameters.push_back(std::move(spec));
      }
      auto validated = validateValues(item,{}); if (!validated) fail(p + "/parameters",validated.diagnostic->message);
      array(v["recipes"],p + "/recipes",128); if (v["recipes"].empty()) fail(p,"Item requires a recipe");
      for (size_t k = 0; k < v["recipes"].size(); ++k) {
        auto rp=p + "/recipes/" + std::to_string(k);
        auto parsed=recipe(v["recipes"][k],rp,item);
        auto defaults=validateRecipeValues(parsed,*validated.value);
        if (!defaults) fail(rp,defaults.diagnostic->message);
        item.recipes.push_back(std::move(parsed));
      }
      provider.items.push_back(std::move(item));
    }
    return provider;
  }
};
}
bool validIdentifier(const std::string& v)
{
  static const std::set<std::string> reserved{"module","function","if","else","for","intersection_for","let","each","true","false","undef","include","use"};
  return !reserved.count(v) && std::regex_match(v,std::regex("[$]?[A-Za-z_][A-Za-z0-9_]*"));
}
bool validRelativePath(const std::string& v, bool empty)
{
  if (v.empty()) return empty;
  if (v.front() == '/' || v.back() == '/' || v.find_first_of("\\:\r\n<>\0",0,7) != std::string::npos) return false;
  size_t start = 0;
  while (start < v.size()) { auto end = v.find('/',start); auto segment = v.substr(start,end-start);
    if (segment.empty() || segment == "." || segment == "..") return false;
    if (end == std::string::npos) break; start = end + 1;
  }
  return true;
}
Result<Json> parseJson(const std::string& bytes, const std::string& path)
{
  if (bytes.size() > 4*1024*1024) return {{},Diagnostic{"LimitExceeded",path,"","JSON exceeds 4 MiB"}};
  auto utf8 = normalizedUtf8(bytes,path); if (!utf8) return {{},utf8.diagnostic};
  Sax sax;
  if (!Json::sax_parse(bytes,&sax)) return {{},Diagnostic{"InvalidJson",path,sax.pointer,sax.error}};
  return {std::move(sax.root),{}};
}
Result<ProviderDefinition> loadCatalog(const std::string& bytes, const std::string& path)
{
  auto json = parseJson(bytes,path); if (!json) return {{},json.diagnostic};
  try { return {Reader(path).catalog(*json.value,sha256(bytes)),{}}; }
  catch (const Invalid& e) { return {{},e.diagnostic}; }
  catch (const std::exception& e) { return {{},Diagnostic{"InvalidCatalog",path,"",e.what()}}; }
}
Result<ParameterValues> validateValues(const ItemDefinition& item, const ParameterValues& supplied)
{
  auto fail = [&](const std::string& key,const std::string& message)->Result<ParameterValues> { return {{},Diagnostic{"InvalidParameter",item.catalogPath,"/parameters/" + key,message}}; };
  ParameterValues out;
  for (const auto& s : item.parameters) {
    auto it = supplied.find(s.id); auto value = it == supplied.end() ? s.defaultValue : it->second;
    bool numeric = s.type == ParameterSpec::Type::Number || s.type == ParameterSpec::Type::Integer;
    if (numeric) {
      if (!std::holds_alternative<double>(value) && !std::holds_alternative<int64_t>(value)) return fail(s.id,"Expected numeric value");
      if (s.type == ParameterSpec::Type::Integer && !std::holds_alternative<int64_t>(value)) return fail(s.id,"Expected integer value");
      double n = std::holds_alternative<double>(value) ? std::get<double>(value) : static_cast<double>(std::get<int64_t>(value));
      if (!std::isfinite(n) || (s.min && (s.exclusiveMin ? n <= *s.min : n < *s.min)) || (s.max && n > *s.max)) return fail(s.id,"Numeric value outside finite bounds");
    } else if (s.type == ParameterSpec::Type::Bool) { if (!std::holds_alternative<bool>(value)) return fail(s.id,"Expected boolean"); }
    else {
      if (!std::holds_alternative<std::string>(value)) return fail(s.id,"Expected string");
      const auto& text = std::get<std::string>(value);
      if (text.size() > s.maxLength) return fail(s.id,"String too long");
      for (unsigned char c : text) if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return fail(s.id,"Unsupported string control character");
      if (!normalizedUtf8(text,item.catalogPath)) return fail(s.id,"Invalid UTF-8 string");
      if (s.type == ParameterSpec::Type::Enum && std::find(s.enumValues.begin(),s.enumValues.end(),text) == s.enumValues.end()) return fail(s.id,"Unknown enum value");
    }
    out.emplace(s.id,std::move(value));
  }
  for (const auto& [key,value] : supplied) if (!out.count(key)) return fail(key,"Unknown input parameter");
  return {std::move(out),{}};
}
}
