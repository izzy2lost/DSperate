#version 450
// GPU present stage: shades one view's quad (nearest from the core's frame,
// LCD grid seams) or the frontend overlay's. The letterbox is the render
// pass's clear; the PiP fade and the overlay are fixed-function blends.
layout(location = 0) in vec2 d;
layout(location = 0) out vec4 dst;
layout(std430, binding = 1) readonly buffer Src { uint src[]; };   // slot base + screen * 49152 + y * 256 + x, 0xAARRGGBB
layout(std430, binding = 2) readonly buffer Over { uint over[]; };  // frontend overlay (OSD, menu), logical frame, pitch lw, 0xAARRGGBB
layout(std430, binding = 3) readonly buffer Tab  { uint tab[]; };   // LCD grid seam bitmasks: per view, 32 words columns then 32 rows
layout(push_constant) uniform P { ivec4 rect; uvec4 a; uvec4 b; vec4 s; } pc;   // present.vert's

// LCD grid: dims the first panel pixel/row of each source pixel/line to
// f/256. Columns/rows depend only on view size, so the CPU precomputes them
// as bitmasks and this just tests two bits a pixel.
bool grid_seam(uint v, int o, int axis) {
  return ((tab[v * 64u + uint(axis * 32 + (o >> 5))] >> uint(o & 31)) & 1u) != 0u;
}

vec3 unpack(uint s) { return vec3(float((s >> 16) & 255u), float((s >> 8) & 255u), float(s & 255u)); }

void main() {
  ivec2 o = ivec2(d);
  if (pc.b.y == 1u) {
    uint lw = pc.a.z;
    uint ov = over[min(uint(pc.rect.y + o.y) * lw + uint(pc.rect.x + o.x), lw * pc.a.w - 1u)];
    dst = vec4(unpack(ov) / 255.0, float(ov >> 24) / 255.0);
    return;
  }
  // floor(o * 256 / w) exactly: the fraction is a multiple of 1/w, so half of that absorbs the float error.
  int sx = min(int(float(o.x) * pc.s.x + pc.s.z), 255), sy = min(int(float(o.y) * pc.s.y + pc.s.w), 191);
  vec3 col = unpack(src[pc.b.z + uint(sy) * 256u + uint(sx)]);
  uint gf = pc.b.w >> 16;
  if ((pc.b.w & 0x100u) != 0u && gf < 256u && (grid_seam(pc.b.w & 0xFFu, o.x, 0) || grid_seam(pc.b.w & 0xFFu, o.y, 1))) col = floor(col * (float(gf) / 256.0));
  dst = vec4(col / 255.0, 1.0);
}
