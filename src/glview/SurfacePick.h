#pragma once
#include "glview/Renderer.h"
#include "glview/SurfacePickMath.h"
#include <string>

class GLView;
class SurfacePick {
public:
  enum class Error { None, Miss, Unsupported, InvalidViewport, BackendFailure };
  struct Result {
    Error error = Error::None;
    uint32_t drawId = 0;
    Vector3d point = Vector3d::Zero(), normal = Vector3d::Zero();
    std::string detail;
  };
  // Caller owns a current GL context; only model surfaces are drawn.
  static Result read(GLView& view, Renderer& renderer, const Renderer::SurfaceContext& context,
                     int x, int y, int width, int height);
  static Result prepareAndRead(GLView& view, Renderer& renderer, Renderer::SurfaceContext& context,
                     int x, int y, int width, int height);
};
