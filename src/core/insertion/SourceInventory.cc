// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/insertion/SourceInventory.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/UserModule.h"
#include "core/function.h"
#include "core/ModuleInstantiation.h"
#include "core/ParserInspection.h"
#include "core/EvaluationSession.h"
#include "core/customizer/CommentParser.h"
#include "core/customizer/ParameterObject.h"
#include "core/TransformNode.h"
#include "core/node.h"
#include "core/anchors/AnchorFrames.h"
#include "openscad.h"
#include <algorithm>
#include <functional>

namespace Insertion {
namespace {
template<class T> Result<T> fail(std::string code,std::string message,std::optional<SourceProvenance::Span> span={}){
  return {{},Diagnostic{std::move(code),std::move(message),std::move(span)}};
}
bool contains(const SourceProvenance::Span& outer,const SourceProvenance::Span& inner){
  return outer.fileKey==inner.fileKey && outer.begin<=inner.begin && outer.end>=inner.end;
}
}
size_t SourceInventory::triviaEnd(const std::string& text,size_t at,size_t end){
  end=std::min(end,text.size());if(at==0 && text.compare(0,3,"\xef\xbb\xbf")==0)at=3;
  while(at<end){const auto c=text[at];if(c==' ' || c=='\t' || c=='\r' || c=='\n'){++at;continue;}
    if(at+1<end && text.compare(at,2,"//")==0){auto next=text.find('\n',at+2);at=next==std::string::npos?end:std::min(end,next+1);continue;}
    if(at+1<end && text.compare(at,2,"/*")==0){auto next=text.find("*/",at+2);if(next==std::string::npos || next+2>end)return at;at=next+2;continue;}
    break;
  }return at;
}
size_t SourceInventory::trailingCommentEnd(const std::string& text,size_t at){
  const auto original=at;
  while(at<text.size() && (text[at]==' ' || text[at]=='\t'))++at;
  if(text.compare(at,2,"//")==0){auto next=text.find('\n',at+2);return next==std::string::npos?text.size():next+1;}
  if(text.compare(at,2,"/*")==0){auto next=text.find("*/",at+2);if(next==std::string::npos)return original;at=next+2;
    while(at<text.size() && (text[at]==' ' || text[at]=='\t'))++at;
    if(at<text.size() && text[at]=='\r')++at;if(at<text.size() && text[at]=='\n')return at+1;return next+2;}
  if(at<text.size() && text[at]=='\r')++at;if(at<text.size() && text[at]=='\n')return at+1;return original;
}
std::string SourceInventory::dominantEol(const std::string& text){
  size_t crlf=0,lf=0;for(size_t i=0;i<text.size();++i)if(text[i]=='\n'){if(i && text[i-1]=='\r')++crlf;else ++lf;}return crlf>lf?"\r\n":"\n";
}
const SourceInput& SourceInventory::input(const std::string& key) const {
  auto found=inputs->consumed().find(key);if(found==inputs->consumed().end())throw ProtocolError("Source was not consumed by the displayed root: "+key);return found->second;
}
std::string SourceInventory::reserve(const std::string& purpose,std::set<std::string>& reservations) const {
  if(purpose.empty() || std::any_of(purpose.begin(),purpose.end(),[](unsigned char c){return !(c>='a' && c<='z') && !(c>='0' && c<='9') && c!='_';}))throw ProtocolError("Generated symbol purpose must be ASCII");
  for(uint64_t n=1;n<1000000;++n){auto candidate="smx_insert_"+purpose+"_"+std::to_string(n);if(!names.count(candidate) && reservations.emplace(candidate).second)return candidate;}throw LimitError("Generated identifier namespace is exhausted");
}
Result<SourceInventory> SourceInventory::capture(std::shared_ptr<const SourceFile> parsed,
  std::shared_ptr<const SourceFileCache> libraries,std::shared_ptr<const EvaluationInputs> inputs,
  std::shared_ptr<const AbstractNode> root,std::shared_ptr<const SourceProvenance::Result> provenance,
  std::shared_ptr<const Semantic::Scene> semantic,const std::string& defines){
  if(!parsed || !libraries || !inputs || !root || !provenance)return fail<SourceInventory>("NoProvenance","The displayed root does not retain native source ownership");
  SourceInventory inventory;inventory.parsed=std::move(parsed);inventory.libraries=std::move(libraries);inventory.inputs=std::move(inputs);inventory.root=std::move(root);inventory.provenance=std::move(provenance);inventory.semantic=std::move(semantic);inventory.defines=defines;
  const auto& syntax=inventory.provenance->syntaxTable();for(const auto& [key,site]:syntax.sites())if(!site.callable.empty())inventory.names.emplace(site.callable);
  std::set<const LocalScope *> visited;
  std::function<void(const std::shared_ptr<const LocalScope>&)> scan=[&](const auto& scope){if(!scope || !visited.emplace(scope.get()).second)return;
    for(const auto& assignment:scope->assignmentOccurrences)inventory.names.emplace(assignment->getName());
    for(const auto& [name,module]:scope->getModuleDeclarations()){inventory.names.emplace(name);for(const auto& parameter:module->parameters)inventory.names.emplace(parameter->getName());scan(module->body);}
    for(const auto& [name,function]:scope->getUserFunctions()){inventory.names.emplace(name);for(const auto& parameter:function->parameters)inventory.names.emplace(parameter->getName());}
    for(const auto& call:scope->moduleInstantiations){if(const auto *site=syntax.site(*call)){inventory.calls.emplace(site->syntax,call);inventory.callScopes.emplace(site->syntax,scope);}scan(call->scope);if(auto conditional=dynamic_cast<const IfElseModuleInstantiation *>(call.get()))scan(conditional->getElseScope());}
  };
  scan(inventory.parsed->scope);
  // Use-library ASTs belong to the retained request cache, never the process-global current cache.
  for(const auto& [key,source]:inventory.inputs->consumed())if(key.rfind("session:",0)!=0){auto file=const_cast<SourceFileCache *>(inventory.libraries.get())->lookup(key);if(file)scan(file->scope);}
  if(inventory.semantic)for(const auto& [id,definition]:inventory.semantic->definitions)if(definition.module)scan(definition.module->body);
  return {std::move(inventory),{}};
}
std::optional<Diagnostic> SourceInventory::editable(const std::string& key,const std::map<std::string,EditableDocument>& documents) const {
  if(!inputs->consumed().count(key))return Diagnostic{"UnconsumedSource","This file is not part of the displayed root",{}};
  auto document=documents.find(key);if(document==documents.end() || !document->second.open)return Diagnostic{"OpenSource","Open this source and restart insertion to choose it explicitly",{}};
  if(!document->second.userOwned)return Diagnostic{"VendorSource","Library and vendor sources cannot be insertion destinations",{}};
  if(!document->second.writable)return Diagnostic{"ReadOnlySource","The chosen source document is read-only",{}};
  if(!input(key).editorId || !input(key).editorRevision)return Diagnostic{"UncapturedEditor","The chosen document was not captured as an open editor by this evaluation",{}};
  return {};
}
Result<SourceProvenance::Span> SourceInventory::body(const SourceProvenance::Site& definition) const {
  if(!definition.wholeComplete || !definition.head || definition.whole.fileKey!=definition.head->fileKey)return fail<SourceProvenance::Span>("IncompleteScope","The native declaration has no complete body",definition.whole);
  const auto& bytes=input(definition.whole.fileKey).utf8;auto open=triviaEnd(bytes,definition.head->end,definition.whole.end);
  if(open>=definition.whole.end || bytes[open]!='{' || !definition.whole.end || bytes[definition.whole.end-1]!='}')return fail<SourceProvenance::Span>("UnsupportedScope","Insertion requires an explicit native brace body",definition.whole);
  return {SourceProvenance::Span{definition.whole.fileKey,definition.whole.fingerprint,open+1,definition.whole.end-1},{}};
}
Result<SourceProvenance::Span> SourceInventory::geometryRegion(const LocalScope& scope,const SourceProvenance::Span& enclosing) const {
  const auto& table=provenance->syntaxTable();std::vector<SourceProvenance::Span> geometry;
  for(const auto& call:scope.moduleInstantiations){const auto *site=table.site(*call);if(!site || !site->wholeComplete || !contains(enclosing,site->whole))return fail<SourceProvenance::Span>("CrossSourceGeometry","Direct geometry includes another source or lacks complete native spans",enclosing);
    if(call->isRoot() || call->isBackground() || call->tag_disabled)return fail<SourceProvenance::Span>("UnsupportedModifier","Whole-Part insertion cannot wrap root, background or disabled direct geometry",site->whole);geometry.push_back(site->whole);}
  if(geometry.empty())return fail<SourceProvenance::Span>("EmptyTarget","The definition has no direct geometry to edit",enclosing);
  std::sort(geometry.begin(),geometry.end(),[](const auto& a,const auto& b){return a.begin<b.begin;});
  auto region=geometry.front();region.end=geometry.back().end;const auto& bytes=input(region.fileKey).utf8;
  for(const auto& assignment:scope.assignmentOccurrences)if(auto span=table.span(assignment->location());span && contains(enclosing,*span) && span->begin>=region.begin)return fail<SourceProvenance::Span>("InterleavedDeclaration","Keep all assignments and annotations before the direct geometry run",*span);
  for(const auto& [name,module]:scope.getModuleDeclarations())if(auto site=table.site(*module);site && contains(enclosing,site->whole) && site->whole.begin>=region.begin)return fail<SourceProvenance::Span>("InterleavedDeclaration","Keep helper declarations before the direct geometry run",site->whole);
  for(const auto& [name,function]:scope.getUserFunctions())if(auto span=table.span(function->location());span && contains(enclosing,*span) && span->begin>=region.begin)return fail<SourceProvenance::Span>("InterleavedDeclaration","Keep function declarations before the direct geometry run",*span);
  for(size_t i=1;i<geometry.size();++i)if(geometry[i-1].end>geometry[i].begin || triviaEnd(bytes,geometry[i-1].end,geometry[i].begin)!=geometry[i].begin)return fail<SourceProvenance::Span>("InterleavedSource","Only whitespace and comments may separate direct geometry statements",region);
  region.end=std::min<uint64_t>(enclosing.end,trailingCommentEnd(bytes,region.end));return {region,{}};
}
Result<SourceTarget> SourceInventory::documentTarget(const std::string& key,const std::map<std::string,EditableDocument>& documents) const {
  if(auto error=editable(key,documents))return {{},error};if(key!=inputs->root().fileKey)return fail<SourceTarget>("DocumentScope","A root Component belongs in the displayed root document");
  const auto& source=input(key);SourceTarget target;target.kind=SourceTarget::Kind::DocumentBody;target.region=target.body={key,source.fingerprint,0,source.utf8.size()};target.label="Document root";return {std::move(target),{}};
}
Result<SourceTarget> SourceInventory::definitionTarget(const std::string& id,const std::string& path,bool component,const std::map<std::string,EditableDocument>& documents) const {
  if(!semantic || !semantic->valid())return fail<SourceTarget>("InvalidSemanticModel","Semantic source ownership is unavailable");auto found=semantic->definitions.find(id);
  if(found==semantic->definitions.end() || !found->second.module || !found->second.syntax)return fail<SourceTarget>("MissingDefinition","The selected semantic definition lacks retained native source");
  const auto& definition=found->second;if(component!=(definition.kind==Semantic::DefinitionKind::Assembly))return fail<SourceTarget>("WrongDestination",component?"A Component must be inserted in its containing Assembly or document root":"Geometry must be inserted in its owning Part");
  auto enclosing=body(*definition.syntax);if(!enclosing)return {{},enclosing.diagnostic};if(auto error=editable(enclosing.value->fileKey,documents)){error->source=definition.syntax->whole;return {{},error};}
  SourceTarget target;target.kind=component?SourceTarget::Kind::AssemblyBody:SourceTarget::Kind::PartBody;target.body=*enclosing.value;target.definitionId=id;target.selectedPath=path;target.label=definition.label.empty()?id:definition.label;target.reusableDefinition=true;
  for(const auto& [key,instance]:semantic->instances)if(instance.definitionId==id)target.affectedPaths.push_back(key);
  if(target.affectedPaths.empty() || std::find(target.affectedPaths.begin(),target.affectedPaths.end(),path)==target.affectedPaths.end())return fail<SourceTarget>("UnknownCoverage","The selected definition has no known current invocation coverage");
  target.shared=target.affectedPaths.size()>1;target.localToWorld=semantic->instances.at(path).localToWorld;
  std::string error;if(!Anchors::supportedInverse(target.localToWorld,error))return fail<SourceTarget>("UnsupportedFrame",error);
  if(component)target.region=target.body;else {auto region=geometryRegion(*definition.module->body,target.body);if(!region)return {{},region.diagnostic};target.region=*region.value;}
  return {std::move(target),{}};
}
std::vector<TargetOption> SourceInventory::targets(size_t nodeIndex,bool component,const std::map<std::string,EditableDocument>& documents) const {
  std::vector<TargetOption> options;
  auto append=[&](Result<SourceTarget> result){options.push_back({std::move(result.value),std::move(result.diagnostic)});};
  if(semantic){auto owner=semantic->partOwners.find(nodeIndex);if(owner!=semantic->partOwners.end()){
    const auto part=semantic->instances.find(owner->second);if(part==semantic->instances.end()){options.push_back({{},Diagnostic{"MissingOwner","The picked Part owner is unavailable",{}}});return options;}
    if(!component)append(definitionTarget(part->second.definitionId,part->first,false,documents));
    else if(part->second.parentPath.empty())append(documentTarget(inputs->root().fileKey,documents));
    else {auto parent=semantic->instances.find(part->second.parentPath);if(parent==semantic->instances.end())options.push_back({{},Diagnostic{"MissingAssembly","The containing Assembly is unavailable",{}}});else append(definitionTarget(parent->second.definitionId,parent->first,true,documents));}
    return options;
  }}
  auto mapping=provenance->forNode(nodeIndex);if(mapping.confidence!=SourceProvenance::Confidence::Unique || mapping.candidateInstances.size()!=1){options.push_back({{},Diagnostic{"AmbiguousTarget",mapping.reason,{}}});return options;}
  if(component){append(documentTarget(inputs->root().fileKey,documents));return options;}
  const auto *selected=provenance->instance(mapping.candidateInstances.front());std::set<SourceProvenance::SyntaxKey> offered;
  while(selected){const auto& site=selected->call;
    Result<SourceTarget> result=fail<SourceTarget>("UnsupportedTarget","The source statement is not uniquely editable",site.whole);
    auto call=calls.find(site.syntax);
    size_t count=0;for(const auto& [id,instance]:provenance->instances())if(instance.call.syntax==site.syntax)++count;
    if(!site.wholeComplete || call==calls.end())result=fail<SourceTarget>("IncompleteStatement","This call has no complete retained native source statement",site.whole);
    else if(count!=1 || selected->emittedNodeIndices.size()!=1)result=fail<SourceTarget>("RepeatedStatement","This source statement has repeated native effects; choose an eligible enclosing statement",site.whole);
    else if(call->second->isRoot() || call->second->isBackground() || call->second->tag_disabled)result=fail<SourceTarget>("UnsupportedModifier","Root, background and disabled statements are unavailable as targets",site.whole);
    else if(call->second->name()=="hull" || call->second->name()=="minkowski" || call->second->name()=="render" || call->second->name()=="projection" || call->second->name()=="import")result=fail<SourceTarget>("DerivedTarget","Choose a proven enclosing positive statement for derived geometry",site.whole);
    else if(auto error=editable(site.whole.fileKey,documents))result={{},error};
    else {
      const auto index=selected->emittedNodeIndices.front();bool located=false,valid=true;Transform3d frame=Transform3d::Identity();
      std::function<void(const std::shared_ptr<const AbstractNode>&,Transform3d,int,bool)> visit=[&](const auto& node,Transform3d ancestor,int polarity,bool forbidden){
        forbidden=forbidden || (node->modinst && (node->modinst->isRoot() || node->modinst->isBackground()));
        if(node->index()==index){if(located && !frame.matrix().isApprox(ancestor.matrix(),1e-12))valid=false;located=true;frame=ancestor;if(polarity<=0 || forbidden)valid=false;
          std::function<void(const std::shared_ptr<const AbstractNode>&)> descendants=[&](const auto& n){if(semantic && semantic->partOwners.count(n->index()))valid=false;for(const auto& child:n->children)descendants(child);};descendants(node);}
        if(auto transform=dynamic_cast<const TransformNode *>(node.get()))ancestor=ancestor*transform->matrix;
        for(size_t i=0;i<node->children.size();++i)visit(node->children[i],ancestor,node->name()=="difference" && i>0?-polarity:polarity,forbidden);
      };visit(root,Transform3d::Identity(),1,false);
      std::string frameError;if(!located || !valid)result=fail<SourceTarget>("NonPositiveTarget","Choose a known positive subtree without semantic descendants or ambiguous placement",site.whole);
      else if(!Anchors::supportedInverse(frame,frameError))result=fail<SourceTarget>("UnsupportedFrame",frameError,site.whole);
      else {SourceTarget target;target.kind=SourceTarget::Kind::OrdinaryStatement;target.region=target.body=site.whole;target.region.end=trailingCommentEnd(input(site.whole.fileKey).utf8,site.whole.end);target.statement=site;target.localToWorld=frame;target.label=site.callable;result={std::move(target),{}};}
    }
    if(offered.emplace(site.syntax).second)append(std::move(result));selected=selected->parent?provenance->instance(*selected->parent):nullptr;
  }
  return options;
}
Result<std::vector<ImportSite>> SourceInventory::imports(const std::string& key) const {
  try {
    const auto& source=input(key);auto sealed=EvaluationInputs(inputs->root(),{},EvaluationInputs::ReadPolicy::RejectMissing,inputs->consumed());SourceFileCache inspectionCache(&sealed);
    NativeEvaluationScope native(sealed,inspectionCache,[](NativeCapability){throw EvaluationCapabilityError("Uncaptured parser resource in insertion source inventory");});
    ParserInspection probe;probe.tokenLimit=8*1024*1024;probe.read=[&](const std::string& path){return sealed.read(std::filesystem::u8path(path)).utf8;};probe.alive=[](){return true;};std::vector<ImportSite> result;
    probe.dependency=[&](const auto& kind,const auto& spelling,const auto& resolved,const Location& location){auto span=provenance->syntaxTable().span(location);if(!span)return;bool fileScope=true;
      for(const auto& [id,site]:provenance->syntaxTable().sites())if((site.kind==SourceProvenance::SiteKind::ModuleCall || site.kind==SourceProvenance::SiteKind::ModuleDefinition) && site.wholeComplete && contains(site.whole,*span))fileScope=false;
      result.push_back({kind,spelling,resolved.empty()?"":EvaluationInputs::fileKey(std::filesystem::u8path(resolved)),*span,fileScope});};
    const auto filename=key==inputs->root().fileKey?parsed->getFullpath():key;SourceFile *raw=nullptr;bool success=false;
    try {success=parseInspection(raw,source.utf8+(key==inputs->root().fileKey?"\n\x03\n"+defines:""),filename,probe);}catch(...){delete raw;throw;}std::unique_ptr<SourceFile> owned(raw);
    if(!success)return fail<std::vector<ImportSite>>("ImportInventory","Native source import inspection failed or exceeded its bounded budget");return {std::move(result),{}};
  }catch(const std::exception& error){return fail<std::vector<ImportSite>>("ImportInventory",error.what());}
}
Result<uint64_t> SourceInventory::filePrefixPoint(const std::string& key) const {
  auto captured=imports(key);if(!captured)return {{},captured.diagnostic};const auto& bytes=input(key).utf8;uint64_t point=triviaEnd(bytes,0,bytes.size());
  for(;;){auto found=std::find_if(captured.value->begin(),captured.value->end(),[&](const auto& site){return site.source.fileKey==key && site.fileScope && site.source.begin==point;});if(found==captured.value->end())break;if(found->source.end<=point)return fail<uint64_t>("ImportInventory","Native import has an invalid source extent",found->source);point=triviaEnd(bytes,found->source.end,bytes.size());}
  return {point,{}};
}
Result<uint64_t> SourceInventory::helperPrefixPoint(const std::string& key) const {
  try {
    auto prefix=filePrefixPoint(key);if(!prefix)return prefix;
    const auto& source=input(key);
    auto sealed=EvaluationInputs(inputs->root(),{},EvaluationInputs::ReadPolicy::RejectMissing,inputs->consumed());
    SourceFileCache inspectionCache(&sealed);
    NativeEvaluationScope native(sealed,inspectionCache,[](NativeCapability){throw EvaluationCapabilityError("Uncaptured parser resource in insertion source inventory");});
    ParserInspection probe;probe.tokenLimit=8*1024*1024;probe.read=[&](const std::string& path){return sealed.read(std::filesystem::u8path(path)).utf8;};probe.alive=[](){return true;};
    probe.dependency=[](const auto&,const auto&,const auto&,const auto&){};
    const auto filename=key==inputs->root().fileKey?parsed->getFullpath():key;
    SourceFile *raw=nullptr;bool success=false;
    try {success=parseInspection(raw,source.utf8,filename,probe);}catch(...){delete raw;throw;}
    std::unique_ptr<SourceFile> owned(raw);if(!success || !raw)return fail<uint64_t>("CustomizerInventory","Native parameter inventory failed");
    CommentParser::collectParameters(source.utf8,raw);
    uint64_t point=*prefix.value;
    // Generated braces must follow existing native Customizer fields. Imports stay at the earlier file prefix.
    for(const auto& assignment:raw->scope->assignments){
      if(!ParameterObject::fromAssignment(assignment.get(),filename,nullptr))continue;
      auto span=provenance->syntaxTable().span(assignment->location());
      if(span && span->fileKey==key && span->end<=source.utf8.size())point=std::max<uint64_t>(point,trailingCommentEnd(source.utf8,span->end));
    }
    return {point,{}};
  }catch(const std::exception& error){return fail<uint64_t>("CustomizerInventory",error.what());}
}
}
