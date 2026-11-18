@echo off
rem Aktualizuje prywatny (ukryty) mod zFastSaveLoad na Workshopie. Widocznosc sie nie zmienia.
rem Uzycie: update.bat "opis zmian"
set L=C:\Program Files (x86)\Steam\steamapps\common\Gothic\launcher
set NOTE=%~1
if "%NOTE%"=="" set NOTE=Update
copy /Y "%~dp0..\build\zFastSaveLoad.dll" "%~dp0Content\System\Autorun\zFastSaveLoad.dll" >nul
"%L%\GothicSteamWorkshopUploader.exe" update --game=g1 --mod=3842156704 --content="%~dp0Content" --changenote="%NOTE%"
pause
