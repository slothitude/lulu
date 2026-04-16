<div align="center">
  <img src="lulu_logo.jpg" alt="Lulu Logo" width="200">
  <h1>Lulu v4.1 — Graph-Native Autonomous Agent</h1>
</div>

Local autonomous agent with an embedded property graph database, DLL-loaded tools, persistent tasks, and simultaneous CLI + Telegram channels. Written in C11.

**v4.0** replaced all flat-file storage with an embedded RyuGraph (Kuzu fork) property graph database. Every action, memory, tool use, and decision becomes a permanent traversable node. **v4.1** adds a standalone updater, `/update` command, NSIS installer, and CI/CD release pipeline.

## Quick Start

```bash
# Build (from bashagent/build/)
cmake -G "MinGW Makefiles" ..
cmake --build .
cp _deps/kuzu-build/src/libryu_shared.dll .

# Run
./agent.exe                    # Always-on agent — CLI + Telegram
./agent.exe "list the files"   # One-shot prompt
./agent.exe --replay           # Log viewer
./agent.exe --replay --last 20 --stage actor
```

## Architecture

```
┌─────────────────────────────────────────────────────┐
│              Two-Thread Architecture                 │
│                                                      │
│  Main Thread (I/O)          Worker Thread            │
│  ┌──────────────┐           ┌──────────────┐        │
│  │ poll channels │           │ agent_think() │        │
│  │  CLI + TG     │           │ decision_pick │        │
│  │ route events  │           │ execute_task() │       │
│  │ → command     │           │ session prune  │       │
│  │ → chat+tools  │           │ Sleep on CV    │       │
│  └──────────────┘           └──────────────┘        │
│                                                      │
│  ┌──────────────────────────────────────────────┐   │
│  │          RyuGraph Property Graph              │   │
│  │                                               │   │
│  │  Nodes: TASK  MEMORY  GOAL  SESSION  MESSAGE │   │
│  │         TOOL_CALL  PROMPT_CACHE  FILE  SCRIPT │   │
│  │         LOG_EVENT                             │   │
│  │                                               │   │
│  │  Rels:  CALLED  USED  RECORDED_IN             │   │
│  └──────────────────────────────────────────────┘   │
│                                                      │
│  Channels:  CLI (stdin) + Telegram (TDLib)           │
│  Tools:     DLL system (runtime-loaded)              │
│  LLM:       OpenAI-compatible (WinHTTP)              │
└─────────────────────────────────────────────────────┘
```

### Graph Schema

All state lives in an embedded property graph. No flat JSON files.

| Node Table | Key Fields | Purpose |
|------------|-----------|---------|
| TASK | id, name, prompt, status, priority, plan, state | Autonomous tasks |
| MEMORY | id, category, key, content, val | Working memory scalars |
| GOAL | id, text, status | Active goals |
| SESSION | id, chat_id, channel | Per-chat sessions |
| MESSAGE | id, session_id, role, content, seq | Chat history |
| TOOL_CALL | id, tool, args_json, result_json, duration_ms | Tool invocations |
| PROMPT_CACHE | hash, response, hit_count | LLM response cache |
| FILE | id, path, hash, size | File tracking |
| SCRIPT | id, name, tool_sequence | Mined workflows |
| LOG_EVENT | id, type, stage, json_data, success | Execution log |

| Rel Table | From → To | Purpose |
|-----------|-----------|---------|
| CALLED | TASK → TOOL_CALL | Task tool invocations |
| USED | TASK → FILE | Files touched by task |
| RECORDED_IN | LOG_EVENT → TASK | Log event linkage |

### execute_task() — Structured Autonomy

Tasks run through internal phases:

1. **Plan** — LLM generates approach using task prompt + previous state
2. **Act** — Tool execution loop (capped at 8 tool steps)
3. **Evaluate** — Check if task is done or needs retry

Each task carries rolling `state`, `last_error`, and `plan` fields so retries aren't blind.

### Decision Engine — Scored Task Scheduling

`decision_pick_task()` replaces simple FIFO with scored selection:
- **Priority** (2x weight), **age bonus** (1.5x, caps at 5min), **failure penalty** (1.5x per attempt)
- **10% epsilon-greedy exploration** — random selection to avoid local optima
- `decision_learn(task_id, success)` updates scoring after execution

### v3 → v4 Migration

On first run, v4 automatically imports data from v3 flat files:
- `state/tasks.json` → TASK nodes
- `state/memory.json` → MEMORY nodes
- `state/prompt_cache.json` → PROMPT_CACHE nodes

