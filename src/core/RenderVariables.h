#pragma once
#include "glview/Camera.h"
#include <optional>
class BuiltinContext;
template <typename T>
class ContextHandle;

class RenderVariables
{
public:
  bool preview;
  double time;
  Camera camera;
  // Exact retained native $vpr values must survive Camera's angle normalization.
  std::optional<Vector3d> cameraVariableRotation;
  void applyToContext(ContextHandle<BuiltinContext>& context) const;
};
