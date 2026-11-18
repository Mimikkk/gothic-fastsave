#!/bin/bash
# usage: dis.sh 0xSTART 0xEND
VS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x86\dumpbin.exe"
"/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x86/dumpbin.exe" /nologo /disasm /range:$1,$2 "C:\Program Files (x86)\Steam\steamapps\common\Gothic\system\GothicMod.exe" | grep -E '^\s+[0-9A-F]{8}:' | sed -E 's/^\s+//; s/: ([0-9A-F]{2} )+\s*/: /'
