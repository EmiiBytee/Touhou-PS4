#!/bin/bash
# PacBrew's ld.lld links against libxml2.so.2, which Ubuntu >= 25.10 no longer ships.
# lld only uses libxml2 for Windows manifest merging (never for ELF), so a stub that
# exports the required versioned symbols is enough. Output: ps4/tools/lib/libxml2.so.2
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=$DIR/lib
mkdir -p "$OUT"
TMP=$(mktemp -d)
LLD=/opt/pacbrew/ps4/openorbis/bin/ld.lld
# "version symbol" pairs, e.g. "LIBXML2_2.4.30 xmlNewDoc"
/usr/bin/objdump -T "$LLD" | awk '/\(LIBXML2_/ {v=$(NF-1); gsub(/[()]/,"",v); print v, $NF}' | sort -u > "$TMP/syms"
{
    echo '#include <stdlib.h>'
    while read -r ver sym; do
        if [ "$sym" = xmlFree ]; then echo 'void *xmlFree;'; else echo "void $sym(void) { abort(); }"; fi
    done < "$TMP/syms"
} > "$TMP/stub.c"
# One version node per version tag, chained in sorted order like the real library.
prev=""
for ver in $(cut -d' ' -f1 "$TMP/syms" | sort -uV); do
    echo "$ver { global: $(awk -v v="$ver" '$1==v {printf "%s; ", $2}' "$TMP/syms") } $prev;" >> "$TMP/stub.map"
    prev=$ver
done
gcc -shared -fPIC -o "$OUT/libxml2.so.2" -Wl,-soname,libxml2.so.2 -Wl,--version-script="$TMP/stub.map" "$TMP/stub.c"
echo "stub: $OUT/libxml2.so.2 ($(wc -l < "$TMP/syms") symbols)"
rm -rf "$TMP"
