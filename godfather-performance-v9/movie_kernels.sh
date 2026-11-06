#!/usr/bin/env bash
# Build a temporary optimized translation unit; never edit the generated input.
set -eu
input=$1
out=$2
tmp=$(mktemp -d "${out}.work.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
sed 's/\r$//' "$input" > "$tmp/input"
for entry in \
  '0046B1FE 905e2e41956cca611aa001302879f77d9011fc5b8ae377fbcea08c83b7ec5dba' \
  '0046AB0D 586140f155ef309a70faf423ff5c39970c2d50ab6963d49daa78ec17048d9f63' \
  '0046C27B 7a354e04e7255ddc66b9b87037f8375de54e85036d8672c6033444f81990aff1'
do
    read -r addr digest <<< "$entry"
    awk -v id="$addr" '$0=="X86_FUNC(__imp__sub_" id ") {" {on=1} $0=="X86_WEAK_FUNC(sub_" id ", __imp__sub_" id ")" {on=0} on' "$tmp/input" > "$tmp/body"
    read -r actual _ < <(sha256sum "$tmp/body")
    if [[ "$actual" != "$digest" ]]; then
        echo "Movie kernel $addr differs; using original translation unit." >&2
        cp "$input" "$tmp/output" && mv -f "$tmp/output" "$out"
        exit 0
    fi
done
awk '
BEGIN { print "#include \"movie_mmx.h\""
 fn["paddsw"]="add_sat"; fn["psubsw"]="sub_sat"; fn["pmulhw"]="mul_hi"; fn["pmullw"]="mul_lo"; fn["paddw"]="add"
}
/^X86_FUNC\(/ {active=($0 ~ /__imp__sub_0046(B1FE|AB0D|C27B)\)/); small=($0 ~ /__imp__sub_0046C27B\)/)}
/^}/ {active=0;small=0}
active && $1=="//" {op=$3; print; next}
active && op in fn && /mmx_map2/ {
 sub(/mmx_map2<[^>]+>/,"gfmovie::" fn[op]); sub(/, \[\].*; \}\); \}/,"); }"); ++changed
}
small && op=="punpcklbw" && /uint8_t a\[8\]/ {$0="\t{ ctx.mm[2] = gfmovie::expand4(uint32_t(ctx.mm[2])); }"; ++changed}
small && op=="punpckhbw" && /uint8_t a\[8\]/ {$0="\t{ ctx.mm[3] = gfmovie::expand4(uint32_t(ctx.mm[3] >> 32)); }"; ++changed}
small && op=="packuswb" && /int16_t a\[4\]/ {$0="\t{ ctx.mm[2] = gfmovie::pack8(ctx.mm[2], ctx.mm[3]); }"; ++changed}
{print; op=""}
END {if(changed!=429) {print "Unexpected MMX replacement count: " changed > "/dev/stderr"; exit 1}}
' "$tmp/input" > "$tmp/output"
mv -f "$tmp/output" "$out"
echo "Godfather: 3 optimized movie kernels"
