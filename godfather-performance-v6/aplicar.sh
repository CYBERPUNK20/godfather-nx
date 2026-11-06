#!/usr/bin/env bash
set -euo pipefail
PACOTE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BACKUP=godfather-performance-v6-backup
FILES=(switch/deko.cpp sdk/runtime/xdk/d3d8.cpp)
for tool in awk sha256sum mktemp grep cp; do
    command -v "$tool" >/dev/null || { echo "Falta o comando $tool" >&2; exit 1; }
done
for file in "${FILES[@]}" switch/build.sh; do
    [[ -f "$file" ]] || { echo "Falta $file. Executa a partir de /c/GodfatherNX." >&2; exit 1; }
done
if grep -q -E 'QUIETDRAW v6' switch/deko.cpp || grep -q -F 'QUIETDRAW v6' sdk/runtime/xdk/d3d8.cpp; then
    if [[ -f "$BACKUP/aplicado.sha256" ]] && sha256sum --check --status "$BACKUP/aplicado.sha256"; then
        echo 'O pacote v6 ja esta aplicado. Nada foi alterado.'; exit 0
    fi
    echo 'Existem partes desta correcao com alteracoes ou sem backup. Nada foi alterado.' >&2; exit 1
fi
[[ ! -e "$BACKUP" ]] || { echo 'Ja existe godfather-performance-v6-backup. Nada foi alterado.' >&2; exit 1; }
source "$PACOTE/contagens.sh"
TEMP="$(mktemp -d .godfather-performance-v6.XXXXXX)"
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
awk -v package="$PACOTE" -v file=d3d8.cpp -v blocks="$XDK_BLOCKS" -f "$PACOTE/editar.awk" sdk/runtime/xdk/d3d8.cpp > "$TEMP/d3d8.cpp"
mkdir -p -- "$BACKUP/switch" "$BACKUP/sdk/runtime/xdk"
for file in "${FILES[@]}"; do cp -p -- "$file" "$BACKUP/$file"; done
sha256sum "${FILES[@]}" > "$BACKUP/antes.sha256"
STARTED=1
cat -- "$TEMP/deko.cpp" > switch/deko.cpp
cat -- "$TEMP/d3d8.cpp" > sdk/runtime/xdk/d3d8.cpp
sha256sum "${FILES[@]}" > "$BACKUP/aplicado.sha256"
DONE=1
echo 'Diagnosticos de desenho v6 aplicados.'
echo 'Copia de seguranca: godfather-performance-v6-backup'
echo 'Agora compila: bash switch/build.sh'
echo 'Depois copia o novo godfather.nro para a Switch. Mantem o TOML atual.'
