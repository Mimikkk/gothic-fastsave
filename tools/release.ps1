# Build, upload to the (hidden) Workshop item and open its page.
#   tools\release.ps1 "what changed"            upload
#   tools\release.ps1 "what changed" -RemoveDev  also remove the local dev copy afterwards
param([Parameter(Mandatory = $true)][string]$Note, [switch]$RemoveDev)
. "$PSScriptRoot\common.ps1"
Invoke-Build
$content = Join-Path $Root "workshop\Content"
Copy-Item $BuildDll (Join-Path $content "System\Autorun\zFastSaveLoad.dll") -Force
$uploader = Join-Path $Gothic "launcher\GothicSteamWorkshopUploader.exe"
& $uploader update --game=g1 --mod=$WorkshopId --content="$content" --changenote="$Note"
if ($LASTEXITCODE -ne 0) { throw "upload failed" }
if ($RemoveDev) { Assert-GameClosed; if (Test-Path $DevDll) { Remove-Item $DevDll -Force; Write-Host "dev copy removed" } }
Write-Host ""
Write-Host "Steam often does not pick up updates of your own hidden item by itself."
Write-Host "If the launcher still copies the old version: unsubscribe + subscribe on the page that opens now."
Start-Process "steam://url/CommunityFilePage/$WorkshopId"
