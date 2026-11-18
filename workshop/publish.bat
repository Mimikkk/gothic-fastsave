@echo off
rem Publikuje zFastSaveLoad do Steam Workshop jako UKRYTY mod (widoczny tylko dla Ciebie).
rem Po publikacji zasubskrybuj go - pojawi sie w Gothic1SteamWorkshopLauncher.
set L=C:\Program Files (x86)\Steam\steamapps\common\Gothic\launcher
"%L%\GothicSteamWorkshopUploader.exe" publish --game=g1 --title="zFastSaveLoad" --description="Union plugin: profiler i przyspieszenie zapisu/wczytywania gry." --tags="Addon" --content="%~dp0Content" --preview="%~dp0preview.jpg"
pause
