#include "glview/InspectionMeshRenderer.h"
#include "glview/VBOBuilder.h"
#include "glview/SurfacePickMath.h"
#include "glview/TriangleStatistics.h"

InspectionMeshRenderer::InspectionMeshRenderer(std::shared_ptr<const InspectionMeshSet> meshes, bool surfacePicking) : surfacePicking(surfacePicking), meshes(std::move(meshes))
{
  std::optional<uint64_t> count=0;
  for (const auto& piece : this->meshes->pieces) {
    visible.insert(piece.drawId);
    const auto box=piece.mesh->getBoundingBox();
    if (!box.isEmpty()) for (int corner=0;corner<8;++corner)
      bounds.extend(piece.localToWorld*box.corner(static_cast<BoundingBox::CornerType>(corner)));
    const auto triangles=preparedTriangleCount(*piece.mesh);
    if (triangles && count) *count+=*triangles; else count=std::nullopt;
  }
  if (!bounds.isEmpty()) origin=bounds.center();
  triangles_={TriangleStatistics::Kind::Final,count,3};
}
void InspectionMeshRenderer::prepare(const ShaderUtils::ShaderInfo *shader)
{
  // Color updates may arrive without a current GL context. Release/rebuild VBOs here.
  if(colorsDirty){containers.clear();colorsDirty=false;}
  if (!containers.empty() || meshes->pieces.empty()) return;
  for (const auto& piece : meshes->pieces) {
    auto container=std::make_unique<VertexStateContainer>();
    VBOBuilder builder(std::make_unique<VertexStateFactory>(),*container);
    builder.addSurfaceData(); builder.writeSurface(); builder.addShaderData();
    builder.allocateBuffers(calcNumVertices(*piece.mesh));
    add_shader_pointers(builder,shader);
    Color4f color;
    const auto mode=piece.role==InspectionPiece::Role::Highlight ? ColorMode::HIGHLIGHT :
      piece.role==InspectionPiece::Role::Background ? ColorMode::BACKGROUND : ColorMode::MATERIAL;
    getShaderColor(mode,piece.color,color);
    const Transform3d recenter(Eigen::Translation3d(-origin));
    builder.create_surface(*piece.mesh,recenter*piece.localToWorld,color,true,true);
    builder.createInterleavedVBOs();
    containers.push_back(std::move(container));
  }
}
void InspectionMeshRenderer::setColorScheme(const ColorScheme& scheme)
{
  const auto previous=colormap_;
  Renderer::setColorScheme(scheme);
  if(previous!=colormap_)colorsDirty=true;
}
void InspectionMeshRenderer::draw(bool edges,const ShaderUtils::ShaderInfo *shader) const
{
  const bool pick=shader && shader->type==ShaderUtils::ShaderType::SURFACE_PICK;
  const bool enableShader=pick || (shader && shader->type==ShaderUtils::ShaderType::EDGE_RENDERING && edges);
  glMatrixMode(GL_MODELVIEW); glPushMatrix();
  if (!pick) glTranslated(origin.x(),origin.y(),origin.z());
  if (enableShader) { glUseProgram(shader->resource.shader_program); VBOUtils::shader_attribs_enable(*shader); }
  glDepthFunc(GL_LEQUAL);
  for (size_t index=0;index<containers.size();++index) {
    const auto& piece=meshes->pieces[index];
    if (!visible.count(piece.drawId) || (pick && piece.role==InspectionPiece::Role::Background)) continue;
    if (pick) {
      const auto rgb=SurfacePickMath::encode(static_cast<uint32_t>(index+1));
      glUniform3f(shader->uniforms.at("frag_idcolor"),rgb[0]/255.0f,rgb[1]/255.0f,rgb[2]/255.0f);
      glUniform1f(shader->uniforms.at("normal_sign"),piece.localToWorld.linear().determinant()<0 ? 1 : -1);
    }
    for (const auto& state : containers[index]->states())
      if (!std::dynamic_pointer_cast<VBOShaderVertexState>(state) || edges) state->draw();
  }
  if (enableShader) { VBOUtils::shader_attribs_disable(*shader); glUseProgram(0); }
  glPopMatrix();
}
Renderer::SurfaceContext InspectionMeshRenderer::prepareSurfacePick(const ShaderUtils::ShaderInfo *shader)
{
  if(!surfacePicking)return {false,"Draft geometry is unavailable for live surface picking",Vector3d::Zero(),{}};
  prepare(shader);
  SurfaceContext result{true,{},origin,{}};
  for (size_t index=0;index<meshes->pieces.size();++index) {
    const auto& piece=meshes->pieces[index];
    if (!visible.count(piece.drawId) || piece.role==InspectionPiece::Role::Background || piece.mesh->isEmpty()) continue;
    const Transform3d drawFrame=Transform3d(Eigen::Translation3d(-origin))*piece.localToWorld;
    result.objects.push_back({piece.drawId,piece.nodeIndex,piece.localToWorld,static_cast<uint32_t>(index+1),
      piece.mesh->getBoundingBox().transformed(drawFrame)});
  }
  return result;
}
