# Build and install the development copy (System\Autorun\zFastSaveLoad_dev.dll).
# While it exists, the Workshop copy of the plugin stays inactive, so the launcher setup can stay as is.
#   tools\dev.ps1            build + install
#   tools\dev.ps1 -Remove    remove the dev copy (back to the Workshop version)
param([switch]$Remove)
. "$PSScriptRoot\common.ps1"
Assert-GameClosed
if ($Remove) {
  if (Test-Path $DevDll) { Remove-Item $DevDll -Force; Write-Host "dev copy removed - the Workshop version is active again" }
  else { Write-Host "no dev copy installed" }
  return
}
Invoke-Build
Copy-Item $BuildDll $DevDll -Force
Write-Host "installed $DevDll ($((Get-FileHash $DevDll).Hash.Substring(0,12)))"
