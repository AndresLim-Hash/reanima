@echo off
rem Compila Reanima (VST3 + Standalone) en modo Release.
rem Doble clic para compilar. El resultado queda en build\Reanima_artefacts\Release.

setlocal
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :fail

cd /d "%~dp0"
"%CMAKE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || goto :fail
"%CMAKE%" --build build --config Release || goto :fail

echo.
echo Compilado correctamente.
echo VST3:       %~dp0build\Reanima_artefacts\Release\VST3\Reanima.vst3
echo Standalone: %~dp0build\Reanima_artefacts\Release\Standalone\Reanima.exe
if "%1"=="" pause
exit /b 0

:fail
echo.
echo ERROR: la compilacion ha fallado.
if "%1"=="" pause
exit /b 1
