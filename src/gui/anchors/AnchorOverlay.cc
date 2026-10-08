#include "gui/anchors/AnchorController.h"
#include "gui/MainWindow.h"
#include "gui/selection/EvaluationBinding.h"
#include "glview/system-gl.h"
#include <type_traits>
namespace Anchors {
void Controller::prepareOverlay(){
  overlayFrame.reset();overlayWorld.reset();
  if(!window || !window->evaluationBinding->current())return;
  std::optional<Transform3d> frame;std::optional<ReferenceValue> world;auto draft=captureDraft();
  if(draft){auto key=referenceKey(draft->key);auto reference=std::dynamic_pointer_cast<const ReferenceDeclaration>(draft->preview->evaluated);if(key && reference){Transform3d owner=Transform3d::Identity();auto scene=window->evaluationBinding->displayScene(window->evaluationBinding->stamp());if(!scene || !scene->semantic)return;if(key->owner.role==OwnerRole::Instance){auto found=scene->semantic->instances.find(key->owner.id);if(found==scene->semantic->instances.end())return;owner=found->second.localToWorld;}auto transformed=transformReference(reference->value,owner);frame=transformed.frame;world=transformed.worldValue;}}
  else if(selected){auto resolved=resolveForDisplay(*selected);frame=resolved.frame;world=resolved.world;}
  overlayFrame=std::move(frame);overlayWorld=std::move(world);
}
void Controller::drawOverlay(){
  if(!window || !window->evaluationBinding->current() || !overlayWorld)return;
  const auto& frame=overlayFrame;const auto& world=overlayWorld;
  glLineWidth(2);glBegin(GL_LINES);
  auto line=[](const Vector3d& a,const Vector3d& b){glVertex3dv(a.data());glVertex3dv(b.data());};
  if(frame){const Vector3d origin=frame->translation();for(int axis=0;axis<3;++axis){glColor3f(axis==0?1.f:0.f,axis==1?1.f:0.f,axis==2?1.f:0.f);line(origin,origin+frame->linear().col(axis)*5);}}
  else std::visit([&](const auto& value){
    using T=std::decay_t<decltype(value)>;glColor3f(1.f,0.75f,0.f);
    if constexpr(std::is_same_v<T,Point>){for(int axis=0;axis<3;++axis){Vector3d delta=Vector3d::Zero();delta[axis]=1;line(value.position-delta,value.position+delta);}}
    else if constexpr(std::is_same_v<T,Axis>)line(value.position-value.direction*5,value.position+value.direction*5);
    else if constexpr(std::is_same_v<T,Plane>){
      // A finite visual patch has no orientation or bounds semantics for the infinite Plane.
      int least=0;value.normal.cwiseAbs().minCoeff(&least);Vector3d basis=Vector3d::Zero();basis[least]=1;
      const Vector3d u=value.normal.cross(basis).normalized()*5,v=value.normal.cross(u).normalized()*5;
      const Vector3d a=value.position-u-v,b=value.position+u-v,c=value.position+u+v,d=value.position-u+v;
      line(a,b);line(b,c);line(c,d);line(d,a);line(value.position,value.position+value.normal*3);
    }
  },*world);
  glEnd();
}
}
