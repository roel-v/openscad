// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/insertion/InsertionPreviewProtocol.h"
#include "core/providers/ProviderRegistry.h"
#include "core/providers/LibraryResolution.h"
#include "core/NativeExecutionGuard.h"
#include "core/parsersettings.h"
#include <catch2/catch_test_macros.hpp>
#include <QCryptographicHash>
#include <QFile>
#include <QCborArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include <QtEndian>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace Insertion;
struct Fixture {
  std::filesystem::path directory=std::filesystem::temp_directory_path()/std::filesystem::u8path("insertion worker 測試 "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Fixture(){REQUIRE(std::filesystem::create_directory(directory));}
  ~Fixture(){std::error_code error;std::filesystem::remove_all(directory,error);}
  std::string key(const std::string& name){return EvaluationInputs::fileKey(directory/std::filesystem::u8path(name));}
  void write(const std::string& name,const std::string& bytes){std::ofstream output(directory/std::filesystem::u8path(name),std::ios::binary);output<<bytes;REQUIRE(output.good());}
  SourceInput input(const std::string& name,const std::string& bytes){return {key(name),bytes,EvaluationInputs::fingerprint(bytes),"test-"+name,1};}
  PreviewRequest request(const std::string& original,const std::string& candidate){
    write("root.scad",original);PreviewRequest r;r.serial=1;r.rootKey=r.targetKey=key("root.scad");r.directory=EvaluationInputs::fileKey(directory);r.sources.emplace(r.rootKey,input("root.scad",original));r.candidate=candidate;r.candidateHash=EvaluationInputs::fingerprint(candidate);r.edits={{0,original.size(),candidate}};r.configurationHash=configurationHash(r.configuration);
    QFile executable(QString::fromUtf8(OPENSCAD_PROVIDER_TEST_EXE));REQUIRE(executable.open(QIODevice::ReadOnly));QCryptographicHash hash(QCryptographicHash::Sha256);REQUIRE(hash.addData(&executable));r.build=hash.result().toHex().toStdString();return r;
  }
};
void readHello(QProcess& process,const std::string& build){
  QByteArray bytes;std::optional<QCborValue> hello;QElapsedTimer deadline;deadline.start();
  while(!hello){const auto remaining=5000-deadline.elapsed();REQUIRE(remaining>0);REQUIRE(process.waitForReadyRead(int(remaining)));bytes+=process.readAllStandardOutput();hello=takeFrame(bytes);}validateHandshake(*hello,build);
}
PreviewResponse run(PreviewRequest request,const QString& executable=QString::fromUtf8(OPENSCAD_PROVIDER_TEST_EXE)){
  request.configurationHash=configurationHash(request.configuration);request=decodeRequest(encodeRequest(request));QProcess process;process.setWorkingDirectory(QString::fromUtf8(request.directory));process.start(executable,{"--insertion-preview-worker"});REQUIRE(process.waitForStarted(5000));readHello(process,request.build);QByteArray bytes;
  auto data=frame(encodeRequest(request));REQUIRE(process.write(data)==data.size());process.closeWriteChannel();REQUIRE(process.waitForFinished(45000));INFO(process.readAllStandardError().toStdString());REQUIRE(process.exitStatus()==QProcess::NormalExit);REQUIRE(process.exitCode()==0);bytes=process.readAllStandardOutput();auto decoded=takeFrame(bytes);REQUIRE(decoded);auto response=decodeResponse(*decoded);for(const auto& diagnostic:response.diagnostics)INFO(diagnostic.message);validateProof(request,response);return response;
}
BoundingBox bounds(const PreviewResponse& response){BoundingBox result;REQUIRE(response.meshes);for(const auto& piece:response.meshes->pieces)for(const auto& vertex:piece.mesh->vertices)result.extend(piece.localToWorld*vertex);return result;}
}
TEST_CASE("sealed_include_no_disk","[insertion-preview]"){
  Fixture f;f.write("missing.scad","cube(999);\n");auto r=f.request("cube(1);\n","include <missing.scad>\n");auto result=run(r);REQUIRE(result.status==PreviewStatus::NeedInput);CHECK(result.missingKey==f.key("missing.scad"));CHECK_FALSE(result.meshes);
  r.sources.emplace(f.key("missing.scad"),f.input("missing.scad","cube(7);\n"));result=run(r);REQUIRE(result.status==PreviewStatus::Success);CHECK(bounds(result).max().x()==7);CHECK(result.consumed.at(f.key("missing.scad"))==EvaluationInputs::fingerprint("cube(7);\n"));
}
TEST_CASE("dirty_nested_use","[insertion-preview]"){
  Fixture f;f.write("include.scad","cube(999);\n");f.write("nested.scad","module shape(){cube(999);}\n");auto r=f.request("include <include.scad>\n","include <include.scad>\ntranslate([10,0,0]) cube(1);\n");
  r.sources.emplace(f.key("include.scad"),f.input("include.scad","use <nested.scad>\nshape();\n"));r.sources.emplace(f.key("nested.scad"),f.input("nested.scad","module shape(){cube([4,5,6]);}\n"));auto result=run(r);REQUIRE(result.status==PreviewStatus::Success);CHECK(bounds(result).max()==Vector3d(11,5,6));CHECK(EvaluationInputs::diskBytes(f.directory/"nested.scad")=="module shape(){cube(999);}\n");
}
TEST_CASE("conflicting_editors","[insertion-preview]"){
  Fixture f;auto r=f.request("cube(1);","cube(2);");auto encoded=encodeRequest(r);auto sources=encoded.value(QStringLiteral("sources")).toArray();auto duplicate=sources[0].toMap();duplicate.insert(QStringLiteral("editor"),QStringLiteral("different-editor"));sources.append(duplicate);encoded.insert(QStringLiteral("sources"),sources);CHECK_THROWS_AS(decodeRequest(encoded),ProtocolError);
}
TEST_CASE("configuration_parity","[insertion-preview]"){
  Fixture f;const std::string source="width=10; // [1:1:100]\ncube([$preview?width:5,$t*10+height,$vpt.z+$vpr.x+$vpd/120+$vpf/25+$fn/16+$fs/2+$fa/12]);\n";
  auto r=f.request(source,source+"// candidate\n");auto& c=r.configuration;c.variables.preview=true;c.variables.time=.2;c.variables.camera.setVpt(3,4,5);c.variables.camera.setVpr(0,0,0);c.variables.camera.setVpd(120);c.variables.camera.setVpf(25);c.fn=16;c.fs=2;c.fa=12;c.defines="height=8;";boost::property_tree::ptree width;width.put_value(27);c.parameters.emplace("width",width);
  auto response=run(r);REQUIRE(response.status==PreviewStatus::Success);CHECK(bounds(response).max().isApprox(Vector3d(27,10,10),1e-12));auto transported=decodeConfiguration(encodeConfiguration(c));CHECK(configurationHash(c)==configurationHash(transported));r.configuration.variables.preview=false;response=run(r);REQUIRE(response.status==PreviewStatus::Success);CHECK(bounds(response).max().x()==5);
}
TEST_CASE("serial_mismatch","[insertion-preview]"){
  Fixture f;auto r=f.request("cube(1);","cube(2);");PreviewResponse response;response.serial=r.serial+1;response.build=r.build;response.candidateHash=r.candidateHash;response.configurationHash=r.configurationHash;CHECK_THROWS_AS(validateProof(r,response),ProtocolError);response.serial=r.serial;response.candidateHash="another";CHECK_THROWS_AS(validateProof(r,response),ProtocolError);
}
TEST_CASE("fractional_camera_configuration_retains_exact_native_values","[insertion-preview]"){
  Fixture f;auto request=f.request("cube(1);","cube($vpr);");
  const Vector3d exact(0.12500000000001,91.1234567890123,359.9999999999999);
  request.configuration.variables.cameraVariableRotation=exact;
  auto transported=decodeConfiguration(encodeConfiguration(request.configuration));
  REQUIRE(transported.variables.cameraVariableRotation);
  CHECK(*transported.variables.cameraVariableRotation==exact);
  CHECK(configurationHash(transported)==configurationHash(request.configuration));
  auto response=run(request);REQUIRE(response.status==PreviewStatus::Success);
  CHECK(bounds(response).max().isApprox(exact,1e-14));
}
TEST_CASE("oversized_payload","[insertion-preview]"){
  QByteArray header(4,Qt::Uninitialized);qToBigEndian(quint32(maxFrameBytes+1),header.data());CHECK_THROWS_AS(takeFrame(header),LimitError);Fixture f;auto r=f.request("cube(1);","cube(2);");auto value=encodeRequest(r);value.insert(QStringLiteral("unknown"),true);CHECK_THROWS_AS(decodeRequest(value),ProtocolError);
  auto data=frame(encodeRequest(r));data.append('x');CHECK_THROWS_AS(takeFrame(data),ProtocolError);data=frame(encodeRequest(r));auto prefix=data.left(3);CHECK_FALSE(takeFrame(prefix));
}
TEST_CASE("external_input_rejected","[insertion-preview]"){
  Fixture f;for(const auto& source:std::vector<std::string>{"import(\"missing.stl\");","surface(file=\"missing.dat\");","text(\"font\");","cube(rands(1,3,1)[0]);"}){auto r=f.request("cube(1);",source);auto result=run(r);CHECK(result.status==PreviewStatus::Unsupported);CHECK_FALSE(result.meshes);}
  f.write("font.ttf","not a font");auto result=run(f.request("cube(1);","use <font.ttf>\ncube(2);"));CHECK(result.status==PreviewStatus::Unsupported);
  result=run(f.request("cube(1);","module import(){cube(3);} module unused(){surface(file=\"missing.dat\");} import();"));REQUIRE(result.status==PreviewStatus::Success);CHECK(bounds(result).max().x()==3);
}
TEST_CASE("original_edit_ranges_accept_adjacency_and_reject_duplicate_starts","[insertion-preview]"){
  Fixture f;auto request=f.request("cube(1);","cube(2);");
  request.edits={{5,6,"2"},{6,7,")"}};
  CHECK(decodeRequest(encodeRequest(request)).candidate==request.candidate);
  request.edits={{5,5,"2"},{5,6,""}};
  CHECK_THROWS_AS(decodeRequest(encodeRequest(request)),ProtocolError);
}
TEST_CASE("seeded_random_repeatable","[insertion-preview]"){
  Fixture f;auto request=f.request("cube(1);","cube([rands(1,3,1,37)[0],1,1]);");auto a=run(request);request.serial=2;auto b=run(request);REQUIRE(a.status==PreviewStatus::Success);REQUIRE(b.status==PreviewStatus::Success);CHECK(bounds(a).max()==bounds(b).max());
}
TEST_CASE("generated_module_shadowing_rejected","[insertion-preview]"){
  Fixture f;auto r=f.request("cube(1);","module provider_shape(){cube(2);} provider_shape();");r.expectedBindings={{"provider_shape",f.key("expected-library.scad"),{r.targetKey,r.candidateHash,0,r.candidate.size()}}};auto result=run(r);CHECK(result.status==PreviewStatus::Invalid);CHECK_FALSE(result.meshes);
}
TEST_CASE("base_state_unchanged","[insertion-preview]"){
  Fixture f;auto r=f.request("cube(10);\n","difference(){cube(10);translate([2,2,-1])cube([6,6,12]);}\n");auto before=EvaluationInputs::diskBytes(f.directory/"root.scad");auto result=run(r);REQUIRE(result.status==PreviewStatus::Success);CHECK(EvaluationInputs::diskBytes(f.directory/"root.scad")==before);CHECK(result.consumed.at(r.rootKey)==r.candidateHash);
  for(const auto& file:std::filesystem::directory_iterator(f.directory))CHECK(file.path().filename()=="root.scad");
}
TEST_CASE("worker_crash","[insertion-preview]"){
  Fixture f;auto r=f.request("cube(1);","cube(2);");r.directory=f.key("absent-directory");QProcess process;process.start(QString::fromUtf8(OPENSCAD_PROVIDER_TEST_EXE),{"--insertion-preview-worker"});REQUIRE(process.waitForStarted(5000));readHello(process,r.build);process.write(frame(encodeRequest(r)));process.closeWriteChannel();REQUIRE(process.waitForFinished(5000));CHECK(process.exitCode()!=0);CHECK(process.readAllStandardOutput().isEmpty());CHECK(EvaluationInputs::diskBytes(f.directory/"root.scad")=="cube(1);");
}
TEST_CASE("cancel_during_geometry","[insertion-preview]"){
  Fixture f;auto r=f.request("cube(1);","minkowski(){sphere(r=50,$fn=1000);sphere(r=20,$fn=1000);}");QProcess process;process.start(QString::fromUtf8(OPENSCAD_PROVIDER_TEST_EXE),{"--insertion-preview-worker"});REQUIRE(process.waitForStarted(5000));readHello(process,r.build);process.write(frame(encodeRequest(r)));process.closeWriteChannel();CHECK_FALSE(process.waitForFinished(100));process.kill();REQUIRE(process.waitForFinished(5000));CHECK(process.exitStatus()==QProcess::CrashExit);CHECK(EvaluationInputs::diskBytes(f.directory/"root.scad")=="cube(1);");
}
TEST_CASE("worker_unicode_spaced_executable_path","[insertion-preview]"){
  Fixture f;auto copy=f.directory/std::filesystem::u8path("native worker 測試.exe");REQUIRE(std::filesystem::copy_file(std::filesystem::u8path(OPENSCAD_PROVIDER_TEST_EXE),copy));auto r=f.request("cube(1);","cube(4);");const auto encodedPath=copy.generic_u8string();auto result=run(r,QString::fromUtf8(reinterpret_cast<const char *>(encodedPath.data()),qsizetype(encodedPath.size())));REQUIRE(result.status==PreviewStatus::Success);CHECK(bounds(result).max().x()==4);
}
TEST_CASE("all_initial_catalog_entries_use_captured_native_inputs","[insertion-preview][insertion-providers]"){
  const auto bosl=qEnvironmentVariable("SCADMAXX_PROVIDER_BOSL2_ROOT"),bolts=qEnvironmentVariable("SCADMAXX_PROVIDER_BOLTS_ROOT");
  if(bosl.isEmpty() || bolts.isEmpty())SKIP("Pinned external library roots must be supplied explicitly");
  Fixture f;NativeExecutionGuard execution;const auto oldPaths=get_library_path();struct Restore {std::vector<std::string> paths;~Restore(){set_library_path(std::move(paths));}} restore{oldPaths};
  auto boslRoot=std::filesystem::canonical(std::filesystem::u8path(bosl.toStdString())),boltsRoot=std::filesystem::canonical(std::filesystem::u8path(bolts.toStdString()));
  set_library_path({EvaluationInputs::fileKey(boslRoot.parent_path()),EvaluationInputs::fileKey(boltsRoot.parent_path())});
  auto context=Providers::nativeResolutionContext(EvaluationInputs::fileKey(f.directory),{{"bosl2",{EvaluationInputs::fileKey(boslRoot),"BOSL2"}},{"bolts",{EvaluationInputs::fileKey(boltsRoot),"BOLTS"}}});REQUIRE(context);
  auto registry=Providers::ProviderRegistry::load({{OPENSCAD_PROVIDER_TEST_DIR}});REQUIRE(registry->diagnostics.empty());REQUIRE(registry->items.size()==8);
  for(const auto& [id,item]:registry->items){INFO(id);auto binding=Providers::ProviderRegistry::bind(registry,id,*context.value);if(!binding)INFO(binding.diagnostic->message);REQUIRE(binding);Providers::SymbolBindings names;unsigned index=0;for(const auto& helper:item.recipes[binding.value->recipeIndex].helpers)names.emplace(helper.id,"smx_fixture_helper_"+std::to_string(++index));
    auto fragment=Providers::ProviderRegistry::generate(*binding.value,{},names);REQUIRE(fragment);auto r=f.request("cube(1);\n","$fn=48;\n"+fragment.value->standaloneSource);r.configuration.libraryPaths=get_library_path();
    for(const auto& library:binding.value->libraries)for(const auto& file:library.files){auto bytes=EvaluationInputs::diskBytes(std::filesystem::u8path(file.canonicalPath));r.sources.emplace(file.canonicalPath,SourceInput{file.canonicalPath,bytes,EvaluationInputs::fingerprint(bytes),{}, {}});}
    for(const auto& module:fragment.value->moduleBindings){auto library=std::find_if(binding.value->libraries.begin(),binding.value->libraries.end(),[&](const auto& lib){return lib.libraryId==module.libraryId;});REQUIRE(library!=binding.value->libraries.end());r.expectedBindings.push_back({module.module,EvaluationInputs::fileKey(std::filesystem::u8path(library->root)/std::filesystem::u8path(module.path)),{r.targetKey,r.candidateHash,0,r.candidate.size()}});}
    auto result=run(r);std::string diagnostics;for(const auto& message:result.diagnostics)diagnostics+=message.message+"\n";INFO(diagnostics);REQUIRE(result.status==PreviewStatus::Success);REQUIRE(!result.empty);CHECK(!result.meshes->pieces.empty());for(const auto& library:binding.value->libraries)for(const auto& file:library.files)CHECK(result.consumed.count(file.canonicalPath)==1);
  }
}
