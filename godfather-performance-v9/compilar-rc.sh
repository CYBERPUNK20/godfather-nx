#!/usr/bin/env bash
set -euo pipefail
p=$(cd "$(dirname "$0")" && pwd)
bash "$p/aplicar.sh"
# Pin the target and directory so an old binary cannot be packed after a host-only build.
BUILD_DIR="$PWD/build-switch" TARGET=godfather_nro bash switch/build.sh
grep -q 'gfmovie::add_sat' build-switch/movie_recomp.103.cpp || { echo 'A variante de video nao foi compilada.' >&2; exit 1; }
nro=build-switch/godfather.nro
[[ -s "$nro" && $(od -An -tx1 -j16 -N4 "$nro" | tr -d ' \n\r') == 4e524f30 ]] || { echo 'NRO invalido.' >&2; exit 1; }
stage=$(mktemp -d "$PWD/.godfather-release.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
name=godfather-nx-v9-rc1.zip
mkdir -p "$stage/switch/godfather-nx" dist
cp "$nro" "$stage/switch/godfather-nx/godfather.nro"
cp "$p/notas.txt" "$stage/LE-ME.txt"
mkdir -p "$stage/licencas"
for f in LICENSE LICENSES.txt SDK-LICENSE.txt THIRD_PARTY.txt; do cp "$f" "$stage/licencas/$f"; done
(cd "$stage"
 sha256sum switch/godfather-nx/godfather.nro > SHA256SUMS.txt
 if command -v zip >/dev/null; then zip -q "$name" switch/godfather-nx/godfather.nro LE-ME.txt SHA256SUMS.txt licencas/*
 elif command -v tar.exe >/dev/null && tar.exe --version | grep -qi bsdtar; then tar.exe -a -cf "$name" switch/godfather-nx/godfather.nro LE-ME.txt SHA256SUMS.txt licencas/*
 elif command -v bsdtar >/dev/null; then bsdtar --format zip -cf "$name" switch/godfather-nx/godfather.nro LE-ME.txt SHA256SUMS.txt licencas/*
 elif [[ -x /c/Windows/System32/tar.exe ]]; then /c/Windows/System32/tar.exe -a -cf "$name" switch/godfather-nx/godfather.nro LE-ME.txt SHA256SUMS.txt licencas/*
 else echo 'NRO compilado. Para o ZIP falta zip ou bsdtar.' >&2; exit 1; fi)
mv -f "$stage/$name" "dist/$name"
echo "Candidata criada: dist/$name"
