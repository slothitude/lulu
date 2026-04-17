# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Lulu v5.0** — a local autonomous agent with a three-stage planner/actor/critic intelligence pipeline, embedded property graph database, HNSW-accelerated semantic memory, SSE streaming with token tracking, and simultaneous CLI + Telegram channels. Written in C11. All state (tasks, memory, sessions, goals, cache, logs) lives in a RyuGraph (Kuzu fork) graph database.

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
- **Worker thread**: `agent_think()` → `decision_pick_task()` → `execute_task()` loop. Sleeps on `CONDITION_VARIABLE g_task_ready` (2s timeout) when idle. `execute_task()` runs a three-stage pipeline: Planner → Actor → Critic (with up to `MAX_CRITIC_RETRIES=2` retries on critic rejection). Falls back to legacy flat LLM loop if pipeline roles are disabled in `agent.json`.

### Planner/Actor/Critic Pipeline (main.c, llm.c, agent_core.c)

Tasks run through three reasoning stages, each with its own system prompt from `agent.json` roles:

1. **Planner** — `llm_chat_with_role("planner", ...)` breaks the goal into concrete steps using rich context gathered by `llm_build_context()` (goal, top-3 semantic memories, top-1 script match, last 5 related tasks). `MAX_CONTEXT_CHARS=12288`.
2. **Actor** — `llm_chat_with_role("actor", ...)` executes tools step-by-step. Output uses structured `<tool_call name="..." args="{...}"/>` tags parsed by `llm_parse_tool_call()` (falls back to regex).
3. **Critic** — `llm_chat_with_role("critic", ...)` evaluates results. Returns structured JSON: `{"status":"done|revise|continue","confidence":0.95,"summary":"..."}`. Triggers revision up to `MAX_CRITIC_RETRIES=2`.

`agent_config_get_role(ac, "planner"|"actor"|"critic")` returns the `PromptTemplate*` for each stage. `agent_core.c` provides `agent_build_prompt()` utilities.

### Graph Database (agent_db.h/c)

All persistence goes through `AgentDB g_adb` (global in main.c). The graph schema has 10 node tables (TASK, MEMORY, GOAL, SESSION, MESSAGE, TOOL_CALL, PROMPT_CACHE, FILE, SCRIPT, LOG_EVENT) and 3 rel tables (CALLED, USED, RECORDED_IN).

**RyuGraph C API uses value-by-pointer pattern**: `ryu_database_init(path, config, &db)` not pointer-return. Strings extracted via `ryu_value_get_string(&val, &str)` must be freed with `ryu_destroy_string(str)`. **Never call `ryu_value_get_string()` on INT64 columns** — use `ryu_value_get_int64()` instead or it segfaults.

**HNSW-lite index**: MEMORY embeddings are indexed with an in-memory HNSW adjacency list (`HNSW_M=16`). `agent_db_memory_build_index()` builds lazily on first search, `agent_db_memory_invalidate_index()` on mutations. Search is O(log n) when index valid, falls back to full scan. Globals: `g_hnsw_nodes`, `g_hnsw_count`, `g_hnsw_valid` (all static in agent_db.c, defined at file top before first use).

**Smart compaction**: `agent_db_compact_smart(adb, max_age_days, similarity_threshold)` merges similar memories (cosine sim > threshold) before deletion, then runs standard compact.

### Module Delegation Pattern

`tasks.c` is a thin wrapper over `agent_db.c` — it preserves the v3 pointer-based `Task*` API using a 16-slot cache (`g_cache[]`), but every mutation writes directly to the graph. `tasks_load()` and `tasks_save()` are no-ops. `Task` is a typedef of `DbTask`.

Similarly, `session.c` maintains an in-memory linked list cache but backs everything to SESSION/MESSAGE nodes. `state.c` memory scalars live as MEMORY nodes. `llm.c` prompt cache uses PROMPT_CACHE nodes. `agent_core.c` provides prompt building utilities shared by the pipeline.

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

