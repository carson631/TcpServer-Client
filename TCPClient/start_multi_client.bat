@echo off
rem Launch N instances of TCPClient for multi-client testing.
rem Usage: start_multi_client.bat [count]   default count = 2

setlocal
set COUNT=%1
if "%COUNT%"=="" set COUNT=2

rem Qt runtime DLLs live here; required when starting the exe outside Qt Creator
set PATH=D:\Qt\6.8.3\mingw_64\bin;%PATH%

set EXE=%~dp0build\Desktop_Qt_6_8_3_MinGW_64_bit_Debug\TCPClient.exe
if not exist "%EXE%" (
    echo [ERROR] exe not found: %EXE%
    echo Build the project in Qt Creator first.
    pause
    exit /b 1
)

for /L %%i in (1,1,%COUNT%) do (
    echo starting client %%i ...
    start "" "%EXE%"
    rem small gap so the OS assigns distinct windows / connections
    ping -n 2 127.0.0.1 >nul
)

echo Done. %COUNT% client window^(s^) launched.
endlocal
