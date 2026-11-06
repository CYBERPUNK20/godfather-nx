#!/usr/bin/env bash
set -euo pipefail
PACOTE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BACKUP=godfather-performance-v7-backup
FILES=(switch/deko.cpp sdk/runtime/gpu/render_thread.cpp)
for tool in awk sha256sum mktemp grep cp; do
    command -v "$tool" >/dev/null || { echo "Falta o comando $tool" >&2; exit 1; }
done
for file in "${FILES[@]}" switch/build.sh; do
    [[ -f "$file" ]] || { echo "Falta $file. Executa a partir de /c/GodfatherNX." >&2; exit 1; }
done
if grep -q -E 'DRAWBANK v7' switch/deko.cpp || grep -q -F 'DRAWBANK v7' sdk/runtime/gpu/render_thread.cpp; then
    if [[ -f "$BACKUP/aplicado.sha256" ]] && sha256sum --check --status "$BACKUP/aplicado.sha256"; then
        echo 'O pacote v7 ja esta aplicado. Nada foi alterado.'; exit 0
    fi
    echo 'Existem partes desta correcao com alteracoes ou sem backup. Nada foi alterado.' >&2; exit 1
fi
[[ ! -e "$BACKUP" ]] || { echo 'Ja existe godfather-performance-v7-backup. Nada foi alterado.' >&2; exit 1; }
source "$PACOTE/contagens.sh"
TEMP="$(mktemp -d .godfather-performance-v7.XXXXXX)"
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
awk -v package="$PACOTE" -v file=deko.cpp -v blocks="$DEKO_BLOCKS" -f "$PACOTE/editar.awk" switch/deko.cpp > "$TEMP/deko.cpp"
awk -v package="$PACOTE" -v file=render_thread.cpp -v blocks="$THREAD_BLOCKS" -f "$PACOTE/editar.awk" sdk/runtime/gpu/render_thread.cpp > "$TEMP/render_thread.cpp"
mkdir -p -- "$BACKUP/switch" "$BACKUP/sdk/runtime/gpu"
for file in "${FILES[@]}"; do cp -p -- "$file" "$BACKUP/$file"; done
sha256sum "${FILES[@]}" > "$BACKUP/antes.sha256"
STARTED=1
cat -- "$TEMP/deko.cpp" > switch/deko.cpp
cat -- "$TEMP/render_thread.cpp" > sdk/runtime/gpu/render_thread.cpp
sha256sum "${FILES[@]}" > "$BACKUP/aplicado.sha256"
DONE=1
echo 'Ajustes de desempenho v7 aplicados.'
echo 'Copia de seguranca: godfather-performance-v7-backup'
echo 'Agora compila: bash switch/build.sh'
echo 'Depois copia o novo godfather.nro para a Switch. Mantem o TOML atual.'
