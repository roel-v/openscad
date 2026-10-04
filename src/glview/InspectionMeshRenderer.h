#pragma once
#include "glview/InspectionPresentation.h"
#include "glview/VBORenderer.h"
#include "glview/VertexState.h"

class InspectionMeshRenderer : public VBORenderer {
public:
  explicit InspectionMeshRenderer(std::shared_ptr<const InspectionMeshSet> meshes);
  void prepare(const ShaderUtils::ShaderInfo *shader) override;
  void draw(bool edges,const ShaderUtils::ShaderInfo *shader) const override;
  BoundingBox getBoundingBox() const override { return bounds; }
  SurfaceContext prepareSurfacePick(const ShaderUtils::ShaderInfo *displayShader) override;
  void setVisible(std::set<uint64_t> ids) { visible=std::move(ids); }
  Vector3d positionOrigin() const { return origin; }
  uint64_t uploadCount() const { return uploads; }
private:
  std::shared_ptr<const InspectionMeshSet> meshes;
  BoundingBox bounds;
  Vector3d origin=Vector3d::Zero();
  uint64_t uploads=0;
  std::set<uint64_t> visible;
  std::vector<std::unique_ptr<VertexStateContainer>> containers;
};
