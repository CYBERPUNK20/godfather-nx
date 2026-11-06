#!/usr/bin/env bash
set -euo pipefail
BACKUP=godfather-performance-v12-backup
FILE=sdk/runtime/gpu/vertex.cpp
[[ -f "$BACKUP/aplicado.sha256" && -f "$BACKUP/antes.sha256" ]] || {
    echo 'Executa a partir de /c/GodfatherNX, onde esta godfather-performance-v12-backup.' >&2; exit 1;
}
if ! sha256sum --check --status "$BACKUP/aplicado.sha256"; then
    echo 'O ficheiro foi modificado depois da instalacao. A reversao parou para preservar essas alteracoes.' >&2
    exit 1
fi
if ! (cd "$BACKUP" && sha256sum --check --status antes.sha256); then
    echo 'A copia de seguranca foi alterada. Nada foi restaurado.' >&2
    exit 1
fi
cp -p -- "$BACKUP/$FILE" "$FILE"
sha256sum --check --status "$BACKUP/antes.sha256"
mv -- "$BACKUP" "${BACKUP}-restaurado-$(date +%Y%m%d-%H%M%S)"
echo 'VERTPREP revertido. As correcoes anteriores foram mantidas.'
echo 'Compila novamente: bash switch/build.sh'