**19 widget types**: VBox, HBox, Margin, Center, Grid, Label, ColorRect, HSeparator, VSeparator, Button, CheckBox, ProgressBar, OptionButton, SpinBox, Panel, PanelContainer, ScrollContainer, TabContainer, **TextInput**.

**Keyboard navigation**: Tab/Shift+Tab cycles focus through connected widgets. Enter triggers Button/TextInput submit. Space toggles CheckBox. Focus tracked via `g_focused_node` and `UiNode.flags` bit 0. Focus ring visual (border highlight). TextInput has blinking cursor, backspace support, placeholder text.

**Resizable windows**: `SDL_WINDOW_RESIZABLE` flag set on creation. `ui_window_update` accepts `width`/`height` props via `SDL_SetWindowSize()`. `SDL_EVENT_WINDOW_RESIZED` triggers layout recalculation.

**11 actions**: `ui_create`, `ui_set_prop`, `ui_theme_set`, `ui_connect`, `ui_destroy`, `ui_render_frame`, `ui_get_tree`, `ui_clear`, `ui_window_open`, `ui_window_close`, `ui_window_update`.

**Interactive mode**: `ui_window_open` spawns a window thread at 60fps. Clicks hit-test the widget tree, lookup `ui_connect` signals, and enqueue events via exported `sdl3_window_poll()`. `tools.c` resolves this via `GetProcAddress` and wires it to `channels_set_sdl3_poll()`. Events arrive as `AgentEvent` with `type="sdl3"`, `action="click"`.

**Layout**: Two-pass — bottom-up `ui_compute_min_size()` then top-down `ui_compute_layout()`. Size flags: FILL=0, EXPAND=1, SHRINK_CENTER=2.

**Props key names**: `range_min`/`range_max`/`range_value` (not min_value/max_value/value).

### Tool DLL System

Tools are runtime-loaded DLLs from `tools/`. Each exports `tool_get_info()` and `tool_get_execute()`. All file operations go through `sandbox_resolve_path()` which blocks path traversal. ABI defined in `runtime/src/include/tool_api.h`.

7 DLL tools: `create_file`, `read_file`, `list_files`, `run_test`, `sdl3_render`, `telegram_send`, `none`. Plus the built-in `replay_script` pseudo-tool.

### Decision Engine (decision_engine.h/c)

Scored task selection: `score = 2.0*priority + 1.5*age_bonus - 2.0*(attempts*1.5)`. 10% epsilon-greedy random exploration.

**Decision learning**: `decision_learn()` adjusts scoring weights based on task outcomes:
- Extracts tool sequences from completed tasks via TOOL_CALL records
- Stores weights as MEMORY nodes (category="decision_weights"): `{"tool_seq":"create_file,read_file","weight":1.3,"samples":5}`
- Exponential moving average update (alpha=0.3) with decay over time
- Weights clamped to [0.1, 3.0], influence `decision_pick_task()` scoring
- Loaded from graph at startup via `load_weights_from_graph()`

**Script replay**: `replay_script` pseudo-tool in `execute_tool()` re-executes stored SCRIPT tool sequences. Planner receives script match via `llm_build_context()` and can choose to replay.

### Thread Safety

Six `CRITICAL_SECTION` locks: `g_state_lock` (memory stats), `g_tasks_lock` (tasks.c), `g_session_lock` (session.c), `g_queue_lock` (channel.c), `g_adb._lock` (graph DB), plus the SDL3 event queue uses `InterlockedExchange` for lock-free SPSC. `CONDITION_VARIABLE g_task_ready` wakes worker on `/goal`. Lock ordering must be consistent to prevent deadlock.

## LLM Layer (llm.h/c)

### SSE Streaming

