// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/insertion/InsertionPreviewProtocol.h"
#include "Feature.h"
#include "openscad.h"
#include "core/Context.h"
#include "core/Value.h"
#include "core/parsersettings.h"
#include "glview/RenderSettings.h"
#include "utils/printutils.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QCborArray>
#include <QCborStreamReader>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace Insertion {
namespace {
QString qs(const std::string& value){return QString::fromUtf8(value.data(),qsizetype(value.size()));}
std::string ss(const QString& value){return value.toUtf8().toStdString();}
QCborMap map(const QCborValue& value,std::initializer_list<const char *> fields){
  if(!value.isMap())throw ProtocolError("Expected a CBOR map");auto result=value.toMap();std::set<QString> allowed,seen;
  for(auto name:fields)allowed.emplace(QString::fromLatin1(name));
  for(auto it=result.begin();it!=result.end();++it)if(!it.key().isString() || !allowed.count(it.key().toString()) || !seen.emplace(it.key().toString()).second)throw ProtocolError("Unknown or duplicate field");
  if(seen!=allowed)throw ProtocolError("Missing required field");return result;
}
QCborValue get(const QCborMap& value,const char *key){return value.value(QString::fromLatin1(key));}
std::string str(const QCborValue& value){if(!value.isString() || value.toString().size()>1024*1024)throw ProtocolError("Invalid string");return ss(value.toString());}
uint64_t integer(const QCborValue& value){if(!value.isInteger() || value.toInteger()<0)throw ProtocolError("Invalid nonnegative integer");return uint64_t(value.toInteger());}
double number(const QCborValue& value){if(!value.isDouble() && !value.isInteger())throw ProtocolError("Invalid number");double d=value.toDouble();if(!std::isfinite(d))throw ProtocolError("Nonfinite number");return d;}
bool boolean(const QCborValue& value){if(!value.isBool())throw ProtocolError("Invalid boolean");return value.toBool();}
QCborArray array(const QCborValue& value,size_t cap=4096){if(!value.isArray() || uint64_t(value.toArray().size())>cap)throw LimitError("Array limit exceeded");return value.toArray();}
qint64 signedInteger(uint64_t value){if(value>uint64_t(std::numeric_limits<qint64>::max()))throw ProtocolError("Integer exceeds protocol range");return qint64(value);}
QCborArray strings(const std::vector<std::string>& values){QCborArray out;for(const auto& value:values)out.append(qs(value));return out;}
std::vector<std::string> strings(const QCborValue& value){std::vector<std::string> out;for(const auto& item:array(value))out.push_back(str(item));return out;}
QCborArray vector(const Vector3d& value){QCborArray out;for(size_t i=0;i<3;++i){if(!std::isfinite(value[i]))throw ProtocolError("Nonfinite vector");out.append(value[i]);}return out;}
Vector3d vector(const QCborValue& value){auto a=array(value,3);if(a.size()!=3)throw ProtocolError("Expected three coordinates");return {number(a[0]),number(a[1]),number(a[2])};}
QCborMap span(const SourceProvenance::Span& s){return {{QStringLiteral("file"),qs(s.fileKey)},{QStringLiteral("hash"),qs(s.fingerprint)},{QStringLiteral("begin"),signedInteger(s.begin)},{QStringLiteral("end"),signedInteger(s.end)}};}
SourceProvenance::Span span(const QCborValue& value){auto m=map(value,{"file","hash","begin","end"});SourceProvenance::Span s{str(get(m,"file")),str(get(m,"hash")),integer(get(m,"begin")),integer(get(m,"end"))};if(s.end<s.begin)throw ProtocolError("Reversed source range");return s;}
QCborMap binding(const GeneratedBinding& b){return {{QStringLiteral("module"),qs(b.module)},{QStringLiteral("file"),qs(b.fileKey)},{QStringLiteral("call"),span(b.call)}};}
GeneratedBinding binding(const QCborValue& value){auto m=map(value,{"module","file","call"});return {str(get(m,"module")),str(get(m,"file")),span(get(m,"call"))};}
QCborMap tree(const boost::property_tree::ptree& value,unsigned depth=0){
  if(depth>64 || value.size()>4096)throw LimitError("Parameter tree limit exceeded");QCborArray children;
  for(const auto& child:value)children.append(QCborMap{{QStringLiteral("key"),qs(child.first)},{QStringLiteral("value"),tree(child.second,depth+1)}});
  return {{QStringLiteral("data"),qs(value.data())},{QStringLiteral("children"),children}};
}
boost::property_tree::ptree tree(const QCborValue& value,unsigned depth=0){
  if(depth>64)throw LimitError("Parameter tree depth exceeded");auto m=map(value,{"data","children"});boost::property_tree::ptree result;result.data()=str(get(m,"data"));
  for(const auto& child:array(get(m,"children"))){auto c=map(child,{"key","value"});result.push_back({str(get(c,"key")),tree(get(c,"value"),depth+1)});}return result;
}
QCborArray color(const Color4f& c){auto v=c.toVector4f();QCborArray out;for(size_t i=0;i<4;++i){if(!std::isfinite(v[i]))throw ProtocolError("Nonfinite color");out.append(double(v[i]));}return out;}
Color4f color(const QCborValue& value){auto a=array(value,4);if(a.size()!=4)throw ProtocolError("Invalid color");Vector4f v;for(size_t i=0;i<4;++i){auto n=number(a[i]);if(n < -1 || n>1)throw ProtocolError("Color outside supported range");v[i]=float(n);}return Color4f(v);}
QCborValue meshes(const std::shared_ptr<const InspectionMeshSet>& set,size_t& triangleCount){
  if(!set)return QCborValue(nullptr);QCborArray pieces;
  for(const auto& p:set->pieces){if(!p.mesh)throw ProtocolError("Missing mesh");auto mesh=p.mesh;QCborArray vertices,faces,colors,indices,matrix;
    for(const auto& v:mesh->vertices)vertices.append(vector(v));
    for(const auto& f:mesh->indices){if(f.size()!=3)throw ProtocolError("Only triangulated payloads are supported");QCborArray face;for(auto i:f){if(i<0 || size_t(i)>=mesh->vertices.size())throw ProtocolError("Invalid vertex index");face.append(i);}faces.append(face);}
    triangleCount+=mesh->indices.size();if(triangleCount>maxTriangles)throw LimitError("Triangle limit exceeded");
    for(const auto& c:mesh->colors)colors.append(color(c));for(auto i:mesh->color_indices)indices.append(i);
    for(size_t row=0;row<4;++row)for(size_t col=0;col<4;++col){auto n=p.localToWorld.matrix()(row,col);if(!std::isfinite(n))throw ProtocolError("Nonfinite mesh transform");matrix.append(n);}
    pieces.append(QCborMap{{QStringLiteral("id"),signedInteger(p.drawId)},{QStringLiteral("vertices"),vertices},{QStringLiteral("triangles"),faces},{QStringLiteral("colors"),colors},{QStringLiteral("colorIndices"),indices},{QStringLiteral("transform"),matrix},{QStringLiteral("color"),color(p.color)},{QStringLiteral("role"),int(p.role)}});
  }
  return QCborMap{{QStringLiteral("identity"),qs(set->resultId)},{QStringLiteral("serial"),signedInteger(set->geometryGeneration)},{QStringLiteral("pieces"),pieces}};
}
std::shared_ptr<const InspectionMeshSet> meshes(const QCborValue& value,size_t& triangleCount){
  if(value.isNull())return {};auto m=map(value,{"identity","serial","pieces"});auto set=std::make_shared<InspectionMeshSet>();set->resultId=str(get(m,"identity"));set->geometryGeneration=integer(get(m,"serial"));std::set<uint64_t> ids;
  for(const auto& item:array(get(m,"pieces"))){auto p=map(item,{"id","vertices","triangles","colors","colorIndices","transform","color","role"});InspectionPiece piece;piece.drawId=integer(get(p,"id"));if(!piece.drawId || !ids.emplace(piece.drawId).second)throw ProtocolError("Duplicate mesh identity");
    auto mesh=std::make_shared<PolySet>(3);for(const auto& v:array(get(p,"vertices"),maxTriangles*3))mesh->vertices.push_back(vector(v));
    for(const auto& f:array(get(p,"triangles"),maxTriangles)){auto face=array(f,3);if(face.size()!=3)throw ProtocolError("Expected triangle");IndexedFace indices;for(const auto& i:face){auto index=integer(i);if(index>=mesh->vertices.size())throw ProtocolError("Out-of-range mesh index");indices.push_back(int(index));}mesh->indices.push_back(std::move(indices));}
    triangleCount+=mesh->indices.size();if(triangleCount>maxTriangles)throw LimitError("Triangle limit exceeded");
    for(const auto& c:array(get(p,"colors"),maxTriangles))mesh->colors.push_back(color(c));
    for(const auto& c:array(get(p,"colorIndices"),maxTriangles)){if(!c.isInteger() || c.toInteger() < -1 || c.toInteger()>=qint64(mesh->colors.size()))throw ProtocolError("Invalid face color index");mesh->color_indices.push_back(int32_t(c.toInteger()));}
    if(!mesh->color_indices.empty() && mesh->color_indices.size()!=mesh->indices.size())throw ProtocolError("Face color count mismatch");mesh->setTriangular(true);
    auto matrix=array(get(p,"transform"),16);if(matrix.size()!=16)throw ProtocolError("Invalid transform");for(size_t r=0;r<4;++r)for(size_t c=0;c<4;++c)piece.localToWorld.matrix()(r,c)=number(matrix[r*4+c]);
    if((piece.localToWorld.matrix().row(3)-Eigen::RowVector4d(0,0,0,1)).norm()>1e-12)throw ProtocolError("Nonaffine transform");piece.mesh=std::move(mesh);piece.color=color(get(p,"color"));auto role=integer(get(p,"role"));if(role>2)throw ProtocolError("Unknown geometry role");piece.role=InspectionPiece::Role(role);set->pieces.push_back(std::move(piece));
  }return set;
}
}
NativeConfiguration captureConfiguration(const Context& context,ParameterSet parameters){
  NativeConfiguration c;c.parameters=std::move(parameters);c.libraryPaths=get_library_path();c.defines=commandline_commands;c.backend=renderBackend3DToString(RenderSettings::inst()->backend3D);
  auto lookup=[&](const char *name)->const Value&{auto value=context.lookup_local_variable(name);if(!value)throw ProtocolError(std::string("Missing retained builtin ")+name);return *value;};
  c.variables.preview=lookup("$preview").toBool();c.variables.time=lookup("$t").toDouble();Vector3d vpt,vpr;
  for(size_t i=0;i<3;++i){vpt[i]=lookup("$vpt").toVector()[i].toDouble();vpr[i]=lookup("$vpr").toVector()[i].toDouble();}
  c.variables.camera.setVpt(vpt.x(),vpt.y(),vpt.z());c.variables.camera.setVpr(vpr.x(),vpr.y(),vpr.z());c.variables.cameraVariableRotation=vpr;c.variables.camera.setVpd(lookup("$vpd").toDouble());c.variables.camera.setVpf(lookup("$vpf").toDouble());
  c.fn=lookup("$fn").toDouble();c.fa=lookup("$fa").toDouble();c.fs=lookup("$fs").toDouble();for(auto feature:std::vector<Feature *>(Feature::begin(),Feature::end()))if(feature->is_enabled())c.features.push_back(feature->get_name());
  c.hardWarnings=OpenSCAD::hardwarnings;c.parameterCheck=OpenSCAD::parameterCheck;c.rangeCheck=OpenSCAD::rangeCheck;c.traceDepth=OpenSCAD::traceDepth;c.traceParameters=OpenSCAD::traceUsermoduleParameters;return c;
}
QCborMap encodeConfiguration(const NativeConfiguration& c){
  QCborArray params;for(const auto& [name,value]:c.parameters)params.append(QCborMap{{QStringLiteral("name"),qs(name)},{QStringLiteral("value"),tree(value)}});
  return {{QStringLiteral("preview"),c.variables.preview},{QStringLiteral("time"),c.variables.time},{QStringLiteral("vpt"),vector(c.variables.camera.getVpt())},{QStringLiteral("vpr"),vector(c.variables.cameraVariableRotation.value_or(c.variables.camera.getVpr()))},{QStringLiteral("vpd"),c.variables.camera.zoomValue()},{QStringLiteral("vpf"),c.variables.camera.fovValue()},{QStringLiteral("fn"),c.fn},{QStringLiteral("fa"),c.fa},{QStringLiteral("fs"),c.fs},{QStringLiteral("features"),strings(c.features)},{QStringLiteral("libraries"),strings(c.libraryPaths)},{QStringLiteral("defines"),qs(c.defines)},{QStringLiteral("backend"),qs(c.backend)},{QStringLiteral("parameters"),params},{QStringLiteral("hardWarnings"),c.hardWarnings},{QStringLiteral("parameterCheck"),c.parameterCheck},{QStringLiteral("rangeCheck"),c.rangeCheck},{QStringLiteral("traceDepth"),c.traceDepth},{QStringLiteral("traceParameters"),c.traceParameters}};
}
NativeConfiguration decodeConfiguration(const QCborValue& value){
  auto m=map(value,{"preview","time","vpt","vpr","vpd","vpf","fn","fa","fs","features","libraries","defines","backend","parameters","hardWarnings","parameterCheck","rangeCheck","traceDepth","traceParameters"});NativeConfiguration c;c.variables.preview=boolean(get(m,"preview"));c.variables.time=number(get(m,"time"));auto vpt=vector(get(m,"vpt")),vpr=vector(get(m,"vpr"));c.variables.camera.setVpt(vpt.x(),vpt.y(),vpt.z());c.variables.camera.setVpr(vpr.x(),vpr.y(),vpr.z());c.variables.camera.setVpd(number(get(m,"vpd")));c.variables.camera.setVpf(number(get(m,"vpf")));c.fn=number(get(m,"fn"));c.fa=number(get(m,"fa"));c.fs=number(get(m,"fs"));
  c.variables.cameraVariableRotation=vpr;
  if(c.fn<0 || c.fa<=0 || c.fs<=0 || c.variables.camera.zoomValue()<=0 || c.variables.camera.fovValue()<=0 || c.variables.camera.fovValue()>=180)throw ProtocolError("Invalid native configuration range");
  c.features=strings(get(m,"features"));c.libraryPaths=strings(get(m,"libraries"));c.defines=str(get(m,"defines"));c.backend=str(get(m,"backend"));if(!renderBackend3DFromString(c.backend))throw ProtocolError("Unknown backend");
  for(const auto& parameter:array(get(m,"parameters"))){auto p=map(parameter,{"name","value"});if(!c.parameters.emplace(str(get(p,"name")),tree(get(p,"value"))).second)throw ProtocolError("Duplicate Customizer parameter");}
  c.hardWarnings=boolean(get(m,"hardWarnings"));c.parameterCheck=boolean(get(m,"parameterCheck"));c.rangeCheck=boolean(get(m,"rangeCheck"));auto depth=integer(get(m,"traceDepth"));if(depth>1000)throw ProtocolError("Trace depth limit");c.traceDepth=int(depth);c.traceParameters=boolean(get(m,"traceParameters"));
  std::set<std::string> known,seen;for(auto f:std::vector<Feature *>(Feature::begin(),Feature::end()))known.emplace(f->get_name());for(const auto& name:c.features)if(!known.count(name) || !seen.emplace(name).second)throw ProtocolError("Unknown or duplicate native feature");return c;
}
std::string configurationHash(const NativeConfiguration& c){return QCryptographicHash::hash(QCborValue(encodeConfiguration(c)).toCbor(),QCryptographicHash::Sha256).toHex().toStdString();}
std::string executableIdentity(){QFile file(QCoreApplication::applicationFilePath());if(!file.open(QIODevice::ReadOnly))throw ProtocolError("Cannot identify executable");QCryptographicHash hash(QCryptographicHash::Sha256);if(!hash.addData(&file))throw ProtocolError("Cannot hash executable");return hash.result().toHex().toStdString();}
std::vector<std::string> compiledFeatures(){std::vector<std::string> out;
#ifdef ENABLE_MANIFOLD
  out.emplace_back("manifold");
#endif
#ifdef ENABLE_CGAL
  out.emplace_back("cgal");
#endif
#ifdef ENABLE_PYTHON
  out.emplace_back("python");
#endif
  for(auto f:std::vector<Feature *>(Feature::begin(),Feature::end()))out.push_back("native:"+f->get_name());return out;
}
QCborMap handshake(){return {{QStringLiteral("version"),protocolVersion},{QStringLiteral("type"),QStringLiteral("hello")},{QStringLiteral("build"),qs(executableIdentity())},{QStringLiteral("features"),strings(compiledFeatures())}};}
void validateHandshake(const QCborValue& value,const std::string& expected){auto m=map(value,{"version","type","build","features"});if(integer(get(m,"version"))!=protocolVersion || str(get(m,"type"))!="hello" || str(get(m,"build"))!=expected || strings(get(m,"features"))!=compiledFeatures())throw ProtocolError("Worker build or feature identity mismatch");}
QCborMap encodeRequest(const PreviewRequest& r){
  QCborArray inputs,edits,bindings,checks;for(const auto& [key,s]:r.sources)inputs.append(QCborMap{{QStringLiteral("file"),qs(key)},{QStringLiteral("bytes"),QByteArray::fromStdString(s.utf8)},{QStringLiteral("hash"),qs(s.fingerprint)},{QStringLiteral("editor"),s.editorId?QCborValue(qs(*s.editorId)):QCborValue(nullptr)},{QStringLiteral("revision"),s.editorRevision?QCborValue(signedInteger(*s.editorRevision)):QCborValue(nullptr)}});
  for(const auto& e:r.edits)edits.append(QCborMap{{QStringLiteral("begin"),signedInteger(e.begin)},{QStringLiteral("end"),signedInteger(e.end)},{QStringLiteral("bytes"),QByteArray::fromStdString(e.replacement)}});for(const auto& b:r.expectedBindings)bindings.append(binding(b));for(const auto& c:r.subtractTargets)checks.append(QCborMap{{QStringLiteral("path"),qs(c.instancePath)},{QStringLiteral("base"),span(c.base)},{QStringLiteral("candidate"),span(c.candidate)}});
  return {{QStringLiteral("version"),protocolVersion},{QStringLiteral("type"),QStringLiteral("request")},{QStringLiteral("serial"),signedInteger(r.serial)},{QStringLiteral("build"),qs(r.build)},{QStringLiteral("root"),qs(r.rootKey)},{QStringLiteral("directory"),qs(r.directory)},{QStringLiteral("target"),qs(r.targetKey)},{QStringLiteral("candidate"),QByteArray::fromStdString(r.candidate)},{QStringLiteral("candidateHash"),qs(r.candidateHash)},{QStringLiteral("configuration"),encodeConfiguration(r.configuration)},{QStringLiteral("configurationHash"),qs(r.configurationHash)},{QStringLiteral("sources"),inputs},{QStringLiteral("edits"),edits},{QStringLiteral("bindings"),bindings},{QStringLiteral("subtractTargets"),checks},{QStringLiteral("affectedPaths"),strings(r.affectedPaths)},{QStringLiteral("itemModule"),qs(r.itemModule)}};
}
PreviewRequest decodeRequest(const QCborValue& value){
  auto m=map(value,{"version","type","serial","build","root","directory","target","candidate","candidateHash","configuration","configurationHash","sources","edits","bindings","subtractTargets","affectedPaths","itemModule"});if(integer(get(m,"version"))!=protocolVersion || str(get(m,"type"))!="request")throw ProtocolError("Unsupported protocol");PreviewRequest r;r.serial=integer(get(m,"serial"));if(!r.serial)throw ProtocolError("Zero draft serial");r.build=str(get(m,"build"));r.rootKey=str(get(m,"root"));r.directory=str(get(m,"directory"));r.targetKey=str(get(m,"target"));if(!get(m,"candidate").isByteArray())throw ProtocolError("Candidate must be bytes");r.candidate=get(m,"candidate").toByteArray().toStdString();r.candidateHash=str(get(m,"candidateHash"));r.configuration=decodeConfiguration(get(m,"configuration"));r.configurationHash=str(get(m,"configurationHash"));
  uint64_t bytes=r.candidate.size();for(const auto& input:array(get(m,"sources"),maxSourceFiles)){auto s=map(input,{"file","bytes","hash","editor","revision"});SourceInput source;source.fileKey=str(get(s,"file"));if(!get(s,"bytes").isByteArray())throw ProtocolError("Source must be bytes");source.utf8=get(s,"bytes").toByteArray().toStdString();bytes+=source.utf8.size();if(bytes>maxSourceBytes)throw LimitError("Source closure exceeds limit");source.fingerprint=str(get(s,"hash"));if(!get(s,"editor").isNull())source.editorId=str(get(s,"editor"));if(!get(s,"revision").isNull())source.editorRevision=integer(get(s,"revision"));if(bool(source.editorId)!=bool(source.editorRevision))throw ProtocolError("Incomplete editor identity");if(source.fingerprint!=EvaluationInputs::fingerprint(source.utf8) || !r.sources.emplace(source.fileKey,source).second)throw ProtocolError("Conflicting source identity");}
  if(!r.sources.count(r.rootKey) || !r.sources.count(r.targetKey) || r.candidateHash!=EvaluationInputs::fingerprint(r.candidate) || r.configurationHash!=configurationHash(r.configuration))throw ProtocolError("Request identity mismatch");
  // Reuse the owner's canonical-key, UTF-8 and seed-conflict validation.
  EvaluationInputs validated(r.sources.at(r.rootKey),{},EvaluationInputs::ReadPolicy::RejectMissing,r.sources);
  EvaluationInputs candidate({r.targetKey,r.candidate,r.candidateHash,{},{}},{},EvaluationInputs::ReadPolicy::RejectMissing);
  uint64_t previousEnd=0,previousBegin=0;bool firstEdit=true;
  for(const auto& edit:array(get(m,"edits"))){auto e=map(edit,{"begin","end","bytes"});if(!get(e,"bytes").isByteArray())throw ProtocolError("Edit must be bytes");ByteEdit b{integer(get(e,"begin")),integer(get(e,"end")),get(e,"bytes").toByteArray().toStdString()};if(b.end<b.begin || b.end>r.sources.at(r.targetKey).utf8.size() || (!firstEdit && (b.begin<previousEnd || b.begin==previousBegin)))throw ProtocolError("Invalid or overlapping original edit range");previousBegin=b.begin;previousEnd=b.end;firstEdit=false;r.edits.push_back(std::move(b));}
  auto reconstructed=r.sources.at(r.targetKey).utf8;for(auto it=r.edits.rbegin();it!=r.edits.rend();++it)reconstructed.replace(size_t(it->begin),size_t(it->end-it->begin),it->replacement);
  if(reconstructed!=r.candidate)throw ProtocolError("Candidate does not match its original-byte edit plan");
  for(const auto& b:array(get(m,"bindings")))r.expectedBindings.push_back(binding(b));for(const auto& check:array(get(m,"subtractTargets"))){auto c=map(check,{"path","base","candidate"});r.subtractTargets.push_back({str(get(c,"path")),span(get(c,"base")),span(get(c,"candidate"))});}r.affectedPaths=strings(get(m,"affectedPaths"));r.itemModule=str(get(m,"itemModule"));return r;
}
QCborMap encodeResponse(const PreviewResponse& r){
  QCborArray diagnostics,consumed,bindings,volumes,paths;for(const auto& d:r.diagnostics)diagnostics.append(QCborMap{{QStringLiteral("code"),qs(d.code)},{QStringLiteral("message"),qs(d.message)},{QStringLiteral("file"),qs(d.fileKey)},{QStringLiteral("offset"),signedInteger(d.offset)},{QStringLiteral("warning"),d.warning},{QStringLiteral("generated"),d.generated}});for(const auto& [key,hash]:r.consumed)consumed.append(QCborMap{{QStringLiteral("file"),qs(key)},{QStringLiteral("hash"),qs(hash)}});for(const auto& b:r.resolvedBindings)bindings.append(binding(b));for(const auto& v:r.volumes)volumes.append(QCborMap{{QStringLiteral("path"),qs(v.path)},{QStringLiteral("before"),v.before},{QStringLiteral("after"),v.after}});for(const auto& [id,path]:r.piecePaths)paths.append(QCborMap{{QStringLiteral("id"),signedInteger(id)},{QStringLiteral("path"),qs(path)}});size_t triangles=0;auto geometry=meshes(r.meshes,triangles),item=meshes(r.itemMeshes,triangles);
  return {{QStringLiteral("version"),protocolVersion},{QStringLiteral("type"),QStringLiteral("response")},{QStringLiteral("serial"),signedInteger(r.serial)},{QStringLiteral("status"),int(r.status)},{QStringLiteral("build"),qs(r.build)},{QStringLiteral("candidateHash"),qs(r.candidateHash)},{QStringLiteral("configurationHash"),qs(r.configurationHash)},{QStringLiteral("missing"),qs(r.missingKey)},{QStringLiteral("diagnostics"),diagnostics},{QStringLiteral("consumed"),consumed},{QStringLiteral("bindings"),bindings},{QStringLiteral("affectedPaths"),strings(r.affectedPaths)},{QStringLiteral("volumes"),volumes},{QStringLiteral("meshes"),geometry},{QStringLiteral("itemMeshes"),item},{QStringLiteral("piecePaths"),paths},{QStringLiteral("empty"),r.empty}};
}
PreviewResponse decodeResponse(const QCborValue& value){
  auto m=map(value,{"version","type","serial","status","build","candidateHash","configurationHash","missing","diagnostics","consumed","bindings","affectedPaths","volumes","meshes","itemMeshes","piecePaths","empty"});if(integer(get(m,"version"))!=protocolVersion || str(get(m,"type"))!="response")throw ProtocolError("Unsupported response protocol");PreviewResponse r;r.serial=integer(get(m,"serial"));auto status=integer(get(m,"status"));if(status>4)throw ProtocolError("Unknown worker status");r.status=PreviewStatus(status);r.build=str(get(m,"build"));r.candidateHash=str(get(m,"candidateHash"));r.configurationHash=str(get(m,"configurationHash"));r.missingKey=str(get(m,"missing"));
  for(const auto& item:array(get(m,"diagnostics"))){auto d=map(item,{"code","message","file","offset","warning","generated"});r.diagnostics.push_back({str(get(d,"code")),str(get(d,"message")),str(get(d,"file")),integer(get(d,"offset")),boolean(get(d,"warning")),boolean(get(d,"generated"))});}for(const auto& item:array(get(m,"consumed"),maxSourceFiles)){auto c=map(item,{"file","hash"});if(!r.consumed.emplace(str(get(c,"file")),str(get(c,"hash"))).second)throw ProtocolError("Duplicate consumed input");}for(const auto& item:array(get(m,"bindings")))r.resolvedBindings.push_back(binding(item));r.affectedPaths=strings(get(m,"affectedPaths"));
  for(const auto& item:array(get(m,"volumes"))){auto v=map(item,{"path","before","after"});r.volumes.push_back({str(get(v,"path")),number(get(v,"before")),number(get(v,"after"))});}size_t triangles=0;r.meshes=meshes(get(m,"meshes"),triangles);r.itemMeshes=meshes(get(m,"itemMeshes"),triangles);for(const auto& item:array(get(m,"piecePaths"))){auto p=map(item,{"id","path"});if(!r.piecePaths.emplace(integer(get(p,"id")),str(get(p,"path"))).second)throw ProtocolError("Duplicate piece path");}r.empty=boolean(get(m,"empty"));return r;
}
void validateProof(const PreviewRequest& request,const PreviewResponse& response){
  if(request.serial!=response.serial || request.build!=response.build || request.candidateHash!=response.candidateHash || request.configurationHash!=response.configurationHash)throw ProtocolError("Stale or mismatched worker response");
  for(const auto& [key,hash]:response.consumed){auto source=request.sources.find(key);if(source==request.sources.end() || hash!=(key==request.targetKey?request.candidateHash:source->second.fingerprint))throw ProtocolError("Worker consumed unexpected bytes");}
  if(response.status==PreviewStatus::Success){if(!response.meshes || response.meshes->geometryGeneration!=request.serial || response.meshes->resultId!="insertion:"+std::to_string(request.serial)+":"+request.candidateHash || response.empty!=response.meshes->pieces.empty())throw ProtocolError("Invalid draft mesh identity");if(response.affectedPaths!=request.affectedPaths)throw ProtocolError("Affected instance proof mismatch");if(!response.consumed.count(request.rootKey) || !response.consumed.count(request.targetKey))throw ProtocolError("Incomplete consumed source proof");}
  if(response.status==PreviewStatus::Success){
    for(const auto& expected:request.expectedBindings){bool found=false;for(const auto& actual:response.resolvedBindings)if(actual.module==expected.module && actual.call.fileKey==expected.call.fileKey && actual.call.begin>=expected.call.begin && actual.call.end<=expected.call.end){if(actual.fileKey!=expected.fileKey || actual.call.fingerprint!=expected.call.fingerprint)throw ProtocolError("Generated binding proof differs from expected source");found=true;}if(!found || (!expected.fileKey.empty() && !response.consumed.count(expected.fileKey)))throw ProtocolError("Generated binding proof is incomplete");}
    if(response.volumes.size()!=request.subtractTargets.size())throw ProtocolError("Subtraction proof is incomplete");
    for(size_t i=0;i<response.volumes.size();++i){const auto& volume=response.volumes[i];if(volume.path!=request.subtractTargets[i].instancePath || volume.before<=0 || volume.after<0 || volume.after>=volume.before-std::max(1e-9,volume.before*1e-10))throw ProtocolError("Subtraction proof does not remove selected volume");}
    for(const auto& [id,path]:response.piecePaths)if(std::none_of(response.meshes->pieces.begin(),response.meshes->pieces.end(),[&](const auto& piece){return piece.drawId==id;}) || path.empty())throw ProtocolError("Mesh ownership proof is invalid");
    if(!request.itemModule.empty() && (!response.itemMeshes || response.itemMeshes->pieces.empty() || response.itemMeshes->geometryGeneration!=request.serial || response.itemMeshes->resultId!="insertion-item:"+std::to_string(request.serial)))throw ProtocolError("Item mesh proof is incomplete");
  }
}
QByteArray frame(const QCborValue& value){auto bytes=value.toCbor();if(uint64_t(bytes.size())>maxFrameBytes)throw LimitError("Frame exceeds limit");QByteArray result(4,Qt::Uninitialized);qToBigEndian(quint32(bytes.size()),result.data());return result+bytes;}
std::optional<QCborValue> takeFrame(QByteArray& bytes){if(bytes.size()<4)return {};auto length=qFromBigEndian<quint32>(bytes.constData());if(!length || length>maxFrameBytes)throw LimitError("Frame exceeds limit");if(uint64_t(bytes.size())<uint64_t(length)+4)return {};if(uint64_t(bytes.size())>uint64_t(length)+4)throw ProtocolError("Trailing protocol data");QCborStreamReader reader(bytes.constData()+4,qsizetype(length));auto value=QCborValue::fromCbor(reader);if(reader.lastError()!=QCborError::NoError || reader.currentOffset()!=length)throw ProtocolError("Malformed or trailing CBOR");bytes.clear();return value;}
}
