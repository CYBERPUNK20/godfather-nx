#!/usr/bin/env bash
set -euo pipefail
PACOTE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BACKUP=godfather-performance-v8-backup
FILES=(sdk/runtime/gpu/backend.h sdk/runtime/xdk/d3d8.cpp sdk/runtime/gpu/render_thread.cpp)
for tool in awk sha256sum mktemp grep cp; do
    command -v "$tool" >/dev/null || { echo "Falta o comando $tool" >&2; exit 1; }
done
for file in "${FILES[@]}" switch/build.sh; do
    [[ -f "$file" ]] || { echo "Falta $file. Executa a partir de /c/GodfatherNX." >&2; exit 1; }
done
if grep -q -F 'SIMPLEBANK v8:' sdk/runtime/gpu/render_thread.cpp; then
    if [[ -f "$BACKUP/aplicado.sha256" ]] && sha256sum --check --status "$BACKUP/aplicado.sha256"; then
        echo 'O pacote v8 ja esta aplicado. Nada foi alterado.'; exit 0
    fi
    echo 'Existem partes desta correcao com alteracoes ou sem backup. Nada foi alterado.' >&2; exit 1
fi
[[ ! -e "$BACKUP" ]] || { echo 'Ja existe godfather-performance-v8-backup. Nada foi alterado.' >&2; exit 1; }
grep -q -F 'DRAWBANK v7:' sdk/runtime/gpu/render_thread.cpp || { echo 'Instala primeiro a v7.' >&2; exit 1; }
TEMP="$(mktemp -d .godfather-performance-v8.XXXXXX)"
STARTED=0
DONE=0
cleanup() {
    if [[ "$STARTED" == 1 && "$DONE" == 0 ]]; then
        echo 'Instalacao interrompida; a restaurar os ficheiros anteriores.' >&2
        for file in "${FILES[@]}"; do cp -p -- "$BACKUP/$file" "$file" || true; done
    fi
    rm -rf -- "$TEMP"
}
trap cleanup EXIT
# Validate both files before touching either source file.
awk -v package="$PACOTE" -v file=backend.h -v blocks=1 -f "$PACOTE/editar.awk" sdk/runtime/gpu/backend.h > "$TEMP/backend.h"
awk -v package="$PACOTE" -v file=d3d8.cpp -v blocks=1 -f "$PACOTE/editar.awk" sdk/runtime/xdk/d3d8.cpp > "$TEMP/d3d8.cpp"
awk -v package="$PACOTE" -v file=render_thread.cpp -v blocks=5 -f "$PACOTE/editar.awk" sdk/runtime/gpu/render_thread.cpp > "$TEMP/render_thread.cpp"
mkdir -p -- "$BACKUP/sdk/runtime/xdk" "$BACKUP/sdk/runtime/gpu"
for file in "${FILES[@]}"; do cp -p -- "$file" "$BACKUP/$file"; done
sha256sum "${FILES[@]}" > "$BACKUP/antes.sha256"
STARTED=1
for file in "${FILES[@]}"; do cat -- "$TEMP/${file##*/}" > "$file"; done
sha256sum "${FILES[@]}" > "$BACKUP/aplicado.sha256"
DONE=1
echo 'Ajustes de desempenho v8 aplicados.'
echo 'Copia de seguranca: godfather-performance-v8-backup'
echo 'Agora compila: bash switch/build.sh'
echo 'Depois copia o novo godfather.nro para a Switch. Mantem o TOML atual.'
