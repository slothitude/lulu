# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Lulu v4.1** — a local autonomous agent with an embedded property graph database. All state (tasks, memory, sessions, goals, cache, logs) lives in a RyuGraph (Kuzu fork) graph database. The agent runs a two-thread architecture: main thread for I/O, worker thread for LLM calls and task execution. CLI + Telegram channels run simultaneously. SDL3 provides an interactive widget system with click handling.

## Build & Run

```bash
# Set toolchain (required before any build command)
PATH="/c/msys64/mingw64/bin:$PATH"

# Build agent from bashagent/build/
cd build
cmake -G "MinGW Makefiles" ..
cmake --build . --target agent
cp _deps/kuzu-build/src/libryu_shared.dll .

# Build SDL3 tool DLL (from bashagent root)
gcc -shared -std=c11 -DTOOL_BUILDING_DLL -I runtime/src/include \
    runtime/tools/sdl3_render.c runtime/src/cJSON.c runtime/src/sandbox.c \
    -o tools/sdl3_render.dll -lSDL3 -lSDL3_image

# Copy all tool DLLs + runtime deps to build/ for testing
cp tools/*.dll build/
cp libs/SDL3.dll libs/SDL3_image.dll build/

# Run
./agent.exe                    # Always-on agent (CLI + Telegram)
./agent.exe "prompt text"      # One-shot mode
./agent.exe --replay           # Log viewer
```

**Requirements**: MSYS2 MinGW64 GCC 15+, CMake 3.20+, SDL3+SDL3_image (optional), TDLib static libs in `libs/` (optional, enables Telegram).

**Note**: Only build the `agent` target. The RyuGraph shell printer has a known MinGW build issue — `cmake --build . --target agent` avoids it.

## Architecture

### Two-Thread Design

- **Main thread**: polls CLI + Telegram + SDL3 channels via `channels_poll()`, routes events to `handle_command()` / `handle_message()` / callback handlers. Never calls LLM or executes tasks.
- **Worker thread**: `agent_think()` → `decision_pick_task()` → `execute_task()` loop. Sleeps on `CONDITION_VARIABLE g_task_ready` (2s timeout) when idle.

### Graph Database (agent_db.h/c)

All persistence goes through `AgentDB g_adb` (global in main.c). The graph schema has 10 node tables (TASK, MEMORY, GOAL, SESSION, MESSAGE, TOOL_CALL, PROMPT_CACHE, FILE, SCRIPT, LOG_EVENT) and 3 rel tables (CALLED, USED, RECORDED_IN).

**RyuGraph C API uses value-by-pointer pattern**: `ryu_database_init(path, config, &db)` not pointer-return. Strings extracted via `ryu_value_get_string(&val, &str)` must be freed with `ryu_destroy_string(str)`. **Never call `ryu_value_get_string()` on INT64 columns** — use `ryu_value_get_int64()` instead or it segfaults.

### Module Delegation Pattern

`tasks.c` is a thin wrapper over `agent_db.c` — it preserves the v3 pointer-based `Task*` API using a 16-slot cache (`g_cache[]`), but every mutation writes directly to the graph. `tasks_load()` and `tasks_save()` are no-ops. `Task` is a typedef of `DbTask`.

Similarly, `session.c` maintains an in-memory linked list cache but backs everything to SESSION/MESSAGE nodes. `state.c` memory scalars live as MEMORY nodes. `llm.c` prompt cache uses PROMPT_CACHE nodes.

### Channels (channel.h/c)

Unified event queue (`AgentEvent`) for CLI stdin, Telegram, and SDL3 window events. Events carry `type` ("cli"/"telegram"/"sdl3") and `action` ("message"/"command"/"callback"/"click"). Telegram callback queries (inline keyboard buttons) and SDL3 widget clicks are routed through the same queue.

### Telegram Rich Channel (telegram.h/c)

When `ENABLE_TELEGRAM` is defined and TDLib libs are in `libs/`, the agent gets full Telegram support:
- Inline keyboard buttons via `tg_send_message_inline()` — sends `[{"text":"Label","callback_data":"value"}]` JSON rows
- Edit-in-place progress via `tg_edit_message()` and `tg_send_message_ex()` (returns msg_id)
- Callback query handling: `updateNewCallbackQuery` parsed in `handle_update()`, dequeued via `tg_get_next_callback()`
- `tg_subscriber.c` auto-sends `[Retry][Ignore]` on errors, `[New Goal][Status]` on completion, edits progress messages in-place
- **Must define `TDJSON_STATIC_DEFINE`** when linking TDLib static libs to prevent `__declspec(dllimport)` errors

