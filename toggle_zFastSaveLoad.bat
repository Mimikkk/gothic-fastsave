@echo off
rem Wlacza / wylacza plugin zFastSaveLoad (lokalna instalacja, bez Workshopu)
set A=C:\Program Files (x86)\Steam\steamapps\common\Gothic\system\Autorun
if exist "%A%\zFastSaveLoad.dll" (
  ren "%A%\zFastSaveLoad.dll" zFastSaveLoad.dll.off
  echo zFastSaveLoad: WYLACZONY
) else if exist "%A%\zFastSaveLoad.dll.off" (
  ren "%A%\zFastSaveLoad.dll.off" zFastSaveLoad.dll
  echo zFastSaveLoad: WLACZONY
) else (
  echo Nie znaleziono pluginu w "%A%"
)
pause
