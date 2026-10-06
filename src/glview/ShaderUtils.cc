#include "glview/ShaderUtils.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <fstream>
#include <set>
#include <stdexcept>

#include "utils/printutils.h"
#include "platform/PlatformUtils.h"

namespace ShaderUtils {

namespace {
std::string load(const std::string& name, std::set<std::string>& visiting)
{
  if (name.find_first_of("/\\") != std::string::npos || !visiting.insert(name).second)
    throw std::runtime_error("Invalid or cyclic shader include: " + name);
  std::string shaderPath = (PlatformUtils::resourcePath("shaders") / name).string();
  std::ostringstream buffer;
  std::ifstream f(shaderPath);
  if (f.is_open()) {
    std::string line;
    while (std::getline(f, line)) {
      if (line.starts_with("#include \"")) {
        auto end = line.find('"', 10);
        if (end == std::string::npos) throw std::runtime_error("Invalid shader include: " + name);
        buffer << load(line.substr(10, end-10), visiting);
      } else buffer << line << '\n';
    }
  } else {
    LOG(message_group::UI_Error, "Cannot open shader source file: '%1$s'", shaderPath);
    throw std::runtime_error("Cannot open shader source: " + name);
  }
  visiting.erase(name);
  return buffer.str();
}
}
std::string loadShaderSource(const std::string& name)
{
  std::set<std::string> visiting;
  return load(name, visiting);
}

ShaderResource compileShaderProgram(const std::string& vs_str, const std::string& fs_str,
                                   const std::unordered_map<std::string, int>& attributes)
{
  int shaderstatus;
  const char *vs_source = vs_str.c_str();
  const char *fs_source = fs_str.c_str();
  // Compile the shaders
  GL_CHECKD(auto vertex_shader = glCreateShader(GL_VERTEX_SHADER));
  glShaderSource(vertex_shader, 1, (const GLchar **)&vs_source, nullptr);
  glCompileShader(vertex_shader);
  glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &shaderstatus);
  if (shaderstatus != GL_TRUE) {
    int loglen;
    char logbuffer[1000];
    glGetShaderInfoLog(vertex_shader, sizeof(logbuffer), &loglen, logbuffer);
    // FIXME: Use OpenCAD log to error instead of stderr
    fprintf(stderr, __FILE__ ": OpenGL vertex shader Error:\n%.*s\n\n", loglen, logbuffer);
    glDeleteShader(vertex_shader);
    return {};
  }

  GL_CHECKD(auto fragment_shader = glCreateShader(GL_FRAGMENT_SHADER));
  glShaderSource(fragment_shader, 1, (const GLchar **)&fs_source, nullptr);
  glCompileShader(fragment_shader);
  glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &shaderstatus);
  if (shaderstatus != GL_TRUE) {
    int loglen;
    char logbuffer[1000];
    glGetShaderInfoLog(fragment_shader, sizeof(logbuffer), &loglen, logbuffer);
    // FIXME: Use OpenCAD log to error instead of stderr
    fprintf(stderr, __FILE__ ": OpenGL fragment shader Error:\n%.*s\n\n", loglen, logbuffer);
    glDeleteShader(vertex_shader); glDeleteShader(fragment_shader);
    return {};
  }

  // Link
  auto shader_prog = glCreateProgram();
  glAttachShader(shader_prog, vertex_shader);
  glAttachShader(shader_prog, fragment_shader);
  for (const auto& [name, index] : attributes)
    if (index >= 0) glBindAttribLocation(shader_prog, index, name.c_str());
  GL_CHECKD(glLinkProgram(shader_prog));

  GLint status;
  glGetProgramiv(shader_prog, GL_LINK_STATUS, &status);
  if (status == GL_FALSE) {
    int loglen;
    char logbuffer[1000];
    glGetProgramInfoLog(shader_prog, sizeof(logbuffer), &loglen, logbuffer);
    // FIXME: Use OpenCAD log to error instead of stderr
    fprintf(stderr, __FILE__ ": OpenGL Program Linker Error:\n%.*s\n\n", loglen, logbuffer);
    glDeleteProgram(shader_prog); glDeleteShader(vertex_shader); glDeleteShader(fragment_shader);
    return {};
  } else {
    glValidateProgram(shader_prog);
    glGetProgramiv(shader_prog, GL_VALIDATE_STATUS, &status);
    if (!status) {
      int loglen;
      char logbuffer[1000];
      glGetProgramInfoLog(shader_prog, sizeof(logbuffer), &loglen, logbuffer);
      // FIXME: Use OpenCAD log to error instead of stderr
      fprintf(stderr, __FILE__ ": OpenGL Program Validation results:\n%.*s\n\n", loglen, logbuffer);
      glDeleteProgram(shader_prog); glDeleteShader(vertex_shader); glDeleteShader(fragment_shader);
      return {};
    }
  }

  return {
    .shader_program = shader_prog,
    .vertex_shader = vertex_shader,
    .fragment_shader = fragment_shader,
  };
}

}  // namespace ShaderUtils
