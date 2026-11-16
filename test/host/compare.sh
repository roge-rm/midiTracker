#!/bin/sh
# compare.sh <baseline> <current>: byte-compares each render, prints the first
# differing frame and how many frames differ.
base=$1; cur=$2; fail=0
for b in "$base"/*.${EXT:-raw}; do
    c="$cur/$(basename "$b")"
    if cmp -s "$b" "$c"; then
        echo "same     $(basename "$b")"
    else
        fail=1
        first=$(cmp "$b" "$c" 2>/dev/null | awk '{print $5}' | tr -d ,)
        diffs=$(cmp -l "$b" "$c" 2>/dev/null | awk '{print int(($1-1)/4)}' | uniq | wc -l)
        echo "DIFFERS  $(basename "$b"): first byte $first (frame $(( (${first:-1}-1)/4 )), $(( (${first:-1}-1)/4/44100 ))s), $diffs frames differ, sizes $(stat -c %s "$b") $(stat -c %s "$c")"
    fi
done
exit $fail
