#!/bin/sh
# render_all.sh <render> <files.txt> <outdir>: renders each file to <outdir>/<n>-<name>.raw
set -e
render=$1; list=$2; dir=$3
mkdir -p "$dir"
rm -f "$dir"/*.raw "$dir"/log.txt
n=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    n=$((n+1))
    name=$(basename "$f" | tr -c 'A-Za-z0-9.\n-' '_')
    "$render" "$f" "${SECONDS:-60}" "$dir/$(printf %02d $n)-$name.raw" 2>>"$dir/log.txt" || echo "FAILED: $f" >>"$dir/log.txt"
done < "$list"
cat "$dir/log.txt"
