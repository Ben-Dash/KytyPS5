@echo off
setlocal enabledelayedexpansion
title KytyPS5 - Ghost of Yotei Runner

echo ===================================================
echo   KytyPS5 - Ghost of Yotei (PPSA26344) Launcher
echo ===================================================
echo.

:: 1. Localizar o executavel do emulador
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
    echo Compile o projeto primeiro ou copie o executavel para a pasta de runtime.
    pause
    exit /b 1
)

:: 2. Localizar a pasta do jogo
if "%YOTEI_GAME_PATH%"=="" (
    if exist "F:\GOI\PPSA26344-app" (
        set "GAME_PATH=F:\GOI\PPSA26344-app"
    ) else (
        set "GAME_PATH=F:\GOI\PPSA26344-app"
    )
) else (
    set "GAME_PATH=%YOTEI_GAME_PATH%"
)

echo Executavel: %KYTY_EXE%
echo Jogo:       %GAME_PATH%
echo Diretorio:  %RUN_DIR%
echo.

cd /d "%RUN_DIR%"

:: 3. Variaveis de ambiente para Ghost of Yotei
set KYTY_COMPUTE_CHAIN=1
set KYTY_READBACK_QUEUE=0
set KYTY_READBACK_TARGETED_PUBLICATION=1
set KYTY_GPU_PALETTE_SANITIZE=0
set KYTY_BINDLESS_KEY_BOUNDS=1
set KYTY_BINDLESS_EAGER=1
set KYTY_BINDLESS_TSHARP_CHECK=1
set KYTY_AVPLAYER_RECYCLE=1
set KYTY_PRESENT_WHEN_READY=1

:: 4. Executar emulador
"%KYTY_EXE%" --game "%GAME_PATH%" --screen-width 1920 --screen-height 1080 --game-preset-priority Performance --gpu-frames-ahead 0 --gpu-timestamp-scale 200 --label-flush-interval-us 2000 --async-pipelines false --defer-dispatches false --relaxed-readback false --speculative-draws true --record-thread true --amd-cpu

echo.
echo ===================================================
echo   O emulador foi finalizado.
echo ===================================================
pause
