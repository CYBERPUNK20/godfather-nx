#!/usr/bin/env bash
set -eu
p=$(cd "$(dirname "$0")" && pwd)
b=godfather-performance-v9-backup
f=switch/CMakeLists.txt
helpers=(switch/movie_kernels.sh switch/movie_mmx.h)
[[ -f "$f" && -f switch/build.sh ]] || { echo 'Executa a partir de /c/GodfatherNX.' >&2; exit 1; }
if [[ -f "$b/aplicado.sha256" ]] && sha256sum -c --status "$b/aplicado.sha256"; then echo 'v9 ja instalada.'; exit 0; fi
[[ ! -e "$b" && ! -e "${helpers[0]}" && ! -e "${helpers[1]}" ]] || { echo 'Conflito com uma instalacao anterior. Nada alterado.' >&2; exit 1; }
grep -q 'SIMPLEBANK v8:' sdk/runtime/gpu/render_thread.cpp || { echo 'Aplica primeiro a v8.' >&2; exit 1; }
if grep -q 'MOVIE9:' "$f"; then echo 'CMake ja alterado; nada instalado.' >&2; exit 1; fi
tmp=$(mktemp -d .gf-movie9.XXXXXX)
started=0
cleanup() {
 if [[ $started == 1 ]]; then cp -p "$b/$f" "$f"; rm -f -- "${helpers[@]}"; rm -rf -- "$b"; fi
 rm -rf -- "$tmp"
}
trap cleanup EXIT
awk -v package="$p" -v file=CMakeLists.txt -v blocks=1 -f "$p/editar.awk" "$f" > "$tmp/new"
mkdir -p "$b/switch"
cp -p "$f" "$b/$f"
sha256sum "$f" > "$b/antes.sha256"
started=1
for h in "${helpers[@]}"; do cp "$p/${h##*/}" "$h"; done
cat "$tmp/new" > "$f"
sha256sum "$f" "${helpers[@]}" > "$b/aplicado.sha256"
started=0
echo 'v9 instalada.'
