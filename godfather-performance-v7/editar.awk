function fail(message) { print "ERRO: " message "; nada foi alterado." > "/dev/stderr"; bad=1; exit 2 }
function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
function readblock(id, side, path, item, rc, n) {
    path=package "/blocos/" file "/" id "-" side ".txt"
    while ((rc=getline item < path) > 0) {
        sub(/\r$/, "", item)
        block[id, side, ++n]=item
    }
    close(path)
    if (rc < 0 || n == 0) fail("Falta o bloco " id "-" side)
    length_block[id, side]=n
}
BEGIN {
    for (id=1; id<=blocks; id++) { readblock(id,"antes"); readblock(id,"depois") }
}
{
    if (NR == 1) crlf=($0 ~ /\r$/)
    sub(/\r$/, "")
    source[NR]=$0
}
END {
    if (bad) exit 2
    # Check every complete block before emitting any replacement. Line numbers
    # and surrounding diagnostics may differ; unexpected function edits stop.
    for (id=1; id<=blocks; id++) {
        n=length_block[id,"antes"]
        hits=0
        for (i=1; i+n-1<=NR; i++) {
            if (trim(source[i]) != trim(block[id,"antes",1])) continue
            matches=1
            for (j=1; j<=n; j++)
                if (trim(source[i+j-1]) != trim(block[id,"antes",j])) { matches=0; break }
            if (matches) { hits++; start[id]=i }
        }
        if (hits != 1) fail("O bloco " id " difere da versao verificada (ocorrencias=" hits ")")
        replacement[start[id]]=id
    }
    eol=crlf ? "\r\n" : "\n"
    for (i=1; i<=NR; i++) {
        if (i in replacement) {
            id=replacement[i]
            for (j=1; j<=length_block[id,"depois"]; j++) printf "%s%s",block[id,"depois",j],eol
            i+=length_block[id,"antes"]-1
        } else printf "%s%s",source[i],eol
    }
}
