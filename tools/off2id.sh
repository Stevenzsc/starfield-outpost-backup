#!/bin/bash
# off2id.sh <rva-hex...> : reverse Address Library 1.16.244 lookup
B="/c/Program Files (x86)/Steam/steamapps/common/Starfield/Data/SFSE/Plugins/versionlib-1-16-244-0.bin"
od -An -tu4 -v -j 96 -w4 "$B" | awk -v want="$*" 'BEGIN{n=split(want,w," "); for(i=1;i<=n;i++) t[strtonum("0x" w[i])]=w[i]} {v=$1+0; if (v in t) print t[v], NR-1}'
