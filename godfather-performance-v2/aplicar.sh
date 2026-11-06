#!/usr/bin/env bash
set -euo pipefail
PACOTE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BACKUP=godfather-performance-v2-backup
FILE=sdk/runtime/gpu/vertex.cpp
for tool in awk cmp sha256sum mktemp grep cp; do
    command -v "$tool" >/dev/null || { echo "Falta o comando $tool" >&2; exit 1; }
done
for file in "$FILE" switch/build.sh; do
    [[ -f "$file" ]] || { echo "Falta $file. Executa a partir de /c/GodfatherNX." >&2; exit 1; }
done
if grep -q -F 'INDEXBATCH v2:' "$FILE"; then
    if [[ -f "$BACKUP/aplicado.sha256" ]] && sha256sum --check --status "$BACKUP/aplicado.sha256"; then
        echo 'INDEXBATCH ja esta aplicado. Nao foi alterado nada.'
        exit 0
    fi
    echo 'Ja existe INDEXBATCH com alteracoes posteriores ou sem backup. Nada foi alterado.' >&2
    exit 1
fi
[[ ! -e "$BACKUP" ]] || { echo 'Ja existe godfather-performance-v2-backup. Nada foi alterado.' >&2; exit 1; }
TEMP="$(mktemp -d .godfather-performance-v2.XXXXXX)"
STARTED=0
DONE=0
cleanup() {
    if [[ "$STARTED" == 1 && "$DONE" == 0 ]]; then
        echo 'Instalacao interrompida; a restaurar o ficheiro anterior.' >&2
        cp -p -- "$BACKUP/$FILE" "$FILE" || true
    fi
    rm -rf -- "$TEMP"
}
trap cleanup EXIT
awk -v package="$PACOTE" -v file=vertex.cpp -v blocks=1 -f "$PACOTE/editar.awk" "$FILE" > "$TEMP/vertex.cpp"
if cmp -s "$FILE" "$TEMP/vertex.cpp"; then
    echo 'Nao foi gerada alteracao. Nada foi instalado.' >&2; exit 1
fi
mkdir -p -- "$BACKUP/sdk/runtime/gpu"
cp -p -- "$FILE" "$BACKUP/$FILE"
sha256sum "$FILE" > "$BACKUP/antes.sha256"
STARTED=1
cat -- "$TEMP/vertex.cpp" > "$FILE"
sha256sum "$FILE" > "$BACKUP/aplicado.sha256"
DONE=1
echo 'INDEXBATCH aplicado em sdk/runtime/gpu/vertex.cpp. Copia de seguranca: godfather-performance-v2-backup'
echo 'Compila: bash switch/build.sh'
echo 'Depois copia o novo NRO para a Switch. Mantem o TOML atual.'