Original files are renamed to `.v3.bak`. Migration only runs once (skipped if graph has data).

## Directory Structure

```
bashagent/
├── CMakeLists.txt              # CMake build (FetchContent for RyuGraph)
├── run.bat                     # Build + run entrypoint
├── config.json                 # LLM credentials (not committed)
├── agent.json                  # Agent behavior config
├── lulu_logo.jpg               # Logo
├── runtime/
│   ├── src/
│   │   ├── main.c             # Core loop, message handling, one-shot
│   │   ├── agent_db.c         # Graph storage layer (RyuGraph)
│   │   ├── channel.c          # Unified CLI + Telegram input
│   │   ├── tasks.c            # Task system (delegates to agent_db)
│   │   ├── session.c          # Per-chat history (graph-backed)
│   │   ├── llm.c              # OpenAI-compatible HTTP client (WinHTTP)
│   │   ├── tools.c            # DLL tool loader
│   │   ├── sandbox.c          # Path traversal protection
│   │   ├── state.c            # Memory, goals, logging (graph-backed)
│   │   ├── decision_engine.c  # Scored task scheduler
│   │   ├── agent_config.c     # JSON config loader
│   │   ├── event_bus.c        # Synchronous pub/sub
│   │   ├── telegram.c         # TDLib JSON wrapper
│   │   ├── cJSON.c            # Vendored JSON parser
│   │   ├── include/
│   │   │   ├── agent_db.h     # Graph storage API
│   │   │   ├── version.h     # Version constants (single source of truth)
│   │   │   ├── tasks.h        # Task types (DbTask alias)
│   │   │   └── ...
│   │   ├── subscribers/       # Event bus subscribers (log, mem, SDL3, TG)
│   │   └── tools/             # Tool DLL sources
│   └── tools/                 # Built DLLs
├── updater/
│   └── updater.c              # Standalone updater (WinHTTP, GitHub Releases)
├── installer/
│   └── lulu.nsi               # NSIS installer script
├── scripts/
│   └── package.sh             # Local build + package script
├── .github/
│   └── workflows/
│       └── release.yml        # CI/CD: build + release on tag push
├── build/                      # CMake build directory
│   ├── agent.exe              # Built binary
│   ├── updater.exe            # Built updater
│   └── libryu_shared.dll      # RyuGraph shared library
├── tools/                     # Runtime-loaded tool DLLs
├── workspace/                 # Sandboxed file operations root
└── state/
    ├── graph.kuzu             # Property graph database
    └── log.jsonl              # Execution log (JSONL, also in graph)
```

## Commands (work from any channel)

| Command | Action |
|---------|--------|
| `/stop` | Shutdown agent |
| `/clear` | Clear this chat's history |
| `/status` | Show uptime, steps, messages, tasks, graph stats |
| `/files` | List workspace files |
| `/tasks` | Show all tasks with status |
| `/decide` | Show last decision engine debug info |
| `/goal <text>` | Create a high-priority autonomous task |
| `/graph <cypher>` | Run read-only Cypher query on the graph |
| `/update` | Check GitHub for new version |
| `/update confirm` | Apply pending update, restart agent |

## Configuration

### config.json — LLM Credentials

```json
{
  "provider": "openai",
  "model": "glm-5.1",
  "endpoint": "https://your-llm-endpoint/chat/completions",
  "apikey": "your-key",
  "max_iterations": 10,
  "http_timeout_ms": 30000
}
```

### agent.json — Agent Behavior

```json
{
  "shared": {
    "tools_list": "auto-populated from loaded DLLs"
  },
  "tools": [
    { "name": "create_file", "enabled": true },
    { "name": "read_file",   "enabled": true },
    { "name": "list_files",  "enabled": true }
  ],
  "behavior": {
    "max_iterations": 10,
    "enable_prompt_cache": true,
    "enable_telegram": true,
    "telegram_bot_token": "your-bot-token",
    "telegram_chat_id": "123456789"
  }
}
```

## Tool System

Tools are loaded as DLLs from the `tools/` directory at runtime. No recompilation needed to add or remove tools.

### Tool ABI

Each DLL exports two functions:

```c
const ToolInfo *tool_get_info(void);       // name + description
tool_execute_fn tool_get_execute(void);     // cJSON *(*fn)(cJSON *args, const char *workspace, char **err)
```

### Built-in Tools

