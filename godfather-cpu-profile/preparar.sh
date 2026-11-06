#!/usr/bin/env bash
set -euo pipefail
[[ -f switch/build.sh && -d build-switch ]] || {
    echo 'Executa este script a partir de /c/GodfatherNX.' >&2; exit 1;
}
NM=''
for candidate in /opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm.exe /opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm; do
    if [[ -x "$candidate" ]]; then NM="$candidate"; break; fi
done
if [[ -z "$NM" ]]; then
    NM="$(command -v aarch64-none-elf-nm || true)"
fi
[[ -n "$NM" ]] || { echo 'Nao encontrei o nm do devkitA64.' >&2; exit 1; }
ELF=''
for candidate in build-switch/godfather.elf build-switch/godfather; do
    if [[ -f "$candidate" ]]; then ELF="$candidate"; break; fi
done
[[ -n "$ELF" ]] || { echo 'Nao encontrei o executavel ELF da compilacao atual em build-switch.' >&2; exit 1; }
[[ -f build-switch/godfather.nro ]] || { echo 'Falta build-switch/godfather.nro.' >&2; exit 1; }
TEMP="$(mktemp -d .godfather-cpu-profile.XXXXXX)"
trap 'rm -rf -- "$TEMP"' EXIT
# Read only: these symbols must come from the ELF that produced the tested NRO.
"$NM" -n -S -C --defined-only "$ELF" > "$TEMP/symbols.txt"
grep -Eq '^[[:xdigit:]]+[[:space:]]+([[:xdigit:]]+[[:space:]]+)?[Tt][[:space:]]' "$TEMP/symbols.txt" || {
    echo 'O ELF nao forneceu simbolos de codigo. Nada foi gerado.' >&2; exit 1;
}
{
    echo '# GodfatherNX CPU profile - symbols of the current build'
    echo "# ELF: $ELF"
    echo '# SHA256 follows:'
    sha256sum "$ELF" build-switch/godfather.nro
    echo '# SYMBOLS: address size type demangled-name'
    cat "$TEMP/symbols.txt"
} > "$TEMP/report.txt"
mv -- "$TEMP/report.txt" godfather-cpu-simbolos.txt
echo 'Criado: C:\GodfatherNX\godfather-cpu-simbolos.txt'
echo 'Nao foi alterado codigo, configuracao ou NRO.'
echo 'Usa na Switch a NRO desta compilacao. Nao recompiles antes de recolher o perfil.'
echo 'Segue o LE-ME: profile = true durante um teste curto; depois volta a false.'
