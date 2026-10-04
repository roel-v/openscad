#include "core/anchors/AnchorSourceEdits.h"
#include "core/design/DesignKinds.h"
#include "core/semantic/SemanticSourceEdits.h"
#include <sstream>
namespace Anchors {
namespace {
std::map<std::string,Design::LiteralKind> fields(const std::string& kind) {
  using K=Design::LiteralKind;std::map<std::string,K> result{{"label",K::String}};
  if(kind=="Anchor")result.insert({{"adjust.translation",K::Vector3},{"adjust.rotation",K::Quaternion}});
  else if(kind=="Attachment")result.insert({{"mode",K::String},{"adjust.offset",K::Vector3},{"adjust.spin",K::Number}});
  else if(kind!="Point" && kind!="Axis" && kind!="Plane")return {};
  return result;
}
Value vector(const Vector3d& value){VectorType out(nullptr);for(int i=0;i<3;++i)out.emplace_back(value[i]);return Value(std::move(out));}
std::string encode(const Value& value){
  if(auto primitive=Semantic::encodeLiteral(value))return *primitive;
  if(value.type()!=Value::Type::OBJECT)return {};
  std::string result="{";bool first=true;for(const auto& key:value.toObject().keys()){
    auto encoded=encode(value.toObject().get(key));if(encoded.empty())return {};
    if(!first)result+=", ";first=false;
    result+=*Semantic::encodeLiteral(Value(key))+": "+encoded;
  }return result+"}";
}
}
std::shared_ptr<const Design::Record> applyReferenceChanges(const Design::Record& before,const std::vector<LiteralChange>& changes,std::string& error) {
  const auto allowed=fields(before.kind);
  if(allowed.empty() || before.definition.type()!=Value::Type::OBJECT){error="Unsupported reference kind or definition";return {};}
  auto owned=Design::ownedValue(before.definition);auto object=owned.toObject();
  for(const auto& change:changes) {
    if(!allowed.count(change.path) || !change.value){error="Unsupported reference edit field";return {};}
    if(change.path.rfind("adjust.",0)==0) {
      ObjectType adjustment(nullptr);if(object.get("adjust").type()==Value::Type::OBJECT)adjustment=object.get("adjust").toObject().clone();
      adjustment.set(change.path.substr(7),Design::ownedValue(*change.value));object.set("adjust",Value(std::move(adjustment)));
    } else object.set(change.path,Design::ownedValue(*change.value));
  }
  registerKinds();std::vector<Design::Diagnostic> diagnostics;
  auto decoded=Design::makeRecord(*Design::kind(before.kind),Value(std::move(object)),{before.source.value_or(Design::DeclarationKey{}),Design::DeclarationScope::File},diagnostics);
  if(!decoded){error=diagnostics.empty()?"Invalid reference edit":diagnostics.front().message;return {};}
  if(decoded->id!=before.id){error="Reference edit cannot change identity";return {};}
  auto result=std::make_shared<Design::Record>();result->id=decoded->id;result->kind=decoded->kind;result->label=decoded->label;result->source=before.source;
  result->definition=Design::ownedValue(decoded->definition);result->evaluated=decoded->evaluated;return result;
}
std::optional<std::vector<Design::SourcePatch>> planReferenceEdit(const Design::Record& before,const SourceInput& input,const std::vector<LiteralChange>& changes,size_t uses,bool editShared,std::string& error) {
  if(uses>1 && !editShared){error="Use Edit shared declaration before changing its "+std::to_string(uses)+" invocations";return {};}
  if(!before.source){error="Transient reference has no source declaration";return {};}
  if(!applyReferenceChanges(before,changes,error))return {};
  auto map=Design::DesignSourceMap::scan(*before.source,input,before.kind);const auto allowed=fields(before.kind);
  std::vector<Design::FieldPatch> patches;std::set<std::string> paths;
  for(const auto& [path,literal]:allowed)paths.insert(path);
  for(const auto& change:changes)patches.push_back({change.path,encode(*change.value),allowed.at(change.path)});
  return map.patches(patches,error,paths);
}
Value capturedDefinition(std::string id,std::string label,const Anchor& anchor,std::optional<std::string> owner) {
  ObjectType object(nullptr);object.set("id",Value(std::move(id)));object.set("label",Value(std::move(label)));
  object.set("position",vector(anchor.position));object.set("normal",vector(anchor.normal));object.set("up",vector(anchor.up));object.set("capture",Value("fixed"));
  if(owner)object.set("owner",Value(std::move(*owner)));return Value(std::move(object));
}
std::optional<std::string> serializeReference(const Design::Record& record,const std::string& variable) {
  if(record.source || !ObjectType::keyIsIdentifier(variable) || fields(record.kind).empty())return {};
  std::vector<Design::Diagnostic> diagnostics;registerKinds();
  if(!Design::makeRecord(*Design::kind(record.kind),record.definition,{},diagnostics))return {};
  const auto encoded=encode(record.definition);if(encoded.empty())return {};
  return "@{type: "+record.kind+"}\n"+variable+" = "+encoded+";\n";
}
}
