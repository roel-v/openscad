#version 120
varying vec3 world_normal;
void main() {
  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
  world_normal = gl_Normal;
}
