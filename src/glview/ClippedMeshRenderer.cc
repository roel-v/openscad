#include "glview/ClippedMeshRenderer.h"
#include "glview/ScopedGLState.h"
#include <stdexcept>

namespace {
void release(ShaderUtils::ShaderResource& resource)
{
  if (resource.shader_program) glDeleteProgram(resource.shader_program);
  if (resource.vertex_shader) glDeleteShader(resource.vertex_shader);
  if (resource.fragment_shader) glDeleteShader(resource.fragment_shader);
  resource = {};
}
void locations(ShaderUtils::ShaderInfo& shader, std::initializer_list<const char *> names)
{
  if (!shader.resource.shader_program) throw std::runtime_error("Section shader compilation failed");
  for (auto name : names) {
    const auto location = glGetUniformLocation(shader.resource.shader_program, name);
    if (location < 0) throw std::runtime_error(std::string("Section shader uniform is unavailable: ") + name);
    shader.uniforms.emplace(name, location);
  }
}
}
ClippedMeshRenderer::ClippedMeshRenderer(std::shared_ptr<Renderer> child,
                                       std::shared_ptr<const ClipState> state)
  : child(std::move(child)), state(std::move(state))
{
  if (!this->child || !this->state) throw std::invalid_argument("Section effect requires a mesh and state");
  const auto bounds = this->child->getBoundingBox();
  if (!bounds.isEmpty()) origin = bounds.center();
  triangles_ = this->child->triangleStatistics();
}
ClippedMeshRenderer::~ClippedMeshRenderer() { release(colorShader.resource); release(pickShader.resource); }
void ClippedMeshRenderer::setColorScheme(const ColorScheme& scheme)
{
  Renderer::setColorScheme(scheme); child->setColorScheme(scheme);
}
void ClippedMeshRenderer::prepareShaders(const ShaderUtils::ShaderInfo *displayShader)
{
  if (colorShader.resource.shader_program && pickShader.resource.shader_program) return;
  if (!displayShader) throw std::runtime_error("Section display requires the native mesh shader");
  colorShader.type = ShaderUtils::ShaderType::EDGE_RENDERING;
  colorShader.attributes = displayShader->attributes;
  colorShader.resource = ShaderUtils::compileShaderProgram(ShaderUtils::loadShaderSource("ClippedSurface.vert"),
    ShaderUtils::loadShaderSource("ClippedSurface.frag"), colorShader.attributes);
  locations(colorShader, {"clip_count", "clip_planes", "show_edges"});
  pickShader.type = ShaderUtils::ShaderType::SURFACE_PICK;
  pickShader.resource = ShaderUtils::compileShaderProgram(ShaderUtils::loadShaderSource("SurfacePick.vert"),
    ShaderUtils::loadShaderSource("SurfacePick.frag"));
  locations(pickShader, {"clip_count", "clip_planes", "frag_idcolor", "normal_sign", "normal_pass"});
}
void ClippedMeshRenderer::prepare(const ShaderUtils::ShaderInfo *displayShader)
{
  prepareShaders(displayShader); child->prepare(displayShader);
}
Renderer::SurfaceContext ClippedMeshRenderer::prepareSurfacePick(const ShaderUtils::ShaderInfo *displayShader)
{
  prepareShaders(displayShader);
  auto context = child->prepareSurfacePick(displayShader); origin = context.positionOrigin; return context;
}
void ClippedMeshRenderer::uniforms(const ShaderUtils::ShaderInfo& shader) const
{
  if (state->planes.size() > ClipState::MaxPlanes) throw std::logic_error("Section plane limit exceeded");
  glUniform1i(shader.uniforms.at("clip_count"), static_cast<GLint>(state->planes.size()));
  GLfloat values[4 * ClipState::MaxPlanes] = {};
  for (size_t i = 0; i < state->planes.size(); ++i) {
    const auto& plane = state->planes[i];
    for (int axis = 0; axis < 3; ++axis) values[4*i+axis] = static_cast<GLfloat>(plane.normal[axis]);
    values[4*i+3] = static_cast<GLfloat>((plane.origin-origin).dot(plane.normal) + plane.epsilon);
  }
  glUniform4fv(shader.uniforms.at("clip_planes"), ClipState::MaxPlanes, values);
}
void ClippedMeshRenderer::draw(bool edges, const ShaderUtils::ShaderInfo *displayShader) const
{
  ScopedGLState saved; ++frames;
  const bool pick = displayShader && displayShader->type == ShaderUtils::ShaderType::SURFACE_PICK;
  const auto& shader = pick ? pickShader : colorShader;
  if (!shader.resource.shader_program) throw std::logic_error("Section shader was not prepared");
  glDisable(GL_CULL_FACE); glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
  glUseProgram(shader.resource.shader_program); uniforms(shader);
  if (pick) {
    GLint pass = 0;
    glGetUniformiv(displayShader->resource.shader_program, displayShader->uniforms.at("normal_pass"), &pass);
    glUniform1i(shader.uniforms.at("normal_pass"), pass);
  } else glUniform1i(shader.uniforms.at("show_edges"), edges);
  glUseProgram(0);
  // The base mesh enables an edge shader only with this flag. Its barycentric state is shared;
  // show_edges controls whether the color program actually draws edge contrast.
  child->draw(!pick, &shader);
}
