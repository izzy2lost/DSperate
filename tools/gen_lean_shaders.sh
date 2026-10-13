#!/bin/sh
# Compile the lean GPU 3D raster's shaders (src/core/gpu/vk/shaders) to the
# SPIR-V blobs vk_shaders.cpp .incbin's. Like tools/gen_shaders.sh, the .spv
# files are tracked so a build needs no shader compiler: run this after
# editing a .vert/.frag or vk_layout.h and commit the blobs that changed.
#
#   tools/gen_lean_shaders.sh
#
# The compiler is $GLSLC, else glslc on PATH, else the Android NDK's
# (shader-tools/<host>/glslc under $ANDROID_NDK_HOME). No -O: the tracked
# blobs are unoptimised. A blob is replaced only when its disassembly
# (spirv-dis, beside glslc, without the header) changes: compilers stamp
# their own generator word in the header, and that alone is not a change.
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)
SH="$DIR/src/core/gpu/vk/shaders"

if [ -n "$GLSLC" ]; then GC="$GLSLC"
elif command -v glslc >/dev/null 2>&1; then GC=glslc
elif [ -n "$ANDROID_NDK_HOME" ] && [ -x "$ANDROID_NDK_HOME/shader-tools/linux-x86_64/glslc" ]; then GC="$ANDROID_NDK_HOME/shader-tools/linux-x86_64/glslc"
else
  echo "gen_lean_shaders: no glslc; set GLSLC or ANDROID_NDK_HOME" >&2
  exit 1
fi

DIS=$(dirname "$GC")/spirv-dis
[ -x "$DIS" ] || DIS=$(command -v spirv-dis || true)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$SH/.."
build() {   # source, output, defines...
  src=$1; out=$2; shift 2
  "$GC" --target-env=vulkan1.1 -I. -Ishaders "$@" "shaders/$src" -o "$TMP/$out"
  if command -v spirv-val >/dev/null 2>&1; then spirv-val "$TMP/$out"; fi
  if [ -n "$DIS" ] && [ -f "shaders/$out" ] && "$DIS" --no-header "$TMP/$out" > "$TMP/new.txt" && "$DIS" --no-header "shaders/$out" > "$TMP/old.txt" && cmp -s "$TMP/new.txt" "$TMP/old.txt"; then
    printf '%-26s unchanged\n' "$out"
  else
    cp "$TMP/$out" "shaders/$out"
    printf '%-26s %6d bytes (updated)\n' "$out" "$(wc -c < "shaders/$out")"
  fi
}

# tri_fs.vert (the fullscreen triangle) is left as built: it never changes.
build lean.vert         lean_vert.spv
build lean.frag         lean_frag.spv
build lean.frag         lean_frag_early.spv      -DDS_EARLY
build lean.frag         lean_frag_opq.spv        -DDS_ALPHA_SEL=1
build lean.frag         lean_frag_trans.spv      -DDS_ALPHA_SEL=2
build lean_resolve.frag lean_resolve_frag.spv
build lean_resolve.frag lean_resolve_1x_frag.spv -DDS_SAMPLES=1
build lean_gather.comp  lean_gather_comp.spv
echo "gen_lean_shaders: done -- commit the .spv files that changed"
