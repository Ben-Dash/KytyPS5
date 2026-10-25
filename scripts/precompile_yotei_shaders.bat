@echo off
setlocal enabledelayedexpansion
title KytyPS5 - Ghost of Yotei Shader Precompiler

echo ===================================================
echo   Ghost of Yotei - Precompilador de Shaders KytyPS5
echo ===================================================
echo.

:: 1. Localizar o executavel
set "KYTY_EXE="
if exist "%~dp0..\..\KytyPS5-Windows-x64\kyty_emulator.exe" (
    set "KYTY_EXE=%~dp0..\..\KytyPS5-Windows-x64\kyty_emulator.exe"
    set "RUN_DIR=%~dp0..\..\KytyPS5-Windows-x64"
) else if exist "F:\KytyPS5-Windows-x64\kyty_emulator.exe" (
    set "KYTY_EXE=F:\KytyPS5-Windows-x64\kyty_emulator.exe"
    set "RUN_DIR=F:\KytyPS5-Windows-x64"
) else if exist "%~dp0..\_Build\windows-custom\kyty_emulator.exe" (
    set "KYTY_EXE=%~dp0..\_Build\windows-custom\kyty_emulator.exe"
    set "RUN_DIR=%~dp0..\_Build\windows-custom"
) else (
    echo [ERRO] kyty_emulator.exe nao foi encontrado!
    pause
    exit /b 1
)

cd /d "%RUN_DIR%"

echo Executavel: %KYTY_EXE%
echo Diretorio:  %RUN_DIR%
echo.
echo Precompilando todos os shaders gravados no journal...
echo Isso compila todos os pipelines Vulkan para a sua GPU
echo e elimina as travadas e tela preta na inicializacao.
echo.

set KYTY_DISABLE_RAY_TRACING=1
"%KYTY_EXE%" --shader-bench "_PipelineCache\PPSA26344.v9.shaders" --precompile-cache "_PipelineCache\PPSA26344.bin" --bindless 1

echo.
echo ===================================================
echo   Precompilacao concluida com sucesso!
echo   Voce ja pode iniciar o jogo pelo run_yotei.bat
echo ===================================================
pause