| DLL | Tool | Purpose |
|-----|------|---------|
| `create_file.dll` | `create_file(path, content)` | Write file to workspace |
| `read_file.dll` | `read_file(path)` | Read file from workspace |
| `list_files.dll` | `list_files()` | List workspace contents |
| `run_test.dll` | `run_test()` | Parse test_results.txt |
| `sdl3_render.dll` | `sdl3_render(action, ...)` | Render shapes/text to images, build widget UIs to PNG |
| `none.dll` | `none(reason)` | No-op |
| `telegram_send.dll` | `telegram_send(text)` | Send via Telegram |

### SDL3 Widget System

`sdl3_render.dll` includes a retained-mode widget system for building dashboards and task management UIs. The DLL stays loaded between calls so the widget tree persists across tool invocations.

**18 widgets**: VBoxContainer, HBoxContainer, MarginContainer, CenterContainer, GridContainer, Label, ColorRect, HSeparator, VSeparator, Button, CheckBox, ProgressBar, OptionButton, SpinBox, Panel, PanelContainer, ScrollContainer, TabContainer.

**Actions**:
| Action | Purpose |
|--------|---------|
| `ui_create` | Create widget, returns `node_id` |
| `ui_set_prop` | Set property (min_w, min_h, text, checked, etc.) |
| `ui_theme_set` | Override style (bg_color, border_color, corner_radius) |
| `ui_connect` | Register signal connection |
| `ui_destroy` | Remove node + subtree |
| `ui_render_frame` | Layout + render to PNG |
| `ui_get_tree` | JSON dump of node tree |
| `ui_clear` | Reset entire tree |

**Example** — building a dashboard:
```
ui_create("VBoxContainer", parent=0, {separation: 10})     → node 1
ui_create("Label", parent=1, {text: "Agent Dashboard"})     → node 2
ui_create("Button", parent=1, {text: "New Task"})           → node 3
ui_create("ProgressBar", parent=1, {range_min:0, range_max:100, range_value:75})  → node 4
ui_render_frame({path: "dashboard.png", width: 600, height: 400})
```

**Rendering**: Dark theme, `SDL_RenderDebugText` (no TTF), two-pass layout (bottom-up min size, top-down distribution). Size flags: FILL=0, EXPAND=1, SHRINK_CENTER=2.

### Adding a New Tool

1. Create `runtime/tools/my_tool.c`:

```c
#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#include "tool_api.h"

static cJSON *my_tool(cJSON *args, const char *workspace, char **error) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

static ToolInfo info = { .api_version = 1, .struct_size = sizeof(ToolInfo),
    .name = "my_tool", .description = "Does something useful" };

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return my_tool; }
```

2. Rebuild with `cmake --build build`.

## Build Requirements

- [MSYS2](https://www.msys2.org/) with MinGW64 GCC 15+
- CMake 3.20+
- SDL3 + SDL3_image (for sdl3_render tool)
- TDLib (for Telegram integration)
- WinHTTP (system library)

## Install & Update

### NSIS Installer

Download `lulu-{version}-setup.exe` from [GitHub Releases](https://github.com/slothitude/lulu/releases) and run. The installer:
- Copies binaries to `Program Files\Lulu\`
- Creates empty `state/`, `workspace/`, `tg_data/` directories
- Optionally adds install dir to PATH
- Optionally installs SDL3 runtime and tool plugins
- Writes `HKLM\Software\Lulu\InstallDir` registry key (used by updater)
- Creates Start Menu shortcuts

### Updating

From any channel (CLI or Telegram):
```
/update              # Check for new version
/update confirm      # Apply update + restart
```

Or run the standalone updater directly:
```
updater.exe --check                    # Check GitHub for latest release
updater.exe --apply --restart          # Download, install, restart agent
updater.exe --apply --install-dir C:\path  # Custom install location
```

The updater preserves user data during updates: `config.json`, `state/`, `workspace/`, `tg_data/` are never overwritten.

### Packaging

Build a distributable zip locally:
```bash
./scripts/package.sh          # → dist/lulu-{version}-win64.zip
```

Push a tag to trigger CI/CD:
```bash
git tag v4.1.0
git push origin v4.1.0        # → GitHub Actions builds + creates draft release
```

## Security

- All file operations sandboxed to `workspace/`
- Path traversal (`..`, absolute paths, system dirs) blocked
- No arbitrary command execution
- Tool DLLs have no LLM access
- `/graph` command restricted to read-only MATCH/RETURN queries
- `config.json` excluded from git (contains API key)

## License

MIT