### SDL3 Interactive Window (sdl3_render.c)

Retained-mode widget system. The DLL stays loaded — node tree persists across calls.

**18 widget types**: VBox, HBox, Margin, Center, Grid, Label, ColorRect, HSeparator, VSeparator, Button, CheckBox, ProgressBar, OptionButton, SpinBox, Panel, PanelContainer, ScrollContainer, TabContainer.

**11 actions**: `ui_create`, `ui_set_prop`, `ui_theme_set`, `ui_connect`, `ui_destroy`, `ui_render_frame`, `ui_get_tree`, `ui_clear`, `ui_window_open`, `ui_window_close`, `ui_window_update`.

**Interactive mode**: `ui_window_open` spawns a window thread at 60fps. Clicks hit-test the widget tree, lookup `ui_connect` signals, and enqueue events via exported `sdl3_window_poll()`. `tools.c` resolves this via `GetProcAddress` and wires it to `channels_set_sdl3_poll()`. Events arrive as `AgentEvent` with `type="sdl3"`, `action="click"`.

**Layout**: Two-pass — bottom-up `ui_compute_min_size()` then top-down `ui_compute_layout()`. Size flags: FILL=0, EXPAND=1, SHRINK_CENTER=2.

**Props key names**: `range_min`/`range_max`/`range_value` (not min_value/max_value/value).

### Tool DLL System

Tools are runtime-loaded DLLs from `tools/`. Each exports `tool_get_info()` and `tool_get_execute()`. All file operations go through `sandbox_resolve_path()` which blocks path traversal. ABI defined in `runtime/src/include/tool_api.h`.

### Decision Engine (decision_engine.h/c)

Scored task selection: `score = 2.0*priority + 1.5*age_bonus - 2.0*(attempts*1.5)`. 10% epsilon-greedy random exploration.

### Thread Safety

Six `CRITICAL_SECTION` locks: `g_state_lock` (memory stats), `g_tasks_lock` (tasks.c), `g_session_lock` (session.c), `g_queue_lock` (channel.c), `g_adb._lock` (graph DB), plus the SDL3 event queue uses `InterlockedExchange` for lock-free SPSC. `CONDITION_VARIABLE g_task_ready` wakes worker on `/goal`. Lock ordering must be consistent to prevent deadlock.

## Key Patterns

- **No Cypher outside agent_db.c** — callers use C functions, not raw queries
- **Cypher escaping**: use `\'` for single quotes (not `''`). Replace `{`/`}` with `(`/`)` inside Cypher string literals
- **Portable asprintf** — `port_asprintf()` in main.c (MSVC lacks `vasprintf`)
- **Tool result truncation** — `MAX_TOOL_RESULT_CHARS=4096` prevents context explosion
- **Schema changes require fresh DB**: `rm -rf state/graph.kuzu`
- **v3 migration** — `agent_db_needs_migration()` imports JSON files, renames to `.v3.bak`

## RyuGraph MinGW Patches

RyuGraph v25.9.2 requires patches in `build/_deps/kuzu-src/` after FetchContent download:
1. `src/include/c_api/helpers.h` + `src/c_api/helpers.cpp` — add `#include <cstdint>`
2. `src/common/file_system/local_file_system.cpp` — add `#include <sys/stat.h>`
3. `src/src/CMakeLists.txt` — add `ws2_32` to WIN32 RYU_LIBRARIES
4. `src/storage/buffer_manager/buffer_manager.cpp` — guard SEH with `_MSC_VER`
5. `third_party/glob/glob/glob.hpp` — use `getenv` instead of `_dupenv_s` for non-MSVC

These patches survive cmake reconfigure but NOT `_deps` folder deletion.

## Link Order

In CMakeLists.txt, system libs (`ssl`, `crypto`, `z`, `ws2_32`, etc.) must come **after** TDLib static libs, since TDLib depends on them. The link order is: RyuGraph → TDLib → system libs.

## Status

All phases complete:
- v4 Phases 1-5: Graph database core, tool recording, semantic memory, script mining, introspection
- SDL3 UI Widget System: 18 widgets, 11 actions, offscreen PNG + interactive window
- Telegram Rich Channel: inline keyboards, edit-in-place, callback handling
- Interactive SDL3 Window: click handling, hit-test, event routing to agent
