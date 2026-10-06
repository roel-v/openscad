uniform int clip_count;
uniform vec4 clip_planes[8];
varying vec3 clip_position;
bool clipRetained() {
  for (int i = 0; i < 8; ++i)
    if (i < clip_count && !(dot(clip_position, clip_planes[i].xyz) > clip_planes[i].w)) return false;
  return true;
}
