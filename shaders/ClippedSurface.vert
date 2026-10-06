#version 120
attribute vec3 barycentric;
varying vec3 vBC;
varying vec4 color;
varying float shading;
varying vec3 clip_position;
void main(void) {
  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
  clip_position = gl_Vertex.xyz;
  vBC = barycentric;
  vec3 normal = normalize(gl_NormalMatrix * gl_Normal);
  vec3 light = normalize(vec3(gl_LightSource[0].position));
  shading = 0.2 + abs(dot(normal, light));
  color = gl_Color;
}
