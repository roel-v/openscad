// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/insertion/InsertionPlanner.h"
#include "core/insertion/InsertionPreviewProtocol.h"
#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/EvaluationSession.h"
#include "core/ScopeContext.h"
#include "core/node.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/provenance/ProvenanceCollector.h"
#include "core/semantic/SemanticCollector.h"
#include "core/providers/LibraryResolution.h"
#include "openscad.h"
#include <catch2/catch_test_macros.hpp>
#include <QFile>
#include <QCryptographicHash>
#include <QProcess>
#include <QElapsedTimer>
#include <chrono>
#include <fstream>

namespace InsertionTest {
using namespace Insertion;
struct Fixture {
  std::filesystem::path directory=std::filesystem::temp_directory_path()/std::filesystem::u8path("insertion source 測試 "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::shared_ptr<EvaluationInputs> inputs;
  std::shared_ptr<SourceProvenance::SyntaxTable> syntax;
  std::shared_ptr<SourceFileCache> cache;
  std::shared_ptr<SourceFile> parsed;
  std::unique_ptr<EvaluationSession> session;
  std::shared_ptr<const Context> builtin;
  std::shared_ptr<const FileContext> file;
  std::shared_ptr<const AbstractNode> root;
  std::shared_ptr<const Semantic::Scene> semantic;
  std::shared_ptr<const SourceProvenance::Result> provenance;
  std::optional<SourceInventory> inventory;
  NativeConfiguration configuration;
  std::map<std::string,EditableDocument> documents;
  Fixture(){REQUIRE(std::filesystem::create_directory(directory));}
  ~Fixture(){inventory.reset();provenance.reset();semantic.reset();root.reset();file.reset();builtin.reset();session.reset();parsed.reset();cache.reset();syntax.reset();inputs.reset();std::error_code error;std::filesystem::remove_all(directory,error);}
  std::string key(const std::string& name="root.scad") const {return EvaluationInputs::fileKey(directory/std::filesystem::u8path(name));}
  void write(const std::string& name,const std::string& bytes){std::ofstream stream(directory/std::filesystem::u8path(name),std::ios::binary);stream<<bytes;REQUIRE(stream.good());}
  void evaluate(const std::string& bytes,std::vector<SourceInput> editors={}){
    static const bool initialized=[](){Builtins::initialize();return true;}();(void)initialized;
    write("root.scad",bytes);inputs=std::make_shared<EvaluationInputs>(SourceInput{key(),bytes,{},"insertion-test-root",1},std::move(editors));syntax=std::make_shared<SourceProvenance::SyntaxTable>(inputs);cache=std::make_shared<SourceFileCache>(inputs.get());
    NativeEvaluationScope native(*inputs,*cache);SourceProvenance::ParseScope capture(syntax.get());SourceFile *raw=nullptr;REQUIRE(parse(raw,bytes,key(),key(),false));parsed.reset(raw);parsed->handleDependencies();
    session=std::make_unique<EvaluationSession>(EvaluationInputs::fileKey(directory));session->provenanceCollector=std::make_shared<SourceProvenance::ProvenanceCollector>(syntax);auto context=Context::create<BuiltinContext>(session.get());configuration.variables.applyToContext(context);builtin=*context;
    AbstractNode::resetIndexCounter();root=parsed->instantiate(builtin,&file);REQUIRE(root);semantic=session->semanticCollector->finish();REQUIRE(semantic->valid());provenance=session->provenanceCollector->finish(*root);inputs->freeze();
    auto captured=SourceInventory::capture(parsed,cache,inputs,root,provenance,semantic);if(!captured)INFO(captured.diagnostic->message);REQUIRE(captured);inventory=std::move(*captured.value);documents={{key(),{true,true,true}}};
  }
  size_t node(const std::string& name,size_t which=0){std::vector<size_t> found;std::function<void(const std::shared_ptr<const AbstractNode>&)> visit=[&](const auto& n){if(n->name()==name)found.push_back(n->index());for(const auto& child:n->children)visit(child);};visit(root);REQUIRE(which<found.size());return found[which];}
  SourceTarget target(const std::string& name="cube",const std::string& label={}){
    auto choices=inventory->targets(node(name),false,documents);for(const auto& choice:choices)if(choice.target && (label.empty() || choice.target->label==label))return *choice.target;
    for(const auto& choice:choices)if(choice.diagnostic)INFO(choice.diagnostic->message);FAIL("No expected eligible source target");return {};
  }
  Providers::GeneratedFragment fragment(const std::string& id,Providers::ParameterValues values={}){
    auto registry=Providers::ProviderRegistry::load({{OPENSCAD_PROVIDER_TEST_DIR}});REQUIRE(registry->diagnostics.empty());auto context=Providers::nativeResolutionContext(EvaluationInputs::fileKey(directory));REQUIRE(context);auto binding=Providers::ProviderRegistry::bind(registry,id,*context.value);REQUIRE(binding);
    Providers::SymbolBindings names;std::set<std::string> reservations;for(const auto& helper:registry->items.at(id).recipes[binding.value->recipeIndex].helpers)names.emplace(helper.id,inventory->reserve("helper",reservations));auto generated=Providers::ProviderRegistry::generate(*binding.value,values,names);if(!generated)INFO(generated.diagnostic->message);REQUIRE(generated);return std::move(*generated.value);
  }
  PreviewResponse preview(const EditPlan& plan){
    QFile executable(QString::fromUtf8(OPENSCAD_PROVIDER_TEST_EXE));REQUIRE(executable.open(QIODevice::ReadOnly));QCryptographicHash hash(QCryptographicHash::Sha256);REQUIRE(hash.addData(&executable));
    auto request=InsertionPlanner::preview(plan,1,hash.result().toHex().toStdString(),EvaluationInputs::fileKey(directory),configuration,*inputs);
    QProcess process;process.setWorkingDirectory(QString::fromUtf8(request.directory));process.start(QString::fromUtf8(OPENSCAD_PROVIDER_TEST_EXE),{"--insertion-preview-worker"});REQUIRE(process.waitForStarted(5000));QByteArray bytes;std::optional<QCborValue> hello;QElapsedTimer timer;timer.start();
    while(!hello){REQUIRE(timer.elapsed()<5000);REQUIRE(process.waitForReadyRead(int(5000-timer.elapsed())));bytes+=process.readAllStandardOutput();hello=takeFrame(bytes);}validateHandshake(*hello,request.build);auto data=frame(encodeRequest(request));REQUIRE(process.write(data)==data.size());process.closeWriteChannel();REQUIRE(process.waitForFinished(45000));REQUIRE(process.exitStatus()==QProcess::NormalExit);REQUIRE(process.exitCode()==0);bytes=process.readAllStandardOutput();auto response=takeFrame(bytes);REQUIRE(response);auto result=decodeResponse(*response);validateProof(request,result);return result;
  }
};
inline BoundingBox bounds(const PreviewResponse& response){BoundingBox out;REQUIRE(response.meshes);for(const auto& piece:response.meshes->pieces)for(const auto& v:piece.mesh->vertices)out.extend(piece.localToWorld*v);return out;}
inline Transform3d placed(const Vector3d& point,bool mate=false){Transform3d out=Transform3d::Identity();out.translate(point);if(mate)out.rotate(Eigen::AngleAxisd(std::acos(-1.0),Vector3d::UnitY()));return out;}
inline void success(const PreviewResponse& response){std::string text;for(const auto& d:response.diagnostics)text+=d.message+"\n";INFO(text);REQUIRE(response.status==PreviewStatus::Success);}
}
