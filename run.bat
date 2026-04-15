@echo off
setlocal

echo ========================================
echo  BashAgent Hybrid Runtime v1
echo ========================================
echo.

REM Build if needed or if --build flag passed
set DO_BUILD=0
for %%a in (%*) do (
    if "%%a"=="--build" set DO_BUILD=1
)
if not exist "runtime\build\agent.exe" set DO_BUILD=1

if "%DO_BUILD%"=="1" (
    echo [BUILD] Compiling runtime...
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
            -o tools\%%~nf.dll
    )

    REM === Build host ===
    echo [BUILD] Compiling host...

    cd runtime\src
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c cJSON.c -o ..\build\cJSON.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c sandbox.c -o ..\build\sandbox.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c tools.c -o ..\build\tools.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c llm.c -o ..\build\llm.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c state.c -o ..\build\state.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c agent_config.c -o ..\build\agent_config.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c agent_core.c -o ..\build\agent_core.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c planner.c -o ..\build\planner.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c actor.c -o ..\build\actor.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c critic.c -o ..\build\critic.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include ^
        -c main.c -o ..\build\main.o

    gcc ..\build\main.o ..\build\sandbox.o ..\build\tools.o ^
        ..\build\llm.o ..\build\state.o ..\build\agent_config.o ^
        ..\build\agent_core.o ..\build\planner.o ..\build\actor.o ^
        ..\build\critic.o ..\build\cJSON.o -o ..\build\agent.exe -lwinhttp

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
