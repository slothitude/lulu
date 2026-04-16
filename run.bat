@echo off
setlocal

echo ========================================
echo  Lulu v4.1.0 — Graph-Native Autonomous Agent
echo ========================================
echo.

set DO_BUILD=0
for %%a in (%*) do (
    if "%%a"=="--build" set DO_BUILD=1
)
if not exist "runtime\build\agent.exe" set DO_BUILD=1

if "%DO_BUILD%"=="1" (
    echo [BUILD] CMake + RyuGraph...
    if not exist "runtime\build" mkdir "runtime\build"
    if not exist "tools" mkdir "tools"

    set PATH=C:\msys64\mingw64\bin;%PATH%

    REM === Build tool DLLs (auto-discovers all .c in runtime/tools/) ===
    echo [BUILD] Compiling tool DLLs...

    for %%f in (runtime\tools\*.c) do (
        echo [BUILD]   %%~nf.dll
        gcc -shared -std=c11 -D_CRT_SECURE_NO_WARNINGS -DTOOL_BUILDING_DLL ^
            -I runtime\src\include ^
            %%f runtime\src\cJSON.c runtime\src\sandbox.c ^
            -o tools\%%~nf.dll -lSDL3 -lSDL3_image
    )

    REM === CMake build ===
    cd runtime\build
    cmake -G "MinGW Makefiles" ../..
    cmake --build .
    cd ..\..

    echo [BUILD] Done.
    echo.
)

echo [RUN] Starting agent...
echo.
runtime\build\agent.exe %*

echo.
echo ========================================
echo  Agent finished.
echo ========================================

endlocal
