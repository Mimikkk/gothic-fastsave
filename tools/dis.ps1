param([string]$Start, [string]$End)
# Disassemble a range of GothicMod.exe (addresses as 0x...)
$db = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x86\dumpbin.exe"
$exe = "C:\Program Files (x86)\Steam\steamapps\common\Gothic\system\GothicMod.exe"
& $db /nologo /disasm "/range:$Start,$End" $exe | Where-Object { $_ -match '^\s+[0-9A-F]{8}:' } |
  ForEach-Object { ($_ -replace '^\s+', '') -replace ':(\s[0-9A-F]{2})+\s+', ':  ' }
