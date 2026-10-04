#pragma once
#include "core/anchors/AttachmentSolver.h"
namespace AnchorTest {
inline std::map<std::string,Semantic::Instance> instances(std::initializer_list<std::pair<std::string,std::string>> paths) {
  std::map<std::string,Semantic::Instance> out;for(const auto& [path,parent]:paths){auto& instance=out[path];instance.path=path;instance.parentPath=parent;}return out;
}
inline Anchors::ReferenceKey key(const std::string& path,const std::string& id="anchor") {return {path.empty()?Anchors::OwnerKey{Anchors::OwnerRole::Document,"document"}:Anchors::instanceOwner(path,""),id};}
inline void reference(Anchors::ReferenceScene& scene,const std::string& path,Anchors::Anchor anchor={},const std::string& id="anchor") {
  auto reference=key(path,id);Anchors::ReferenceRecord record;record.key=reference;record.value=anchor;scene.references.emplace(reference,std::move(record));
}
inline Anchors::Attachment relation(const std::string& id,const std::string& child,const std::string& target) {
  Anchors::Attachment out;out.key=key("",id);out.child={key(child),child};out.target={key(target),target};
  auto declaration=std::make_shared<Design::Record>();declaration->id=id;declaration->kind="Attachment";declaration->source=Design::DeclarationKey{"fixture.scad","fingerprint",id,Design::DeclarationHost::Assignment,10,90};out.declaration=std::move(declaration);return out;
}
inline void add(Anchors::ReferenceScene& scene,Anchors::Attachment relation) {auto key=relation.key;scene.attachments.emplace(key,std::move(relation));}
}
