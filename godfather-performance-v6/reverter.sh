#!/usr/bin/env bash
set -euo pipefail
BACKUP=godfather-performance-v6-backup
FILES=(switch/deko.cpp sdk/runtime/xdk/d3d8.cpp)
[[ -f "$BACKUP/aplicado.sha256" && -f "$BACKUP/antes.sha256" ]] || {
    echo 'Executa a partir de /c/GodfatherNX, onde esta godfather-performance-v6-backup.' >&2; exit 1;
}
if ! sha256sum --check --status "$BACKUP/aplicado.sha256"; then
    echo 'Os ficheiros foram modificados depois da instalacao. A reversao parou para preservar essas alteracoes.' >&2
    exit 1
fi
if ! (cd "$BACKUP" && sha256sum --check --status antes.sha256); then
    echo 'A copia de seguranca foi alterada. Nada foi restaurado.' >&2
    exit 1
fi
for file in "${FILES[@]}"; do cp -p -- "$BACKUP/$file" "$file"; done
sha256sum --check --status "$BACKUP/antes.sha256"
mv -- "$BACKUP" "${BACKUP}-restaurado-$(date +%Y%m%d-%H%M%S)"
echo 'Diagnosticos de desenho v6 revertidos. As alteracoes anteriores foram mantidas.'
echo 'Compila novamente: bash switch/build.sh'
