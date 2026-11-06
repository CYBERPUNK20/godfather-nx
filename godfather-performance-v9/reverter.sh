#!/usr/bin/env bash
set -eu
b=godfather-performance-v9-backup
sha256sum --check --status "$b/aplicado.sha256" && (cd "$b" && sha256sum --check --status antes.sha256) || { echo 'Ficheiros diferentes do backup; reversao interrompida.' >&2; exit 1; }
cp -p "$b/switch/CMakeLists.txt" switch/CMakeLists.txt
rm -- switch/movie_kernels.sh switch/movie_mmx.h
mv "$b" "$b-restaurado-$(date +%Y%m%d-%H%M%S)"
echo 'v8 reposta. Compila novamente com bash switch/build.sh.'
