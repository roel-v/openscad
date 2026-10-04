#include "glview/SurfacePick.h"
#include "glview/GLView.h"
#include "glview/ScopedGLState.h"
#include "glview/ShaderUtils.h"
#include <algorithm>

namespace {
class Resources {
public:
  GLuint framebuffer=0, textures[2]={0,0}, depth=0;
  ShaderUtils::ShaderResource shader{};
  ~Resources() {
    if (shader.shader_program) glDeleteProgram(shader.shader_program);
    if (shader.vertex_shader) glDeleteShader(shader.vertex_shader);
    if (shader.fragment_shader) glDeleteShader(shader.fragment_shader);
    if (depth) glDeleteRenderbuffers(1, &depth);
    glDeleteTextures(2, textures);
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
  }
};
}

SurfacePick::Result SurfacePick::prepareAndRead(GLView& view, Renderer& renderer, Renderer::SurfaceContext& context,
                                               int x, int y, int width, int height)
{
  if (!hasGLVersion3()) return {Error::Unsupported, 0, {}, {}, "Picking requires OpenGL 3 framebuffer support"};
  ScopedGLState saved;
  try {
    context = renderer.prepareSurfacePick(view.edge_shader.get());
    return read(view, renderer, context, x, y, width, height);
  } catch (const std::overflow_error& error) { return {Error::Unsupported, 0, {}, {}, error.what()}; }
  catch (const std::exception& error) { return {Error::BackendFailure, 0, {}, {}, error.what()}; }
}

SurfacePick::Result SurfacePick::read(GLView& view, Renderer& renderer,
  const Renderer::SurfaceContext& context, int x, int y, int width, int height)
{
  if (!context.supported) return {Error::Unsupported, 0, {}, {}, context.detail};
  if (context.objects.empty()) return {Error::Miss};
  if (width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height)
    return {Error::InvalidViewport};
  if (context.objects.size() > SurfacePickMath::MaxDrawId ||
      std::any_of(context.objects.begin(), context.objects.end(), [](const auto& object) {
        return object.drawId == 0 || object.pickToken == 0 || object.pickToken > SurfacePickMath::MaxDrawId;
      })) return {Error::Unsupported, 0, {}, {}, "Surface draw ID capacity exceeded"};
  if (!hasGLVersion3())
    return {Error::Unsupported, 0, {}, {}, "Picking requires depth/stencil FBO and RGBA32F support"};
  ScopedGLState saved;
  Resources resources;
  while (glGetError() != GL_NO_ERROR) {}
  glGenFramebuffers(1, &resources.framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, resources.framebuffer);
  glGenTextures(2, resources.textures);
  for (int pass=0; pass<2; ++pass) {
    glBindTexture(GL_TEXTURE_2D, resources.textures[pass]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, pass ? GL_RGBA32F : GL_RGBA8,
      width, height, 0, GL_RGBA, pass ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
  }
  glGenRenderbuffers(1, &resources.depth);
  glBindRenderbuffer(GL_RENDERBUFFER, resources.depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, resources.depth);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resources.textures[0], 0);
  glDrawBuffer(GL_COLOR_ATTACHMENT0); glReadBuffer(GL_COLOR_ATTACHMENT0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    return {Error::BackendFailure, 0, {}, {}, "ID framebuffer is incomplete"};
  resources.shader = ShaderUtils::compileShaderProgram(ShaderUtils::loadShaderSource("SurfacePick.vert"),
    ShaderUtils::loadShaderSource("SurfacePick.frag"));
  GLint linked=0;
  glGetProgramiv(resources.shader.shader_program, GL_LINK_STATUS, &linked);
  if (!linked) return {Error::BackendFailure, 0, {}, {}, "Surface shader link failed"};
  ShaderUtils::ShaderInfo shader{resources.shader, ShaderUtils::ShaderType::SURFACE_PICK};
  for (const auto name : {"frag_idcolor", "normal_sign", "normal_pass"}) {
    const auto location = glGetUniformLocation(resources.shader.shader_program, name);
    if (location < 0) return {Error::BackendFailure, 0, {}, {}, "Surface shader uniform is unavailable"};
    shader.uniforms[name] = location;
  }
  view.setupCamera();
  // setupCamera stores the complete modelview but leaves translation removed
  // for decorations. Load that exact matrix for model surfaces.
  glMatrixMode(GL_MODELVIEW);
  glLoadMatrixd(view.modelview);
  glTranslated(context.positionOrigin.x(), context.positionOrigin.y(), context.positionOrigin.z());
  Eigen::Matrix4d modelview, projection;
  glGetDoublev(GL_MODELVIEW_MATRIX, modelview.data());
  glGetDoublev(GL_PROJECTION_MATRIX, projection.data());
  std::array<uint8_t, 3> rgb{};
  GLfloat depth=1, normal[4]={0,0,0,0};
  for (int pass=0; pass<2; ++pass) {
    glBindFramebuffer(GL_FRAMEBUFFER, resources.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resources.textures[pass], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      return {Error::BackendFailure, 0, {}, {}, "Normal framebuffer is incomplete"};
    glViewport(0, 0, width, height);
    for (const auto state : {GL_BLEND, GL_DITHER, GL_FRAMEBUFFER_SRGB, GL_LIGHTING, GL_MULTISAMPLE,
                            GL_ALPHA_TEST, GL_FOG, GL_COLOR_LOGIC_OP, GL_SCISSOR_TEST, GL_STENCIL_TEST,
                            GL_POLYGON_OFFSET_FILL, GL_CULL_FACE, GL_RASTERIZER_DISCARD}) glDisable(state);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE); glStencilMask(~GLuint(0));
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glCullFace(GL_BACK);
    glClearColor(0, 0, 0, 0); glClearDepth(1); glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glUseProgram(resources.shader.shader_program);
    glUniform1i(shader.uniforms.at("normal_pass"), pass);
    glUniform1f(shader.uniforms.at("normal_sign"), 1);
    // The renderer binds the supplied shader for shading. OpenCSG's visibility
    // stages must enter with the same fixed-function state as the display path.
    glUseProgram(0);
    renderer.draw(false, &shader);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, resources.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (!pass) {
      glReadPixels(x, y, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
      glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
      if (SurfacePickMath::decode(rgb) == 0 || depth >= 1) return {Error::Miss};
    } else glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, normal);
    if (glGetError() != GL_NO_ERROR) return {Error::BackendFailure, 0, {}, {}, "Surface readback GL error"};
  }
  const auto point = SurfacePickMath::unproject(x, y, depth, width, height, projection, modelview, context.positionOrigin);
  Vector3d outward(normal[0], normal[1], normal[2]);
  if (!point || !outward.allFinite() || outward.squaredNorm() < 1e-20 || normal[3] == 0)
    return {Error::BackendFailure, 0, {}, {}, "Invalid surface depth or normal"};
  return {Error::None, SurfacePickMath::decode(rgb), *point, outward.normalized(), {}};
}