`llm_chat_stream(prompt, max_retries, on_token_fn, user_data)` opens an SSE connection and fires `on_token_fn(token, user_data)` per token. Falls back to standard JSON response on parse error. Main thread shows tokens in CLI as they arrive.

### Token Usage Tracking

`track_usage()` parses `usage` from LLM responses (`prompt_tokens`, `completion_tokens`), accumulates in static `g_token_usage` (type `LLMTokenUsage`). `/status` displays cumulative counts.

### Multi-Provider Routing

`llm_add_provider(name, endpoint, api_key, model)` registers up to 8 named providers. `llm_set_role_provider(role, provider_name)` routes pipeline stages to different providers (e.g., powerful model for planning, fast model for actor/critic). `find_provider(role)` resolves at call time.

## Testing (runtime/tests/)

26 automated tests across 5 suites:

| Suite | File | Tests |
|-------|------|-------|
| Sandbox | `test_sandbox.c` | 17 — path traversal, UNC, null bytes, device names, case bypass |
| Tools | `test_tools.c` | 6 — cJSON parsing, JSON edge cases |
| AgentDB | `test_agent_db.c` | 1 stub (needs RyuGraph runtime) |
| Channel | `test_channel.c` | 1 stub (needs full runtime) |
| Tasks | `test_tasks.c` | 1 stub (needs full runtime) |

Build: `gcc -std=c11 -I../src/include -o test_runner.exe test_runner.c test_sandbox.c test_tools.c test_agent_db.c test_channel.c test_tasks.c ../src/sandbox.c ../src/cJSON.c`

Test harness: `test_harness.h` provides `TEST(name)`, `ASSERT(cond)`, `ASSERT_EQ(a,b)`, `ASSERT_STR(a,b)`, `ASSERT_NULL(p)`, `ASSERT_NOT_NULL(p)` macros. Shared counters (`g_tests_run`, `g_tests_passed`, `g_tests_failed`) are `extern` in header, defined in `test_runner.c`.

CI: `.github/workflows/ci.yml` builds agent + test runner on push.

## Security (sandbox.c)

- Path traversal (`..`, absolute paths, system dirs) blocked
- UNC path escape (`\\?\`, `\\server`) blocked
- Long path prefix (`\\?\C:\...`) bypass blocked
- Null byte injection in filenames blocked
- Windows device names (CON, PRN, AUX, NUL, COM1-COM9, LPT1-LPT9) blocked
- Case-insensitive workspace prefix check on Windows (`_strnicmp`)
- `/graph` restricted to read-only MATCH/RETURN queries
- No arbitrary command execution; tool DLLs have no LLM access

## Key Patterns

- **No Cypher outside agent_db.c** — callers use C functions, not raw queries
- **Cypher escaping**: use `\'` for single quotes (not `''`). Replace `{`/`}` with `(`/`)` inside Cypher string literals
- **Portable asprintf** — `port_asprintf()` in main.c (MSVC lacks `vasprintf`)
- **Tool result truncation** — `MAX_TOOL_RESULT_CHARS=4096` prevents context explosion
- **Pipeline constants**: `MAX_CRITIC_RETRIES=2`, `MAX_CONTEXT_CHARS=12288`
- **HNSW globals must be defined at file top** in agent_db.c — `HNSWNode`, `g_hnsw_nodes`, `cosine_sim_hnsw`, `hnsw_free` all before `agent_db_memory_search()` first use
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
- v5 Phase 6: Planner/Actor/Critic pipeline with structured tool calls, context management
- v5 Phase 7: Decision learning with exponential moving average weights, script auto-replay
- v5 Phase 8: SDL3 TextInput widget (19th), keyboard navigation, resizable windows
- v5 Phase 9: HNSW-lite index for O(log n) embedding search, smart memory compaction
- v5 Phase 10: 26 automated tests, sandbox hardening (UNC/null bytes/device names), CI workflow
- v5 Phase 11: SSE streaming, token usage tracking, multi-provider routing
