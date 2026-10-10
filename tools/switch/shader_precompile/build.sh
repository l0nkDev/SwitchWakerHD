#!/usr/bin/env bash
# Builds tools/switch/shader_precompile (build/shader_precompile/shader_precompile) in the dksh_cache container (image
# wwhd-dksh-tools, tools/switch/dksh_cache/Dockerfile) and runs it there with the given arguments, if any. Paths are
# relative to the repository root (mounted at /src); absolute paths under $HOME work too.
#     tools/switch/shader_precompile/build.sh <shadercache_manifest.bin> <programs.bin> <out shadercache_gl.bin>
set -euo pipefail
cd "$(dirname "$0")/../../.."
engine=$(command -v podman || command -v docker || true)
[[ -n "$engine" ]] || { echo "podman or docker is required" >&2; exit 1; }
image=wwhd-dksh-tools
"$engine" image inspect "$image" >/dev/null 2>&1 || "$engine" build -t "$image" tools/switch/dksh_cache
jobs=${WWHD_JOBS:-4}
"$engine" run --rm -v "$PWD":/src -v "$HOME":"$HOME" -w /src "$image" bash -c "
    cmake -S tools/switch/shader_precompile -B build/shader_precompile -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null &&
    ninja -C build/shader_precompile -j $jobs shader_precompile &&
    if [[ \$# -gt 0 ]]; then build/shader_precompile/shader_precompile \"\$@\"; fi" bash "$@"
