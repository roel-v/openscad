#version 120
uniform vec3 frag_idcolor;
uniform float normal_sign;
uniform bool normal_pass;
varying vec3 world_normal;
void main() {
  if (normal_pass) {
    float length_squared = dot(world_normal, world_normal);
    gl_FragColor = length_squared > 0.0
      ? vec4(normal_sign * normalize(world_normal), 1.0) : vec4(0.0);
  } else {
    gl_FragColor = vec4(frag_idcolor, 1.0);
  }
}
