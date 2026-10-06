#version 120
varying vec3 world_normal;
varying vec3 clip_position;
void main() {
  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
  world_normal = gl_Normal;
  clip_position = gl_Vertex.xyz;
}
