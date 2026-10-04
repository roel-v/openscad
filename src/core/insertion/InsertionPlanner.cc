// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/insertion/InsertionPlanner.h"
#include "core/insertion/InsertionPreviewProtocol.h"
#include "core/insertion/PlacementDraft.h"
#include "core/providers/ScadFragmentWriter.h"
#include "core/providers/ContentDigest.h"
#include "core/anchors/AnchorFrames.h"
#include <algorithm>
#include <cmath>

namespace Insertion {
namespace {
template<class T> Result<T> fail(std::string code,std::string message,std::optional<SourceProvenance::Span> span={}){return {{},Diagnostic{std::move(code),std::move(message),std::move(span)}};}
std::string eol(std::string text,const std::string& newline){if(newline=="\n")return text;std::string result;for(char c:text){if(c=='\n')result+='\r';result+=c;}return result;}
bool utf8Boundary(const std::string& bytes,uint64_t position){return position<=bytes.size() && (position==bytes.size() || (static_cast<unsigned char>(bytes[size_t(position)])&0xc0)!=0x80);}
}
Result<std::vector<PlannedPatch>> InsertionPlanner::coalesce(const SourceInput& source,std::vector<PlannedPatch> patches){
  std::stable_sort(patches.begin(),patches.end(),[](const auto& a,const auto& b){return a.edit.begin<b.edit.begin;});std::vector<PlannedPatch> result;
  for(auto patch:patches){const auto& edit=patch.edit;if(edit.end<edit.begin || !utf8Boundary(source.utf8,edit.begin) || !utf8Boundary(source.utf8,edit.end) || source.utf8.substr(size_t(edit.begin),size_t(edit.end-edit.begin))!=patch.expected)return fail<std::vector<PlannedPatch>>("SourceConflict","A planned patch differs from its captured original bytes");
    if(edit.begin==edit.end && edit.replacement.empty())continue;
    if(!result.empty() && edit.begin==result.back().edit.begin){if(edit.begin!=edit.end || result.back().edit.begin!=result.back().edit.end)return fail<std::vector<PlannedPatch>>("OverlappingPatches","Only insertions at the same original byte offset can be combined");result.back().edit.replacement+=edit.replacement;}
    else {if(!result.empty() && edit.begin<result.back().edit.end)return fail<std::vector<PlannedPatch>>("OverlappingPatches","Planned source ranges overlap");result.push_back(std::move(patch));}
  }return {std::move(result),{}};
}
Result<std::string> InsertionPlanner::apply(const SourceInput& source,const std::vector<PlannedPatch>& patches){
  if(source.fingerprint!=EvaluationInputs::fingerprint(source.utf8))return fail<std::string>("SourceConflict","The captured source hash changed");auto checked=coalesce(source,patches);if(!checked)return {{},checked.diagnostic};auto candidate=source.utf8;
  for(auto it=checked.value->rbegin();it!=checked.value->rend();++it)candidate.replace(size_t(it->edit.begin),size_t(it->edit.end-it->edit.begin),it->edit.replacement);return {std::move(candidate),{}};
}
std::string InsertionPlanner::transformSource(const Transform3d& transform){
  if(!transform.matrix().allFinite() || !transform.matrix().row(3).isApprox(Eigen::RowVector4d(0,0,0,1),1e-12))throw ProtocolError("Placement must be a finite affine transform");
  auto number=[](double value){return Providers::scalarSource(value==0?0.0:value);};auto vec=[&](const Vector3d& value){return "["+number(value.x())+", "+number(value.y())+", "+number(value.z())+"]";};
  // Simplification is exact. A near-canonical rotation retains its full matrix.
  if(transform.linear()==Matrix3d::Identity())return "translate("+vec(transform.translation())+")\n";
  std::string out="multmatrix([";for(size_t row=0;row<4;++row){if(row)out+=", ";out+='[';for(size_t col=0;col<4;++col){if(col)out+=", ";out+=number(transform.matrix()(row,col));}out+=']';}return out+"])\n";
}
Result<std::string> InsertionPlanner::imports(const SourceInventory& inventory,const std::string& key,const Providers::GeneratedFragment& fragment,uint64_t prefixPoint){
  if(fragment.fileImports.empty())return {std::string(),{}};auto captured=inventory.imports(key);if(!captured)return {{},captured.diagnostic};std::string output;std::optional<uint64_t> previous;
  bool missingPrevious=false;
  for(const auto& request:fragment.fileImports){auto library=std::find_if(fragment.binding.libraries.begin(),fragment.binding.libraries.end(),[&](const auto& value){return value.libraryId==request.libraryId;});if(library==fragment.binding.libraries.end())return fail<std::string>("ImportConflict","A required file import has no matched library");
    const auto expected=EvaluationInputs::fileKey(std::filesystem::u8path(library->root)/std::filesystem::u8path(request.path));const auto kind=request.kind==Providers::ImportRequest::Kind::Include?"include":"use";std::vector<const ImportSite *> matches;
    for(const auto& site:*captured.value)if(site.resolved==expected){
      if(site.source.fileKey!=key || !site.fileScope || site.kind!=kind)return fail<std::string>("ImportConflict","A matching library import has an incompatible scope or native include/use kind",site.source);matches.push_back(&site);
    }
    if(matches.size()>1)return fail<std::string>("ImportConflict","The required library import occurs more than once",matches.front()->source);
    if(!matches.empty()){
      // Helpers are emitted at the file prefix. Existing imports after that prefix remain effective declarations,
      // but ordered imports must all be present in their native order to be reused together.
      if(missingPrevious || matches.front()->source.end>prefixPoint || (previous && matches.front()->source.begin<=*previous))return fail<std::string>("ImportConflict","Existing required imports have inconsistent declared order or follow executable source",matches.front()->source);
      previous=matches.front()->source.begin;
    }else {
      missingPrevious=true;
      const auto spelling=(library->importPrefix.empty()?"":library->importPrefix+"/")+request.path;
      output+=std::string(kind)+" <"+spelling+">\n";
    }
  }
  return {std::move(output),{}};
}
Result<EditPlan> InsertionPlanner::geometry(const SourceInventory& inventory,const SourceTarget& target,const Providers::GeneratedFragment& fragment,const Transform3d& placement,bool sharedApproved){
  try {
    if(fragment.insertionRole!=Providers::InsertionRole::Geometry)return fail<EditPlan>("WrongDestination","Component items require a containing Assembly or document destination");
    if(target.kind!=SourceTarget::Kind::PartBody && target.kind!=SourceTarget::Kind::OrdinaryStatement)return fail<EditPlan>("WrongDestination","Geometry insertion requires a Part body or complete positive statement");
    if(target.shared && !sharedApproved)return fail<EditPlan>("SharedScopeRequired","Approve editing the shared definition and its listed current instances before preview");
    const auto& source=inventory.input(target.region.fileKey);if(target.region.fingerprint!=source.fingerprint || target.region.end>source.utf8.size())return fail<EditPlan>("SourceConflict","The selected region no longer matches captured source");
    std::string error;if(!Anchors::supportedInverse(target.localToWorld,error))return fail<EditPlan>("UnsupportedFrame",error);
    const auto newline=SourceInventory::dominantEol(source.utf8);auto prefixPosition=inventory.filePrefixPoint(source.fileKey);if(!prefixPosition)return {{},prefixPosition.diagnostic};const auto prefixPoint=*prefixPosition.value;if(prefixPoint>target.region.begin)return fail<EditPlan>("ImportConflict","The chosen geometry precedes the legal file prefix");auto required=imports(inventory,source.fileKey,fragment,prefixPoint);if(!required)return {{},required.diagnostic};
    std::set<std::string> reservations;auto module=inventory.reserve("item",reservations);
    const auto importsSource=eol(*required.value,newline);auto helperPosition=inventory.helperPrefixPoint(source.fileKey);if(!helperPosition)return {{},helperPosition.diagnostic};const auto helperPoint=*helperPosition.value;if(helperPoint>target.region.begin)return fail<EditPlan>("CustomizerScope","The selected statement precedes existing Customizer fields; choose an enclosing statement after those declarations");
    const auto prefix=eol((helperPoint>0 && source.utf8[size_t(helperPoint)-1]!='\n'?"\n":"")+fragment.provenanceComment+fragment.helperSource+"module "+module+"() {\n"+fragment.geometrySource+"}\n",newline);
    const auto placed=eol("    // Fixed local placement.\n    "+transformSource(placement)+"        "+module+"();\n",newline);
    const bool subtract=fragment.operation==Providers::Operation::Subtractive;
    const auto opening=eol(subtract?"difference() {\n    union() {\n":"union() {\n",newline);
    const bool endsNewline=target.region.end>target.region.begin && source.utf8[size_t(target.region.end)-1]=='\n';
    const auto closing=(endsNewline?"":newline)+(subtract?"    }"+newline:"")+placed+"}"+newline;
    EditPlan plan;plan.target=target;plan.provider=fragment.binding;plan.itemModule=module;plan.sharedApproved=sharedApproved;plan.affectedPaths=target.affectedPaths;
    auto patches=coalesce(source,{{{prefixPoint,prefixPoint,importsSource},""},{{helperPoint,helperPoint,prefix},""},{{target.region.begin,target.region.begin,opening},""},{{target.region.end,target.region.end,closing},""}});if(!patches)return {{},patches.diagnostic};plan.patches=std::move(*patches.value);auto candidate=apply(source,plan.patches);if(!candidate)return {{},candidate.diagnostic};plan.candidate=std::move(*candidate.value);plan.candidateHash=EvaluationInputs::fingerprint(plan.candidate);
    uint64_t prefixBegin=helperPoint+importsSource.size(); // Prefix is first when its offset is coalesced with the wrapper.
    const SourceProvenance::Span generated{source.fileKey,plan.candidateHash,prefixBegin,prefixBegin+prefix.size()};
    for(const auto& binding:fragment.moduleBindings){auto library=std::find_if(fragment.binding.libraries.begin(),fragment.binding.libraries.end(),[&](const auto& lib){return lib.libraryId==binding.libraryId;});if(library==fragment.binding.libraries.end())return fail<EditPlan>("MissingBinding","Generated module lacks library evidence");plan.expectedBindings.push_back({binding.module,EvaluationInputs::fileKey(std::filesystem::u8path(library->root)/std::filesystem::u8path(binding.path)),generated});}
    if(subtract){if(target.kind==SourceTarget::Kind::PartBody){for(const auto& path:target.affectedPaths)plan.subtractTargets.push_back({path,{},{}});}else {
      if(!target.statement)return fail<EditPlan>("IncompleteStatement","Subtraction requires an exact original native statement");
      uint64_t begin=target.region.begin+importsSource.size()+prefix.size();
      SourceProvenance::Span after{source.fileKey,plan.candidateHash,begin,begin+opening.size()+(target.region.end-target.region.begin)+closing.size()-newline.size()};
      plan.subtractTargets.push_back({"",target.statement->whole,after});
    }}return {std::move(plan),{}};
  }catch(const std::exception& error){return fail<EditPlan>("InvalidPlan",error.what());}
}
Result<EditPlan> InsertionPlanner::component(const SourceInventory& inventory,const SourceTarget& target,const Providers::GeneratedFragment& fragment,const PlacementDraft& placement,bool sharedApproved){
  try {
    if(fragment.insertionRole!=Providers::InsertionRole::Component || fragment.operation!=Providers::Operation::Additive)return fail<EditPlan>("WrongDestination","Separate Components require an additive Component recipe");
    if(target.kind!=SourceTarget::Kind::AssemblyBody && target.kind!=SourceTarget::Kind::DocumentBody)return fail<EditPlan>("WrongDestination","A Component belongs in its containing Assembly or the document root");
    if(placement.mode==PlacementDraft::Mode::FixedGeometry)return fail<EditPlan>("WrongPlacement","Component placement requires a fixed Component or named Attachment");
    if(target.shared && !sharedApproved)return fail<EditPlan>("SharedScopeRequired","Approve inserting into every listed Assembly instance before preview");
    const auto scene=inventory.semanticScene();const auto references=scene?Anchors::referenceScene(*scene):nullptr;const auto& source=inventory.input(target.body.fileKey);
    if(target.body.fingerprint!=source.fingerprint || target.body.end>source.utf8.size())return fail<EditPlan>("SourceConflict","The destination no longer matches captured source");
    auto world=placement.worldPlacement(target,fragment,scene);if(!world)return {{},world.diagnostic};
    auto relative=[&](const std::string& path)->std::optional<std::string>{if(target.kind==SourceTarget::Kind::DocumentBody)return path;if(path==target.selectedPath)return std::string();if(path.rfind(target.selectedPath+"/",0)==0)return path.substr(target.selectedPath.size()+1);return {};};
    std::string ownerPath,targetReference;bool relation=placement.mode==PlacementDraft::Mode::NamedAttachment || !placement.fixedOwnerPath.empty();
    if(placement.mode==PlacementDraft::Mode::NamedAttachment){if(!placement.namedTarget || !references || !references->references.count(*placement.namedTarget))return fail<EditPlan>("MissingReference","Choose an existing current Anchor");const auto& key=*placement.namedTarget;
      if(key.owner.role==Anchors::OwnerRole::Document){if(target.kind!=SourceTarget::Kind::DocumentBody || key.owner.id!=scene->documentPath)return fail<EditPlan>("SharedReferenceScope","A shared Assembly requires an Anchor in its own relative scope");ownerPath="";}
      else {auto path=relative(key.owner.id);if(!path)return fail<EditPlan>("SharedReferenceScope","The named Anchor is outside the destination's relative Assembly scope");ownerPath=*path;}targetReference=key.id;
    }else if(!placement.fixedOwnerPath.empty()){auto path=relative(placement.fixedOwnerPath);if(!path)return fail<EditPlan>("SharedReferenceScope","The captured owner is outside the destination's relative Assembly scope");ownerPath=*path;}
    std::set<std::string> symbols;const auto partModule=inventory.reserve("part",symbols),contactVariable=inventory.reserve("contact",symbols);
    std::set<std::string> definitionIds,siblingIds,anchorIds,attachmentIds;
    if(scene){for(const auto& [id,definition]:scene->definitions)definitionIds.emplace(id);for(const auto& [path,instance]:scene->instances)if(instance.parentPath==target.selectedPath || std::find(target.affectedPaths.begin(),target.affectedPaths.end(),instance.parentPath)!=target.affectedPaths.end())siblingIds.emplace(instance.id);}
    if(references){
      std::set<std::string> scopePaths=target.kind==SourceTarget::Kind::DocumentBody?std::set<std::string>{""}:std::set<std::string>(target.affectedPaths.begin(),target.affectedPaths.end());
      std::set<std::string> ownerPaths;
      for(const auto& scope:scopePaths){if(ownerPath.empty())ownerPaths.emplace(scope);else if(ownerPath.front()=='/')ownerPaths.emplace(ownerPath);else ownerPaths.emplace(scope+"/"+ownerPath);}
      for(const auto& [key,record]:references->references){const auto path=key.owner.role==Anchors::OwnerRole::Document?"":key.owner.id;if(ownerPaths.count(path))anchorIds.emplace(key.id);}
      for(const auto& [key,record]:references->attachments){const auto path=key.owner.role==Anchors::OwnerRole::Document?"":key.owner.id;if(scopePaths.count(path))attachmentIds.emplace(key.id);}
    }
    auto reserveId=[](const std::string& purpose,std::set<std::string>& occupied){for(uint64_t n=1;n<1000000;++n){auto id="smx_insert_"+purpose+"_"+std::to_string(n);if(occupied.emplace(id).second)return id;}throw LimitError("Generated semantic namespace is exhausted");};
    const auto partId=reserveId("definition",definitionIds),componentId=reserveId("component",siblingIds);
    const auto string=[](const std::string& value){return Providers::scalarSource(value);};const auto number=[](double value){return Providers::scalarSource(value);};const auto vector=[&](const Vector3d& value){return "["+number(value.x())+", "+number(value.y())+", "+number(value.z())+"]";};const auto frameVector=[&](const std::array<double,3>& value){return vector(Vector3d(value[0],value[1],value[2]));};
    const auto& item=fragment.binding.snapshot->items.at(fragment.binding.qualifiedId);const auto newline=SourceInventory::dominantEol(source.utf8);auto prefixPosition=inventory.filePrefixPoint(source.fileKey);if(!prefixPosition)return {{},prefixPosition.diagnostic};const auto prefixPoint=*prefixPosition.value;auto required=imports(inventory,source.fileKey,fragment,prefixPoint);if(!required)return {{},required.diagnostic};
    const auto importsSource=eol(*required.value,newline);auto helperPosition=inventory.helperPrefixPoint(source.fileKey);if(!helperPosition)return {{},helperPosition.diagnostic};const auto helperPoint=*helperPosition.value;
    std::string prefix=fragment.provenanceComment+fragment.helperSource+"@{type: Part, id: "+string(partId)+", label: "+string(item.label)+"}\nmodule "+partModule+"() {\n    @{type: Anchor}\n    "+contactVariable+" = {id: \"contact\", position: "+frameVector(fragment.frame.origin)+", normal: "+frameVector(fragment.frame.normal)+", up: "+frameVector(fragment.frame.up)+"};\n"+fragment.geometrySource+"}\n";
    if(helperPoint>0 && source.utf8[size_t(helperPoint)-1]!='\n')prefix="\n"+prefix;
    std::string addition="\n";
    if(relation){
      if(placement.mode==PlacementDraft::Mode::FixedComponent){targetReference=reserveId("anchor",anchorIds);const auto variable=inventory.reserve("anchor",symbols);const auto& anchor=placement.fixedTarget;
        addition+="    // Fixed owner-local capture; follows placement, not surface changes.\n    @{type: Anchor}\n    "+variable+" = {id: "+string(targetReference)+", owner: "+string(ownerPath)+", position: "+vector(anchor.position)+", normal: "+vector(anchor.normal)+", up: "+vector(anchor.up)+", capture: \"fixed\"};\n";
      }
      addition+="    @{type: Component, id: "+string(componentId)+"}\n    "+partModule+"();\n";
      const auto attachmentId=reserveId("attachment",attachmentIds),variable=inventory.reserve("attachment",symbols);
      addition+="    @{type: Attachment}\n    "+variable+" = {id: "+string(attachmentId)+", child: {component: "+string(componentId)+", anchor: \"contact\"}, target: {component: "+string(ownerPath)+", anchor: "+string(targetReference)+"}, mode: "+string(placement.flipped?"align":"mate")+", offset: "+vector(placement.offset)+", spin: "+number(placement.spinDegrees)+"};\n";
    }else {addition+="// Fixed document-frame Component placement.\n"+transformSource(*world.value)+"    @{type: Component, id: "+string(componentId)+"}\n    "+partModule+"();\n";}
    prefix=eol(std::move(prefix),newline);addition=eol(std::move(addition),newline);EditPlan plan;plan.target=target;plan.provider=fragment.binding;plan.itemModule=partModule;plan.sharedApproved=sharedApproved;plan.affectedPaths=target.affectedPaths;
    auto patches=coalesce(source,{{{prefixPoint,prefixPoint,importsSource},""},{{helperPoint,helperPoint,prefix},""},{{target.body.end,target.body.end,addition},""}});if(!patches)return {{},patches.diagnostic};plan.patches=std::move(*patches.value);auto candidate=apply(source,plan.patches);if(!candidate)return {{},candidate.diagnostic};plan.candidate=std::move(*candidate.value);plan.candidateHash=EvaluationInputs::fingerprint(plan.candidate);
    const SourceProvenance::Span generated{source.fileKey,plan.candidateHash,helperPoint+importsSource.size(),helperPoint+importsSource.size()+prefix.size()};for(const auto& binding:fragment.moduleBindings){auto library=std::find_if(fragment.binding.libraries.begin(),fragment.binding.libraries.end(),[&](const auto& lib){return lib.libraryId==binding.libraryId;});if(library==fragment.binding.libraries.end())return fail<EditPlan>("MissingBinding","Generated Component module lacks library evidence");plan.expectedBindings.push_back({binding.module,EvaluationInputs::fileKey(std::filesystem::u8path(library->root)/std::filesystem::u8path(binding.path)),generated});}
    return {std::move(plan),{}};
  }catch(const std::exception& error){return fail<EditPlan>("InvalidPlan",error.what());}
}
PreviewRequest InsertionPlanner::preview(const EditPlan& plan,uint64_t serial,const std::string& build,const std::string& directory,const NativeConfiguration& configuration,const EvaluationInputs& inputs){
  PreviewRequest request;request.serial=serial;request.build=build;request.rootKey=inputs.root().fileKey;request.directory=directory;request.targetKey=plan.target.region.fileKey;request.candidate=plan.candidate;request.candidateHash=plan.candidateHash;request.configuration=configuration;request.configurationHash=configurationHash(configuration);request.sources=inputs.consumed();
  for(const auto& patch:plan.patches)request.edits.push_back(patch.edit);request.expectedBindings=plan.expectedBindings;request.subtractTargets=plan.subtractTargets;request.affectedPaths=plan.affectedPaths;request.itemModule=plan.itemModule;
  for(const auto& library:plan.provider.libraries)for(const auto& file:library.files){auto found=request.sources.find(file.canonicalPath);auto bytes=found==request.sources.end()?EvaluationInputs::diskBytes(std::filesystem::u8path(file.canonicalPath)):found->second.utf8;auto normalized=Providers::normalizedUtf8(bytes,file.canonicalPath);if(!normalized || Providers::sha256(*normalized.value)!=file.normalizedHash)throw ProtocolError("A provider library differs from its matched captured profile");if(found==request.sources.end())request.sources.emplace(file.canonicalPath,SourceInput{file.canonicalPath,bytes,EvaluationInputs::fingerprint(bytes),{}, {}});}
  return decodeRequest(encodeRequest(request));
}
}
