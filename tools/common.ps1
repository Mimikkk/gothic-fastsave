# Shared settings for the dev / release / profile scripts.
$Root      = Split-Path $PSScriptRoot -Parent
$Gothic    = "C:\Program Files (x86)\Steam\steamapps\common\Gothic"
$System    = Join-Path $Gothic "system"
$Autorun   = Join-Path $System "Autorun"
$DevDll    = Join-Path $Autorun "zFastSaveLoad_dev.dll"
$BuildDll  = Join-Path $Root "build\zFastSaveLoad.dll"
$WorkshopId = "3842156704"
$VsVars    = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
$env:PATH += ";C:\Program Files (x86)\Microsoft Visual Studio\Installer"

function Invoke-Build {
  if (-not (Test-Path (Join-Path $Root "build\build.ninja"))) {
    cmd /c "`"$VsVars`" x86 >nul && cmake -S `"$Root`" -B `"$Root\build`" -G Ninja -DCMAKE_BUILD_TYPE=Release" | Out-Null
  }
  $out = cmd /c "`"$VsVars`" x86 >nul && cmake --build `"$Root\build`" 2>&1"
  $err = $out | Select-String -Pattern 'error|FAILED'
  if ($LASTEXITCODE -ne 0 -or $err) { $out | Select-Object -Last 30; throw "build failed" }
  Write-Host "build ok: $BuildDll"
}

function Assert-GameClosed {
  if (Get-Process GothicMod, Gothic -ErrorAction SilentlyContinue) { throw "Gothic is running - close the game first" }
}
