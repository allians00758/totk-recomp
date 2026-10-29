#!/bin/bash
# usage: tools/link_gamelib.sh <objects folder> <game folder>
# Links game code compiled outside make (one .o per generated .c, e.g. another game generated with gen.py --no-sites
# and compiled with zig cc) into <game folder>/swgame.so with the id this checkout's runtime loads (make gamelib-id),
# as runtime/gamebuild.c does at the end of the start screen's build. Run where the runtime is built (clang, lld).
set -euo pipefail
cd "$(dirname "$0")/.."
OBJ=${1:?objects folder} DST=${2:?game folder}
CC=${CC:-clang}
ID=$(make -s gamelib-id)
[ -n "$ID" ] || { echo "no gamelib id (make gamelib-id)"; exit 1; }
n=$(ls "$OBJ"/*.o | wc -l)
echo "gamelib id $ID, $n objects"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
$CC -O1 -fPIC -fvisibility=hidden -Iruntime -DGAMELIB_ID="\"$ID\"" -c tools/gamelib_entry.c -o "$W/gamelib_entry.o"
# (the generated objects, then the entry; a response file: thousands of paths)
for o in "$OBJ"/*.o; do printf '"%s"\n' "$o"; done > "$W/link.rsp"
echo "\"$W/gamelib_entry.o\"" >> "$W/link.rsp"
$CC -shared -fuse-ld=lld -o "$DST/swgame.so.tmp" @"$W/link.rsp"
mv "$DST/swgame.so.tmp" "$DST/swgame.so"
echo "$ID" > "$DST/swgame.so.id"
ls -la "$DST/swgame.so"
