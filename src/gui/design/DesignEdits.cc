#include "gui/design/DesignEdits.h"
#include "core/design/DesignSourceFields.h"
#include "core/design/DesignReferences.h"
#include "core/SourceFile.h"
#include "gui/Editor.h"
#include "gui/MainWindow.h"
#include "gui/TabManager.h"
#include "gui/selection/EvaluationBinding.h"
#include <QFileInfo>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace Design {
DesignEdits::DesignEdits(MainWindow *window):window(window){}
namespace {
std::string scalar(double value){std::ostringstream stream;stream.imbue(std::locale::classic());stream<<std::setprecision(std::numeric_limits<double>::max_digits10)<<value;return stream.str();}
std::string vector(const Vector3d& value){return "["+scalar(value.x())+", "+scalar(value.y())+", "+scalar(value.z())+"]";}
std::string quote(const std::string& text){std::ostringstream stream;stream<<Value(text);return stream.str();}
std::optional<SourceInput> captured(const Selection::EvaluatedScene& scene,const Record& record){
  if(!record.source || !scene.inputs)return {};
  auto found=scene.inputs->consumed().find(record.source->fileKey);return found==scene.inputs->consumed().end()?std::nullopt:std::optional<SourceInput>(found->second);
}
EditorInterface *target(MainWindow& window,const SourceInput& input){
  if(!input.editorId || !input.editorRevision)return nullptr;
  for(auto *editor:window.tabManager->editorList){
    if(editor->documentId().toStdString()!=*input.editorId)continue;
    if(input.fileKey.rfind("session:",0)!=0 && EvaluationInputs::fileKey(editor->filepath.toStdString())!=input.fileKey)return nullptr;
    if(editor->sourceRevision!=*input.editorRevision || editor->sourceFingerprint.toStdString()!=input.fingerprint || editor->isSourceReadOnly())return nullptr;
    return editor;
  }
  return nullptr;
}
}
PreparedEdit DesignEdits::prepare(const std::string& id,const std::vector<FieldChange>& changes,const Selection::Stamp& stamp)const{
  if(!window || !window->evaluationBinding->validate(stamp))return {{},{},"Evaluation is stale"};
  auto scene=window->evaluationBinding->scene(stamp);
  if(!scene || !scene->designObjects)return {{},{},"Design declarations are unavailable"};
  auto found=scene->designObjects->records.find(id);if(found==scene->designObjects->records.end())return {{},{},"Design id is missing or invalid"};
  return prepare(*found->second,changes,stamp);
}
PreparedEdit DesignEdits::prepare(const Record& record,const std::vector<FieldChange>& changes,const Selection::Stamp& stamp)const{
  auto failure=[](const std::string& error){return PreparedEdit{{},{},error};};
  if(!window || !window->evaluationBinding->validate(stamp))return failure("Evaluation is stale");
  if(!record.source || record.source->host!=DeclarationHost::Assignment)return failure("Unsupported declaration host");
  if(record.kind!="Section" && record.kind!="View")return failure("Kind requires its own lifecycle edit adapter");
  auto scene=window->evaluationBinding->scene(stamp);if(!scene)return failure("Evaluation is unavailable");
  auto input=captured(*scene,record);if(!input)return failure("Declaration source is not captured");
  auto *editor=target(*window,*input);if(!editor)return failure("Open captured source is required; evaluate again after opening it, or unlock it");
  auto sourceMap=DesignSourceMap::scan(*record.source,*input,record.kind);
  std::string error;std::vector<SourcePatch> patches;
  if(changes.size()==1 && changes.front().kind==ChangeKind::DeleteDeclaration){auto deletion=sourceMap.deletion(error);if(!deletion)return failure(error);patches.push_back(*deletion);}
  else{
    std::vector<FieldPatch> fields;
    for(const auto& change:changes){
      switch(change.kind){
      case ChangeKind::Label:fields.push_back({"label",quote(change.label),LiteralKind::String});break;
      case ChangeKind::Translation:
        if(record.kind!="Section" || !change.translation.allFinite())return failure("Translation requires a finite Section adjustment");
        fields.push_back({"adjust.translation",vector(change.translation),LiteralKind::Vector3});break;
      case ChangeKind::Rotation:{
        if(record.kind!="Section" || !change.rotation.coeffs().allFinite() || change.rotation.norm()<1e-12)return failure("Rotation requires a finite nonzero Section quaternion");
        auto q=change.rotation.normalized();fields.push_back({"adjust.rotation","["+scalar(q.x())+", "+scalar(q.y())+", "+scalar(q.z())+", "+scalar(q.w())+"]",LiteralKind::Quaternion});break;
      }
      case ChangeKind::Offset:
        if(record.kind!="Section" || !std::isfinite(change.offset))return failure("Offset requires a finite Section adjustment");
        fields.push_back({"adjust.offset",scalar(change.offset),LiteralKind::Number});break;
      default:return failure("Deletion and insertion cannot be mixed with field changes");
      }
    }
    auto prepared=sourceMap.patches(fields,error);if(!prepared)return failure(error);patches=std::move(*prepared);
  }
  SourceEditing::EditRequest request{editor->documentId(),editor->sourceRevision,editor->sourceFingerprint,{}};
  for(const auto& patch:patches)request.edits.append({qsizetype(patch.range.begin),qsizetype(patch.range.end),QByteArray::fromStdString(patch.expected),QByteArray::fromStdString(patch.replacement)});
  QVector<SourceEditing::ByteEdit> validated;
  auto check=SourceEditing::validate(editor->toPlainText().toUtf8(),editor->documentId(),editor->sourceRevision,editor->sourceFingerprint,editor->isSourceReadOnly(),request,validated);
  if(check.error!=SourceEditing::EditError::None)return failure(check.detail.toStdString());
  return {editor,std::move(request),{}};
}
PreparedEdit DesignEdits::insert(const std::vector<std::shared_ptr<const Record>>& records,EditorInterface& destination,const Selection::Stamp& stamp)const{
  auto failure=[](const std::string& error){return PreparedEdit{{},{},error};};
  if(!window || !window->evaluationBinding->validate(stamp))return failure("Evaluation is stale");
  if(!window->tabManager->editorList.contains(&destination) || destination.isSourceReadOnly())return failure("Destination must be an open writable editor");
  auto scene=window->evaluationBinding->scene(stamp);bool included=destination.documentId()==stamp.documentId;
  auto destinationKey=destination.filepath.isEmpty()?std::string():EvaluationInputs::fileKey(destination.filepath.toStdString());
  if(!included){auto found=scene->inputs->consumed().find(destinationKey);included=found!=scene->inputs->consumed().end() && found->second.editorId && *found->second.editorId==destination.documentId().toStdString();}
  if(!included)return failure("Destination is outside the current root's include closure; add its include explicitly and reevaluate");
  auto bytes=destination.toPlainText().toUtf8();
  // Parse destination names with the native AST retained for this evaluation; no text-name guesses.
  std::set<std::string> names;
  auto parsed=scene->backing->parsedRoot;
  for(const auto& assignment:parsed->scope->assignmentOccurrences)names.insert(assignment->getName());
  QByteArray insertion;
  if(!bytes.isEmpty() && !bytes.endsWith('\n'))insertion.append(bytes.contains("\r\n")?"\r\n":"\n");
  std::set<std::string> ids;
  for(const auto& record:records){
    if(!record || record->source || record->kind!="Section" && record->kind!="View")return failure("Insertion requires unsourced Section/View definitions");
    if(!ids.insert(record->id).second || !names.insert(declarationName(*record)).second)return failure("Destination id or variable collision");
    auto declaration=QByteArray::fromStdString(serialize(*record));if(bytes.contains("\r\n"))declaration.replace("\n","\r\n");insertion.append(declaration);
  }
  return {&destination,{destination.documentId(),destination.sourceRevision,destination.sourceFingerprint,{{bytes.size(),bytes.size(),{},insertion}}},{}};
}
}
