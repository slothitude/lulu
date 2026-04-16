# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Lulu v4.0** — a local autonomous agent with an embedded property graph database. All state (tasks, memory, sessions, goals, cache, logs) lives in a RyuGraph (Kuzu fork) graph database. The agent runs a two-thread architecture: main thread for I/O, worker thread for LLM calls and task execution.

## Build & Run

```bash
# Set toolchain (required before any build command)
PATH="/c/msys64/mingw64/bin:$PATH"

# Build from bashagent/build/
cd build
cmake -G "MinGW Makefiles" ..
cmake --build .
cp _deps/kuzu-build/src/libryu_shared.dll .

# Run
./agent.exe                    # Always-on agent (CLI + Telegram)
./agent.exe "prompt text"      # One-shot mode
./agent.exe --replay           # Log viewer

# Tool DLLs (compiled separately by run.bat)
run.bat --build
```

**Requirements**: MSYS2 MinGW64 GCC 15+, CMake 3.20+, SDL3+SDL3_image (optional), TDLib (optional).

## Architecture

### Two-Thread Design

- **Main thread**: polls CLI + Telegram channels, routes events to `handle_command()` / `handle_message()`. Never calls LLM or executes tasks.
- **Worker thread**: `agent_think()` → `decision_pick_task()` → `execute_task()` loop. Sleeps on `CONDITION_VARIABLE g_task_ready` (2s timeout) when idle.

### Graph Database (agent_db.h/c)

All persistence goes through `AgentDB g_adb` (global in main.c). The graph schema has 10 node tables (TASK, MEMORY, GOAL, SESSION, MESSAGE, TOOL_CALL, PROMPT_CACHE, FILE, SCRIPT, LOG_EVENT) and 3 rel tables (CALLED, USED, RECORDED_IN).

**RyuGraph C API uses value-by-pointer pattern**: `ryu_database_init(path, config, &db)` not pointer-return. Strings extracted via `ryu_value_get_string(&val, &str)` must be freed with `ryu_destroy_string(str)`.

### Module Delegation Pattern

`tasks.c` is a thin wrapper over `agent_db.c` — it preserves the v3 pointer-based `Task*` API using a 16-slot cache (`g_cache[]`), but every mutation writes directly to the graph. `tasks_load()` and `tasks_save()` are no-ops. `Task` is a typedef of `DbTask`.

Similarly, `session.c` maintains an in-memory linked list cache but backs everything to SESSION/MESSAGE nodes. `state.c` memory scalars live as MEMORY nodes. `llm.c` prompt cache uses PROMPT_CACHE nodes.

### Decision Engine (decision_engine.h/c)

Scored task selection replaces FIFO: `score = 2.0*priority + 1.5*age_bonus - 2.0*(attempts*1.5)`. 10% epsilon-greedy random exploration. `decision_learn(task_id, success)` called after each task execution.

### Tool DLL System

Tools are runtime-loaded DLLs from `tools/`. Each exports `tool_get_info()` and `tool_get_execute()`. All file operations go through `sandbox_resolve_path()` which blocks path traversal. ABI defined in `runtime/src/include/tool_api.h`.

### SDL3 UI Widget System (`runtime/tools/sdl3_render.c`)

A retained-mode widget system rendered via SDL3 software renderer to PNG. The DLL stays loaded between calls so the node tree persists across tool invocations. The LLM drives the UI declaratively via sequential `ui_*` JSON actions.

**Build**: `gcc -shared -std=c11 -DTOOL_BUILDING_DLL -I runtime/src/include runtime/tools/sdl3_render.c runtime/src/cJSON.c runtime/src/sandbox.c -o tools/sdl3_render.dll -lSDL3 -lSDL3_image`

**18 widget types**: VBoxContainer, HBoxContainer, MarginContainer, CenterContainer, GridContainer, Label, ColorRect, HSeparator, VSeparator, Button, CheckBox, ProgressBar, OptionButton, SpinBox, Panel, PanelContainer, ScrollContainer, TabContainer.

**Actions**:
| Action | Purpose |
|--------|---------|
| `ui_create` | Create widget, returns `node_id` |
| `ui_set_prop` | Set property (min_w, min_h, flag_h, flag_v, etc.) |
| `ui_theme_set` | Override node style (bg_color, border_color, corner_radius) |
| `ui_connect` | Register signal (passive — stored for future interactive mode) |
| `ui_destroy` | Remove node + subtree |
| `ui_render_frame` | Layout + render to PNG (requires `path`, `width`, `height`) |
| `ui_get_tree` | JSON dump of full node tree |
| `ui_clear` | Reset entire tree |

Legacy `render`/`window`/`info` actions preserved for backward compat.

**Layout**: Two-pass — bottom-up `ui_compute_min_size()` then top-down `ui_compute_layout()`. Size flags: FILL=0 (default), EXPAND=1, SHRINK_CENTER=2.

**Rendering**: Dark theme with `SDL_RenderDebugText` (no TTF). Canvas bg `(0.10,0.10,0.12)`. Per-node `StyleBoxFlat` controls bg_color, border_color, border_width, corner_radius, padding.

**Node pool**: 256 max nodes, 64 max children per node. Static `g_nodes[]` array.

### Thread Safety

Five `CRITICAL_SECTION` locks: `g_state_lock` (memory stats), `g_tasks_lock` (tasks.c), `g_session_lock` (session.c), `g_queue_lock` (channel.c), `g_adb._lock` (graph DB). `CONDITION_VARIABLE g_task_ready` wakes worker on `/goal`.

### Channels

`channel.h/c` provides a unified event queue (`AgentEvent`) for both CLI stdin and Telegram input. No mode switching — both polled in the same loop.

## Key Patterns

- **No Cypher outside agent_db.c** — callers use C functions, not raw queries
- **Portable asprintf** — `port_asprintf()` in main.c (MSVC lacks `vasprintf`)
- **Tool result truncation** — `MAX_TOOL_RESULT_CHARS=4096` prevents context explosion
- **Ring buffer history** — `ChatMessage history[64]` shifts on overflow, not circular index
- **v3 migration** — `agent_db_needs_migration()` checks empty graph, imports JSON files, renames to `.v3.bak`

## v4 Status

- Phase 1: Graph database core — **complete**
- Phase 2: Tool call recording, embeddings — **complete**
- Phase 3: Vector search, semantic memory — **complete**
- Phase 4: Script mining, replay — **complete**
- Phase 5: History, diff, graph introspection — **complete**
- SDL3 UI Widget System — **complete** (18 widgets, 8 actions, offscreen PNG rendering)

## RyuGraph MinGW Patches

RyuGraph v25.9.2 requires patches in `build/_deps/kuzu-src/` after FetchContent download:
1. `src/include/c_api/helpers.h` + `src/c_api/helpers.cpp` — add `#include <cstdint>`
2. `src/common/file_system/local_file_system.cpp` — add `#include <sys/stat.h>`
3. `src/src/CMakeLists.txt` — add `ws2_32` to WIN32 RYU_LIBRARIES
4. `src/storage/buffer_manager/buffer_manager.cpp` — guard SEH with `_MSC_VER`
5. `third_party/glob/glob/glob.hpp` — use `getenv` instead of `_dupenv_s` for non-MSVC

These patches survive cmake reconfigure but NOT `_deps` folder deletion.
