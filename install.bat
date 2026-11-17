@echo off
rem Copia Reanima.vst3 a la carpeta de plugins VST3 del sistema (pide permisos de administrador).

net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "SRC=%~dp0build\Reanima_artefacts\Release\VST3\Reanima.vst3"
rem Todos los plugins de la marca van juntos en VST3\Blackpanda. La copia suelta de versiones anteriores se retira.
set "DST=C:\Program Files\Common Files\VST3\Blackpanda\Reanima.vst3"
set "OLD=C:\Program Files\Common Files\VST3\Reanima.vst3"
if exist "%OLD%" rmdir /s /q "%OLD%"

if not exist "%SRC%" (
    echo No existe %SRC%. Ejecuta build.bat primero.
    pause
    exit /b 1
)

if exist "%DST%" rmdir /s /q "%DST%"
xcopy "%SRC%" "%DST%\" /e /i /y >nul && echo Instalado en %DST% || echo ERROR al copiar.
pause
