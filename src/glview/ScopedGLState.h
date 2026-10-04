#pragma once
#include "glview/system-gl.h"
// Compatibility-profile state used by the existing VBO renderers and GUI overlays.
class ScopedGLState {
public:
  ScopedGLState() {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&drawFramebuffer); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&readFramebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM,&program); glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&arrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&elementBuffer); glGetIntegerv(GL_RENDERBUFFER_BINDING,&renderbuffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&packBuffer); glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpackBuffer);
    glGetIntegerv(GL_MATRIX_MODE,&matrixMode);
    glPushAttrib(GL_ALL_ATTRIB_BITS); glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glMatrixMode(GL_MODELVIEW); glPushMatrix();
  }
  ~ScopedGLState() {
    glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix(); glMatrixMode(matrixMode);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,drawFramebuffer); glBindFramebuffer(GL_READ_FRAMEBUFFER,readFramebuffer);
    glPopClientAttrib(); glPopAttrib(); glUseProgram(program);
    glBindBuffer(GL_ARRAY_BUFFER,arrayBuffer); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,elementBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER,packBuffer); glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpackBuffer);
  }
  ScopedGLState(const ScopedGLState&)=delete;
  ScopedGLState& operator=(const ScopedGLState&)=delete;
private:
  GLint drawFramebuffer,readFramebuffer,program,arrayBuffer,elementBuffer,matrixMode,renderbuffer,packBuffer,unpackBuffer;
};
