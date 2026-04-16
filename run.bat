@echo off
setlocal

echo ========================================
echo  Lulu v3 — Always-On Autonomous Agent
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
            -o tools\%%~nf.dll -lSDL3 -lSDL3_image
    )

    REM === Build host ===
    echo [BUILD] Compiling host...

    cd runtime\src

    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c cJSON.c -o ..\build\cJSON.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c sandbox.c -o ..\build\sandbox.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c tools.c -o ..\build\tools.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c llm.c -o ..\build\llm.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c state.c -o ..\build\state.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c agent_config.c -o ..\build\agent_config.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c event_bus.c -o ..\build\event_bus.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c channel.c -o ..\build\channel.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c tasks.c -o ..\build\tasks.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c session.c -o ..\build\session.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c decision_engine.c -o ..\build\decision_engine.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c subscribers\log_subscriber.c -o ..\build\log_subscriber.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c subscribers\mem_subscriber.c -o ..\build\mem_subscriber.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c subscribers\sdl3_debugger.c -o ..\build\sdl3_debugger.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -I include -c subscribers\tg_subscriber.c -o ..\build\tg_subscriber.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -DENABLE_TELEGRAM -DTDJSON_STATIC_DEFINE -I include -c telegram.c -o ..\build\telegram.o
    gcc -std=c11 -D_CRT_SECURE_NO_WARNINGS -DENABLE_TELEGRAM -I include -c main.c -o ..\build\main.o

    REM === Link with g++ for TDLib C++ support ===
    g++ ..\build\main.o ..\build\sandbox.o ..\build\tools.o ^
        ..\build\llm.o ..\build\state.o ..\build\agent_config.o ^
        ..\build\event_bus.o ..\build\channel.o ..\build\tasks.o ..\build\session.o ^
        ..\build\decision_engine.o ^
        ..\build\log_subscriber.o ..\build\mem_subscriber.o ^
        ..\build\sdl3_debugger.o ..\build\tg_subscriber.o ..\build\telegram.o ^
        ..\build\cJSON.o ^
        -o ..\build\agent.exe ^
        -L..\..\libs ^
        -ltdjson_static -ltdjson_private -ltdclient -ltdcore ^
        -ltde2e -ltdmtproto -ltddb -ltdnet -ltdactor ^
        -ltdutils -ltdsqlite -ltdtl -ltdapi ^
        -lssl -lcrypto -lz -lws2_32 -lwinhttp ^
        -lpsapi -liphlpapi -lbcrypt -lcrypt32 -luserenv -lncrypt -lcrypto

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
