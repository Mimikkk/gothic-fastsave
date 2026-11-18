# Turn the sampler dumps of the last game run into flamegraphs and open them.
# Needs Profiler=1 in GOTHIC.INI [ZFASTSAVELOAD].
#   tools\profile.ps1             dumps of the last game run (everything since the newest STARTUP dump)
#   tools\profile.ps1 -Minutes 30 dumps from the last 30 minutes
param([int]$Minutes = 0, [switch]$NoOpen)
. "$PSScriptRoot\common.ps1"
$src = Join-Path $System "zFastSaveLoad_prof"
$dumps = Get-ChildItem $src -Filter *.txt | Sort-Object LastWriteTime
if ($Minutes -gt 0) { $since = (Get-Date).AddMinutes(-$Minutes) }
else {
  $start = $dumps | Where-Object Name -like '*STARTUP*' | Select-Object -Last 1
  $since = if ($start) { $start.LastWriteTime.AddSeconds(-1) } else { (Get-Date).AddHours(-1) }
}
$dumps = $dumps | Where-Object LastWriteTime -ge $since
if (-not $dumps) { throw "no dumps found in $src since $since" }

$out = Join-Path $Root ("prof\" + (Get-Date -Format 'yyyyMMdd_HHmmss'))
$groups = @{}
foreach ($d in $dumps) {
  $g = switch -Regex ($d.Name) {
    'STARTUP' { 'startup' }
    'SAVE.*Fast__OFF' { 'save_off' }  'SAVE' { 'save' }
    'LOAD.*Fast__OFF' { 'load_off' }  'LOAD' { 'load' }
    'CHANGELEVEL' { 'changelevel' }   default { 'other' }
  }
  # the first LOAD of a run comes from the main menu: keep it apart
  if ($g -eq 'load' -and -not $groups.ContainsKey('load_menu') -and -not $groups.ContainsKey('load')) { $g = 'load_menu' }
  if (-not $groups.ContainsKey($g)) { $groups[$g] = @() }
  $groups[$g] += $d.FullName
}
$names = Join-Path $Root "deps\gothic-api\ZenGin\Gothic_I_Classic\Names.txt"
foreach ($g in $groups.Keys) {
  $dir = Join-Path $out $g
  python -I (Join-Path $Root "tools\flame.py") $names $dir @($groups[$g]) | Out-File (Join-Path $out "$g.txt") -Encoding utf8
  Get-Content (Join-Path $out "$g.txt") -TotalCount 3
  if (-not $NoOpen) { Get-ChildItem $dir -Filter *.html | ForEach-Object { Start-Process $_.FullName } }
}
Write-Host "results: $out"
