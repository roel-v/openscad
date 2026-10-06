#version 120
#include "ClipDiscard.glsl"
uniform bool show_edges;
varying vec4 color;
varying vec3 vBC;
varying float shading;
float edgeFactor() {
  vec3 d = fwidth(vBC);
  vec3 fade = smoothstep(vec3(0.0), 1.414*d, vBC);
  return min(min(fade.x,fade.y),fade.z);
}
void main(void) {
  if (!clipRetained()) discard;
  vec4 face = vec4(color.rgb * shading,color.a);
  vec4 edge = vec4((color.rgb + vec3(1.0))/2.0,1.0);
  gl_FragColor = show_edges ? mix(edge,face,edgeFactor()) : face;
}
