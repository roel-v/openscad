// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/insertion/InsertionPreviewWorker.h"
#include "core/insertion/InsertionPreviewProtocol.h"
#include "Feature.h"
#include "openscad.h"
#include "core/BuiltinContext.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/Tree.h"
#include "core/customizer/CommentParser.h"
#include "core/customizer/ParameterObject.h"
#include "core/design/DesignCollector.h"
#include "core/provenance/ProvenanceCollector.h"
#include "core/semantic/SemanticCollector.h"
#include "core/anchors/ReferenceScene.h"
#include "core/parsersettings.h"
#include "core/TransformNode.h"
#include "core/ColorNode.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySetUtils.h"
#include "geometry/manifold/ManifoldGeometry.h"
#include "glview/RenderSettings.h"
#include "utils/printutils.h"
#include <QCoreApplication>
#include <QDir>
#include <QtEndian>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <set>
#include <tuple>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace Insertion {
namespace {
namespace fs=std::filesystem;
struct Evaluation {
  std::shared_ptr<EvaluationInputs> inputs;
  std::shared_ptr<SourceProvenance::SyntaxTable> syntax;
  std::unique_ptr<SourceFileCache> cache;
  std::unique_ptr<SourceFile> parsed;
  std::unique_ptr<EvaluationSession> session;
  std::shared_ptr<const Context> builtin;
  std::shared_ptr<const FileContext> file;
  std::shared_ptr<const AbstractNode> root;
  std::shared_ptr<const Semantic::Scene> semantic;
  std::shared_ptr<const SourceProvenance::Result> provenance;
  std::vector<PreviewDiagnostic> messages;
};
thread_local Evaluation *activeEvaluation=nullptr;
std::string parserFilename(const SourceInput& input,const std::string& directory){return input.fileKey.rfind("session:",0)==0?(fs::u8path(directory)/"Untitled.scad").generic_string():input.fileKey;}
uint64_t locationOffset(const SourceInput& source,const Location& location){
  uint64_t offset=0;for(int line=1;line<location.firstLine();++line){auto found=source.utf8.find('\n',offset);if(found==std::string::npos)return source.utf8.size();offset=found+1;}return std::min<uint64_t>(source.utf8.size(),offset+std::max(0,location.firstColumn()-1));
}
void collectMessage(const Message& m,void *){
  if(!activeEvaluation)return;PreviewDiagnostic d;d.code=getGroupName(m.group);d.message=m.msg;
  d.warning=m.group==message_group::Warning || m.group==message_group::Deprecated || m.group==message_group::Font_Warning || m.group==message_group::UI_Warning;
  if(!m.loc.isNone()){auto key=EvaluationInputs::fileKey(m.loc.filePath());auto input=activeEvaluation->inputs->consumed().find(key);
    if(input==activeEvaluation->inputs->consumed().end() && activeEvaluation->inputs->root().fileKey.rfind("session:",0)==0)input=activeEvaluation->inputs->consumed().find(activeEvaluation->inputs->root().fileKey);
    if(input!=activeEvaluation->inputs->consumed().end()){d.fileKey=input->first;d.offset=locationOffset(input->second,m.loc);}}
  activeEvaluation->messages.push_back(std::move(d));if(activeEvaluation->messages.size()>4096)throw LimitError("Diagnostic limit exceeded");
}
struct MessageScope {
  Evaluation *previous=activeEvaluation;OutputHandlerFunc *handler=outputhandler;void *data=outputhandler_data;
  explicit MessageScope(Evaluation& evaluation){activeEvaluation=&evaluation;set_output_handler(collectMessage,nullptr,nullptr);resetSuppressedMessages();}
  ~MessageScope(){activeEvaluation=previous;set_output_handler(handler,nullptr,data);}
};
void applyConfiguration(const NativeConfiguration& config){
  Feature::enable_all(false);for(const auto& feature:config.features)Feature::enable_feature(feature);set_library_path(config.libraryPaths);
  RenderSettings::inst()->backend3D=*renderBackend3DFromString(config.backend);OpenSCAD::hardwarnings=config.hardWarnings;OpenSCAD::parameterCheck=config.parameterCheck;OpenSCAD::rangeCheck=config.rangeCheck;OpenSCAD::traceDepth=config.traceDepth;OpenSCAD::traceUsermoduleParameters=config.traceParameters;
}
void capabilities(EvaluationSession& session){session.capabilityGuard=[](NativeCapability capability){throw EvaluationCapabilityError("Insertion preview requires captured deterministic inputs; native capability "+std::to_string(int(capability))+" is unsupported");};}
std::unique_ptr<Evaluation> evaluate(const PreviewRequest& request,bool candidate){
  auto result=std::make_unique<Evaluation>();auto seeds=request.sources;
  if(candidate){auto& target=seeds.at(request.targetKey);target.utf8=request.candidate;target.fingerprint=request.candidateHash;target.editorId.reset();target.editorRevision.reset();}
  result->inputs=std::make_shared<EvaluationInputs>(seeds.at(request.rootKey),std::vector<SourceInput>{},EvaluationInputs::ReadPolicy::RejectMissing,seeds);
  result->syntax=std::make_shared<SourceProvenance::SyntaxTable>(result->inputs);result->cache=std::make_unique<SourceFileCache>(result->inputs.get());
  NativeEvaluationScope scope(*result->inputs,*result->cache,[](NativeCapability){throw EvaluationCapabilityError("Native parser resource requires captured input support");});MessageScope messages(*result);applyConfiguration(request.configuration);
  SourceProvenance::ParseScope syntax(result->syntax.get());SourceFile *raw=nullptr;const auto filename=parserFilename(result->inputs->root(),request.directory);
  const auto fulltext=result->inputs->root().utf8+"\n\x03\n"+request.configuration.defines;
  bool parsed=false;try{parsed=parse(raw,fulltext,filename,filename,false);}catch(...){delete raw;throw;}result->parsed.reset(raw);if(!parsed || !raw)throw ProtocolError("Candidate native parse failed");raw->handleDependencies();CommentParser::collectParameters(fulltext,raw);
  // Customizer evaluates its schema in a guarded native context, then imports exact captured values.
  {
    EvaluationSession customizer(request.directory);capabilities(customizer);auto builtin=Context::create<BuiltinContext>(&customizer);request.configuration.variables.applyToContext(builtin);builtin->set_variable("$fn",Value(request.configuration.fn));builtin->set_variable("$fa",Value(request.configuration.fa));builtin->set_variable("$fs",Value(request.configuration.fs));
    auto context=Context::create<FileContext>(*builtin,raw);auto parameters=ParameterObjects::fromSourceFile(raw,context.operator->());
    for(const auto& [name,value]:request.configuration.parameters){auto parameter=std::find_if(parameters.begin(),parameters.end(),[&](const auto& p){return p->name()==name;});if(parameter==parameters.end() || !(*parameter)->importValue(value,false))throw ProtocolError("Captured Customizer field is no longer valid: "+name);}
    parameters.importValues(request.configuration.parameters);parameters.apply(raw);
  }
  result->session=std::make_unique<EvaluationSession>(request.directory);capabilities(*result->session);result->session->provenanceCollector=std::make_shared<SourceProvenance::ProvenanceCollector>(result->syntax);
  auto builtin=Context::create<BuiltinContext>(result->session.get());result->builtin=*builtin;request.configuration.variables.applyToContext(builtin);builtin->set_variable("$fn",Value(request.configuration.fn));builtin->set_variable("$fa",Value(request.configuration.fa));builtin->set_variable("$fs",Value(request.configuration.fs));
  AbstractNode::resetIndexCounter();auto root=raw->instantiate(*builtin,&result->file);if(!root)throw ProtocolError("Candidate has no native root");const Location *secondRoot=nullptr;auto tagged=find_root_tag(root,&secondRoot);result->root=tagged?tagged:root;if(secondRoot)throw ProtocolError("Multiple native root modifiers are unsupported");
  result->semantic=result->session->semanticCollector->finish();if(!result->semantic->valid())throw ProtocolError("Candidate semantic hierarchy is invalid");
  auto design=result->session->designCollector->finish();if(!design->diagnostics.empty())throw ProtocolError("Candidate design metadata is invalid");
  if(auto anchors=Anchors::referenceScene(*result->semantic);anchors && (!anchors->relationsValid || !anchors->diagnostics.empty()))throw ProtocolError("Candidate attachment graph is invalid");
  result->provenance=result->session->provenanceCollector->finish(*result->root);result->inputs->freeze();return result;
}
std::shared_ptr<const PolySet> geometryMesh(const std::shared_ptr<const AbstractNode>& node,const std::string& directory,Color4f color={}){
  Tree tree(node,directory);GeometryEvaluator evaluator(tree);auto geometry=evaluator.evaluateGeometry(*node,true);if(!geometry || geometry->isEmpty())return {};
  if(geometry->getDimension()!=3)throw ProtocolError("Insertion preview requires convertible 3D geometry");
#ifdef ENABLE_MANIFOLD
  if(auto manifold=std::dynamic_pointer_cast<const ManifoldGeometry>(geometry);manifold && manifold->getManifold().Status()!=manifold::Manifold::Error::NoError)throw ProtocolError("Native Manifold evaluation failed");
#endif
  if(color.isValid()){auto colored=std::shared_ptr<Geometry>(geometry->copy());colored->setColor(color);geometry=std::move(colored);}
  auto mesh=PolySetUtils::getGeometryAsPolySet(geometry);if(!mesh)throw ProtocolError("Candidate cannot be converted to a mesh");if(mesh->isEmpty())return {};if(!mesh->isTriangular())return PolySetUtils::tessellate_faces(*mesh);return mesh;
}
double volume(const std::shared_ptr<const PolySet>& mesh){if(!mesh)return 0;long double total=0;for(const auto& f:mesh->indices){if(f.size()!=3)throw ProtocolError("Volume requires native triangulation");total+=static_cast<long double>(mesh->vertices[f[0]].dot(mesh->vertices[f[1]].cross(mesh->vertices[f[2]])))/6;}return std::abs(double(total));}
std::shared_ptr<const AbstractNode> sourceNode(const Evaluation& evaluation,const SourceProvenance::Span& span){
  std::shared_ptr<const AbstractNode> found;for(const auto& [id,instance]:evaluation.provenance->instances())if(instance.call.whole.fileKey==span.fileKey && instance.call.whole.begin==span.begin && instance.call.whole.end==span.end){
    for(auto index:instance.emittedNodeIndices){std::deque<std::shared_ptr<const AbstractNode>> path;auto node=evaluation.root->getNodeByID(int(index),path);if(!node)continue;if(found && found!=node)throw ProtocolError("Target statement has repeated native emissions");found=node;}}
  if(!found)throw ProtocolError("Target native subtree is unavailable");return found;
}
std::shared_ptr<const AbstractNode> targetNode(const Evaluation& evaluation,const VolumeCheck& check,bool candidate){
  if(!check.instancePath.empty()){auto found=evaluation.semantic->instances.find(check.instancePath);if(found==evaluation.semantic->instances.end() || !found->second.localRoot)throw ProtocolError("Affected semantic target is unavailable");return found->second.localRoot;}
  return sourceNode(evaluation,candidate?check.candidate:check.base);
}
void appendMesh(InspectionMeshSet& set,const std::shared_ptr<const AbstractNode>& root,const std::string& directory,const Transform3d& transform,InspectionPiece::Role role,Color4f color,size_t& count,std::map<uint64_t,std::string>& paths,const std::string& path){
  auto mesh=geometryMesh(root,directory,color);if(!mesh)return;count+=mesh->indices.size();if(count>maxTriangles)throw LimitError("Candidate exceeds triangle limit");const uint64_t id=set.pieces.size()+1;set.pieces.push_back({id,{},mesh,transform,Color4f(),role});if(!path.empty())paths.emplace(id,path);
}
std::shared_ptr<const InspectionMeshSet> candidateMeshes(const Evaluation& evaluation,const PreviewRequest& request,PreviewResponse& response,size_t& count){
  auto set=std::make_shared<InspectionMeshSet>();set->resultId="insertion:"+std::to_string(request.serial)+":"+request.candidateHash;set->geometryGeneration=request.serial;
  if(evaluation.semantic->marked){auto append=[&](const Semantic::Piece& p){appendMesh(*set,p.root,request.directory,p.localToWorld,p.background?InspectionPiece::Role::Background:p.highlight?InspectionPiece::Role::Highlight:InspectionPiece::Role::Solid,p.inheritedColor,count,response.piecePaths,p.instancePath);};for(const auto& p:evaluation.semantic->parts)append(p);for(const auto& p:evaluation.semantic->residual)append(p);}
  else {
    appendMesh(*set,evaluation.root,request.directory,Transform3d::Identity(),InspectionPiece::Role::Solid,{},count,response.piecePaths,"");
    // Native background/highlight subtrees retain their ancestor transforms and inherited color.
    std::function<void(const std::shared_ptr<const AbstractNode>&,Transform3d,Color4f,bool)> visit;
    visit=[&](const auto& node,Transform3d ancestor,Color4f inherited,bool insideRole){bool role=node->modinst && (node->modinst->isBackground() || node->modinst->isHighlight());if(role && !insideRole)appendMesh(*set,node,request.directory,ancestor,node->modinst->isBackground()?InspectionPiece::Role::Background:InspectionPiece::Role::Highlight,inherited,count,response.piecePaths,"");if(auto transform=dynamic_cast<const TransformNode *>(node.get()))ancestor=ancestor*transform->matrix;if(auto color=dynamic_cast<const ColorNode *>(node.get()))inherited=color->color;for(const auto& child:node->children)visit(child,ancestor,inherited,insideRole || role);};visit(evaluation.root,Transform3d::Identity(),Color4f(),false);
  }return set;
}
uint64_t originalOffset(uint64_t offset,const PreviewRequest& request,bool& generated){
  int64_t delta=0;for(const auto& edit:request.edits){auto begin=int64_t(edit.begin)+delta,end=begin+int64_t(edit.replacement.size());if(int64_t(offset)<begin)break;if(int64_t(offset)<end){generated=true;return edit.begin;}delta+=int64_t(edit.replacement.size())-int64_t(edit.end-edit.begin);}return uint64_t(int64_t(offset)-delta);
}
void warnings(const Evaluation& base,const Evaluation& candidate,const PreviewRequest& request,PreviewResponse& response){
  using Key=std::tuple<std::string,std::string,std::string,uint64_t>;std::multiset<Key> old;for(const auto& d:base.messages)if(d.warning)old.emplace(d.code,d.message,d.fileKey,d.offset);
  for(auto d:candidate.messages){if(d.fileKey==request.targetKey)d.offset=originalOffset(d.offset,request,d.generated);if(d.warning){auto found=old.find({d.code,d.message,d.fileKey,d.offset});if(d.generated || found==old.end())throw ProtocolError("Candidate adds a native warning: "+d.message);old.erase(found);}if(d.code=="ERROR" || d.code=="Parser error")throw ProtocolError("Candidate native error: "+d.message);response.diagnostics.push_back(std::move(d));}
}
void bindings(const Evaluation& evaluation,const PreviewRequest& request,PreviewResponse& response){
  for(const auto& expected:request.expectedBindings){bool found=false;std::set<std::pair<uint64_t,uint64_t>> seen;for(const auto& [id,instance]:evaluation.provenance->instances()){
    const auto& call=instance.call;if(call.callable!=expected.module || call.whole.fileKey!=expected.call.fileKey || call.whole.begin<expected.call.begin || call.whole.end>expected.call.end)continue;const auto actual=instance.definition?instance.definition->whole.fileKey:"";
    if(actual!=expected.fileKey)throw ProtocolError("Generated module resolves to an unexpected definition: "+expected.module);found=true;if(seen.emplace(call.whole.begin,call.whole.end).second)response.resolvedBindings.push_back({expected.module,actual,call.whole});}
    if(!found)throw ProtocolError("Generated module was not evaluated: "+expected.module);
  }
}
void writeFrame(const QCborValue& value){auto bytes=frame(value);if(std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout)!=size_t(bytes.size()) || std::fflush(stdout)!=0)throw ProtocolError("Worker output pipe failed");}
QCborValue readFrame(){unsigned char header[4];if(std::fread(header,1,4,stdin)!=4)throw ProtocolError("Incomplete worker request header");auto count=qFromBigEndian<quint32>(header);if(!count || count>maxFrameBytes)throw LimitError("Worker request frame exceeds limit");QByteArray bytes(reinterpret_cast<const char *>(header),4);bytes.resize(qsizetype(count)+4);if(std::fread(bytes.data()+4,1,count,stdin)!=count)throw ProtocolError("Incomplete worker request");auto value=takeFrame(bytes);if(!value)throw ProtocolError("Incomplete request frame");if(std::fgetc(stdin)!=EOF)throw ProtocolError("Trailing worker request bytes");return *value;}
}
PreviewResponse evaluatePreview(const PreviewRequest& request){
  PreviewResponse response;response.serial=request.serial;response.build=request.build;response.candidateHash=request.candidateHash;response.configurationHash=request.configurationHash;
  try {
    if(request.build!=executableIdentity())throw ProtocolError("Worker executable identity differs from parent");
#ifdef ENABLE_PYTHON
    throw EvaluationCapabilityError("This build includes an external Python evaluator without captured callback support");
#endif
    auto base=evaluate(request,false),candidate=evaluate(request,true);NativeEvaluationScope native(*candidate->inputs,*candidate->cache);MessageScope messages(*candidate);
    for(const auto& path:request.affectedPaths)if(!candidate->semantic->instances.count(path) || !base->semantic->instances.count(path))throw ProtocolError("Affected instance coverage changed");
    bindings(*candidate,request,response);size_t triangles=0;response.meshes=candidateMeshes(*candidate,request,response,triangles);response.empty=response.meshes->pieces.empty();
    for(const auto& check:request.subtractTargets){auto before=volume(geometryMesh(targetNode(*base,check,false),request.directory)),after=volume(geometryMesh(targetNode(*candidate,check,true),request.directory));if(before<=0 || after>=before-std::max(1e-9,before*1e-10))throw ProtocolError("NoIntersection: cutter does not remove volume from the selected positive target");response.volumes.push_back({check.instancePath,before,after});}
    if(!request.itemModule.empty()){auto set=std::make_shared<InspectionMeshSet>();set->resultId="insertion-item:"+std::to_string(request.serial);set->geometryGeneration=request.serial;
      for(const auto& [id,instance]:candidate->provenance->instances())if(instance.call.callable==request.itemModule && instance.definition && instance.definition->whole.fileKey==request.targetKey){for(auto index:instance.emittedNodeIndices){std::deque<std::shared_ptr<const AbstractNode>> path;auto node=candidate->root->getNodeByID(int(index),path);if(node){std::map<uint64_t,std::string> unused;appendMesh(*set,node,request.directory,Transform3d::Identity(),InspectionPiece::Role::Solid,{},triangles,unused,"");break;}}if(!set->pieces.empty())break;}if(set->pieces.empty())throw ProtocolError("Generated item produced no geometry");response.itemMeshes=std::move(set);}
    warnings(*base,*candidate,request,response);for(const auto& [key,input]:candidate->inputs->consumed())response.consumed.emplace(key,input.fingerprint);response.affectedPaths=request.affectedPaths;response.status=PreviewStatus::Success;
  }catch(const MissingCapturedInput& missing){response.status=PreviewStatus::NeedInput;response.missingKey=missing.key;response.diagnostics.push_back({"NeedInput",missing.what(),missing.key});}
  catch(const EvaluationCapabilityError& error){response.status=PreviewStatus::Unsupported;response.diagnostics.push_back({"Unsupported",error.what()});}
  catch(const LimitError& error){response.status=PreviewStatus::LimitExceeded;response.diagnostics.push_back({"LimitExceeded",error.what()});}
  catch(const std::exception& error){response.status=PreviewStatus::Invalid;response.diagnostics.push_back({"Invalid",error.what()});}
  if(response.status!=PreviewStatus::Success){response.meshes.reset();response.itemMeshes.reset();response.piecePaths.clear();}return response;
}
int previewWorkerMain(int argc,char **argv){
  try {
    if(argc!=2)throw ProtocolError("Internal worker accepts no additional arguments");initializeNativeRuntime();initializeNativeEvaluator();
#ifdef _WIN32
    if(_setmode(_fileno(stdin),_O_BINARY)==-1 || _setmode(_fileno(stdout),_O_BINARY)==-1)throw ProtocolError("Cannot open binary worker pipes");
#endif
    QCoreApplication application(argc,argv);writeFrame(handshake());auto request=decodeRequest(readFrame());if(!QDir::setCurrent(QString::fromUtf8(request.directory)))throw ProtocolError("Invalid captured resolution directory");auto response=evaluatePreview(request);writeFrame(encodeResponse(response));return 0;
  }catch(const std::exception& error){std::fprintf(stderr,"Insertion worker: %s\n",error.what());return 2;}
}
}
