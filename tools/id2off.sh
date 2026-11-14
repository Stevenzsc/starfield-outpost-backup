#!/bin/bash
# id2off.sh <id...> : Address Library 1.16.244 ID -> RVA
B="/c/Program Files (x86)/Steam/steamapps/common/Starfield/Data/SFSE/Plugins/versionlib-1-16-244-0.bin"
for id in "$@"; do printf "%s 0x%08X\n" "$id" "$(od -An -tu4 -j $((0x60 + id*4)) -N4 "$B" | tr -d ' ')"; done
