#pragma once
#include "glview/Renderer.h"
#include "glview/ClippingState.h"
#include <memory>

// Decorates the shared mesh draw path; ownership, masks and transforms remain in its child.
class ClippedMeshRenderer : public Renderer {
public:
  ClippedMeshRenderer(std::shared_ptr<Renderer> child, std::shared_ptr<const ClipState> state);
  ~ClippedMeshRenderer() override;
  void prepare(const ShaderUtils::ShaderInfo *) override;
  void draw(bool edges, const ShaderUtils::ShaderInfo *) const override;
  BoundingBox getBoundingBox() const override { return child->getBoundingBox(); }
  SurfaceContext prepareSurfacePick(const ShaderUtils::ShaderInfo *) override;
  void setColorScheme(const ColorScheme&) override;
  uint64_t drawCount() const { return frames; }
private:
  std::shared_ptr<Renderer> child;
  std::shared_ptr<const ClipState> state;
  Vector3d origin = Vector3d::Zero();
  ShaderUtils::ShaderInfo colorShader{}, pickShader{};
  mutable uint64_t frames = 0;
  void prepareShaders(const ShaderUtils::ShaderInfo *displayShader);
  void uniforms(const ShaderUtils::ShaderInfo&) const;
};
